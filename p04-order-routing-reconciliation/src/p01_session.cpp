#include "deps/p01_session.hpp"

#include <algorithm>

namespace router::p01 {

std::string_view to_string(SessionState s) {
    switch (s) {
        case SessionState::Disconnected: return "Disconnected";
        case SessionState::Connecting: return "Connecting";
        case SessionState::LogonSent: return "LogonSent";
        case SessionState::Synchronizing: return "Synchronizing";
        case SessionState::Active: return "Active";
        case SessionState::Down: return "Down";
    }
    return "Unknown";
}

std::string_view to_string(StableSendResult::State s) {
    switch (s) {
        case StableSendResult::State::AssignedOriginalSeq: return "AssignedOriginalSeq";
        case StableSendResult::State::AlreadyAssigned: return "AlreadyAssigned";
        case StableSendResult::State::NotReady: return "NotReady";
        case StableSendResult::State::Unknown: return "Unknown";
        case StableSendResult::State::Fatal: return "Fatal";
        case StableSendResult::State::NotSupported: return "NotSupported";
    }
    return "Unknown";
}

SessionEngine::SessionEngine(SessionImage& image, FailpointRegistry& fp)
    : image_(image), fp_(fp) {
    generation_ = ++image_.owner_generation;
}

std::optional<SessionImage::Binding> SessionEngine::lookup_binding(
    const CommandId& command_id) const {
    auto it = image_.bindings.find(command_id.str());
    if (it == image_.bindings.end()) return std::nullopt;
    return it->second;
}

StableSendResult SessionEngine::send_once(const CommandId& command_id, const Hash& payload_hash,
                                          const std::string& app_frame) {
    StableSendResult out;

    // A P01 build without the contract must say so rather than send something
    // that P04 would then have to call issue-once (PB-03).
    if (!image_.stable_send_supported) {
        out.state = StableSendResult::State::NotSupported;
        return out;
    }

    // Only the current owner of the session may write to it. Two engines on one
    // session would both allocate from next_out and break R-I06 (OR-F-15).
    if (!owns_session()) {
        out.state = StableSendResult::State::Fatal;
        return out;
    }

    // Integrity of our own history first. A session whose resend history was
    // wiped cannot prove what any sequence carried, so it refuses to issue
    // anything at all until an operator resolves the uncertainty (OR-E-13,
    // OR-F-13). Issuing a fresh original here would be the silent claim that
    // design doc 6.1 forbids.
    if (image_.resend_history_lost) {
        out.state = StableSendResult::State::Fatal;
        return out;
    }

    auto existing = image_.bindings.find(command_id.str());
    if (existing != image_.bindings.end()) {
        if (sha256(existing->second.frame) != existing->second.frame_sha) {
            out.state = StableSendResult::State::Fatal;
            return out;
        }
        if (existing->second.payload_hash != payload_hash) {
            // OR-E-07: the same command id with different bytes. Issuing a second
            // sequence here would create two orders from one intent.
            out.state = StableSendResult::State::Fatal;
            return out;
        }
        // OR-E-05, OR-E-06, PB-01: the same sequence comes back, however many
        // times the caller asks and whatever happened in between.
        out.state = StableSendResult::State::AlreadyAssigned;
        out.original_seq = existing->second.original_seq;
        // A retry after a lost reply may still need the bytes on the wire. The
        // session decides that from its own history, not from P04.
        if (!existing->second.written && ready_for_app() && !image_.blackhole) {
            existing->second.written = true;
            image_.wire_log.push_back(existing->second.frame);
            if (peer_ != nullptr) peer_->on_app_frame(existing->second.frame);
        }
        return out;
    }

    if (!ready_for_app()) {
        // Nothing is bound and nothing is written: the backlog stays with P04
        // (OR-E-10, OR-F-02, OR-F-03).
        out.state = StableSendResult::State::NotReady;
        return out;
    }

    // CW-06: dying before the binding is durable. No sequence was consumed and
    // no bytes went out.
    fp_.check(fp::kBeforeSendBinding);

    SessionImage::Binding b;
    b.original_seq = image_.next_out++;
    b.epoch = image_.epoch;
    b.payload_hash = payload_hash;
    b.frame = app_frame;
    b.frame_sha = sha256(app_frame);
    b.written = false;
    image_.bindings[command_id.str()] = b;

    // CW-07: the binding is durable, the write has not happened. A restart must
    // reuse this sequence, never allocate a new one.
    fp_.check(fp::kAfterSendBinding);

    FaultKind kind = FaultKind::Crash;

    // CW-08: a frame cut in half. The peer sees a fragment; whether it can make
    // sense of it is the peer's business, and P04 may not treat the ambiguity as
    // either success or failure.
    if (fp_.should_fire(fp::kPartialWireWrite, kind)) {
        std::string partial = app_frame.substr(0, app_frame.size() / 2);
        // A complete FIX field block always ends with SOH. Trimming any trailing
        // delimiter makes the truncation visible to the peer's framing layer,
        // which is what really happens when a write is cut short.
        while (!partial.empty() && partial.back() == '') partial.pop_back();
        image_.bindings[command_id.str()].written = true;
        image_.wire_log.push_back(partial);
        if (peer_ != nullptr && !image_.blackhole) peer_->on_app_frame(partial);
        out.state = StableSendResult::State::Unknown;
        out.original_seq = b.original_seq;
        return out;
    }

    // CW-05: the write happened and the reply was lost. P04 must treat this as
    // unknown transport, look the binding up again, and never mint a new
    // ClOrdID (R-I16, PB-01).
    if (fp_.should_fire(fp::kDropSendOnceReply, kind)) {
        image_.bindings[command_id.str()].written = true;
        image_.wire_log.push_back(app_frame);
        if (peer_ != nullptr && !image_.blackhole) peer_->on_app_frame(app_frame);
        if (kind == FaultKind::Crash) throw CrashMarker(fp::kDropSendOnceReply);
        out.state = StableSendResult::State::Unknown;
        out.original_seq = b.original_seq;
        return out;
    }

    image_.bindings[command_id.str()].written = true;
    image_.wire_log.push_back(app_frame);
    // OR-E-20: the socket took the bytes and the network swallowed them.
    if (peer_ != nullptr && !image_.blackhole) peer_->on_app_frame(app_frame);

    // CW-09: the full frame is out and then the process dies before any
    // business evidence arrives.
    fp_.check(fp::kAfterWireWrite);

    out.state = StableSendResult::State::AssignedOriginalSeq;
    out.original_seq = b.original_seq;
    return out;
}

std::optional<DurableFixInbound> SessionEngine::peek_inbound() const {
    for (const DurableFixInbound& in : image_.inbox)
        if (image_.acked.find(in.durable_inbox_id) == image_.acked.end()) return in;
    return std::nullopt;
}

void SessionEngine::ack_inbound_after_commit(DurableReceiptId id) {
    image_.acked.insert(id);
    // Acknowledged rows are dropped from the front only, so ordering is kept.
    while (!image_.inbox.empty() &&
           image_.acked.find(image_.inbox.front().durable_inbox_id) != image_.acked.end())
        image_.inbox.pop_front();
}

std::size_t SessionEngine::pending_inbound() const {
    return static_cast<std::size_t>(std::count_if(
        image_.inbox.begin(), image_.inbox.end(), [this](const DurableFixInbound& in) {
            return image_.acked.find(in.durable_inbox_id) == image_.acked.end();
        }));
}

DurableReceiptId SessionEngine::deliver_inbound(const std::string& frame) {
    // An application frame outside an established session is a session layer
    // problem. P01 refuses it here so that it never becomes a business event
    // (OR-F-16, OR-Q-02).
    if (image_.state != SessionState::Active) {
        image_.refused_inbound.push_back(frame);
        return 0;
    }

    DurableFixInbound in;
    in.session_key = image_.identity.key();
    in.epoch = image_.epoch;
    in.msg_seq_num = image_.next_in++;
    in.durable_inbox_id = image_.next_receipt++;
    in.frame = frame;
    in.raw_sha256 = sha256(frame);
    image_.inbox.push_back(in);
    // CW-11: the receipt is durable in P01 and P04 has not seen it yet. A
    // restart has to redeliver it.
    fp_.check(fp::kAfterInboxCommit);
    return in.durable_inbox_id;
}

void SessionEngine::redeliver(DurableReceiptId id) {
    // At-least-once delivery in the raw: clearing the acknowledgement makes the
    // same receipt visible again, with the same durable_inbox_id. If the row was
    // already dropped from the queue the caller re-delivers the frame instead.
    image_.acked.erase(id);
}

void SessionEngine::disconnect() {
    // Bindings deliberately survive a disconnect: that is the whole point of
    // StableSendV1 and what keeps R-I06 true across a reconnect.
    image_.state = SessionState::Down;
}

void SessionEngine::reconnect(bool reset_sequences) {
    image_.state = SessionState::Synchronizing;
    if (reset_sequences) {
        // A bilateral reset moves the epoch, so old receipts cannot alias new
        // ones (design doc 15.2, OR-F-12, OR-L-15).
        ++image_.epoch;
        image_.next_out = 1;
        image_.next_in = 1;
        // Receipt numbering restarts with the epoch, but only once the queue has
        // drained. An unprocessed inbound event is never discarded to make the
        // numbering tidy (R-I25); the applied-receipt key carries the epoch, so
        // a reused number cannot alias an old one either (OR-L-15).
        if (image_.inbox.empty()) {
            image_.acked.clear();
            image_.next_receipt = 1;
        }
        if (image_.reset_on_logon) image_.resend_history_lost = true;
    }
    image_.state = SessionState::Active;
}

}  // namespace router::p01
