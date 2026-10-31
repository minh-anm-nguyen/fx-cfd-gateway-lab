#include "deps/p05_risk.hpp"

namespace router::p05 {

std::string_view to_string(Verdict v) {
    switch (v) {
        case Verdict::Approve: return "APPROVE";
        case Verdict::Reject: return "REJECT";
        case Verdict::Unavailable: return "UNAVAILABLE";
    }
    return "UNKNOWN";
}

RiskDecision FakeRisk::reserve(const RiskRequest& req) {
    ++reserve_calls_;
    RiskDecision d;

    // Idempotency first: a retried RPC for a command that already holds a
    // reservation returns the same reservation, it does not take a second one
    // (OR-A-17).
    auto it = by_command_.find(req.command_id.str());
    if (it != by_command_.end()) {
        d.verdict = Verdict::Approve;
        d.reservation_id = it->second;
        d.valid_until = clock_.wall_now() + static_cast<WallMicros>(ttl_);
        return d;
    }

    if (verdict_ == Verdict::Unavailable) {
        d.verdict = Verdict::Unavailable;
        d.reason = "risk service unavailable";
        return d;
    }
    if (verdict_ == Verdict::Reject) {
        d.verdict = Verdict::Reject;
        d.reason = "limit exceeded";
        return d;
    }

    d.verdict = Verdict::Approve;
    d.reservation_id = "RSK-" + std::to_string(next_id_++);
    d.valid_until = clock_.wall_now() + static_cast<WallMicros>(ttl_);
    by_command_[req.command_id.str()] = d.reservation_id;
    reservations_[d.reservation_id] = true;
    return d;
}

void FakeRisk::release(const std::string& reservation_id) {
    ++release_calls_;
    auto it = reservations_.find(reservation_id);
    if (it == reservations_.end()) return;
    if (!it->second) return;  // already released, so this call does nothing
    it->second = false;
    ++effective_releases_;
}

bool FakeRisk::held(const std::string& reservation_id) const {
    auto it = reservations_.find(reservation_id);
    return it != reservations_.end() && it->second;
}

void FakeRisk::revoke(const std::string& reservation_id) {
    // Models a limit being pulled after approval but before dispatch
    // (OR-B-15, OR-G-10). The reservation stops being held, which the router
    // must notice before it sends.
    auto it = reservations_.find(reservation_id);
    if (it != reservations_.end()) it->second = false;
}

}  // namespace router::p05
