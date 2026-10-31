// FIX 4.4 acceptor session.
//
// Reference: design doc 5.1 (the session actor owns session state and never
// decides fills), 5.3 (runtime states) and matrix groups A, I and K.
//
// The hard rule this class exists to enforce is invariant I-01: no application
// message reaches the order actor before the session is authenticated,
// synchronised and the frame is durably committed. Everything else here -
// heartbeats, resend planning, sequence policy - is in service of that, plus
// invariant I-15 on the outbound side, where send_once() is the only door to a
// sequence number.
//
// Transport is abstracted so the whole session can be driven from a test
// without a socket. That is not only convenience: scenarios K-07 (one-byte
// chunks), K-08 (RST mid-frame) and K-14 (peer stops reading) need a transport
// that fails in specific ways on demand, which a real socket will not do
// reliably.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "lp/clock.hpp"
#include "lp/codec.hpp"
#include "lp/framer.hpp"
#include "lp/order_actor.hpp"
#include "lp/store.hpp"

namespace lpsim {

class ITransport {
public:
    virtual ~ITransport() = default;
    // Returns the number of bytes accepted. A short write is normal and is
    // retried; it must never be reported as a complete one.
    virtual std::size_t write(std::string_view bytes) = 0;
    virtual void close() = 0;
    virtual bool connected() const = 0;
};

// Design doc 5.3. LISTENING means the acceptor socket is ready, NOT that
// application messages may be processed.
enum class SessionPhase {
    Initializing,
    Listening,
    LogonPending,
    Synchronizing,
    Active,
    Draining,
    Halted,
};

std::string_view to_string(SessionPhase p);

struct SessionConfig {
    std::string session_id = "LP-SESSION-1";
    std::string sender_comp_id = "SIMLP01";  // the LP
    std::string target_comp_id = "TRADER01";  // the client
    std::string begin_string = std::string(kBeginStringFix44);

    int heartbeat_seconds = 30;
    int min_heartbeat_seconds = 1;
    int max_heartbeat_seconds = 300;
    std::int64_t sending_time_tolerance_s = 120;

    // Credentials. Compared when require_credentials is set; never logged
    // (L-07). An empty expected password with require_credentials set is a
    // configuration error, not a wildcard.
    bool require_credentials = false;
    std::string username;
    std::string password;

    // A sequence reset asked for by the peer is refused unless the profile
    // allows it. Design doc ADR-09: no automatic reset on reconnect (A-10).
    bool allow_reset_on_logon = false;

    std::size_t max_frame_bytes = 1048576;
    std::size_t max_buffer_bytes = 2097152;
    std::uint64_t max_resend_span = 5000;   // bounded replay (I-09)
    int max_resend_requests_per_connection = 50;
};

// What the session did with one inbound frame, for tests and audit.
enum class InboundVerdict {
    Accepted,
    Duplicate,          // already committed at (session, epoch, seq)
    IgnoredPossDup,     // low sequence with 43=Y
    RejectedWire,       // malformed frame or header
    RejectedSession,    // bad CompIDs, version, credentials, policy
    QueuedGap,          // sequence ahead of expectation, resend requested
    NotActive,          // application message before the session was ready
    Disconnected,       // the session closed the connection
    Halted,
};

std::string_view to_string(InboundVerdict v);

class AcceptorSession {
public:
    AcceptorSession(SessionConfig cfg, IBusinessStore& store, OrderActor& actor,
                    IClock& clock, ITransport& transport);

    // Load persisted sequence state. Never resets it (ADR-09).
    bool recover();

    SessionPhase phase() const { return phase_; }
    HaltReason halt_reason() const { return halt_; }
    const std::string& halt_detail() const { return halt_detail_; }

    std::uint64_t next_in() const { return state_.next_in; }
    std::uint64_t next_out() const { return state_.next_out; }
    std::uint64_t epoch() const { return state_.epoch; }

    // A new TCP connection. The generation number fences callbacks from an
    // older connection, so a delayed write can never land on a new socket
    // (A-19, H-09, K-11).
    void on_connected(std::uint64_t generation);
    void on_disconnect(std::uint64_t generation);
    std::uint64_t generation() const { return generation_; }

    // Feed received bytes. Returns one verdict per complete frame processed.
    std::vector<InboundVerdict> on_bytes(std::string_view bytes, std::uint64_t generation);

    // Heartbeat, TestRequest and liveness timeouts.
    void on_timer();

    // Assign sequence numbers to pending outbox rows and write them. Called
    // after the actor commits and after a reconnect, so a report that was
    // committed while the peer was away is delivered once the peer returns.
    std::size_t dispatch_outbox();

    // Frames the transport accepted, for assertions.
    const std::vector<std::string>& sent_frames() const { return sent_; }
    void clear_sent() { sent_.clear(); }

    // Graceful shutdown: send Logout and stop admitting application messages.
    void request_logout(std::string_view text);

    std::size_t resend_requests_served() const { return resend_served_; }
    std::size_t duplicates_ignored() const { return duplicates_ignored_; }

private:
    InboundVerdict process_frame(std::string_view raw);
    InboundVerdict handle_logon(const DecodedMessage& m);
    InboundVerdict handle_session_message(const DecodedMessage& m);
    InboundVerdict handle_application(const DecodedMessage& m, std::string_view raw);
    InboundVerdict handle_resend_request(const DecodedMessage& m);
    InboundVerdict handle_sequence_reset(const DecodedMessage& m);

    // Commit the inbound identity, then report whether it is new.
    bool commit_and_check(const DecodedMessage& m, std::string_view raw, bool needs_delivery,
                          bool* duplicate);

    void send_admin(std::string_view msg_type, const CanonicalBody& body);
    void send_reject(const DecodedMessage& m, SessionRejectReason reason,
                     std::string_view text, int ref_tag);
    void send_logout(std::string_view text);
    void send_resend_request(std::uint64_t begin, std::uint64_t end);
    void send_gap_fill(std::uint64_t from_seq, std::uint64_t new_seq_no);

    // Allocate an outbound sequence number for a SESSION-level message. Admin
    // traffic is not journaled for replay: FIX answers a resend over admin
    // messages with a GapFill, never a retransmitted heartbeat (I-07).
    std::uint64_t take_admin_seq();

    bool write_frame(const std::string& frame);
    void halt(HaltReason r, std::string detail);
    bool stale(std::uint64_t generation) const { return generation != generation_; }

    SessionConfig cfg_;
    IBusinessStore& store_;
    OrderActor& actor_;
    IClock& clock_;
    ITransport& transport_;
    FixFramer framer_;

    SessionState state_;
    SessionPhase phase_ = SessionPhase::Initializing;
    HaltReason halt_ = HaltReason::None;
    std::string halt_detail_;

    std::uint64_t generation_ = 0;
    bool logon_seen_ = false;
    int negotiated_heartbeat_s_ = 30;

    SteadyTime last_rx_{};
    SteadyTime last_tx_{};
    bool test_request_outstanding_ = false;
    std::string pending_test_req_id_;
    int test_request_counter_ = 0;

    std::uint64_t gap_requested_through_ = 0;
    int resend_requests_sent_ = 0;
    std::size_t resend_served_ = 0;
    std::size_t duplicates_ignored_ = 0;

    std::vector<std::string> sent_;
};

}  // namespace lpsim
