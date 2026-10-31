#include "router/failpoint.hpp"

namespace router {

void FailpointRegistry::arm(const std::string& point, FaultKind kind, int skip, int times) {
    Arm a;
    a.kind = kind;
    a.skip = skip;
    a.times = times;
    arms_[point] = a;
}

void FailpointRegistry::disarm(const std::string& point) { arms_.erase(point); }

void FailpointRegistry::clear() {
    arms_.clear();
    hits_.clear();
}

bool FailpointRegistry::should_fire(const std::string& point, FaultKind& kind) {
    ++hits_[point];
    auto it = arms_.find(point);
    if (it == arms_.end()) return false;
    Arm& a = it->second;
    ++a.hits;
    if (a.hits <= a.skip) return false;
    if (a.times >= 0 && a.fired >= a.times) return false;
    ++a.fired;
    kind = a.kind;
    return true;
}

void FailpointRegistry::check(const std::string& point) {
    FaultKind kind = FaultKind::Crash;
    if (!should_fire(point, kind)) return;
    if (kind == FaultKind::Crash) throw CrashMarker(point);
}

int FailpointRegistry::hits(const std::string& point) const {
    auto it = hits_.find(point);
    return it == hits_.end() ? 0 : it->second;
}

int FailpointRegistry::fired(const std::string& point) const {
    auto it = arms_.find(point);
    return it == arms_.end() ? 0 : it->second.fired;
}

}  // namespace router
