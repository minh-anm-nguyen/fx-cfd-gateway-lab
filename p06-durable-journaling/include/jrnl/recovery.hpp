// Recovery supervisor and the admission gate.
//
// Section 14 draws the phase machine and makes one point the code has to
// honour literally: READY is not reached because a process is listening on a
// port or a database ping succeeded. Every phase below has to pass on
// evidence, and two of the three restart kinds cannot reach READY on local
// evidence at all.
//
//   STOPPED -> ACQUIRE_FENCE -> VERIFY_STORAGE -> REPLAY_LOCAL
//           -> VERIFY_PROJECTIONS -> RECONCILE_REMOTE -> RECON_ONLY
//           -> READY | FAULTED
//
// Dispatch starts disabled and is only ever enabled at the end. JI-24.
#pragma once

#include <set>
#include <string>
#include <vector>

#include "jrnl/metrics.hpp"
#include "jrnl/outbox.hpp"
#include "jrnl/replay.hpp"
#include "jrnl/snapshot.hpp"
#include "jrnl/store.hpp"

namespace jrnl {

enum class Phase {
    Stopped,
    AcquireFence,
    VerifyStorage,
    ReplayLocal,
    VerifyProjections,
    ReconcileRemote,
    ReconOnly,
    Ready,
    Quarantine,
    Faulted,
};

std::string_view to_string(Phase p);

// How this instance came up. The distinction is not cosmetic: section 14
// says a process restart on an intact primary may continue after remote
// reconciliation, while a restore or a promotion may be missing transactions
// the counterparty already acted on (JI-31, JI-32).
enum class RecoveryMode {
    ProcessRestart,
    RestoredFromBackup,
    PromotedStandby,
};

std::string_view to_string(RecoveryMode m);

// What the rest of the system asks before it does anything outward-facing.
class IAdmissionGate {
public:
    virtual ~IAdmissionGate() = default;
    // May a new business command be accepted for this risk domain?
    virtual bool may_admit(const TenantId& tenant, const RiskDomainId& domain) const = 0;
    // May an outbound effect be dispatched for this risk domain?
    virtual bool may_dispatch(const TenantId& tenant, const RiskDomainId& domain) const = 0;
    virtual std::string why_blocked(const TenantId& tenant,
                                    const RiskDomainId& domain) const = 0;
};

// A gate that permits everything. For unit tests of components that are not
// the gate; never a default in a running configuration.
class OpenGate final : public IAdmissionGate {
public:
    bool may_admit(const TenantId&, const RiskDomainId&) const override { return true; }
    bool may_dispatch(const TenantId&, const RiskDomainId&) const override { return true; }
    std::string why_blocked(const TenantId&, const RiskDomainId&) const override { return ""; }
};

struct RecoveryReport {
    Phase phase = Phase::Stopped;
    RecoveryMode mode = RecoveryMode::ProcessRestart;

    bool storage_valid = false;
    bool journal_valid = false;
    bool projections_valid = false;
    bool fencing_valid = false;
    bool external_reconciliation_required = false;
    bool dispatch_enabled = false;

    std::vector<Incident> incidents;
    std::vector<std::string> details;
    std::set<RiskDomainId> frozen_domains;

    std::size_t streams_checked = 0;
    std::size_t events_checked = 0;
    std::size_t unresolved_effects = 0;
    std::size_t open_cases = 0;
    std::int64_t duration_ms = 0;

    bool has(Incident i) const;
    bool ready() const { return phase == Phase::Ready; }
};

class RecoverySupervisor final : public IAdmissionGate {
public:
    struct Options {
        TenantId tenant;
        std::string owner = "gateway-1";
        std::vector<RiskDomainId> domains;
        std::int64_t lease_ms = 30000;
        // Section 14: after a restore or a promotion, local history can be
        // behind what the counterparty already did. Without independent
        // evidence the instance stays in RECON_ONLY. Setting this to false
        // is what a test does to show the unsafe alternative.
        bool require_external_evidence = true;
        // Supplied by an operator or a reconciliation job once the
        // counterparty's own execution history has been compared.
        bool external_evidence_provided = false;
        Metrics* metrics = nullptr;
    };

    RecoverySupervisor(SqliteAtomicStore& store, Options opt);

    // Runs the phases in order and stops at the first one that cannot pass.
    RecoveryReport verify_and_replay(RecoveryMode mode);

    // --- IAdmissionGate ---
    bool may_admit(const TenantId& tenant, const RiskDomainId& domain) const override;
    bool may_dispatch(const TenantId& tenant, const RiskDomainId& domain) const override;
    std::string why_blocked(const TenantId& tenant, const RiskDomainId& domain) const override;

    Phase phase() const { return report_.phase; }
    const RecoveryReport& report() const { return report_; }
    FenceToken token(const RiskDomainId& domain) const;

    // Ownership loss or a health breach drops a live instance back out of
    // ACTIVE without waiting for a restart (section 14's state diagram).
    void demote(const std::string& reason);

    // An operator supplying reconciliation evidence. Recorded, not assumed:
    // the approvals string ends up on the recovery cases it closes.
    void provide_external_evidence(const std::string& approvals);

private:
    bool acquire_fences(RecoveryReport* rep);

    SqliteAtomicStore& store_;
    Options opt_;
    RecoveryReport report_;
    std::map<RiskDomainId, FenceToken> tokens_;
    std::string demotion_reason_;
    std::string approvals_;
};

}  // namespace jrnl
