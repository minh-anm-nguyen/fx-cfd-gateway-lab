#include "risk/failpoint.hpp"

#include <array>
#include <cstdlib>
#include <string>

namespace risk {
namespace {

struct Entry {
  Failpoint fp;
  std::string_view name;
  std::string_view window;
};

constexpr std::array<Entry, 21> kTable{{
    {Failpoint::None, "NONE", "-"},
    {Failpoint::BeforeAuthz, "BEFORE_AUTHZ", "CW-01"},
    {Failpoint::AfterAuthzBeforeIdempotency, "AFTER_AUTHZ_BEFORE_IDEMPOTENCY", "CW-02"},
    {Failpoint::AfterSnapshotBeforeMath, "AFTER_SNAPSHOT_BEFORE_MATH", "CW-03"},
    {Failpoint::AfterMathBeforeDomainLock, "AFTER_MATH_BEFORE_DOMAIN_LOCK", "CW-04"},
    {Failpoint::AfterLockBeforeBegin, "AFTER_LOCK_BEFORE_BEGIN", "CW-05"},
    {Failpoint::AfterCommandInsert, "AFTER_COMMAND_INSERT", "CW-06"},
    {Failpoint::AfterReservationInsert, "AFTER_RESERVATION_INSERT", "CW-07"},
    {Failpoint::AfterProjectionUpdate, "AFTER_PROJECTION_UPDATE", "CW-08"},
    {Failpoint::AfterAuditInsert, "AFTER_AUDIT_INSERT", "CW-09"},
    {Failpoint::AfterOutboxInsert, "AFTER_OUTBOX_INSERT", "CW-10"},
    {Failpoint::AfterRevisionCas, "AFTER_REVISION_CAS", "CW-11"},
    {Failpoint::AfterDurableCommitBeforeAck, "AFTER_DURABLE_COMMIT_BEFORE_ACK", "CW-12"},
    {Failpoint::AfterAllowBeforeClaim, "AFTER_ALLOW_BEFORE_CLAIM", "CW-13"},
    {Failpoint::AfterClaimBeforeSequence, "AFTER_CLAIM_BEFORE_SEQUENCE", "CW-14"},
    {Failpoint::AfterSequenceBeforeWrite, "AFTER_SEQUENCE_BEFORE_WRITE", "CW-15"},
    {Failpoint::AfterSocketWriteBeforeAck, "AFTER_SOCKET_WRITE_BEFORE_ACK", "CW-16"},
    {Failpoint::AfterInboundDurableBeforeApply, "AFTER_INBOUND_DURABLE_BEFORE_APPLY", "CW-17"},
    {Failpoint::AfterLedgerStageBeforeCommit, "AFTER_LEDGER_STAGE_BEFORE_COMMIT", "CW-18"},
    {Failpoint::AfterApplyCommitBeforeCursorAck, "AFTER_APPLY_COMMIT_BEFORE_CURSOR_ACK", "CW-19"},
    {Failpoint::AfterReleaseCommitBeforeAck, "AFTER_RELEASE_COMMIT_BEFORE_ACK", "CW-20"},
}};

bool parse_action(std::string_view text, FailAction& out) {
  if (text == "commit_fail") { out = FailAction::CommitFail; return true; }
  if (text == "commit_ack_lost") { out = FailAction::CommitAckLost; return true; }
  if (text == "fence_reject") { out = FailAction::FenceReject; return true; }
  if (text == "abort") { out = FailAction::Abort; return true; }
  return false;
}

}  // namespace

std::string_view failpoint_name(Failpoint fp) noexcept {
  for (const Entry& e : kTable) {
    if (e.fp == fp) return e.name;
  }
  return "UNKNOWN";
}

std::string_view failpoint_window_id(Failpoint fp) noexcept {
  for (const Entry& e : kTable) {
    if (e.fp == fp) return e.window;
  }
  return "-";
}

bool parse_failpoint(std::string_view text, Failpoint& out) noexcept {
  for (const Entry& e : kTable) {
    if (e.name == text || e.window == text) {
      out = e.fp;
      return true;
    }
  }
  return false;
}

void FailpointRegistry::arm(Failpoint fp, FailAction action, int times, int skip) {
  std::lock_guard<std::mutex> lock(mu_);
  ArmedFailpoint a;
  a.action = action;
  a.remaining = times;
  a.skip = skip;
  armed_[fp] = std::move(a);
}

void FailpointRegistry::arm_hook(Failpoint fp, std::function<void()> hook, int times,
                                 int skip) {
  std::lock_guard<std::mutex> lock(mu_);
  ArmedFailpoint a;
  a.action = FailAction::Hook;
  a.remaining = times;
  a.skip = skip;
  a.hook = std::move(hook);
  armed_[fp] = std::move(a);
}

void FailpointRegistry::disarm(Failpoint fp) {
  std::lock_guard<std::mutex> lock(mu_);
  armed_.erase(fp);
}

void FailpointRegistry::disarm_all() {
  std::lock_guard<std::mutex> lock(mu_);
  armed_.clear();
}

bool FailpointRegistry::reach(Failpoint fp, Hit& hit) {
  std::function<void()> hook;
  FailAction action = FailAction::CommitFail;
  bool fire = false;
  {
    std::lock_guard<std::mutex> lock(mu_);
    trace_.push_back(fp);
    ++counts_[fp];

    const auto it = armed_.find(fp);
    if (it != armed_.end() && it->second.remaining > 0) {
      if (it->second.skip > 0) {
        --it->second.skip;
      } else {
        --it->second.remaining;
        action = it->second.action;
        hook = it->second.hook;
        fire = true;
        if (it->second.remaining == 0 && action != FailAction::Hook) {
          armed_.erase(it);
        }
      }
    }
  }
  if (!fire) return false;
  hit.action = action;
  hit.hook = std::move(hook);
  return true;
}

std::vector<Failpoint> FailpointRegistry::trace() const {
  std::lock_guard<std::mutex> lock(mu_);
  return trace_;
}

void FailpointRegistry::clear_trace() {
  std::lock_guard<std::mutex> lock(mu_);
  trace_.clear();
}

std::map<Failpoint, int> FailpointRegistry::reach_counts() const {
  std::lock_guard<std::mutex> lock(mu_);
  return counts_;
}

bool arm_from_spec(FailpointRegistry& registry, std::string_view spec_view) {
  const std::string spec(spec_view);
  std::vector<std::string> parts;
  std::size_t start = 0;
  while (start <= spec.size()) {
    const std::size_t colon = spec.find(':', start);
    if (colon == std::string::npos) {
      parts.push_back(spec.substr(start));
      break;
    }
    parts.push_back(spec.substr(start, colon - start));
    start = colon + 1;
  }
  if (parts.size() < 2) return false;

  Failpoint fp = Failpoint::None;
  FailAction action = FailAction::CommitFail;
  if (!parse_failpoint(parts[0], fp)) return false;
  if (!parse_action(parts[1], action)) return false;

  int times = 1;
  int skip = 0;
  if (parts.size() >= 3 && !parts[2].empty()) times = std::atoi(parts[2].c_str());
  if (parts.size() >= 4 && !parts[3].empty()) skip = std::atoi(parts[3].c_str());
  if (times <= 0 || skip < 0) return false;
  registry.arm(fp, action, times, skip);
  return true;
}

void arm_from_environment(FailpointRegistry& registry) {
  const char* raw = std::getenv("RISK_FAILPOINT");
  if (raw == nullptr) return;
  (void)arm_from_spec(registry, raw);
}

}  // namespace risk
