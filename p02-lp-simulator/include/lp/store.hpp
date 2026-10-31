// Durable business store: the interface the four transaction boundaries of
// design doc 10.2 are expressed through.
//
//   TX-A  commit_inbound()    (session_id, epoch, MsgSeqNum) + durable inbox
//   TX-B  commit_business()   registry + order + quote + fill + outbox, atomic
//   TX-C  send_once()         outbox event_id -> ONE outbound sequence number
//   TX-D  mark_sent()         dispatch bookkeeping only
//
// Two properties of this shape matter more than the storage engine behind it:
//
//   * no socket write happens inside TX-B. The business decision is durable
//     before anything leaves the process, and the report is a consequence of
//     committed state rather than a promise about state (invariant I-14).
//   * send_once() is the ONLY way a report gets an original sequence number,
//     and an event_id that already has one can never get a second. That is
//     invariant I-15, and design doc 3.2 is explicit that a stock QuickFIX
//     sender does not give it to you for free: it has to be implemented and
//     proven, which is what the W5/W6 crash tests do.
//
// What this store does NOT claim: that the peer received anything. A successful
// commit means the local intent is durable, not that the report was applied
// remotely (design doc 10.5).
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "lp/domain.hpp"
#include "lp/quote_book.hpp"
#include "lp/result.hpp"
#include "lp/scheduler.hpp"

namespace lpsim {

// ---------------------------------------------------------------------------
// Persisted session state
// ---------------------------------------------------------------------------

struct SessionState {
    std::string session_id;
    std::string sender_comp_id;  // the LP side
    std::string target_comp_id;  // the client side
    std::uint64_t epoch = 1;
    std::uint64_t next_in = 1;
    std::uint64_t next_out = 1;
};

// ---------------------------------------------------------------------------
// Request registry
// ---------------------------------------------------------------------------

struct RegistryRow {
    BusinessKey key;
    RequestType type = RequestType::New;
    std::uint64_t canonical_hash = 0;
    std::string order_id;  // the chain this request targeted, when known
    RequestOutcome outcome = RequestOutcome::Pending;
    BizReject reject = BizReject::None;  // preserved so a resend gets the same answer
};

// ---------------------------------------------------------------------------
// TX-B payload: every effect of one business decision, committed together
// ---------------------------------------------------------------------------

struct QuoteConsumption {
    std::string symbol;
    std::uint64_t version = 0;
    Side taker_side = Side::Buy;
    Units qty = 0;
};

struct BusinessTxn {
    std::vector<RegistryRow> registry;
    std::vector<OrderChain> chains;  // inserted or updated wholesale
    std::vector<Fill> fills;
    std::vector<OutboxEntry> outbox;
    std::vector<QuoteConsumption> quote_consumptions;
    std::vector<QuoteSlice> quote_updates;

    // Scheduler effects. An executed id is recorded in the SAME transaction as
    // the effect it produced, which is what makes W9 safe: either both landed
    // or neither did.
    std::vector<std::string> executed_event_ids;
    std::vector<ScheduledEvent> new_events;

    // Allocator high-water marks after this decision. Persisting them with the
    // rows that consumed them is what closes C-14.
    std::uint64_t next_order_counter = 0;
    std::uint64_t next_exec_counter = 0;
    std::uint64_t next_fill_counter = 0;
    std::uint64_t next_event_counter = 0;

    // Inbound identities whose business delivery this decision acknowledges.
    std::vector<std::uint64_t> acked_inbound_seqs;

    std::vector<std::string> audit;
};

// ---------------------------------------------------------------------------
// TX-C
// ---------------------------------------------------------------------------

// Builds the full original frame for a given sequence number. The store calls
// it inside the transaction so that the frame it journals is byte-identical to
// the one the socket writer will send and to the one a later resend replays.
using FrameEncoder = std::function<std::string(std::uint64_t seq)>;

struct SendOnceResult {
    std::uint64_t msg_seq_num = 0;
    std::string frame;
    // false means the event already owned a sequence number and the stored
    // frame was returned unchanged. A caller that sees this must not treat the
    // frame as a new original message (I-12).
    bool newly_assigned = false;
};

// ---------------------------------------------------------------------------
// Startup recovery
// ---------------------------------------------------------------------------

struct RecoveryReport {
    bool ok = false;
    HaltReason halt = HaltReason::None;
    std::string detail;

    SessionState session;
    std::vector<OrderChain> chains;
    std::vector<Fill> fills;
    std::vector<OutboxEntry> outbox;
    std::vector<QuoteSlice> quotes;
    std::vector<RegistryRow> registry;
    std::vector<ScheduledEvent> pending_events;
    std::vector<std::string> executed_event_ids;

    std::uint64_t next_order_counter = 1001;
    std::uint64_t next_exec_counter = 1;
    std::uint64_t next_fill_counter = 1;
    std::uint64_t next_event_counter = 1;

    std::size_t pending_inbox = 0;
    bool unclean_shutdown = false;
};

struct StoreConfig {
    std::string path;  // SQLite database file
    std::string session_id;
    std::string sender_comp_id;
    std::string target_comp_id;
    bool wal = true;
    bool synchronous_full = true;
    // Bounded resources (design doc 12.1). Reaching a cap is a controlled
    // refusal, never a reason to drop a committed row (I-17).
    std::size_t max_pending_outbox = 100000;
    std::size_t max_inbox = 10000;
    std::size_t max_live_orders = 100000;
};

class IBusinessStore {
public:
    virtual ~IBusinessStore() = default;

    // Open or create the store and take exclusive ownership of it. A second
    // process on the same path is refused rather than allowed to interleave
    // writes (J-12).
    virtual StoreError open(const StoreConfig& cfg) = 0;
    virtual void close() = 0;

    // Verify the store against itself and produce the state to resume from.
    // Any inconsistency sets halt and ok=false: recovery never guesses.
    virtual RecoveryReport recover_and_verify() = 0;

    // TX-A. *duplicate is set when this (session, epoch, seq) was already
    // committed, in which case nothing changes and no business delivery is due.
    //
    // needs_delivery distinguishes an application message, which owes the
    // business layer a delivery and stays pending until acknowledged, from a
    // session-level message, which only has to advance NextNumIn. Both occupy
    // the same (session, epoch, seq) key space, so the de-duplication layer
    // stays uniform across admin and application traffic (I-16).
    virtual StoreError commit_inbound(const InboundIdentity& id, std::string_view raw_frame,
                                      bool needs_delivery, bool* duplicate) = 0;

    // Undelivered inbound rows, oldest first, for re-dispatch after a restart.
    virtual StoreError pending_inbox(
        std::vector<std::pair<std::uint64_t, std::string>>* out) = 0;

    // TX-B.
    virtual StoreError commit_business(const BusinessTxn& txn) = 0;

    // TX-C.
    virtual StoreError send_once(const std::string& event_id, const CanonicalBody& body,
                                 const FrameEncoder& encode, SendOnceResult* out) = 0;

    // TX-D.
    virtual StoreError mark_sent(const std::string& event_id) = 0;

    // Allocate an outbound sequence number for a SESSION-level message.
    //
    // Admin traffic is not journaled for replay: FIX answers a resend over
    // heartbeats and logons with a GapFill rather than a retransmitted
    // heartbeat (I-07). It does, however, consume sequence numbers, so the
    // counter has to advance in the store. Keeping a second counter in memory
    // would let a restart hand an already-used number to a business report.
    virtual StoreError take_admin_seq(std::uint64_t* seq) = 0;

    // Outbox rows that still need dispatching, in creation order.
    virtual StoreError pending_outbox(std::vector<OutboxEntry>* out) = 0;

    // Stored original frames for a resend range, inclusive. Missing history in
    // the requested range is reported as Inconsistent so the caller can HALT
    // instead of fabricating a gap fill over a trade (I-08).
    virtual StoreError outbound_history(
        std::uint64_t begin_seq, std::uint64_t end_seq,
        std::vector<std::pair<std::uint64_t, std::string>>* out) = 0;

    virtual StoreError load_session_state(SessionState* out) = 0;

    // Advance NextNumIn after an inbound commit, or apply an agreed sequence
    // reset. A reset changes session numbers only: orders, fills and the
    // business de-duplication namespace are untouched (design doc 3.4, I-16).
    virtual StoreError apply_coordinated_reset(std::uint64_t new_epoch) = 0;

    // Diagnostics for metrics and the invariant checker.
    virtual std::size_t count_fills() const = 0;
    virtual std::size_t count_outbox_pending() const = 0;
};

}  // namespace lpsim
