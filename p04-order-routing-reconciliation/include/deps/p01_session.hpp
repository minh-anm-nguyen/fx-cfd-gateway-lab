// P01 -- the FIX session engine, as P04 is allowed to see it.
//
// Design doc 6.1 proposes two things P01 does not have yet, and is blunt about
// the consequence: "Chua phai API co san cua P01; P01 phai implement va kiem
// thu". So this header models the *contract*, and the fake below can be built
// with the contract switched off (`stable_send_supported = false`) so that
// PB-03 can prove P04 refuses to claim issue-once when the platform cannot
// provide it.
//
// The two contracts:
//   StableSendV1  -- (session, command_id) maps to at most one original
//                    outgoing sequence number, forever. Same payload gives the
//                    same sequence; a different payload is a hard conflict.
//                    This is what R-I06 rests on.
//   FixInboundV1  -- ordered, at-least-once durable delivery with a receipt ID
//                    that P04 acknowledges only after its own commit.
//
// Everything about sequence numbers, resend and reconnect stays on this side of
// the boundary (design doc 5.1: P04 may not touch NextNumIn/NextNumOut).
#pragma once

#include <cstdint>
#include <deque>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "router/failpoint.hpp"
#include "router/hash.hpp"
#include "router/ids.hpp"

namespace router::p01 {

struct SessionIdentity {
    std::string sender_comp_id;
    std::string target_comp_id;
    std::string qualifier;
    std::string key() const { return sender_comp_id + ":" + target_comp_id + ":" + qualifier; }
};

enum class SessionState { Disconnected, Connecting, LogonSent, Synchronizing, Active, Down };
std::string_view to_string(SessionState s);

// One durable inbound application frame (design doc 6.1).
struct DurableFixInbound {
    std::string session_key;
    SessionEpoch epoch = 0;
    MsgSeqNum msg_seq_num = 0;
    DurableReceiptId durable_inbox_id = 0;
    std::string frame;
    Hash raw_sha256;
};

struct StableSendResult {
    enum class State {
        AssignedOriginalSeq,  // a fresh original sequence was bound and written
        AlreadyAssigned,      // this command already owns a sequence
        NotReady,             // session not ACTIVE, nothing was bound
        Unknown,              // bound, but whether the bytes left is unknown
        Fatal,                // same command_id with a different payload
        NotSupported,         // P01 build without StableSendV1
    };
    State state = State::NotReady;
    std::optional<MsgSeqNum> original_seq;
};

std::string_view to_string(StableSendResult::State s);

// The durable side of the session: survives a P04 restart, because it is P01's
// own store rather than P04's.
struct SessionImage {
    struct Binding {
        MsgSeqNum original_seq = 0;
        // Which storage epoch issued the sequence. A bilateral reset restarts
        // MsgSeqNum at 1, so a sequence number is only unique within an epoch;
        // without this field two unrelated commands across a day rollover would
        // look like a double original send.
        SessionEpoch epoch = 0;
        Hash payload_hash;
        std::string frame;
        // Digest of the stored frame. A resend store whose bytes no longer hash
        // to this cannot prove what was issued, so send_once fails closed
        // instead of claiming issue-once (OR-E-13).
        Hash frame_sha;
        bool written = false;  // did the frame actually reach the socket?
    };

    SessionIdentity identity;
    SessionEpoch epoch = 1;
    SessionState state = SessionState::Active;
    MsgSeqNum next_out = 1;
    MsgSeqNum next_in = 1;

    std::map<std::string, Binding> bindings;  // command_id -> binding
    std::deque<DurableFixInbound> inbox;      // ordered, pending acknowledgement
    std::set<DurableReceiptId> acked;
    DurableReceiptId next_receipt = 1;

    // Capability and misconfiguration switches used by the F-series tests.
    bool stable_send_supported = true;
    bool reset_on_logon = false;
    bool resend_history_lost = false;  // OR-E-13, OR-F-13
    bool blackhole = false;            // bytes accepted but never delivered, OR-E-20
    // Incremented by every engine that attaches. An engine whose generation is
    // no longer the current one has lost the session lock, which is how a split
    // brain is prevented (OR-F-15).
    std::uint64_t owner_generation = 0;

    std::vector<std::string> wire_log;      // frames handed to the peer
    std::vector<std::string> refused_inbound;  // frames arriving out of session
};

// Anything the session hands the frame to. The LP simulator implements it; a
// test can also pass nothing and inspect `wire_log` directly.
class IPeer {
public:
    virtual ~IPeer() = default;
    virtual void on_app_frame(const std::string& frame) = 0;
};

// The engine below is the in-tree stand-in. Project 08 substitutes a subclass
// that drives the real P01 session for the FIX rules and the socket, and P06's
// StableSendStore for the issue-once binding and the durable inbox -- which is
// where those two actually live, P01 having no command-keyed binding of its own
// and P06 shipping `jrnl::StableSendStore` for exactly this boundary.
//
// Only the behavioural members are virtual. The state accessors read `image_`,
// and a substituted engine owns the image it passes to this constructor, so
// they keep answering for it without an override.
class SessionEngine {
public:
    SessionEngine(SessionImage& image, FailpointRegistry& fp);
    virtual ~SessionEngine() = default;

    SessionEngine(const SessionEngine&) = delete;
    SessionEngine& operator=(const SessionEngine&) = delete;

    virtual void attach_peer(IPeer* peer) { peer_ = peer; }

    SessionState state() const { return image_.state; }
    void set_state(SessionState s) { image_.state = s; }
    bool ready_for_app() const { return image_.state == SessionState::Active; }
    const SessionIdentity& identity() const { return image_.identity; }
    SessionEpoch epoch() const { return image_.epoch; }

    // The StableSendV1 entry point. `app_frame` is the application field block;
    // this engine prepends nothing visible to P04 but owns 34/49/52/56 on the
    // real wire.
    virtual StableSendResult send_once(const CommandId& command_id, const Hash& payload_hash,
                                       const std::string& app_frame);

    // Looks up an existing binding without sending. Used by recovery to find out
    // whether a sequence was already issued (design doc 15.3 step 3).
    virtual std::optional<SessionImage::Binding> lookup_binding(const CommandId& command_id) const;

    // --- FixInboundV1 ------------------------------------------------------
    // Next unacknowledged receipt, in order. Nothing is removed until the
    // consumer acknowledges after its own commit.
    virtual std::optional<DurableFixInbound> peek_inbound() const;
    virtual void ack_inbound_after_commit(DurableReceiptId id);
    virtual std::size_t pending_inbound() const;

    // Called by the peer side. Returns the receipt id.
    virtual DurableReceiptId deliver_inbound(const std::string& frame);
    // At-least-once delivery in the raw: hands the same receipt out again.
    virtual void redeliver(DurableReceiptId id);

    // Session lifecycle events a test can drive.
    virtual void disconnect();
    virtual void reconnect(bool reset_sequences);
    void bump_epoch() { ++image_.epoch; }

    SessionImage& image() { return image_; }
    const SessionImage& image() const { return image_; }
    bool owns_session() const { return generation_ == image_.owner_generation; }

protected:
    SessionImage& image_;
    FailpointRegistry& fp_;
    IPeer* peer_ = nullptr;
    std::uint64_t generation_ = 0;
};

}  // namespace router::p01
