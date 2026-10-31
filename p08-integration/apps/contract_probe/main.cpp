// The M1 gate.
//
// Section 29 gives M1 one exit criterion -- "Version-pinned contracts +
// isolated Compose lab" -- and section 2 says what happens when a pin
// disagrees: BLOCKED, not a best-effort start. This program is the first half
// of that, the part that runs before any container does.
//
// It reports READY and exits 0, or prints every disagreement and exits 2.
// Exit 2 rather than 1 so a crash of the probe itself is distinguishable from
// a clean refusal to start: EI-54 forbids a release gate from reading
// BLOCKED as a pass, and it cannot do that if a signal and a verdict look
// alike.
#include <cstdio>
#include <string>

#include "gw/contract.hpp"
#include "gw/evidence.hpp"
#include "gw/lab_guard.hpp"
#include "router/config.hpp"

int main() {
    std::printf("P08 integration lab -- contract gate (harness version %d)\n\n",
                gw::kHarnessVersion);

    std::printf("%-5s %-14s %-8s %s\n", "ID", "LIBRARY", "SCHEMA", "SINGLE OWNER OF");
    for (const gw::ComponentPin& c : gw::components()) {
        char schema[16];
        if (c.store_schema < 0) {
            std::snprintf(schema, sizeof schema, "%s", "-");
        } else {
            std::snprintf(schema, sizeof schema, "%d", c.store_schema);
        }
        std::printf("%-5.*s %-14.*s %-8s %.*s\n",
                    static_cast<int>(c.id.size()), c.id.data(),
                    static_cast<int>(c.library.size()), c.library.data(),
                    schema,
                    static_cast<int>(c.owns.size()), c.owns.data());
    }

    std::printf("\nnumeric contract: %d price digits, px_scale %lld, qty_scale %lld\n",
                gw::kNumericContract.price_digits,
                static_cast<long long>(gw::kNumericContract.px_scale),
                static_cast<long long>(gw::kNumericContract.qty_scale));

    // Printed, not asserted. The ladder is the vocabulary every adapter has to
    // speak, and having it in the run log means an artifact from this lab says
    // which rungs existed when it was produced.
    std::printf("\nevidence ladder:");
    for (const gw::Evidence e : {gw::Evidence::IntentRecorded, gw::Evidence::FixQueued,
                                 gw::Evidence::SocketWritten, gw::Evidence::LpAccepted,
                                 gw::Evidence::Executed, gw::Evidence::UnknownOutcome}) {
        const std::string_view name = gw::to_string(e);
        std::printf(" %.*s", static_cast<int>(name.size()), name.data());
    }
    std::printf("\n\n");

    gw::GateReport report = gw::verify_contracts();

    // Section 19's first bullet, on the lab this binary would actually drive.
    // Reported through the same gate because a mismatched pin and an endpoint
    // that can reach a venue are the same kind of answer: nothing ran, and
    // nothing may.
    const std::string trading_mode = gw::trading_mode_from_env();
    gw::GateReport isolation =
        gw::verify_isolation(router::Config::lab_default(), trading_mode);
    for (gw::Finding& f : isolation.findings) {
        report.findings.push_back(std::move(f));
    }
    if (isolation.status != gw::GateStatus::Ready) report.status = gw::GateStatus::Blocked;

    std::printf("isolation: %s=%s, allowlist %zu host(s)\n",
                std::string(gw::kTradingModeVar).c_str(),
                trading_mode.empty() ? "<unset>" : trading_mode.c_str(),
                router::Config::lab_default().endpoints.allowed_hosts.size());
    // Said out loud because READY must not be read as more than it is. Section
    // 19 asks for egress blocked at the network, "not merely app flag", and a
    // refusal to connect does not constrain a process that never asks.
    std::printf("           app-level only; egress still has to be blocked at the "
                "host/network firewall\n\n");

    for (const gw::Finding& f : report.findings) {
        std::printf("  %-32.*s %s\n", static_cast<int>(f.check.size()), f.check.data(),
                    f.detail.c_str());
    }

    const std::string_view verdict = gw::to_string(report.status);
    std::printf("gate: %.*s\n", static_cast<int>(verdict.size()), verdict.data());
    return report.status == gw::GateStatus::Ready ? 0 : 2;
}
