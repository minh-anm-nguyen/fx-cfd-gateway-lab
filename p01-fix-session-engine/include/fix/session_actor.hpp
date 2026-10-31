// FIX 4.4 session engine (initiator).
//
// Single owner: every mutation of session state happens inside one of the entry
// points below, which the driver calls from one thread (design doc 4.2,
// invariant I-01). The engine performs no I/O of its own: it writes bytes
// through ITransport, asks for connections through IConnectionController, and
// hands application messages to IApplicationSink only after a durable commit.
//
// States and transitions follow design doc 6. The distinction that matters most
// is TCP_CONNECTED != FIX_READY (invariant I-10): ACTIVE is reached only after
// a validated Logon and completed synchronisation.
#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "fix/audit.hpp"
#include "fix/clock.hpp"
#include "fix/codec.hpp"
#include "fix/config.hpp"
#include "fix/framer.hpp"
#include "fix/metrics.hpp"
#include "fix/ports.hpp"
#include "fix/resend_planner.hpp"
#include "fix/store.hpp"

namespace fixse {

enum class SessionState {
    Stopped,
    Connecting,
    TcpConnected,
    AwaitLogon,
    Synchronizing,
    Active,
    LoggingOut,
    Backoff,
    Faulted,
};

std::string_view to_string(SessionState s);

struct EngineDeps {
    const SessionConfig* cfg = nullptr;
    ISessionStore* store = nullptr;
    IClock* clock = nullptr;
    ITransport* transport = nullptr;
    IConnectionController* controller = nullptr;
    IApplicationSink* sink = nullptr;
    Metrics* metrics = nullptr;
    AuditLog* audit = nullptr;
};

enum class SubmitResult {
    Accepted,
    RejectedNotActive,     // L-07: no unsequenced send during recovery
    RejectedBackpressure,  // outbound queue cap reached
    RejectedFaulted,
    RejectedStoreFailure,  // I-06: nothing goes on the wire without a commit
    RejectedBadMessage,
};

std::string_view to_string(SubmitResult r);

class SessionEngine {
public:
    explicit SessionEngine(const EngineDeps& deps);

    // Load persisted state, verify store consistency and prepare the inbox
    // dispatcher. Returns a store error when the engine must not start.
    StoreError start();

    // Driver entry points. All of them tolerate a stale generation: a callback
    // for a connection that is no longer current is ignored and audited.
    void on_connected(uint64_t generation);
    void on_bytes(std::string_view bytes, uint64_t generation);
    void on_disconnect(uint64_t generation, std::string_view reason);
    void on_timer(SteadyTime now);

    void request_graceful_stop();
    // Operator action requiring dual control; refuses without two approvers.
    bool operator_coordinated_reset(const std::string& approver_a, const std::string& approver_b,
                                    const std::string& reason);

    // Submit an application message. Accepted only in ACTIVE, and only once the
    // outbound commit succeeded.
    SubmitResult submit_app_message(std::string_view msg_type,
                                    const std::vector<std::pair<int, std::string>>& body);

    // Introspection for the driver, the metrics endpoint and the tests.
    SessionState state() const { return state_; }
    FaultReason fault_reason() const { return fault_; }
    const std::string& fault_detail() const { return fault_detail_; }
    uint64_t epoch() const { return st_.epoch; }
    uint64_t next_in() const { return st_.next_in; }
    uint64_t next_out() const { return st_.next_out; }
    uint64_t active_generation() const { return active_generation_; }
    // Readiness is ACTIVE only: a health check must not report ready while a
    // gap is still being recovered (scenario K-09).
    bool ready() const { return state_ == SessionState::Active; }
    bool test_request_pending() const { return pending_test_req_id_.has_value(); }
    const std::string& pending_test_req_id() const;
    size_t future_buffer_size() const { return future_.size(); }
    size_t future_buffer_bytes() const { return future_bytes_; }
    size_t pending_tx_bytes() const { return tx_.size() - tx_written_; }
    uint64_t resend_requests_sent() const { return resend_requests_sent_; }
    int resend_cycles() const { return resend_cycles_; }
    std::optional<std::pair<uint64_t, uint64_t>> open_gap() const;
    size_t pending_inbox() const { return pending_inbox_; }
    bool inbox_paused() const { return inbox_paused_; }
    uint64_t garbled_frames() const { return garbled_frames_; }
    // Sequences the peer skipped with a GapFill and that were therefore never
    // recovered. Accepting a GapFill is not the same as having the messages,
    // so this count is kept and surfaced rather than cleared.
    uint64_t skipped_unrecovered() const { return skipped_unrecovered_; }
    // Bytes the engine will accept into the outbound queue before applying
    // backpressure to the application.
    size_t tx_queue_cap() const;

private:
    // --- outbound -----------------------------------------------------------
    // Commit an original message, then queue its bytes. Returns false when the
    // commit failed, in which case nothing was queued (invariant I-06).
    bool send_original(std::string_view msg_type,
                       const std::vector<std::pair<int, std::string>>& body);
    // Queue a retransmission: original sequence, PossDupFlag=Y, fresh
    // SendingTime, recomputed BodyLength and CheckSum. Never advances
    // NextNumOut (invariant I-05, design doc 17.5).
    bool send_retransmission(const ResendAction& action);
    bool send_gapfill(const ResendAction& action);
    void queue_bytes(std::string_view bytes);
    void flush_tx();

    void send_logon();
    void send_logout(std::string_view text);
    void send_heartbeat(std::string_view test_req_id);
    void send_test_request();
    void send_resend_request(uint64_t begin, uint64_t end);
    void send_session_reject(const DecodedMessage& msg, RejectReason reason,
                             std::string_view text);

    // --- inbound ------------------------------------------------------------
    void handle_frame(std::string_view raw);
    void handle_decoded(const DecodedMessage& msg);
    bool validate_identity(const DecodedMessage& msg);
    bool validate_sending_time(const DecodedMessage& msg);
    void handle_in_sequence(const DecodedMessage& msg);
    void handle_sequence_too_high(const DecodedMessage& msg);
    void handle_sequence_too_low(const DecodedMessage& msg);
    void apply_session_message(const DecodedMessage& msg);
    void on_logon(const DecodedMessage& msg);
    void on_logout(const DecodedMessage& msg);
    void on_test_request(const DecodedMessage& msg);
    void on_heartbeat(const DecodedMessage& msg);
    void on_resend_request(const DecodedMessage& msg);
    void on_sequence_reset(const DecodedMessage& msg);

    bool commit_inbound_and_dispatch(const DecodedMessage& msg);
    void drain_future_buffer();
    void buffer_future(const DecodedMessage& msg);
    void dispatch_pending_inbox();
    void open_gap(uint64_t from, uint64_t to);
    void close_gap();

    // --- lifecycle ----------------------------------------------------------
    void transition(SessionState next, std::string_view reason);
    void enter_fault(FaultReason reason, std::string_view detail);
    void enter_backoff(std::string_view reason);
    void reset_connection_state();
    void check_recovery_progress();
    void maybe_finish_synchronising();
    uint64_t next_backoff_ms();
    void audit_event(AuditEvent kind, uint64_t from, uint64_t to, std::string_view reason);
    bool is_app_allowed(std::string_view msg_type) const;

    EngineDeps d_;
    const SessionConfig& cfg_;
    SessionState state_ = SessionState::Stopped;
    PersistedState st_;
    bool started_ = false;

    FaultReason fault_ = FaultReason::None;
    std::string fault_detail_;

    FixFramer framer_;
    uint64_t active_generation_ = 0;
    bool connection_live_ = false;
    bool stop_requested_ = false;

    // Outbound byte queue. A frame is appended only after its commit, and a
    // partial write resumes from exactly where it stopped (scenario A-13).
    std::string tx_;
    size_t tx_written_ = 0;

    // Timers, all on the monotonic clock.
    SteadyTime now_{};
    SteadyTime last_inbound_valid_{};
    SteadyTime last_outbound_{};
    SteadyTime frame_started_{};
    bool frame_started_armed_ = false;
    SteadyTime next_connect_at_{};
    bool next_connect_armed_ = false;
    SteadyTime logon_deadline_{};
    bool logon_deadline_armed_ = false;
    uint64_t backoff_ms_ = 0;
    uint64_t backoff_rng_ = 0x9E3779B97F4A7C15ull;

    std::optional<std::string> pending_test_req_id_;
    SteadyTime pending_test_deadline_{};
    uint64_t test_req_counter_ = 0;

    // Recovery state for an inbound gap.
    bool gap_open_ = false;
    uint64_t gap_begin_ = 0;
    uint64_t gap_end_ = 0;
    SteadyTime recovery_started_{};
    SteadyTime last_resend_request_at_{};
    bool last_resend_request_armed_ = false;
    int resend_cycles_ = 0;
    uint64_t resend_requests_sent_ = 0;
    int resend_requests_served_ = 0;
    SteadyTime resend_window_started_{};

    // Bounded out-of-order buffer: sequence number to raw frame.
    std::map<uint64_t, std::string> future_;
    size_t future_bytes_ = 0;

    size_t pending_inbox_ = 0;
    uint64_t garbled_frames_ = 0;
    uint64_t skipped_unrecovered_ = 0;
    bool inbox_paused_ = false;

    std::string empty_;
};

}  // namespace fixse
