#include "fix/session_actor.hpp"

#include <algorithm>

#include "fix/failpoint.hpp"

namespace fixse {

std::string_view to_string(SessionState s) {
    switch (s) {
        case SessionState::Stopped: return "STOPPED";
        case SessionState::Connecting: return "CONNECTING";
        case SessionState::TcpConnected: return "TCP_CONNECTED";
        case SessionState::AwaitLogon: return "AWAIT_LOGON";
        case SessionState::Synchronizing: return "SYNCHRONIZING";
        case SessionState::Active: return "ACTIVE";
        case SessionState::LoggingOut: return "LOGGING_OUT";
        case SessionState::Backoff: return "BACKOFF";
        case SessionState::Faulted: return "FAULTED";
    }
    return "UNKNOWN";
}

std::string_view to_string(SubmitResult r) {
    switch (r) {
        case SubmitResult::Accepted: return "Accepted";
        case SubmitResult::RejectedNotActive: return "RejectedNotActive";
        case SubmitResult::RejectedBackpressure: return "RejectedBackpressure";
        case SubmitResult::RejectedFaulted: return "RejectedFaulted";
        case SubmitResult::RejectedStoreFailure: return "RejectedStoreFailure";
        case SubmitResult::RejectedBadMessage: return "RejectedBadMessage";
    }
    return "Unknown";
}

SessionEngine::SessionEngine(const EngineDeps& deps)
    : d_(deps),
      cfg_(*deps.cfg),
      framer_(deps.cfg->max_frame_bytes, deps.cfg->max_read_buffer_bytes) {}

const std::string& SessionEngine::pending_test_req_id() const {
    return pending_test_req_id_.has_value() ? *pending_test_req_id_ : empty_;
}

std::optional<std::pair<uint64_t, uint64_t>> SessionEngine::open_gap() const {
    if (!gap_open_) return std::nullopt;
    return std::make_pair(gap_begin_, gap_end_);
}

size_t SessionEngine::tx_queue_cap() const { return cfg_.max_read_buffer_bytes * 4; }

void SessionEngine::audit_event(AuditEvent kind, uint64_t from, uint64_t to,
                                std::string_view reason) {
    if (d_.store != nullptr)
        d_.store->append_audit(AuditRecord{kind, st_.epoch, from, to, std::string(reason)});
    if (d_.audit != nullptr)
        d_.audit->event(to_string(kind), {{"epoch", std::to_string(st_.epoch)},
                                          {"seq_from", std::to_string(from)},
                                          {"seq_to", std::to_string(to)},
                                          {"next_in", std::to_string(st_.next_in)},
                                          {"next_out", std::to_string(st_.next_out)},
                                          {"state", std::string(to_string(state_))},
                                          {"reason", std::string(reason)}});
}

StoreError SessionEngine::start() {
    now_ = d_.clock->steady_now();
    last_inbound_valid_ = now_;
    last_outbound_ = now_;

    const StoreError e = d_.store->load_state(&st_);
    if (e != StoreError::None) {
        // A store that cannot be read, or that fails its own integrity check,
        // must not be papered over with a fresh session (G-14, G-16).
        enter_fault(e == StoreError::Corrupt ? FaultReason::StoreCorrupt
                                             : FaultReason::StoreInconsistent,
                    std::string("load_state: ") + std::string(to_string(e)));
        return e;
    }

    const ConsistencyReport rep = d_.store->verify_consistency();
    if (!rep.ok) {
        enter_fault(FaultReason::StoreInconsistent, rep.problem);
        return StoreError::Inconsistent;
    }
    if (!rep.clean_shutdown)
        audit_event(AuditEvent::UncleanShutdownDetected, st_.next_in, st_.next_out,
                    "previous run did not record a clean shutdown");

    pending_inbox_ = rep.pending_inbox_rows;
    d_.metrics->set(metric::kInboxPending, static_cast<int64_t>(pending_inbox_));
    d_.metrics->set(metric::kNextIn, static_cast<int64_t>(st_.next_in));
    d_.metrics->set(metric::kNextOut, static_cast<int64_t>(st_.next_out));

    if (cfg_.redact_secret_fields && d_.audit != nullptr) {
        d_.audit->set_secret_tags(cfg_.secret_tags);
        d_.audit->set_max_value_bytes(cfg_.max_audit_value_bytes);
        d_.audit->add_secret_value(cfg_.password);
    }

    if (!cfg_.tls_enabled && d_.audit != nullptr) {
        // Say it once, at every start: a matching CompID pair is not
        // authentication and plain TCP is not confidential (design doc 11.2,
        // scenario J-02).
        d_.audit->event("TransportNotAuthenticated",
                        {{"detail",
                          "plain TCP session: CompID validation is not authentication and the "
                          "transport is not encrypted"},
                         {"session", cfg_.session_id()}});
    }

    started_ = true;
    // Anything already committed but not yet handed to the application is
    // replayed first, in sequence order (scenario G-06).
    dispatch_pending_inbox();

    transition(SessionState::Connecting, "start");
    d_.controller->request_connect();
    return StoreError::None;
}

void SessionEngine::transition(SessionState next, std::string_view reason) {
    if (state_ == next) return;
    const SessionState from = state_;
    state_ = next;
    d_.metrics->set(metric::kState, static_cast<int64_t>(next));
    if (d_.audit != nullptr)
        d_.audit->event("StateTransition", {{"from", std::string(to_string(from))},
                                            {"to", std::string(to_string(next))},
                                            {"reason", std::string(reason)}});
}

void SessionEngine::enter_fault(FaultReason reason, std::string_view detail) {
    if (state_ == SessionState::Faulted) return;
    fault_ = reason;
    fault_detail_ = std::string(detail);
    d_.metrics->inc(metric::kFaults);
    audit_event(AuditEvent::Faulted, st_.next_in, st_.next_out,
                std::string(to_string(reason)) + ": " + std::string(detail));
    transition(SessionState::Faulted, to_string(reason));
    // Stop transmitting: a FAULTED engine holds untrusted state, and sending
    // anything from it could make reconciliation harder (design doc 6).
    tx_.clear();
    tx_written_ = 0;
    if (connection_live_ && d_.controller != nullptr) {
        connection_live_ = false;
        d_.controller->request_disconnect(active_generation_, "faulted");
    }
}

uint64_t SessionEngine::next_backoff_ms() {
    if (backoff_ms_ == 0) backoff_ms_ = static_cast<uint64_t>(cfg_.reconnect_initial_ms);
    else backoff_ms_ = std::min<uint64_t>(backoff_ms_ * 2,
                                          static_cast<uint64_t>(cfg_.reconnect_max_ms));
    // Deterministic jitter: a fixed generator keeps scenario replays
    // reproducible while still spreading reconnect attempts (scenario A-12).
    backoff_rng_ = backoff_rng_ * 6364136223846793005ull + 1442695040888963407ull;
    const uint64_t span = static_cast<uint64_t>(cfg_.reconnect_jitter_percent);
    if (span == 0) return backoff_ms_;
    const uint64_t magnitude = (backoff_ms_ * span) / 100;
    const uint64_t offset = magnitude == 0 ? 0 : (backoff_rng_ >> 33) % (2 * magnitude + 1);
    const uint64_t jittered = backoff_ms_ + offset;
    return jittered > magnitude ? jittered - magnitude : 1;
}

void SessionEngine::reset_connection_state() {
    framer_.reset();
    tx_.clear();
    tx_written_ = 0;
    pending_test_req_id_.reset();
    frame_started_armed_ = false;
    logon_deadline_armed_ = false;
    future_.clear();
    future_bytes_ = 0;
    d_.metrics->set(metric::kRecoveryBufferMessages, 0);
    d_.metrics->set(metric::kRxBufferBytes, 0);
    if (gap_open_) {
        // The gap is re-detected during resynchronisation; it is not resolved by
        // a reconnect (scenario E-11).
        audit_event(AuditEvent::GapDetected, gap_begin_, gap_end_,
                    "gap still open when the connection dropped");
        gap_open_ = false;
    }
}

void SessionEngine::enter_backoff(std::string_view reason) {
    reset_connection_state();
    if (stop_requested_) {
        transition(SessionState::Stopped, reason);
        return;
    }
    const uint64_t ms = next_backoff_ms();
    next_connect_at_ = now_ + Millis(static_cast<long long>(ms));
    next_connect_armed_ = true;
    d_.metrics->inc(metric::kReconnects);
    transition(SessionState::Backoff, reason);
    if (d_.audit != nullptr)
        d_.audit->event("BackoffScheduled",
                        {{"delay_ms", std::to_string(ms)}, {"reason", std::string(reason)}});
}

// ---------------------------------------------------------------------------
// Outbound
// ---------------------------------------------------------------------------

void SessionEngine::queue_bytes(std::string_view bytes) {
    tx_.append(bytes);
}

void SessionEngine::flush_tx() {
    if (d_.transport == nullptr) return;
    // Whether this call actually had bytes to push. The after-write failpoint
    // only makes sense when something was written, so a no-op flush must not
    // consume an occurrence.
    const bool had_data = tx_written_ < tx_.size();
    while (connection_live_ && tx_written_ < tx_.size()) {
        if (fp::fire(fp::point::kSocketBeforeWrite) == fp::Action::ReturnError) break;
        const std::string_view remaining(tx_.data() + tx_written_, tx_.size() - tx_written_);
        const long n = d_.transport->write(active_generation_, remaining);
        if (n < 0) {
            connection_live_ = false;
            d_.controller->request_disconnect(active_generation_, "write failed");
            return;
        }
        if (n == 0) break;  // would block; the next timer tick retries
        tx_written_ += static_cast<size_t>(n);
        d_.metrics->inc(metric::kBytesWritten, static_cast<uint64_t>(n));
        if (static_cast<size_t>(n) < remaining.size()) {
            // A partial write is normal; the loop resumes at the exact byte
            // offset so frame bytes are never duplicated or reordered (A-13).
            d_.metrics->inc(metric::kPartialWrites);
            fp::fire(fp::point::kSocketAfterPartialWrite);
        }
    }
    if (tx_written_ == tx_.size()) {
        tx_.clear();
        tx_written_ = 0;
        if (had_data) fp::fire(fp::point::kSocketAfterWrite);
    } else if (tx_written_ > 65536) {
        tx_.erase(0, tx_written_);
        tx_written_ = 0;
    }
}

bool SessionEngine::send_original(std::string_view msg_type,
                                  const std::vector<std::pair<int, std::string>>& body) {
    if (state_ == SessionState::Faulted) return false;
    // Fail closed at the sequence boundary rather than wrapping (scenario E-19).
    if (st_.next_out == UINT64_MAX) {
        enter_fault(FaultReason::SequenceRollback, "NextNumOut reached the 64 bit boundary");
        return false;
    }

    const uint64_t seq = st_.next_out;
    OutboundFields f;
    f.msg_type = std::string(msg_type);
    f.seq_num = seq;
    f.sender_comp_id = cfg_.sender_comp_id;
    f.target_comp_id = cfg_.target_comp_id;
    f.sending_time = d_.clock->wall_now();
    f.body = body;
    const std::string frame = encode(f);

    OutboundRecord rec;
    rec.seq = seq;
    rec.msg_type = std::string(msg_type);
    rec.payload = frame;
    rec.original_sending_time = format_utc_timestamp(f.sending_time);

    // Durable first, wire second. This ordering is the whole of invariant I-06:
    // a frame that reached the peer always has a committed history row, so a
    // resend can answer for it.
    const StoreError e = d_.store->commit_outbound(rec, seq + 1);
    if (e != StoreError::None) {
        d_.metrics->inc(metric::kStoreErrors);
        enter_fault(FaultReason::StoreCommitFailed,
                    std::string("commit_outbound: ") + std::string(to_string(e)));
        return false;
    }
    st_.next_out = seq + 1;
    d_.metrics->set(metric::kNextOut, static_cast<int64_t>(st_.next_out));

    queue_bytes(frame);
    last_outbound_ = now_;
    flush_tx();
    return true;
}

bool SessionEngine::send_retransmission(const ResendAction& action) {
    std::string frame;
    if (!rebuild_as_retransmission(action.payload, d_.clock->wall_now(), &frame)) {
        enter_fault(FaultReason::CorruptOutboundHistory,
                    "stored frame for sequence " + std::to_string(action.seq) +
                        " could not be rebuilt");
        return false;
    }
    // No commit and no NextNumOut change: a retransmission reuses the original
    // sequence number (invariant I-05, design doc 17.5).
    queue_bytes(frame);
    last_outbound_ = now_;
    d_.metrics->inc(metric::kResendMessages);
    return true;
}

bool SessionEngine::send_gapfill(const ResendAction& action) {
    OutboundFields f;
    f.msg_type = std::string(msg_type::SequenceReset);
    f.seq_num = action.seq;  // the first skipped sequence
    f.sender_comp_id = cfg_.sender_comp_id;
    f.target_comp_id = cfg_.target_comp_id;
    f.sending_time = d_.clock->wall_now();
    f.poss_dup = true;
    f.orig_sending_time = action.original_sending_time.empty()
                              ? format_utc_timestamp(f.sending_time)
                              : action.original_sending_time;
    f.body.emplace_back(tag::GapFillFlag, "Y");
    f.body.emplace_back(tag::NewSeqNo, std::to_string(action.new_seq_no));
    queue_bytes(encode(f));
    last_outbound_ = now_;
    d_.metrics->inc(metric::kGapFillRanges);
    audit_event(AuditEvent::GapFillSent, action.seq, action.new_seq_no - 1,
                "administrative messages skipped");
    return true;
}

void SessionEngine::send_logon() {
    std::vector<std::pair<int, std::string>> body;
    body.emplace_back(tag::EncryptMethod, "0");
    body.emplace_back(tag::HeartBtInt, std::to_string(cfg_.heartbeat_seconds));
    // ResetSeqNumFlag is sent only when the counterparty agreement allows it.
    // Defaulting it to Y is how sequence gaps get hidden and reconciliation
    // stops being trustworthy (design doc 8.4, scenario C-13).
    if (cfg_.allow_reset_on_logon) body.emplace_back(tag::ResetSeqNumFlag, "Y");
    if (!cfg_.username.empty()) {
        body.emplace_back(tag::Username, cfg_.username);
        body.emplace_back(tag::Password, cfg_.password);
    }
    d_.metrics->inc(metric::kLogonAttempts);
    if (!send_original(msg_type::Logon, body)) return;
    logon_deadline_ = now_ + Millis(static_cast<long long>(cfg_.heartbeat_seconds) * 1000);
    logon_deadline_armed_ = true;
    transition(SessionState::AwaitLogon, "logon sent");
}

void SessionEngine::send_logout(std::string_view text) {
    std::vector<std::pair<int, std::string>> body;
    if (!text.empty()) body.emplace_back(tag::Text, std::string(text));
    send_original(msg_type::Logout, body);
}

void SessionEngine::send_heartbeat(std::string_view test_req_id) {
    std::vector<std::pair<int, std::string>> body;
    if (!test_req_id.empty()) body.emplace_back(tag::TestReqID, std::string(test_req_id));
    if (send_original(msg_type::Heartbeat, body)) d_.metrics->inc(metric::kHeartbeatsSent);
}

void SessionEngine::send_test_request() {
    ++test_req_counter_;
    const std::string id =
        "TR-" + std::to_string(st_.epoch) + "-" + std::to_string(test_req_counter_);
    std::vector<std::pair<int, std::string>> body;
    body.emplace_back(tag::TestReqID, id);
    if (!send_original(msg_type::TestRequest, body)) return;
    pending_test_req_id_ = id;
    // The deadline is the larger of the heartbeat interval and the configured
    // test timeout (design doc 10).
    const long long ms = std::max<long long>(
        static_cast<long long>(cfg_.heartbeat_seconds) * 1000, cfg_.test_request_timeout_ms);
    pending_test_deadline_ = now_ + Millis(ms);
    d_.metrics->inc(metric::kTestRequests);
}

void SessionEngine::send_resend_request(uint64_t begin, uint64_t end) {
    std::vector<std::pair<int, std::string>> body;
    body.emplace_back(tag::BeginSeqNo, std::to_string(begin));
    body.emplace_back(tag::EndSeqNo,
                      cfg_.resend_mode == ResendMode::OpenEnded ? "0" : std::to_string(end));
    if (!send_original(msg_type::ResendRequest, body)) return;
    ++resend_requests_sent_;
    ++resend_cycles_;
    last_resend_request_at_ = now_;
    last_resend_request_armed_ = true;
    d_.metrics->inc(metric::kResendRequestsSent);
    audit_event(AuditEvent::ResendRequested, begin, end, "inbound gap recovery");
}

void SessionEngine::send_session_reject(const DecodedMessage& msg, RejectReason reason,
                                        std::string_view text) {
    std::vector<std::pair<int, std::string>> body;
    body.emplace_back(tag::RefSeqNum, std::to_string(msg.seq_num));
    if (!msg.msg_type.empty()) body.emplace_back(tag::RefMsgType, std::string(msg.msg_type));
    body.emplace_back(tag::SessionRejectReason, std::to_string(static_cast<int>(reason)));
    if (!text.empty()) body.emplace_back(tag::Text, std::string(text));
    if (send_original(msg_type::Reject, body)) d_.metrics->inc(metric::kSessionRejectsSent);
}

bool SessionEngine::is_app_allowed(std::string_view mt) const {
    if (mt.empty()) return false;
    if (is_session_message(mt)) return false;
    if (cfg_.app_msg_types.empty()) return true;
    return std::find(cfg_.app_msg_types.begin(), cfg_.app_msg_types.end(), std::string(mt)) !=
           cfg_.app_msg_types.end();
}

SubmitResult SessionEngine::submit_app_message(
    std::string_view mt, const std::vector<std::pair<int, std::string>>& body) {
    if (state_ == SessionState::Faulted) {
        d_.metrics->inc(metric::kAppSubmitRejected);
        return SubmitResult::RejectedFaulted;
    }
    // Nothing application level leaves the engine before the session is
    // synchronised: an unsequenced send during recovery is exactly what
    // scenario L-07 forbids.
    if (state_ != SessionState::Active) {
        d_.metrics->inc(metric::kAppSubmitRejected);
        return SubmitResult::RejectedNotActive;
    }
    if (!is_app_allowed(mt)) {
        d_.metrics->inc(metric::kAppSubmitRejected);
        return SubmitResult::RejectedBadMessage;
    }
    if (pending_tx_bytes() > tx_queue_cap()) {
        d_.metrics->inc(metric::kAppSubmitRejected);
        return SubmitResult::RejectedBackpressure;
    }
    if (!send_original(mt, body)) {
        d_.metrics->inc(metric::kAppSubmitRejected);
        return SubmitResult::RejectedStoreFailure;
    }
    return SubmitResult::Accepted;
}

// ---------------------------------------------------------------------------
// Connection lifecycle
// ---------------------------------------------------------------------------

void SessionEngine::on_connected(uint64_t generation) {
    now_ = d_.clock->steady_now();
    if (state_ == SessionState::Faulted || state_ == SessionState::Stopped) return;

    // A connect notification for an older generation is ignored: the engine
    // must not adopt a socket it already abandoned (scenario A-15).
    if (generation < active_generation_) {
        audit_event(AuditEvent::StateTransition, generation, active_generation_,
                    "ignored stale on_connected");
        return;
    }
    active_generation_ = generation;
    connection_live_ = true;
    reset_connection_state();
    d_.store->record_connection_generation(generation);
    d_.metrics->set(metric::kConnected, 1);
    backoff_ms_ = 0;  // a successful connect resets the backoff ladder
    transition(SessionState::TcpConnected, "tcp connected");
    send_logon();
}

void SessionEngine::on_disconnect(uint64_t generation, std::string_view reason) {
    now_ = d_.clock->steady_now();
    if (generation < active_generation_) {
        // A late disconnect for a socket that has already been replaced must
        // not tear down the current one (playbook PB-10).
        audit_event(AuditEvent::StateTransition, generation, active_generation_,
                    "ignored stale on_disconnect");
        return;
    }
    // A generation ahead of the current one is the connector reporting that the
    // connect it was asked for never came up. That is this session's failure to
    // handle, not a stale callback.
    active_generation_ = generation;
    connection_live_ = false;
    d_.metrics->set(metric::kConnected, 0);
    if (state_ == SessionState::Faulted) return;
    if (state_ == SessionState::LoggingOut || stop_requested_) {
        reset_connection_state();
        transition(SessionState::Stopped, reason);
        return;
    }
    enter_backoff(reason);
}

void SessionEngine::request_graceful_stop() {
    now_ = d_.clock->steady_now();
    stop_requested_ = true;
    if (state_ == SessionState::Synchronizing || state_ == SessionState::Active) {
        send_logout("graceful stop");
        // A logout does NOT reset sequence numbers: that happens only through a
        // scheduled, agreed reset (design doc 7.4, invariant I-12).
        logon_deadline_ = now_ + Millis(static_cast<long long>(cfg_.heartbeat_seconds) * 1000);
        logon_deadline_armed_ = true;
        transition(SessionState::LoggingOut, "graceful stop requested");
        return;
    }
    if (state_ != SessionState::Faulted) {
        if (connection_live_) d_.controller->request_disconnect(active_generation_, "stop");
        reset_connection_state();
        transition(SessionState::Stopped, "graceful stop requested");
    }
}

bool SessionEngine::operator_coordinated_reset(const std::string& approver_a,
                                               const std::string& approver_b,
                                               const std::string& reason) {
    // Dual control, audited, and never a side effect of a reconnect or a date
    // rollover (scenarios J-15, K-11, PB-12).
    const StoreError e = d_.store->coordinated_reset(approver_a, approver_b, reason);
    if (e != StoreError::None) {
        audit_event(AuditEvent::SequenceResetRefused, st_.next_in, st_.next_out,
                    "coordinated reset refused: " + std::string(to_string(e)));
        return false;
    }
    PersistedState fresh;
    if (d_.store->load_state(&fresh) != StoreError::None) {
        enter_fault(FaultReason::StoreInconsistent, "state unreadable after coordinated reset");
        return false;
    }
    st_ = fresh;
    d_.metrics->set(metric::kNextIn, static_cast<int64_t>(st_.next_in));
    d_.metrics->set(metric::kNextOut, static_cast<int64_t>(st_.next_out));
    audit_event(AuditEvent::CoordinatedResetApplied, st_.next_in, st_.next_out, reason);
    return true;
}

void SessionEngine::on_timer(SteadyTime now) {
    now_ = now;
    if (state_ == SessionState::Faulted || state_ == SessionState::Stopped) return;

    if (state_ == SessionState::Backoff && next_connect_armed_ && now_ >= next_connect_at_) {
        next_connect_armed_ = false;
        transition(SessionState::Connecting, "backoff elapsed");
        d_.controller->request_connect();
        return;
    }

    // A Logon that is never answered, or a Logout that is never acknowledged,
    // must not hold the session open for ever (A-06, C-02).
    if ((state_ == SessionState::AwaitLogon || state_ == SessionState::LoggingOut) &&
        logon_deadline_armed_ && now_ >= logon_deadline_) {
        logon_deadline_armed_ = false;
        const bool logging_out = state_ == SessionState::LoggingOut;
        audit_event(AuditEvent::StateTransition, st_.next_in, st_.next_out,
                    logging_out ? "logout acknowledgement timed out" : "logon timed out");
        connection_live_ = false;
        d_.controller->request_disconnect(active_generation_, "handshake timeout");
        if (logging_out) {
            reset_connection_state();
            transition(SessionState::Stopped, "logout timeout");
        } else {
            enter_backoff("logon timeout");
        }
        return;
    }

    // A frame that starts and never completes is a slowloris attempt (J-05).
    if (frame_started_armed_ && framer_.frame_in_progress() &&
        now_ - frame_started_ >= Millis(cfg_.partial_frame_deadline_ms)) {
        frame_started_armed_ = false;
        audit_event(AuditEvent::StateTransition, 0, 0, "incomplete frame deadline exceeded");
        connection_live_ = false;
        d_.controller->request_disconnect(active_generation_, "incomplete frame deadline");
        enter_backoff("incomplete frame deadline");
        return;
    }

    const bool logged_on = state_ == SessionState::Synchronizing ||
                           state_ == SessionState::Active || state_ == SessionState::LoggingOut;
    if (logged_on) {
        const Millis hb(static_cast<long long>(cfg_.heartbeat_seconds) * 1000);
        if (now_ - last_outbound_ >= hb) send_heartbeat("");

        // Only a VALID inbound frame counts as liveness: a flood of malformed
        // bytes does not prove the peer is processing FIX (D-14, PB-08).
        if (!pending_test_req_id_.has_value() && now_ - last_inbound_valid_ >= hb)
            send_test_request();

        if (pending_test_req_id_.has_value() && now_ >= pending_test_deadline_) {
            d_.metrics->inc(metric::kTestRequestTimeouts);
            audit_event(AuditEvent::TestRequestTimeout, 0, 0,
                        "no heartbeat matched the pending TestReqID");
            pending_test_req_id_.reset();
            connection_live_ = false;
            d_.controller->request_disconnect(active_generation_, "test request timeout");
            enter_backoff("test request timeout");
            return;
        }
    }

    if (pending_inbox_ > 0) dispatch_pending_inbox();
    check_recovery_progress();
    flush_tx();
}

// ---------------------------------------------------------------------------
// Inbound
// ---------------------------------------------------------------------------

void SessionEngine::on_bytes(std::string_view bytes, uint64_t generation) {
    now_ = d_.clock->steady_now();
    if (generation != active_generation_ || !connection_live_) {
        // Bytes belonging to a socket that is no longer current are discarded:
        // applying them would let a stale callback mutate the live session
        // (playbook PB-10, scenario K-06).
        audit_event(AuditEvent::StateTransition, generation, active_generation_,
                    "ignored bytes from a stale connection generation");
        return;
    }
    // Nothing is processed once the engine is FAULTED: the state it would be
    // applied to is no longer trusted (design doc 6).
    if (state_ == SessionState::Faulted || state_ == SessionState::Stopped) return;

    // Backpressure: while the application has not drained the durable inbox,
    // stop consuming the socket rather than growing memory (scenario I-01).
    if (inbox_paused_) {
        dispatch_pending_inbox();
        if (inbox_paused_) return;
    }

    const WireError ae = framer_.append(bytes);
    if (ae != WireError::None) {
        d_.metrics->inc(metric::kMalformedFrames);
        audit_event(AuditEvent::StateTransition, 0, 0,
                    std::string("receive buffer limit: ") + std::string(to_string(ae)));
        connection_live_ = false;
        d_.controller->request_disconnect(active_generation_, "receive buffer limit");
        enter_backoff("receive buffer limit");
        return;
    }
    d_.metrics->max_of(metric::kRxBufferBytes, static_cast<int64_t>(framer_.buffered()));

    std::string_view raw;
    WireError err = WireError::None;
    while (framer_.next(&raw, &err)) {
        handle_frame(raw);
        if (state_ == SessionState::Faulted || !connection_live_) return;
        if (inbox_paused_) return;
    }

    if (is_malformed(err)) {
        // A framing error means the position in the byte stream can no longer
        // be trusted, so the connection is dropped instead of guessing where
        // the next frame begins (B-04, B-09).
        d_.metrics->inc(metric::kMalformedFrames);
        audit_event(AuditEvent::StateTransition, 0, 0,
                    std::string("framing error: ") + std::string(to_string(err)));
        framer_.reset();
        connection_live_ = false;
        d_.controller->request_disconnect(active_generation_, "framing error");
        enter_backoff(std::string("framing error: ") + std::string(to_string(err)));
        return;
    }

    if (framer_.frame_in_progress()) {
        if (!frame_started_armed_) {
            frame_started_ = now_;
            frame_started_armed_ = true;
        }
    } else {
        frame_started_armed_ = false;
    }
    flush_tx();
}

void SessionEngine::handle_frame(std::string_view raw) {
    DecodeOptions opt;
    opt.max_fields = cfg_.max_fields_per_frame;
    opt.begin_string = cfg_.begin_string;
    opt.allowed_app_msg_types = cfg_.app_msg_types.empty() ? nullptr : &cfg_.app_msg_types;

    DecodedMessage msg;
    const WireError e = decode(raw, opt, &msg);
    if (e == WireError::None) {
        handle_decoded(msg);
        return;
    }

    d_.metrics->inc(metric::kMalformedFrames);

    // Without a usable MsgSeqNum the message is garbled: FIX 4.4 says to ignore
    // it rather than reject it, because a Reject would have to reference a
    // sequence number that was never readable.
    if (msg.seq_num == 0) {
        ++garbled_frames_;
        audit_event(AuditEvent::StateTransition, 0, 0,
                    std::string("garbled frame ignored: ") + std::string(to_string(e)));
        // A peer that only produces garbage is not a live FIX peer (D-14).
        if (garbled_frames_ >= 64) {
            connection_live_ = false;
            d_.controller->request_disconnect(active_generation_, "garbled frame flood");
            enter_backoff("garbled frame flood");
        }
        return;
    }

    if (msg.seq_num > st_.next_in) {
        // The message is invalid, but its sequence number still proves a gap.
        handle_sequence_too_high(msg);
        return;
    }
    if (msg.seq_num < st_.next_in) {
        d_.metrics->inc(metric::kDuplicateInbound);
        audit_event(AuditEvent::DuplicateIgnored, msg.seq_num, msg.seq_num,
                    "invalid duplicate below NextNumIn");
        return;
    }

    // In sequence but invalid: reject and advance sequencing, which is what
    // FIX 4.4 prescribes for a session level reject.
    RejectReason reason = RejectReason::Other;
    switch (e) {
        case WireError::BadBeginString: reason = RejectReason::ValueIsIncorrect; break;
        case WireError::MissingRequiredHeader: reason = RejectReason::RequiredTagMissing; break;
        case WireError::DuplicateHeaderTag: reason = RejectReason::TagAppearsMoreThanOnce; break;
        case WireError::MissingMsgType:
            reason = RejectReason::TagSpecifiedOutOfRequiredOrder;
            break;
        case WireError::BadTimestamp: reason = RejectReason::IncorrectDataFormat; break;
        case WireError::NonPrintableValue: reason = RejectReason::IncorrectDataFormat; break;
        case WireError::UnsupportedMsgType: reason = RejectReason::InvalidMsgType; break;
        case WireError::EmptyTagValue: reason = RejectReason::TagSpecifiedWithoutValue; break;
        case WireError::BadTagNumber: reason = RejectReason::InvalidTagNumber; break;
        default: reason = RejectReason::ValueIsIncorrect; break;
    }
    send_session_reject(msg, reason, to_string(e));
    const StoreError se = d_.store->commit_inbound(nullptr, msg.seq_num + 1);
    if (se != StoreError::None) {
        d_.metrics->inc(metric::kStoreErrors);
        enter_fault(FaultReason::StoreCommitFailed,
                    std::string("commit_inbound after reject: ") + std::string(to_string(se)));
        return;
    }
    st_.next_in = msg.seq_num + 1;
    d_.metrics->set(metric::kNextIn, static_cast<int64_t>(st_.next_in));
}

bool SessionEngine::validate_identity(const DecodedMessage& msg) {
    // The peer SenderCompID must be our configured target, and vice versa. A
    // CompID match is not authentication on its own (design doc 11.2), but a
    // mismatch is always a contract failure (C-04, C-05).
    if (msg.sender_comp_id != cfg_.target_comp_id || msg.target_comp_id != cfg_.sender_comp_id) {
        d_.metrics->inc(metric::kLogonRejects);
        audit_event(AuditEvent::LogonRejected, msg.seq_num, msg.seq_num,
                    "CompID mismatch: sender=" + std::string(msg.sender_comp_id) +
                        " target=" + std::string(msg.target_comp_id));
        send_logout("CompID problem");
        connection_live_ = false;
        d_.controller->request_disconnect(active_generation_, "compid mismatch");
        enter_backoff("compid mismatch");
        return false;
    }
    return true;
}

bool SessionEngine::validate_sending_time(const DecodedMessage& msg) {
    const auto now_wall = d_.clock->wall_now();
    const auto diff =
        now_wall > msg.sending_time ? now_wall - msg.sending_time : msg.sending_time - now_wall;
    if (diff <= std::chrono::seconds(cfg_.sending_time_tolerance_seconds)) {
        // A retransmission whose OrigSendingTime is later than its SendingTime
        // is self-contradictory: it claims to replay something that has not
        // happened yet (scenario F-25).
        if (msg.poss_dup && msg.orig_sending_time.has_value() &&
            *msg.orig_sending_time > msg.sending_time + std::chrono::seconds(1)) {
            audit_event(AuditEvent::SequenceResetRefused, msg.seq_num, msg.seq_num,
                        "OrigSendingTime is later than SendingTime on a retransmission");
            send_session_reject(msg, RejectReason::SendingTimeAccuracyProblem,
                                "OrigSendingTime is ahead of SendingTime");
            return false;
        }
        return true;
    }

    // Outside the agreed tolerance the message is rejected and the session is
    // closed, rather than silently accepting a stale or future dated message
    // (scenario H-03).
    audit_event(AuditEvent::StateTransition, msg.seq_num, msg.seq_num,
                "SendingTime outside tolerance: " + std::string(msg.sending_time_raw));
    send_session_reject(msg, RejectReason::SendingTimeAccuracyProblem,
                        "SendingTime accuracy problem");
    send_logout("SendingTime accuracy problem");
    connection_live_ = false;
    d_.controller->request_disconnect(active_generation_, "sending time out of tolerance");
    enter_backoff("sending time out of tolerance");
    return false;
}

void SessionEngine::handle_decoded(const DecodedMessage& msg) {
    // Before a validated Logon the only meaningful messages are Logon, Logout
    // and Reject. Application data arriving first must never be delivered, and
    // an administrative message does not make the session authenticated
    // (scenarios C-09, C-10, invariant I-10).
    if (state_ == SessionState::TcpConnected || state_ == SessionState::AwaitLogon) {
        if (msg.msg_type != msg_type::Logon && msg.msg_type != msg_type::Logout &&
            msg.msg_type != msg_type::Reject) {
            audit_event(AuditEvent::LogonRejected, msg.seq_num, msg.seq_num,
                        "message type " + std::string(msg.msg_type) + " received before Logon");
            send_logout("message received before Logon");
            connection_live_ = false;
            d_.controller->request_disconnect(active_generation_, "message before logon");
            enter_backoff("message before logon");
            return;
        }
    }

    if (!validate_identity(msg)) return;
    if (!validate_sending_time(msg)) return;

    // Liveness is only refreshed by a frame that passed validation (D-14).
    last_inbound_valid_ = now_;
    garbled_frames_ = 0;

    if (msg.seq_num == st_.next_in) {
        handle_in_sequence(msg);
        if (state_ != SessionState::Faulted && connection_live_) drain_future_buffer();
        maybe_finish_synchronising();
        return;
    }
    if (msg.seq_num > st_.next_in) {
        handle_sequence_too_high(msg);
        return;
    }
    handle_sequence_too_low(msg);
}

void SessionEngine::handle_in_sequence(const DecodedMessage& msg) {
    if (msg.seq_num == UINT64_MAX) {
        enter_fault(FaultReason::SequenceRollback, "NextNumIn reached the 64 bit boundary");
        return;
    }
    if (is_session_message(msg.msg_type)) {
        apply_session_message(msg);
        return;
    }
    commit_inbound_and_dispatch(msg);
}

void SessionEngine::handle_sequence_too_high(const DecodedMessage& msg) {
    const uint64_t from = st_.next_in;
    const uint64_t to = msg.seq_num - 1;

    // FIX 4.4 gives Logon, Logout, ResendRequest and SequenceReset their own
    // handling when MsgSeqNum is higher than expected: they are acted on, and
    // the gap is requested separately. Applying the generic buffer rule to them
    // would stall the handshake (scenarios E-15 to E-18).
    if (msg.msg_type == msg_type::Logon) {
        on_logon(msg);  // establishes the session without advancing NextNumIn
        if (state_ == SessionState::Faulted || !connection_live_) return;
        open_gap(from, to);
        send_resend_request(from, to);
        return;
    }
    if (msg.msg_type == msg_type::ResendRequest) {
        // Serve the peer request first, then ask for our own gap, so neither
        // side waits for the other (scenario E-17, F-14).
        on_resend_request(msg);
        if (state_ == SessionState::Faulted || !connection_live_) return;
        open_gap(from, to);
        send_resend_request(from, to);
        return;
    }
    if (msg.msg_type == msg_type::SequenceReset) {
        const auto gap_flag = msg.find(tag::GapFillFlag);
        const bool gap_fill = gap_flag.has_value() && *gap_flag == "Y";
        if (!gap_fill) {
            // The Reset form sets NewSeqNo and ignores MsgSeqNum entirely.
            on_sequence_reset(msg);
            return;
        }
        // A GapFill that is itself ahead of the expected sequence cannot be
        // applied: doing so would skip the very messages being recovered
        // (scenario F-22).
        audit_event(AuditEvent::GapFillReceived, msg.seq_num, msg.seq_num,
                    "GapFill arrived ahead of NextNumIn and was not applied");
        open_gap(from, to);
        send_resend_request(from, to);
        return;
    }
    if (msg.msg_type == msg_type::Logout) {
        audit_event(AuditEvent::StateTransition, msg.seq_num, msg.seq_num,
                    "Logout arrived with a sequence gap");
        open_gap(from, to);
        send_resend_request(from, to);
        on_logout(msg);
        return;
    }

    // Everything else, application messages included, is buffered in sequence
    // order and delivered only once the gap is filled (scenarios E-02, E-08).
    buffer_future(msg);
    open_gap(from, to);
    send_resend_request(from, to);
}

void SessionEngine::handle_sequence_too_low(const DecodedMessage& msg) {
    if (!msg.poss_dup) {
        // A sequence below the expected one without PossDupFlag means the two
        // sides disagree about history. FIX 4.4 requires a Logout and
        // disconnect; silently accepting it would rewind our own state
        // (scenario E-04, invariant I-02).
        audit_event(AuditEvent::SequenceResetRefused, msg.seq_num, st_.next_in,
                    "MsgSeqNum too low without PossDupFlag");
        send_logout("MsgSeqNum too low, expecting " + std::to_string(st_.next_in) +
                    " but received " + std::to_string(msg.seq_num));
        connection_live_ = false;
        d_.controller->request_disconnect(active_generation_, "sequence too low");
        enter_backoff("sequence too low");
        return;
    }

    d_.metrics->inc(metric::kDuplicateInbound);

    // A retransmission of something already committed must not create a second
    // inbox row (invariant I-04). It also must not carry different content:
    // same sequence with a different payload is an integrity problem, not a
    // duplicate (scenario E-20).
    if (msg.is_app()) {
        InboundRecord stored;
        bool found = false;
        if (d_.store->load_inbox_record(msg.seq_num, &stored, &found) == StoreError::None &&
            found) {
            DecodeOptions opt;
            opt.max_fields = cfg_.max_fields_per_frame;
            DecodedMessage prev;
            const bool prev_ok = decode(stored.payload, opt, &prev) == WireError::None;
            const bool same_type = prev_ok && prev.msg_type == msg.msg_type;
            bool same_body = prev_ok;
            if (prev_ok) {
                for (const Field& f : msg.fields) {
                    if (f.tag == tag::BodyLength || f.tag == tag::CheckSum ||
                        f.tag == tag::SendingTime || f.tag == tag::PossDupFlag ||
                        f.tag == tag::OrigSendingTime)
                        continue;
                    const auto before = prev.find(f.tag);
                    if (!before.has_value() || *before != f.value) {
                        same_body = false;
                        break;
                    }
                }
            }
            if (!same_type || !same_body) {
                enter_fault(FaultReason::IntegrityMismatch,
                            "retransmission of sequence " + std::to_string(msg.seq_num) +
                                " differs from the committed message");
                return;
            }
        }
    }
    audit_event(AuditEvent::DuplicateIgnored, msg.seq_num, msg.seq_num,
                "PossDup below NextNumIn, already processed");
}

void SessionEngine::buffer_future(const DecodedMessage& msg) {
    // The buffer is capped by both count and bytes, so a peer that jumps a
    // hundred thousand sequence numbers costs one entry, not a hundred thousand
    // (scenarios E-03, E-14, invariant I-09).
    if (future_.size() >= cfg_.max_future_messages ||
        future_bytes_ + msg.raw.size() > cfg_.max_future_bytes) {
        audit_event(AuditEvent::GapDetected, msg.seq_num, msg.seq_num,
                    "out of order buffer full, relying on peer retransmission");
        return;
    }
    const auto it = future_.find(msg.seq_num);
    if (it != future_.end()) return;  // already buffered
    future_bytes_ += msg.raw.size();
    future_.emplace(msg.seq_num, std::string(msg.raw));
    d_.metrics->max_of(metric::kRecoveryBufferMessages, static_cast<int64_t>(future_.size()));
}

void SessionEngine::drain_future_buffer() {
    while (!future_.empty() && future_.begin()->first == st_.next_in) {
        const std::string raw = future_.begin()->second;
        future_bytes_ -= future_.begin()->second.size();
        future_.erase(future_.begin());

        DecodeOptions opt;
        opt.max_fields = cfg_.max_fields_per_frame;
        opt.begin_string = cfg_.begin_string;
        opt.allowed_app_msg_types = cfg_.app_msg_types.empty() ? nullptr : &cfg_.app_msg_types;
        DecodedMessage msg;
        if (decode(raw, opt, &msg) != WireError::None) {
            // It decoded when it was buffered, so failing now means the buffer
            // itself is untrustworthy.
            enter_fault(FaultReason::IntegrityMismatch,
                        "buffered frame for sequence " + std::to_string(st_.next_in) +
                            " no longer decodes");
            return;
        }
        handle_in_sequence(msg);
        if (state_ == SessionState::Faulted || !connection_live_) return;
    }
    if (gap_open_ && st_.next_in > gap_end_) close_gap();
}

void SessionEngine::open_gap(uint64_t from, uint64_t to) {
    if (!gap_open_) {
        gap_open_ = true;
        gap_begin_ = from;
        gap_end_ = to;
        recovery_started_ = now_;
        resend_cycles_ = 0;
        d_.metrics->inc(metric::kGapDetected);
        d_.metrics->set(metric::kUnresolvedGaps,
                        static_cast<int64_t>(skipped_unrecovered_ + (to - from + 1)));
        audit_event(AuditEvent::GapDetected, from, to, "inbound sequence gap");
        if (state_ == SessionState::Active) transition(SessionState::Synchronizing, "gap detected");
    } else {
        if (to > gap_end_) gap_end_ = to;
    }
}

void SessionEngine::close_gap() {
    if (!gap_open_) return;
    const uint64_t from = gap_begin_;
    const uint64_t to = gap_end_;
    gap_open_ = false;
    resend_cycles_ = 0;
    last_resend_request_armed_ = false;
    // Closing the gap does not erase sequences a GapFill skipped: the metric
    // keeps reporting them so an operator can reconcile.
    d_.metrics->set(metric::kUnresolvedGaps, static_cast<int64_t>(skipped_unrecovered_));
    audit_event(AuditEvent::ResendServed, from, to,
                skipped_unrecovered_ == 0
                    ? "inbound gap recovered in order"
                    : "inbound gap closed with " + std::to_string(skipped_unrecovered_) +
                          " sequences skipped and never recovered");
}

void SessionEngine::maybe_finish_synchronising() {
    if (state_ == SessionState::Synchronizing && !gap_open_)
        transition(SessionState::Active, "synchronised");
}

void SessionEngine::check_recovery_progress() {
    if (!gap_open_) return;
    if (now_ - recovery_started_ >= Millis(cfg_.max_recovery_duration_ms)) {
        // A gap the peer never fills is escalated instead of being retried for
        // ever (scenario F-24).
        enter_fault(FaultReason::RecoveryTimeout,
                    "gap " + std::to_string(gap_begin_) + ".." + std::to_string(gap_end_) +
                        " not recovered within the configured window");
        return;
    }
    if (resend_cycles_ >= cfg_.max_resend_cycles) {
        audit_event(AuditEvent::ResendStormBlocked, gap_begin_, gap_end_,
                    "resend cycle limit reached");
        enter_fault(FaultReason::ResendStorm, "resend cycle limit reached while recovering");
        return;
    }
    // Re-request at an interval that exhausts the cycle budget exactly at the
    // recovery deadline, so the two limits cannot contradict each other.
    const long long interval =
        std::max<long long>(1, cfg_.max_recovery_duration_ms /
                                   std::max(1, cfg_.max_resend_cycles));
    if (last_resend_request_armed_ && now_ - last_resend_request_at_ >= Millis(interval))
        send_resend_request(gap_begin_, gap_end_);
}

bool SessionEngine::commit_inbound_and_dispatch(const DecodedMessage& msg) {
    const bool app = msg.is_app();
    InboundRecord rec;
    rec.seq = msg.seq_num;
    rec.msg_type = std::string(msg.msg_type);
    rec.payload = std::string(msg.raw);

    // Durable first: an application message is not considered processed until
    // its inbox row and the new NextNumIn are committed together (invariant
    // I-07).
    const StoreError e = d_.store->commit_inbound(app ? &rec : nullptr, msg.seq_num + 1);
    if (e != StoreError::None) {
        d_.metrics->inc(metric::kStoreErrors);
        enter_fault(FaultReason::StoreCommitFailed,
                    std::string("commit_inbound: ") + std::string(to_string(e)));
        return false;
    }
    st_.next_in = msg.seq_num + 1;
    d_.metrics->set(metric::kNextIn, static_cast<int64_t>(st_.next_in));

    if (!app) return true;

    ++pending_inbox_;
    d_.metrics->set(metric::kInboxPending, static_cast<int64_t>(pending_inbox_));

    fp::fire(fp::point::kSinkBeforeDeliver);
    const bool accepted = d_.sink != nullptr && d_.sink->deliver(rec.seq, rec.msg_type, rec.payload);
    fp::fire(fp::point::kSinkAfterDeliver);

    if (accepted) {
        // A crash between the delivery and this mark means the sink will see
        // the event again after a restart: delivery is at-least-once and the
        // sink has to be idempotent (scenario G-07, invariant I-11).
        d_.store->mark_delivered(rec.seq);
        if (pending_inbox_ > 0) --pending_inbox_;
        d_.metrics->inc(metric::kAppDelivered);
    } else {
        inbox_paused_ = true;
    }
    d_.metrics->set(metric::kInboxPending, static_cast<int64_t>(pending_inbox_));

    if (pending_inbox_ >= cfg_.max_inbox_pending) {
        inbox_paused_ = true;
        audit_event(AuditEvent::InboxOverflow, rec.seq, rec.seq,
                    "durable inbox backlog reached the configured cap");
    }
    return true;
}

void SessionEngine::dispatch_pending_inbox() {
    if (d_.sink == nullptr) return;
    constexpr size_t kBatch = 256;
    std::vector<InboundRecord> rows;
    if (d_.store->load_pending_inbox(kBatch, &rows) != StoreError::None) {
        d_.metrics->inc(metric::kStoreErrors);
        return;
    }
    bool blocked = false;
    size_t delivered = 0;
    for (const InboundRecord& rec : rows) {
        fp::fire(fp::point::kSinkBeforeDeliver);
        const bool ok = d_.sink->deliver(rec.seq, rec.msg_type, rec.payload);
        fp::fire(fp::point::kSinkAfterDeliver);
        if (!ok) {
            blocked = true;
            break;
        }
        d_.store->mark_delivered(rec.seq);
        d_.metrics->inc(metric::kAppDelivered);
        ++delivered;
    }
    pending_inbox_ = pending_inbox_ > delivered ? pending_inbox_ - delivered : 0;
    if (!blocked && rows.size() < kBatch) pending_inbox_ = 0;
    inbox_paused_ = blocked || pending_inbox_ >= cfg_.max_inbox_pending;
    d_.metrics->set(metric::kInboxPending, static_cast<int64_t>(pending_inbox_));
}

// ---------------------------------------------------------------------------
// Session message handling
// ---------------------------------------------------------------------------

void SessionEngine::apply_session_message(const DecodedMessage& msg) {
    if (msg.msg_type == msg_type::Logon) {
        on_logon(msg);
        return;
    }
    if (msg.msg_type == msg_type::SequenceReset) {
        on_sequence_reset(msg);
        return;
    }
    if (msg.msg_type == msg_type::TestRequest) {
        on_test_request(msg);
        return;
    }
    if (!commit_inbound_and_dispatch(msg)) return;
    if (msg.msg_type == msg_type::Heartbeat) {
        on_heartbeat(msg);
        return;
    }
    if (msg.msg_type == msg_type::ResendRequest) {
        on_resend_request(msg);
        return;
    }
    if (msg.msg_type == msg_type::Logout) {
        on_logout(msg);
        return;
    }
    if (msg.msg_type == msg_type::Reject) {
        audit_event(AuditEvent::StateTransition, msg.seq_num, msg.seq_num,
                    "peer session level Reject: " +
                        std::string(msg.find(tag::Text).value_or("no text")));
        return;
    }
}

void SessionEngine::on_logon(const DecodedMessage& msg) {
    // An unsolicited reset request is never honoured implicitly. Losing
    // sequence history on a peer request would make every later
    // reconciliation unverifiable (scenarios C-14, J-07, F-23).
    const auto reset_flag = msg.find(tag::ResetSeqNumFlag);
    if (reset_flag.has_value() && *reset_flag == "Y") {
        if (!cfg_.allow_reset_on_logon) {
            audit_event(AuditEvent::SequenceResetRefused, st_.next_in, st_.next_out,
                        "peer Logon requested ResetSeqNumFlag outside the agreement");
            enter_fault(FaultReason::UnauthorizedSequenceReset,
                        "peer requested a sequence reset that the configuration forbids");
            return;
        }
        // Configured, pre-agreed reset: still recorded as a coordinated reset
        // with named approvals so the audit trail shows why history restarted.
        if (!operator_coordinated_reset("session-schedule", "counterparty-agreement",
                                        "ResetSeqNumFlag accepted on Logon per configuration"))
            return;
    }

    int hb = 0;
    if (!msg.find_int(tag::HeartBtInt, &hb) || hb != cfg_.heartbeat_seconds) {
        // A HeartBtInt of zero disables liveness detection and a mismatched one
        // means the two sides disagree about timeouts (scenario C-15).
        d_.metrics->inc(metric::kLogonRejects);
        audit_event(AuditEvent::LogonRejected, msg.seq_num, msg.seq_num,
                    "HeartBtInt mismatch: peer sent " +
                        std::string(msg.find(tag::HeartBtInt).value_or("none")));
        send_logout("HeartBtInt mismatch");
        connection_live_ = false;
        d_.controller->request_disconnect(active_generation_, "heartbeat interval mismatch");
        enter_backoff("heartbeat interval mismatch");
        return;
    }

    if (state_ == SessionState::Active || state_ == SessionState::Synchronizing) {
        // A second Logon on an established session is refused, and refusing it
        // never resets sequence numbers (scenario C-08).
        audit_event(AuditEvent::LogonRejected, msg.seq_num, msg.seq_num,
                    "duplicate Logon on an established session");
        send_logout("duplicate Logon");
        connection_live_ = false;
        d_.controller->request_disconnect(active_generation_, "duplicate logon");
        enter_backoff("duplicate logon");
        return;
    }

    logon_deadline_armed_ = false;
    if (msg.seq_num == st_.next_in) {
        if (!commit_inbound_and_dispatch(msg)) return;
    }
    // Logon accepted: synchronised only once no gap remains, so ACTIVE is not
    // reached by merely completing the handshake (invariant I-10).
    transition(SessionState::Synchronizing, "logon accepted");
}

void SessionEngine::on_logout(const DecodedMessage& msg) {
    const std::string text(msg.find(tag::Text).value_or(""));
    audit_event(AuditEvent::StateTransition, msg.seq_num, msg.seq_num,
                "peer Logout: " + (text.empty() ? std::string("no text") : text));

    if (state_ == SessionState::LoggingOut || stop_requested_) {
        connection_live_ = false;
        d_.controller->request_disconnect(active_generation_, "logout complete");
        reset_connection_state();
        transition(SessionState::Stopped, "logout acknowledged");
        return;
    }
    // Peer initiated: acknowledge, close, and keep sequence state for the next
    // session (invariant I-12).
    send_logout("responding to peer Logout");
    connection_live_ = false;
    d_.controller->request_disconnect(active_generation_, "peer logout");
    enter_backoff("peer logout");
}

void SessionEngine::on_test_request(const DecodedMessage& msg) {
    const auto id = msg.find(tag::TestReqID);
    if (!id.has_value()) {
        // Without a TestReqID there is nothing to echo, so the request is
        // rejected rather than answered with a guess (scenario D-08).
        send_session_reject(msg, RejectReason::RequiredTagMissing, "TestReqID missing");
        if (!commit_inbound_and_dispatch(msg)) return;
        return;
    }
    const std::string echo(*id);
    if (!commit_inbound_and_dispatch(msg)) return;
    send_heartbeat(echo);
}

void SessionEngine::on_heartbeat(const DecodedMessage& msg) {
    const auto id = msg.find(tag::TestReqID);
    if (!pending_test_req_id_.has_value()) return;

    if (id.has_value() && *id == *pending_test_req_id_) {
        pending_test_req_id_.reset();
        audit_event(AuditEvent::StateTransition, msg.seq_num, msg.seq_num,
                    "pending TestRequest answered");
        return;
    }
    // A background heartbeat, or one carrying a different TestReqID, does not
    // answer the outstanding request: the deadline keeps running (D-05, D-06,
    // playbook PB-08).
    audit_event(AuditEvent::StateTransition, msg.seq_num, msg.seq_num,
                "heartbeat did not match the pending TestReqID " + *pending_test_req_id_);
}

void SessionEngine::on_resend_request(const DecodedMessage& msg) {
    uint64_t begin = 0;
    uint64_t end = 0;
    if (!msg.find_u64(tag::BeginSeqNo, &begin) || !msg.find_u64(tag::EndSeqNo, &end)) {
        send_session_reject(msg, RejectReason::RequiredTagMissing,
                            "BeginSeqNo or EndSeqNo missing");
        return;
    }

    // Answering an unbounded stream of resend requests is an amplification
    // vector, so the count is capped per session (scenarios J-06, F-12,
    // playbook PB-11).
    ++resend_requests_served_;
    if (resend_requests_served_ > cfg_.max_resend_cycles) {
        audit_event(AuditEvent::ResendStormBlocked, begin, end,
                    "resend requests served exceeded the configured limit");
        enter_fault(FaultReason::ResendStorm, "peer resend request limit exceeded");
        return;
    }

    const ResendRange range = resolve_range(begin, end, st_.next_out - 1);
    if (!range.valid) {
        // An invalid range is a session level error; no history is scanned
        // (scenarios F-03, F-04).
        send_session_reject(msg, RejectReason::ValueIsIncorrect, range.detail);
        return;
    }

    uint64_t pos = range.begin;
    while (pos <= range.end) {
        // Chunking keeps a one million sequence request from being materialised
        // in memory all at once (scenarios F-11, I-06).
        const uint64_t chunk_end = std::min(range.end, pos + cfg_.resend_chunk_size - 1);
        std::vector<OutboundRecord> history;
        const StoreError e = d_.store->load_outbound_range(pos, chunk_end, &history);
        if (e == StoreError::Corrupt) {
            enter_fault(FaultReason::CorruptOutboundHistory,
                        "outbound history failed its checksum while serving a resend");
            return;
        }
        if (e != StoreError::None) {
            d_.metrics->inc(metric::kStoreErrors);
            enter_fault(FaultReason::StoreCommitFailed,
                        std::string("load_outbound_range: ") + std::string(to_string(e)));
            return;
        }

        ResendRange chunk;
        chunk.valid = true;
        chunk.begin = pos;
        chunk.end = chunk_end;
        const ResendPlan plan = plan_resend(chunk, history);
        if (!plan.ok) {
            // Claiming a successful replay with a GapFill over a message that
            // is simply gone would be a lie to the counterparty (F-16, G-20).
            audit_event(AuditEvent::IntegrityFailure, pos, chunk_end, plan.detail);
            enter_fault(plan.fault, plan.detail);
            return;
        }
        for (const ResendAction& a : plan.actions) {
            const bool ok =
                a.kind == ResendAction::Kind::ReplayApp ? send_retransmission(a) : send_gapfill(a);
            if (!ok) return;
        }
        pos = chunk_end + 1;
    }

    audit_event(AuditEvent::ResendServed, range.begin, range.end, "resend request served");
    flush_tx();
}

void SessionEngine::on_sequence_reset(const DecodedMessage& msg) {
    const auto gap_flag = msg.find(tag::GapFillFlag);
    const bool gap_fill = gap_flag.has_value() && *gap_flag == "Y";

    uint64_t new_seq = 0;
    if (!msg.find_u64(tag::NewSeqNo, &new_seq)) {
        send_session_reject(msg, RejectReason::RequiredTagMissing, "NewSeqNo missing");
        return;
    }

    if (gap_fill) {
        if (new_seq <= st_.next_in) {
            // Equal makes no progress and would loop; lower would roll the
            // expected sequence backwards (scenarios F-18, F-19).
            audit_event(AuditEvent::SequenceResetRefused, new_seq, st_.next_in,
                        "GapFill NewSeqNo is not ahead of NextNumIn");
            send_session_reject(msg, RejectReason::ValueIsIncorrect,
                                "NewSeqNo must be greater than the expected sequence");
            return;
        }
        const uint64_t skipped_from = msg.seq_num;
        const uint64_t skipped_to = new_seq - 1;

        if (cfg_.gapfill_policy == GapFillPolicy::FailClosed) {
            // The session layer cannot prove what was skipped: a GapFill
            // carries no evidence about the messages it replaces. Under a
            // fail-closed agreement the engine refuses to advance and
            // escalates for manual reconciliation (playbook PB-07, F-20).
            skipped_unrecovered_ += skipped_to - skipped_from + 1;
            d_.metrics->set(metric::kUnresolvedGaps,
                            static_cast<int64_t>(skipped_unrecovered_));
            audit_event(AuditEvent::GapFillQuarantined, skipped_from, skipped_to,
                        "GapFill would skip sequences that were never recovered");
            enter_fault(FaultReason::GapFillOverApplicationMessage,
                        "GapFill skipped " + std::to_string(skipped_from) + ".." +
                            std::to_string(skipped_to) +
                            " and the agreement forbids skipping unrecovered sequences");
            return;
        }

        const StoreError e = d_.store->commit_inbound(nullptr, new_seq);
        if (e != StoreError::None) {
            d_.metrics->inc(metric::kStoreErrors);
            enter_fault(FaultReason::StoreCommitFailed,
                        std::string("commit_inbound for GapFill: ") + std::string(to_string(e)));
            return;
        }
        st_.next_in = new_seq;
        d_.metrics->set(metric::kNextIn, static_cast<int64_t>(st_.next_in));
        // Accepted so that recovery can finish, but the skipped range stays
        // visible for ever: accepting a GapFill is NOT the same as having
        // recovered those messages, and nothing downstream may assume it is.
        skipped_unrecovered_ += skipped_to - skipped_from + 1;
        d_.metrics->inc(metric::kGapFillRanges);
        d_.metrics->set(metric::kUnresolvedGaps, static_cast<int64_t>(skipped_unrecovered_));
        audit_event(AuditEvent::GapFillQuarantined, skipped_from, skipped_to,
                    "GapFill accepted; these sequences were never recovered");
        if (gap_open_ && st_.next_in > gap_end_) close_gap();
        drain_future_buffer();
        maybe_finish_synchronising();
        return;
    }

    // Reset form: it ignores MsgSeqNum entirely, which is exactly why it has to
    // be gated by policy rather than trusted (scenarios F-21, J-07).
    if (!cfg_.allow_unsolicited_seq_reset) {
        audit_event(AuditEvent::SequenceResetRefused, new_seq, st_.next_in,
                    "unsolicited SequenceReset-Reset is not permitted by configuration");
        send_session_reject(msg, RejectReason::ValueIsIncorrect,
                            "SequenceReset-Reset is not permitted on this session");
        enter_fault(FaultReason::UnauthorizedSequenceReset,
                    "peer sent SequenceReset-Reset outside the agreement");
        return;
    }
    if (new_seq <= st_.next_in) {
        audit_event(AuditEvent::SequenceResetRefused, new_seq, st_.next_in,
                    "SequenceReset-Reset would not move the sequence forward");
        send_session_reject(msg, RejectReason::ValueIsIncorrect, "NewSeqNo must move forward");
        return;
    }
    const StoreError e = d_.store->commit_inbound(nullptr, new_seq);
    if (e != StoreError::None) {
        enter_fault(FaultReason::StoreCommitFailed,
                    std::string("commit_inbound for SequenceReset: ") + std::string(to_string(e)));
        return;
    }
    st_.next_in = new_seq;
    d_.metrics->set(metric::kNextIn, static_cast<int64_t>(st_.next_in));
    audit_event(AuditEvent::GapFillReceived, new_seq, new_seq,
                "SequenceReset-Reset applied under an explicit configuration allowance");
    if (gap_open_ && st_.next_in > gap_end_) close_gap();
    drain_future_buffer();
    maybe_finish_synchronising();
}

}  // namespace fixse
