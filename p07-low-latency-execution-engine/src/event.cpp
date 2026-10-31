#include "llx/event.hpp"

namespace llx {

std::string_view to_string(EventType t) {
    switch (t) {
        case EventType::None: return "NONE";
        case EventType::NewOrder: return "NEW_ORDER";
        case EventType::CancelRequest: return "CANCEL_REQUEST";
        case EventType::ReplaceRequest: return "REPLACE_REQUEST";
        case EventType::ExecutionReport: return "EXECUTION_REPORT";
        case EventType::SessionControl: return "SESSION_CONTROL";
        case EventType::CommitResult: return "COMMIT_RESULT";
        case EventType::Tick: return "TICK";
    }
    return "UNKNOWN";
}

std::string_view to_string(Lane l) {
    switch (l) {
        case Lane::SessionControl: return "session_control";
        case Lane::AcceptedReceipt: return "accepted_receipt";
        case Lane::CancelReplace: return "cancel_replace";
        case Lane::NewOrder: return "new_order";
        case Lane::Telemetry: return "telemetry";
    }
    return "unknown";
}

std::string_view to_string(ExecType t) {
    switch (t) {
        case ExecType::None: return "NONE";
        case ExecType::New: return "NEW";
        case ExecType::PartialFill: return "PARTIAL_FILL";
        case ExecType::Fill: return "FILL";
        case ExecType::Canceled: return "CANCELED";
        case ExecType::Replaced: return "REPLACED";
        case ExecType::Rejected: return "REJECTED";
        case ExecType::Expired: return "EXPIRED";
    }
    return "UNKNOWN";
}

std::string_view to_string(AdmissionOutcome o) {
    switch (o) {
        case AdmissionOutcome::AcceptedForProcessing: return "ACCEPTED_FOR_PROCESSING";
        case AdmissionOutcome::RejectedOverload: return "REJECTED_OVERLOAD";
        case AdmissionOutcome::HeldForReconciliation: return "HELD_FOR_RECONCILIATION";
        case AdmissionOutcome::RetryLaterNoAdmission: return "RETRY_LATER_NO_ADMISSION";
        case AdmissionOutcome::InvalidInput: return "INVALID_INPUT";
        case AdmissionOutcome::EngineHalted: return "ENGINE_HALTED";
        case AdmissionOutcome::DuplicateIgnored: return "DUPLICATE_IGNORED";
        case AdmissionOutcome::OutOfOrder: return "OUT_OF_ORDER";
        case AdmissionOutcome::StaleEpoch: return "STALE_EPOCH";
    }
    return "UNKNOWN";
}

std::string_view to_string(ServiceMode m) {
    switch (m) {
        case ServiceMode::Warmup: return "WARMUP";
        case ServiceMode::Normal: return "NORMAL";
        case ServiceMode::Watch: return "WATCH";
        case ServiceMode::ShedNew: return "SHED_NEW";
        case ServiceMode::ReconOnly: return "RECON_ONLY";
        case ServiceMode::Halted: return "HALTED";
    }
    return "UNKNOWN";
}

std::string_view to_string(CommitOutcome o) {
    switch (o) {
        case CommitOutcome::Committed: return "COMMITTED";
        case CommitOutcome::Rejected: return "REJECTED";
        case CommitOutcome::Unknown: return "COMMIT_UNKNOWN";
    }
    return "UNKNOWN";
}

std::string_view to_string(Span s) {
    // The names carry the span definition, because doc 3 forbids adding or
    // averaging across spans and a bare "L3" in a report invites exactly that.
    switch (s) {
        case Span::L0_ReceiveToRoute: return "L0_receive_to_route";
        case Span::L1_QueueWait: return "L1_queue_wait";
        case Span::L2_DecisionCompute: return "L2_decision_compute";
        case Span::L3_DurableAdmission: return "L3_durable_admission";
        case Span::L4_DispatchEligibility: return "L4_dispatch_eligibility";
        case Span::L5_Reconciliation: return "L5_reconciliation";
        case Span::L6_SimulatedEndToEnd: return "L6_simulated_end_to_end";
        case Span::Count: return "count";
    }
    return "unknown";
}

std::string_view to_string(Side s) {
    switch (s) {
        case Side::None: return "NONE";
        case Side::Buy: return "BUY";
        case Side::Sell: return "SELL";
    }
    return "UNKNOWN";
}

}  // namespace llx
