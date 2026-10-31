// The P01 boundary: stable outbound send and durable inbound receipts.
//
// Section 6 names the open integration blocker plainly. If P01's session
// store is a different database from P06's journal, there is no single
// transaction spanning the two, and the contract degrades to: P01 persists a
// stable (session, command_id, payload_hash) -> original MsgSeqNum mapping
// BEFORE the socket write, and delivers inbound receipts at least once.
//
// This file models that boundary honestly by putting the mapping in its own
// database file. Two stores means two commits, which is exactly the hazard
// the doc describes, and it is the only way the crash windows W13 to W21 can
// be reproduced rather than asserted.
//
// What send_once guarantees (JI-18, JI-19): the same key always returns the
// same original sequence number, and a retry never mints a second one. What
// it does NOT guarantee, and must never be read as: that the peer received
// the bytes, or that the LP acted on them (JI-16, JI-17).
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "jrnl/clock.hpp"
#include "jrnl/hash.hpp"
#include "jrnl/ids.hpp"
#include "jrnl/result.hpp"
#include "jrnl/sql.hpp"

namespace jrnl {

struct SendBinding {
    // The sequence number is durably bound to this key. It does not say the
    // bytes reached anyone.
    bool bound = false;
    std::uint64_t original_seq = 0;
    // The call could not establish whether a send happened. The only safe
    // responses are HOLD and reconciliation (JI-20).
    bool ambiguous = false;
    // The key already existed with different content.
    bool conflict = false;
    std::string detail;
};

class IStableFixSend {
public:
    virtual ~IStableFixSend() = default;
    virtual SendBinding send_once(const SessionKey& session, const CommandId& command,
                                  const Digest& payload_hash, const std::string& payload) = 0;
};

// One inbound receipt as P01 holds it, before P06 has applied anything.
struct RawReceipt {
    SessionKey session_key;
    std::uint64_t session_epoch = 0;
    std::uint64_t receipt_id = 0;
    std::string raw;
    Digest raw_hash{};
    bool delivered = false;
    bool acked = false;
};

class StableSendStore final : public IStableFixSend {
public:
    struct Options {
        std::string path;
        const IClock* clock = nullptr;
        // Where the bytes go once the sequence is durable. Returning false
        // models a socket error; the callback is also where the partial
        // write and lost callback windows are injected.
        std::function<bool(const SessionKey&, std::uint64_t, const std::string&)> wire;
    };

    static StoreError open(const Options& opt, std::unique_ptr<StableSendStore>* out,
                           std::string* why);
    ~StableSendStore() override;

    SendBinding send_once(const SessionKey& session, const CommandId& command,
                          const Digest& payload_hash, const std::string& payload) override;

    // What sequence, if any, is bound to this key. Used by reconciliation
    // after a restart to answer "did we already issue this?" without issuing
    // anything (section 10, the W14 row).
    bool bound_sequence(const SessionKey& session, const CommandId& command,
                        std::uint64_t* seq, Digest* hash) const;

    // How many distinct original sequence numbers this session ever handed
    // out. The release gate for JI-19 is that this never exceeds the number
    // of distinct commands.
    std::uint64_t allocated_sequences(const SessionKey& session) const;

    // Count of keys whose sequence is durable but whose bytes were never
    // confirmed written. These are the UNKNOWN candidates after a restart.
    std::size_t unconfirmed_sends(const SessionKey& session) const;

    // --- Inbound half ---
    // P01 persists the raw message before it tells anyone about it (W18).
    StoreError persist_raw(const RawReceipt& r);
    // Everything persisted but not yet acknowledged by P06. Redelivered on
    // every call until acked, which is what makes delivery at-least-once.
    std::vector<RawReceipt> undelivered(const SessionKey& session, std::size_t max) const;
    StoreError mark_delivered(const SessionKey& session, std::uint64_t epoch,
                              std::uint64_t receipt_id);
    // Called only after P06 has committed the business transaction.
    StoreError ack_delivery(const SessionKey& session, std::uint64_t epoch,
                            std::uint64_t receipt_id);
    std::size_t unacked_count(const SessionKey& session) const;

    // Session epoch handling. A reconnect bumps the epoch, and a receipt id
    // is only unique within one epoch.
    std::uint64_t current_epoch(const SessionKey& session) const;
    StoreError bump_epoch(const SessionKey& session, std::uint64_t* now);

    Db& db() { return *db_; }

private:
    StableSendStore() = default;

    std::unique_ptr<Db> db_;
    const IClock* clock_ = nullptr;
    std::unique_ptr<SystemClock> owned_clock_;
    std::function<bool(const SessionKey&, std::uint64_t, const std::string&)> wire_;
};

}  // namespace jrnl
