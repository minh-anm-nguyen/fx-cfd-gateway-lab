#include "lp/session.hpp"

#include <algorithm>

#include "lp/failpoint.hpp"

namespace lpsim {

std::string_view to_string(SessionPhase p) {
    switch (p) {
        case SessionPhase::Initializing: return "INITIALIZING";
        case SessionPhase::Listening: return "LISTENING";
        case SessionPhase::LogonPending: return "LOGON_PENDING";
        case SessionPhase::Synchronizing: return "SYNCHRONIZING";
        case SessionPhase::Active: return "ACTIVE";
        case SessionPhase::Draining: return "DRAINING";
        case SessionPhase::Halted: return "HALTED";
    }
    return "?";
}

std::string_view to_string(InboundVerdict v) {
    switch (v) {
        case InboundVerdict::Accepted: return "Accepted";
        case InboundVerdict::Duplicate: return "Duplicate";
        case InboundVerdict::IgnoredPossDup: return "IgnoredPossDup";
        case InboundVerdict::RejectedWire: return "RejectedWire";
        case InboundVerdict::RejectedSession: return "RejectedSession";
        case InboundVerdict::QueuedGap: return "QueuedGap";
        case InboundVerdict::NotActive: return "NotActive";
        case InboundVerdict::Disconnected: return "Disconnected";
        case InboundVerdict::Halted: return "Halted";
    }
    return "?";
}

AcceptorSession::AcceptorSession(SessionConfig cfg, IBusinessStore& store, OrderActor& actor,
                                 IClock& clock, ITransport& transport)
    : cfg_(std::move(cfg)),
      store_(store),
      actor_(actor),
      clock_(clock),
      transport_(transport),
      framer_(cfg_.max_frame_bytes, cfg_.max_buffer_bytes) {
    negotiated_heartbeat_s_ = cfg_.heartbeat_seconds;
}

void AcceptorSession::halt(HaltReason r, std::string detail) {
    phase_ = SessionPhase::Halted;
    halt_ = r;
    halt_detail_ = std::move(detail);
}

bool AcceptorSession::recover() {
    if (store_.load_session_state(&state_) != StoreError::None) {
        halt(HaltReason::StoreCorrupt, "session state unreadable");
        return false;
    }
    if (state_.sender_comp_id != cfg_.sender_comp_id ||
        state_.target_comp_id != cfg_.target_comp_id) {
        // Resuming under different CompIDs would trade on someone else's
        // session history (A-18, J-20).
        halt(HaltReason::IdentityMismatch, "persisted CompIDs differ from configuration");
        return false;
    }
    phase_ = SessionPhase::Listening;
    return true;
}

void AcceptorSession::on_connected(std::uint64_t generation) {
    if (phase_ == SessionPhase::Halted) return;
    generation_ = generation;
    framer_.reset();
    logon_seen_ = false;
    test_request_outstanding_ = false;
    pending_test_req_id_.clear();
    gap_requested_through_ = 0;
    resend_requests_sent_ = 0;
    last_rx_ = clock_.steady_now();
    last_tx_ = clock_.steady_now();
    phase_ = SessionPhase::LogonPending;
}

void AcceptorSession::on_disconnect(std::uint64_t generation) {
    if (stale(generation)) return;  // a stale callback cannot change state
    framer_.reset();
    logon_seen_ = false;
    if (phase_ != SessionPhase::Halted) phase_ = SessionPhase::Listening;
    // Sequence numbers, the epoch, orders and fills all survive. A TCP
    // disconnect is not a session reset (design doc 3.4, A-07).
}

// ---------------------------------------------------------------------------
// Inbound
// ---------------------------------------------------------------------------

std::vector<InboundVerdict> AcceptorSession::on_bytes(std::string_view bytes,
                                                      std::uint64_t generation) {
    std::vector<InboundVerdict> out;
    if (stale(generation)) {
        // Bytes from a connection that has already been replaced are dropped:
        // they must not advance sequence state (A-19, K-11).
        out.push_back(InboundVerdict::Disconnected);
        return out;
    }
    if (phase_ == SessionPhase::Halted) {
        out.push_back(InboundVerdict::Halted);
        return out;
    }

    const WireError ae = framer_.append(bytes);
    if (ae != WireError::None) {
        // The buffer cap was reached without a frame: a slow-loris or an
        // oversized frame. Close rather than grow (L-08).
        send_logout("frame exceeds configured bounds");
        transport_.close();
        out.push_back(InboundVerdict::RejectedWire);
        return out;
    }

    last_rx_ = clock_.steady_now();

    while (true) {
        std::string_view frame;
        WireError we = WireError::None;
        if (!framer_.next(&frame, &we)) {
            if (we == WireError::Incomplete) break;
            // A malformed frame cannot be resynchronised past safely, so the
            // connection ends. No business state has been touched (B-15, B-16).
            send_logout("malformed frame");
            transport_.close();
            out.push_back(InboundVerdict::RejectedWire);
            break;
        }
        out.push_back(process_frame(frame));
        if (phase_ == SessionPhase::Halted || !transport_.connected()) break;
    }
    return out;
}

InboundVerdict AcceptorSession::process_frame(std::string_view raw) {
    DecodedMessage m;
    DecodeOptions opt;
    opt.begin_string = cfg_.begin_string;
    const WireError de = decode(raw, opt, &m);
    if (de != WireError::None) {
        if (de == WireError::BadBeginString) {
            // A different FIX version is not something to negotiate (A-05).
            send_logout("unsupported BeginString");
            transport_.close();
            return InboundVerdict::RejectedSession;
        }
        // Before a successful Logon there is no session to reject into, so the
        // connection simply closes.
        if (phase_ == SessionPhase::LogonPending) {
            transport_.close();
            return InboundVerdict::RejectedWire;
        }
        send_reject(m, SessionRejectReason::IncorrectDataFormat, to_string(de), 0);
        return InboundVerdict::RejectedWire;
    }

    // --- identity ----------------------------------------------------------
    // The peer's SenderCompID is our TargetCompID and vice versa (design doc
    // 3.1). A mismatch is refused before anything else (A-03, A-04).
    if (m.sender_comp_id != cfg_.target_comp_id || m.target_comp_id != cfg_.sender_comp_id) {
        send_logout("CompID mismatch");
        transport_.close();
        return InboundVerdict::RejectedSession;
    }

    // --- SendingTime -------------------------------------------------------
    {
        const auto skew = std::chrono::duration_cast<std::chrono::seconds>(
                              clock_.wall_now() - m.sending_time)
                              .count();
        const std::int64_t abs_skew = skew < 0 ? -skew : skew;
        if (abs_skew > cfg_.sending_time_tolerance_s) {
            if (phase_ == SessionPhase::LogonPending) {
                transport_.close();
                return InboundVerdict::RejectedSession;
            }
            send_reject(m, SessionRejectReason::SendingTimeAccuracyProblem,
                        "SendingTime outside tolerance", tag::SendingTime);
            return InboundVerdict::RejectedSession;
        }
    }

    if (m.msg_type == msg_type::Logon) return handle_logon(m);

    if (phase_ == SessionPhase::LogonPending) {
        // Anything before Logon, application or admin, is refused and the
        // connection closes. This is invariant I-01 at its narrowest (A-01).
        transport_.close();
        return InboundVerdict::NotActive;
    }

    if (is_session_message(m.msg_type)) return handle_session_message(m);
    return handle_application(m, raw);
}

bool AcceptorSession::commit_and_check(const DecodedMessage& m, std::string_view raw,
                                       bool needs_delivery, bool* duplicate) {
    InboundIdentity id;
    id.session_id = cfg_.session_id;
    id.epoch = state_.epoch;
    id.msg_seq_num = m.seq_num;
    id.poss_dup = m.poss_dup;
    id.poss_resend = m.poss_resend;

    const StoreError se = store_.commit_inbound(id, raw, needs_delivery, duplicate);
    if (se != StoreError::None) {
        // Without a durable inbound identity there is no safe way to process
        // the message: a crash would lose the fact that it arrived, and
        // processing it anyway would risk doing it twice (J-01, J-11).
        halt(HaltReason::StoreCommitFailed,
             std::string("inbound commit refused: ") + std::string(to_string(se)));
        return false;
    }
    if (!*duplicate) state_.next_in = m.seq_num + 1;
    return true;
}

InboundVerdict AcceptorSession::handle_logon(const DecodedMessage& m) {
    if (logon_seen_) {
        // A second Logon on a live connection is a protocol error and must not
        // reset anything (A-14).
        send_logout("Logon already completed on this connection");
        transport_.close();
        return InboundVerdict::RejectedSession;
    }

    // --- HeartBtInt --------------------------------------------------------
    int hb = cfg_.heartbeat_seconds;
    if (!m.find_int(tag::HeartBtInt, &hb)) {
        send_logout("HeartBtInt missing or malformed");
        transport_.close();
        return InboundVerdict::RejectedSession;
    }
    if (hb < cfg_.min_heartbeat_seconds || hb > cfg_.max_heartbeat_seconds) {
        // Zero or an absurd interval is refused explicitly rather than turned
        // into an infinite timer (A-13).
        send_logout("HeartBtInt outside the agreed range");
        transport_.close();
        return InboundVerdict::RejectedSession;
    }

    // --- credentials -------------------------------------------------------
    if (cfg_.require_credentials) {
        const auto user = m.find(tag::Username);
        const auto pass = m.find(tag::Password);
        const bool ok = user && pass && *user == cfg_.username && *pass == cfg_.password;
        if (!ok) {
            // The reason is deliberately vague on the wire and the password is
            // never logged (A-06, L-07).
            send_logout("authentication failed");
            transport_.close();
            return InboundVerdict::RejectedSession;
        }
    }

    // --- ResetSeqNumFlag ---------------------------------------------------
    const auto reset_flag = m.find(tag::ResetSeqNumFlag);
    const bool wants_reset = reset_flag && *reset_flag == "Y";
    if (wants_reset && !cfg_.allow_reset_on_logon) {
        // Fail closed: an unapproved reset would destroy the ability to
        // reconcile, so it is refused rather than honoured (A-10, ADR-09).
        send_logout("sequence reset not permitted by the agreed profile");
        transport_.close();
        return InboundVerdict::RejectedSession;
    }
    if (wants_reset) {
        // Session numbers only. Orders, fills, the ExecID allocator and the
        // business de-duplication namespace are untouched (design doc 3.4).
        if (store_.apply_coordinated_reset(state_.epoch + 1) != StoreError::None) {
            halt(HaltReason::StoreCommitFailed, "coordinated reset failed to commit");
            return InboundVerdict::Halted;
        }
        store_.load_session_state(&state_);
    }

    // --- sequence ----------------------------------------------------------
    const std::uint64_t expected = state_.next_in;
    if (m.seq_num < expected && !m.poss_dup) {
        // A sequence number below expectation without PossDup is unrecoverable
        // at the session layer (A-12).
        send_logout("MsgSeqNum too low");
        transport_.close();
        return InboundVerdict::RejectedSession;
    }

    logon_seen_ = true;
    negotiated_heartbeat_s_ = hb;
    last_rx_ = clock_.steady_now();

    bool duplicate = false;
    if (m.seq_num == expected) {
        if (!commit_and_check(m, m.raw, false, &duplicate)) return InboundVerdict::Halted;
    }

    // Logon acknowledgement, mirroring the negotiated interval.
    {
        CanonicalBody body;
        body.emplace_back(tag::EncryptMethod, "0");
        body.emplace_back(tag::HeartBtInt, std::to_string(hb));
        if (wants_reset) body.emplace_back(tag::ResetSeqNumFlag, "Y");
        send_admin(msg_type::Logon, body);
    }

    if (m.seq_num > expected) {
        // The peer is ahead: synchronise before anything is processed. The
        // session is NOT active yet, so an application message that follows
        // cannot be delivered (A-11, B-20, H-19).
        phase_ = SessionPhase::Synchronizing;
        send_resend_request(expected, 0);
        gap_requested_through_ = m.seq_num;
        return InboundVerdict::QueuedGap;
    }

    phase_ = SessionPhase::Active;
    dispatch_outbox();
    return InboundVerdict::Accepted;
}

InboundVerdict AcceptorSession::handle_session_message(const DecodedMessage& m) {
    // Low sequence handling applies to every message type.
    const std::uint64_t expected = state_.next_in;
    if (m.seq_num < expected) {
        if (!m.poss_dup) {
            // I-10: a repeat without PossDup is a protocol error, never applied.
            send_logout("MsgSeqNum too low");
            transport_.close();
            return InboundVerdict::RejectedSession;
        }
        ++duplicates_ignored_;
        return InboundVerdict::IgnoredPossDup;
    }

    if (m.msg_type == msg_type::SequenceReset) return handle_sequence_reset(m);

    if (m.seq_num > expected) {
        // A ResendRequest is answered even while a gap is outstanding, because
        // refusing it would deadlock both sides.
        if (m.msg_type == msg_type::ResendRequest) return handle_resend_request(m);
        if (gap_requested_through_ < m.seq_num) {
            send_resend_request(expected, 0);
            gap_requested_through_ = m.seq_num;
        }
        return InboundVerdict::QueuedGap;
    }

    bool duplicate = false;
    if (!commit_and_check(m, m.raw, false, &duplicate)) return InboundVerdict::Halted;
    if (duplicate) {
        ++duplicates_ignored_;
        return InboundVerdict::Duplicate;
    }

    if (m.msg_type == msg_type::Heartbeat) {
        if (test_request_outstanding_) {
            const auto id = m.find(tag::TestReqID);
            // Only a Heartbeat echoing the exact TestReqID resolves the probe;
            // an unrelated heartbeat does not prove liveness of the request.
            if (id && *id == pending_test_req_id_) {
                test_request_outstanding_ = false;
                pending_test_req_id_.clear();
            }
        }
        return InboundVerdict::Accepted;
    }
    if (m.msg_type == msg_type::TestRequest) {
        const auto id = m.find(tag::TestReqID);
        CanonicalBody body;
        if (id) body.emplace_back(tag::TestReqID, std::string(*id));
        send_admin(msg_type::Heartbeat, body);
        return InboundVerdict::Accepted;
    }
    if (m.msg_type == msg_type::ResendRequest) return handle_resend_request(m);
    if (m.msg_type == msg_type::Logout) {
        // A Logout while reports are still pending does not discard them: the
        // outbox survives and is delivered after the next logon (A-16).
        send_logout("logout acknowledged");
        transport_.close();
        phase_ = SessionPhase::Listening;
        return InboundVerdict::Accepted;
    }
    if (m.msg_type == msg_type::Reject) {
        // A session-level reject from the peer is recorded, not acted on: it
        // refers to a message we sent, and guessing a remedy would be worse.
        return InboundVerdict::Accepted;
    }
    return InboundVerdict::Accepted;
}

InboundVerdict AcceptorSession::handle_sequence_reset(const DecodedMessage& m) {
    const auto gap_fill = m.find(tag::GapFillFlag);
    const bool is_gap_fill = gap_fill && *gap_fill == "Y";

    std::uint64_t new_seq = 0;
    if (!m.find_u64(tag::NewSeqNo, &new_seq) || new_seq == 0) {
        send_reject(m, SessionRejectReason::ValueIsIncorrect, "NewSeqNo invalid",
                    tag::NewSeqNo);
        return InboundVerdict::RejectedSession;
    }

    if (!is_gap_fill && !cfg_.allow_reset_on_logon) {
        // An unsolicited SequenceReset-Reset is a reset by another name.
        send_logout("unsolicited SequenceReset-Reset is not permitted");
        transport_.close();
        return InboundVerdict::RejectedSession;
    }
    if (new_seq < state_.next_in) {
        send_reject(m, SessionRejectReason::ValueIsIncorrect, "NewSeqNo below expectation",
                    tag::NewSeqNo);
        return InboundVerdict::RejectedSession;
    }

    // Advance session expectations only. Business de-duplication is untouched,
    // so a ClOrdID already used stays used (I-14, I-15, C-07).
    state_.next_in = new_seq;
    InboundIdentity id;
    id.session_id = cfg_.session_id;
    id.epoch = state_.epoch;
    id.msg_seq_num = new_seq - 1;
    bool duplicate = false;
    store_.commit_inbound(id, m.raw, false, &duplicate);

    if (phase_ == SessionPhase::Synchronizing && state_.next_in > gap_requested_through_) {
        phase_ = SessionPhase::Active;
        dispatch_outbox();
    }
    return InboundVerdict::Accepted;
}

InboundVerdict AcceptorSession::handle_resend_request(const DecodedMessage& m) {
    if (++resend_requests_sent_ > cfg_.max_resend_requests_per_connection) {
        // Bounded replay: an endless resend loop is refused rather than served
        // (I-09).
        send_logout("resend request rate exceeded");
        transport_.close();
        return InboundVerdict::RejectedSession;
    }

    std::uint64_t begin = 0;
    std::uint64_t end = 0;
    if (!m.find_u64(tag::BeginSeqNo, &begin) || !m.find_u64(tag::EndSeqNo, &end)) {
        send_reject(m, SessionRejectReason::RequiredTagMissing, "resend range missing",
                    tag::BeginSeqNo);
        return InboundVerdict::RejectedSession;
    }
    // EndSeqNo 0 means "through the end of the stream" in FIX 4.4.
    const std::uint64_t last = state_.next_out == 0 ? 0 : state_.next_out - 1;
    if (end == 0 || end > last) end = last;
    if (begin == 0) begin = 1;
    if (end < begin) {
        send_gap_fill(begin, state_.next_out);
        return InboundVerdict::Accepted;
    }
    if (end - begin + 1 > cfg_.max_resend_span) end = begin + cfg_.max_resend_span - 1;

    std::vector<std::pair<std::uint64_t, std::string>> history;
    if (store_.outbound_history(begin, end, &history) != StoreError::None) {
        halt(HaltReason::StoreCorrupt, "outbound history unreadable");
        return InboundVerdict::Halted;
    }

    // Walk the requested range in order. A sequence number present in history
    // is replayed verbatim with 43=Y and 122; a run of sequence numbers that
    // belonged to admin traffic is covered by one GapFill. A sequence number
    // that should carry an application report but has no stored frame is NOT
    // gap-filled: that would silently erase a trade, so it halts (I-07, I-08,
    // PB-13).
    std::size_t idx = 0;
    std::uint64_t gap_from = 0;
    for (std::uint64_t seq = begin; seq <= end; ++seq) {
        const bool have = idx < history.size() && history[idx].first == seq;
        if (have) {
            if (gap_from != 0) {
                send_gap_fill(gap_from, seq);
                gap_from = 0;
            }
            std::string replay;
            if (!rebuild_as_retransmission(history[idx].second, clock_.wall_now(), &replay)) {
                halt(HaltReason::MissingOutboundHistory,
                     "stored frame at sequence " + std::to_string(seq) + " is unusable");
                return InboundVerdict::Halted;
            }
            if (!write_frame(replay)) return InboundVerdict::Accepted;
            ++resend_served_;
            ++idx;
        } else {
            if (gap_from == 0) gap_from = seq;
        }
    }
    if (gap_from != 0) send_gap_fill(gap_from, end + 1);
    return InboundVerdict::Accepted;
}

InboundVerdict AcceptorSession::handle_application(const DecodedMessage& m,
                                                   std::string_view raw) {
    const std::uint64_t expected = state_.next_in;

    if (m.seq_num < expected) {
        if (!m.poss_dup) {
            send_logout("MsgSeqNum too low");
            transport_.close();
            return InboundVerdict::RejectedSession;
        }
        // A session retransmission of an application message: the business
        // layer already saw it, and it is not delivered again (I-02, C-02).
        ++duplicates_ignored_;
        return InboundVerdict::IgnoredPossDup;
    }
    if (m.seq_num > expected) {
        // Out of order: nothing is delivered until sequencing is safe (B-20,
        // K-10).
        if (gap_requested_through_ < m.seq_num) {
            send_resend_request(expected, 0);
            gap_requested_through_ = m.seq_num;
        }
        return InboundVerdict::QueuedGap;
    }
    if (phase_ != SessionPhase::Active) {
        // Synchronizing or draining: the frame is not even committed, so the
        // peer will resend it once the session is usable (A-15, H-19).
        return InboundVerdict::NotActive;
    }

    if (!is_supported_app_request(m.msg_type)) {
        // 35=H and anything else outside the profile: committed for sequence
        // continuity, then answered with 35=j (B-17).
        bool duplicate = false;
        if (!commit_and_check(m, raw, false, &duplicate)) return InboundVerdict::Halted;
        if (duplicate) return InboundVerdict::Duplicate;
        std::string ref_id;
        if (const auto cl = m.find(tag::ClOrdID)) ref_id = std::string(*cl);
        actor_.on_unsupported(m.msg_type, m.seq_num, ref_id);
        dispatch_outbox();
        return InboundVerdict::Accepted;
    }

    // TX-A before any business effect.
    bool duplicate = false;
    if (!commit_and_check(m, raw, true, &duplicate)) return InboundVerdict::Halted;
    if (duplicate) {
        ++duplicates_ignored_;
        return InboundVerdict::Duplicate;
    }

    AppCommand cmd;
    InboundIdentity id;
    id.session_id = cfg_.session_id;
    id.epoch = state_.epoch;
    id.msg_seq_num = m.seq_num;
    id.poss_dup = m.poss_dup;
    id.poss_resend = m.poss_resend;

    const std::string business_day = business_day_utc(clock_.wall_now());
    const BizReject vr = actor_.validator().validate(m, id, cfg_.target_comp_id, business_day,
                                                     clock_.wall_now(), &cmd);
    if (vr != BizReject::None) {
        // A validation failure still owes the client an answer, and the answer
        // is an application message built by the actor so that it lands in the
        // same durable outbox as any other report.
        //
        // It goes through reject_command, NOT on_command: on_command is the
        // accept path, and handing it a command the validator refused would
        // accept the order and drop the reason on the floor.
        if (vr == BizReject::UnsupportedMsgType) {
            actor_.on_unsupported(m.msg_type, m.seq_num, cmd.key.cl_ord_id);
        } else {
            actor_.reject_command(cmd, vr);
        }
        if (actor_.state() == ActorState::Halted) {
            halt(actor_.halt_reason(), actor_.halt_detail());
            return InboundVerdict::Halted;
        }
        dispatch_outbox();
        return InboundVerdict::Accepted;
    }

    actor_.on_command(cmd);
    if (actor_.state() == ActorState::Halted) {
        halt(actor_.halt_reason(), actor_.halt_detail());
        return InboundVerdict::Halted;
    }
    dispatch_outbox();
    return InboundVerdict::Accepted;
}

// ---------------------------------------------------------------------------
// Outbound
// ---------------------------------------------------------------------------

std::uint64_t AcceptorSession::take_admin_seq() {
    // The counter lives in the store, so an admin message and a business report
    // can never be handed the same number across a restart.
    std::uint64_t seq = 0;
    if (store_.take_admin_seq(&seq) != StoreError::None) {
        halt(HaltReason::StoreCommitFailed, "could not allocate an outbound sequence number");
        return 0;
    }
    state_.next_out = seq + 1;
    return seq;
}

bool AcceptorSession::write_frame(const std::string& frame) {
    if (fp::Action::ReturnError == LPSIM_FP(fp::point::kSocketBeforeWrite)) return false;
    const std::size_t n = transport_.write(frame);
    if (n == frame.size()) {
        last_tx_ = clock_.steady_now();
        sent_.push_back(frame);
        LPSIM_FP(fp::point::kSocketAfterPartialWrite);
        return true;
    }
    // A short or failed write means the peer may have received anything from
    // nothing to everything. The caller must not treat the report as
    // acknowledged; recovery is the session resend mechanism (W7, K-08).
    LPSIM_FP(fp::point::kSocketAfterPartialWrite);
    return false;
}

void AcceptorSession::send_admin(std::string_view mt, const CanonicalBody& body) {
    OutboundFields f;
    f.msg_type = std::string(mt);
    f.seq_num = take_admin_seq();
    f.sender_comp_id = cfg_.sender_comp_id;
    f.target_comp_id = cfg_.target_comp_id;
    f.sending_time = clock_.wall_now();
    for (const auto& kv : body) f.body.emplace_back(kv.first, kv.second);
    write_frame(encode(f));
}

void AcceptorSession::send_reject(const DecodedMessage& m, SessionRejectReason reason,
                                  std::string_view text, int ref_tag) {
    CanonicalBody body;
    body.emplace_back(tag::RefSeqNum, std::to_string(m.seq_num));
    if (!m.msg_type.empty()) body.emplace_back(tag::RefMsgType, std::string(m.msg_type));
    if (ref_tag != 0) body.emplace_back(tag::RefTagID, std::to_string(ref_tag));
    body.emplace_back(tag::SessionRejectReason,
                      std::to_string(static_cast<int>(reason)));
    body.emplace_back(tag::Text, std::string(text));
    send_admin(msg_type::Reject, body);
}

void AcceptorSession::send_logout(std::string_view text) {
    CanonicalBody body;
    body.emplace_back(tag::Text, std::string(text));
    send_admin(msg_type::Logout, body);
}

void AcceptorSession::send_resend_request(std::uint64_t begin, std::uint64_t end) {
    CanonicalBody body;
    body.emplace_back(tag::BeginSeqNo, std::to_string(begin));
    body.emplace_back(tag::EndSeqNo, std::to_string(end));
    send_admin(msg_type::ResendRequest, body);
}

void AcceptorSession::send_gap_fill(std::uint64_t from_seq, std::uint64_t new_seq_no) {
    // A GapFill occupies the sequence numbers it covers, so it carries the
    // first of them rather than taking a fresh one.
    OutboundFields f;
    f.msg_type = std::string(msg_type::SequenceReset);
    f.seq_num = from_seq;
    f.sender_comp_id = cfg_.sender_comp_id;
    f.target_comp_id = cfg_.target_comp_id;
    f.sending_time = clock_.wall_now();
    f.poss_dup = true;
    f.orig_sending_time = format_utc_timestamp(clock_.wall_now());
    f.body.emplace_back(tag::GapFillFlag, "Y");
    f.body.emplace_back(tag::NewSeqNo, std::to_string(new_seq_no));
    write_frame(encode(f));
}

std::size_t AcceptorSession::dispatch_outbox() {
    if (phase_ != SessionPhase::Active && phase_ != SessionPhase::Draining) return 0;

    std::vector<OutboxEntry> pending;
    if (store_.pending_outbox(&pending) != StoreError::None) {
        halt(HaltReason::StoreCorrupt, "outbox unreadable");
        return 0;
    }

    std::size_t written = 0;
    for (const OutboxEntry& e : pending) {
        const std::string mt = std::string(msg_type_for(e.type));
        const CanonicalBody body = e.body;

        SendOnceResult res;
        // TX-C. The encoder runs inside the store transaction so the journaled
        // frame is exactly what goes on the wire and exactly what a later
        // resend replays.
        const FrameEncoder enc = [&](std::uint64_t seq) {
            OutboundFields f;
            f.msg_type = mt;
            f.seq_num = seq;
            f.sender_comp_id = cfg_.sender_comp_id;
            f.target_comp_id = cfg_.target_comp_id;
            f.sending_time = clock_.wall_now();
            for (const auto& kv : body) f.body.emplace_back(kv.first, kv.second);
            return encode(f);
        };

        const StoreError se = store_.send_once(e.event_id, body, enc, &res);
        if (se == StoreError::Conflict) {
            halt(HaltReason::OutboxConflict,
                 "send_once refused a changed payload for event " + e.event_id);
            return written;
        }
        if (se == StoreError::Inconsistent) {
            halt(HaltReason::MissingOutboundHistory,
                 "outbox event " + e.event_id + " has no committed intent");
            return written;
        }
        if (se != StoreError::None) {
            // Could not bind a sequence number: stop, keep the row pending and
            // try again later with the SAME event_id (W5).
            return written;
        }

        // A row that already owned a sequence number is re-sent as the stored
        // original frame; NextNumOut did not move (I-12, PB-02).
        if (!write_frame(res.frame)) return written;
        ++written;

        // TX-D. Losing this is harmless: the row stays SeqAssigned and the next
        // dispatch re-sends the same frame with the same sequence number (W8).
        store_.mark_sent(e.event_id);
    }
    // Keep the in-memory view of NextNumOut aligned with the store, which owns
    // it for business reports.
    SessionState refreshed;
    if (store_.load_session_state(&refreshed) == StoreError::None) {
        state_.next_out = std::max(state_.next_out, refreshed.next_out);
    }
    return written;
}

void AcceptorSession::request_logout(std::string_view text) {
    if (phase_ == SessionPhase::Halted) return;
    phase_ = SessionPhase::Draining;
    send_logout(text);
}

// ---------------------------------------------------------------------------
// Timers
// ---------------------------------------------------------------------------

void AcceptorSession::on_timer() {
    if (phase_ != SessionPhase::Active && phase_ != SessionPhase::Synchronizing &&
        phase_ != SessionPhase::Draining)
        return;

    const auto hb = std::chrono::seconds(negotiated_heartbeat_s_);
    const SteadyTime now = clock_.steady_now();

    if (now - last_tx_ >= hb) send_admin(msg_type::Heartbeat, CanonicalBody{});

    if (test_request_outstanding_) {
        // The probe went unanswered: the connection is not usable even though
        // the socket has not reported an error (A-17, K-17).
        if (now - last_rx_ >= hb * 2) {
            send_logout("TestRequest not answered");
            transport_.close();
            phase_ = SessionPhase::Listening;
        }
        return;
    }

    if (now - last_rx_ >= hb + hb / 5) {
        ++test_request_counter_;
        pending_test_req_id_ = "TR-" + std::to_string(test_request_counter_);
        test_request_outstanding_ = true;
        CanonicalBody body;
        body.emplace_back(tag::TestReqID, pending_test_req_id_);
        send_admin(msg_type::TestRequest, body);
    }
}

}  // namespace lpsim
