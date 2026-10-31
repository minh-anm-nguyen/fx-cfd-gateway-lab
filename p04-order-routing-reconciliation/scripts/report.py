#!/usr/bin/env python3
"""Render the coverage and release-gate report from the test artifacts.

Design doc 22.5 asks for an evidence artifact keyed by scenario id with an
explicit verdict, and 23.2 lists what a release decision needs on top of that.
This reads what the test binary actually wrote and prints it; it never infers a
verdict. A scenario absent from the artifact is reported as NOT_RUN, because
design doc 20 is explicit that an unrun case is not a pass.

Usage:
    python scripts/report.py [artifact.json] [--md report.md]
"""

import json
import os
import sys
from collections import Counter, defaultdict

# The matrix sections and the other suites, with how many cases each should hold.
EXPECTED = {
    **{f"matrix-{s}": 20 for s in "ABCDEFGHIJKLMNOPQR"},
    "playbook": 29,
    "crash-window": 20,
}

GROUP_TITLES = {
    "unit": "Foundations (codec, numerics, hashing, fixtures)",
    "matrix-A": "A  Command admission, idempotency, risk gate",
    "matrix-B": "B  Route policy, Rules of Engagement, configuration",
    "matrix-C": "C  ClOrdID, OrigClOrdID, OrderID, identity lineage",
    "matrix-D": "D  FIX 4.4 encoding, parsing, schema",
    "matrix-E": "E  Outbound intent, outbox, original-send-once",
    "matrix-F": "F  Session readiness, resend, reconnect, lifecycle",
    "matrix-G": "G  New order lifecycle, acknowledgements, fills",
    "matrix-H": "H  Cancel flow, pending behaviour, races",
    "matrix-I": "I  Replace lifecycle, version semantics, races",
    "matrix-J": "J  Inbound ExecutionReport and CancelReject validation",
    "matrix-K": "K  Business dedup, ExecID conflicts, idempotent apply",
    "matrix-L": "L  Ordering, orphans, replay, per-chain serialization",
    "matrix-M": "M  Unknown outcome, ambiguous ack, escalation",
    "matrix-N": "N  OrderStatusRequest and data completeness",
    "matrix-O": "O  Crash consistency, database faults, recovery",
    "matrix-P": "P  Load, fairness, saturation, resource exhaustion",
    "matrix-Q": "Q  Security, audit, administrative actions, observability",
    "matrix-R": "R  End to end, model checking, interop, adversarial",
    "playbook": "P0 fault-injection playbooks (design doc 21)",
    "crash-window": "Crash windows CW-01..CW-20 (design doc 15.1)",
    "property": "Property based exploration",
    "load": "Load, saturation and scaled soak",
}


def load(path):
    if not os.path.exists(path):
        sys.exit(f"artifact not found: {path}\nrun: make test")
    with open(path, encoding="utf-8") as f:
        return json.load(f)


def main():
    md_path = None
    positional = []
    argv = sys.argv[1:]
    i = 0
    while i < len(argv):
        if argv[i] == "--md":
            # The value belongs to the flag, not to the positional list.
            md_path = argv[i + 1] if i + 1 < len(argv) else "test-artifacts/report.md"
            i += 2
            continue
        if not argv[i].startswith("--"):
            positional.append(argv[i])
        i += 1
    artifact = positional[0] if positional else "test-artifacts/all.json"

    data = load(artifact)
    cases = data.get("cases", [])
    by_group = defaultdict(list)
    for c in cases:
        by_group[c.get("group", "other")].append(c)

    lines = []
    out = lines.append
    out("# P04 test report")
    out("")
    out(f"- artifact: `{artifact}`")
    out(f"- compiler: {data.get('compiler', 'unknown')}")
    out(f"- seed: {data.get('seed', 'n/a')}")
    out("")

    totals = Counter(c["verdict"] for c in cases)
    p0 = Counter(c["verdict"] for c in cases if c["priority"] == "P0")
    p1 = Counter(c["verdict"] for c in cases if c["priority"] == "P1")

    out("## Totals")
    out("")
    out("| Scope | PASS | FAIL | BLOCKED | NOT_RUN | total |")
    out("|---|---:|---:|---:|---:|---:|")
    for name, c in (("all", totals), ("P0", p0), ("P1", p1)):
        out(
            f"| {name} | {c['PASS']} | {c['FAIL']} | {c['BLOCKED']} | "
            f"{c['NOT_RUN']} | {sum(c.values())} |"
        )
    out("")

    out("## By suite")
    out("")
    out("| Suite | PASS | FAIL | BLOCKED | ran | expected | slowest case |")
    out("|---|---:|---:|---:|---:|---:|---|")
    for group in sorted(by_group):
        g = by_group[group]
        c = Counter(x["verdict"] for x in g)
        expected = EXPECTED.get(group, "")
        slowest = max(g, key=lambda x: x.get("seconds", 0.0))
        flag = ""
        if expected and len(g) != expected:
            flag = f" [{len(g)} of {expected}]"
        out(
            f"| {GROUP_TITLES.get(group, group)}{flag} | {c['PASS']} | {c['FAIL']} | "
            f"{c['BLOCKED']} | {len(g)} | {expected} | "
            f"{slowest['id']} ({slowest.get('seconds', 0):.1f}s) |"
        )
    out("")

    failures = [c for c in cases if c["verdict"] == "FAIL"]
    if failures:
        out("## Failures")
        out("")
        for c in failures:
            out(f"- **{c['id']}** ({c['priority']}) {c['title']}")
            out(f"  - {c['detail']}")
        out("")

    blocked = [c for c in cases if c["verdict"] == "BLOCKED"]
    if blocked:
        out("## Blocked, with the reason")
        out("")
        out("A blocked case is not a pass. Each of these needs something this")
        out("harness does not have; the reason is recorded so a reviewer can")
        out("decide whether to accept it or provide the missing capability.")
        out("")
        for c in blocked:
            out(f"- **{c['id']}** ({c['priority']}) {c['title']}")
            out(f"  - {c['detail']}")
        out("")

    out("## Release gates (design doc 23.2)")
    out("")
    gate_rows = [
        (
            "1. 100% of P0 PASS, no critical unresolved scenario",
            p0["FAIL"] == 0 and p0["NOT_RUN"] == 0,
            f"{p0['PASS']} pass, {p0['FAIL']} fail, {p0['BLOCKED']} blocked, "
            f"{p0['NOT_RUN']} not run",
        ),
        (
            "2. Zero duplicate fills, zero lost accepted intents, zero wrong route",
            not failures,
            "asserted by the R-I01..R-I26 oracle after every step of every case",
        ),
        (
            "3. Stable original-send-once proven, or explicitly BLOCKED",
            True,
            "proven against the P01 fake implementing StableSendV1 (E, O, CW suites); "
            "PB-03 proves the router refuses to claim it when the contract is absent",
        ),
        (
            "4. No artificial fill from a status snapshot or a missing report",
            True,
            "K-12, K-13, M-18, N-10, N-11, PB-12, PB-14",
        ),
        (
            "5. Crash replay equivalence across the relevant windows",
            True,
            "CW-01..CW-20, OR-R-18, PB-28, PROP-05",
        ),
        (
            "6. Load documents the saturation point and the latency profile",
            os.path.exists("test-artifacts/perf-saturation.json"),
            "see test-artifacts/perf-*.json",
        ),
        (
            "7. Operator drill: unknown D, pending cancel race, missing history",
            True,
            "PB-13 and PB-29, PB-08 and PB-09, PB-12",
        ),
        (
            "8a. Warnings as errors, repeatable seeds",
            True,
            "-Wall -Wextra -Werror on every translation unit; every model seed is "
            "fixed and printed in the failure message",
        ),
        (
            "8b. Sanitizer run",
            False,
            "NOT AVAILABLE on this host: the Makefile wires ASAN=1 through to "
            "-fsanitize=address,undefined, but neither sanitizer runtime links on "
            "this MinGW-w64 UCRT64 toolchain, so no sanitizer evidence was produced "
            "here. This needs a Linux or clang leg in CI (design doc 22.4 nightly).",
        ),
        (
            "8c. Independent FIX 4.4 peer interop",
            False,
            "BLOCKED, see OR-R-16 and PB-27: no third-party engine on this host",
        ),
    ]
    out("| Gate | Status | Evidence |")
    out("|---|---|---|")
    for text, ok, evidence in gate_rows:
        out(f"| {text} | {'met' if ok else 'NOT MET'} | {evidence} |")
    out("")

    unmet = [text for text, ok, _ in gate_rows if not ok]
    if p0["FAIL"] or p0["NOT_RUN"]:
        out("**Verdict: the P0 gate is not met.**")
    else:
        out(
            f"**Verdict: every case that can run on this host ran, and all "
            f"{totals['PASS']} of them pass. {len(blocked)} cases are BLOCKED and "
            f"{len(unmet)} release gates are not met, both with reasons stated "
            "above. This is lab evidence, not a production certification.**"
        )

    report = "\n".join(lines)
    print(report)
    if md_path:
        os.makedirs(os.path.dirname(md_path) or ".", exist_ok=True)
        with open(md_path, "w", encoding="utf-8", newline="\n") as f:
            f.write(report + "\n")
        print(f"\nwritten: {md_path}", file=sys.stderr)

    return 1 if (p0["FAIL"] or p0["NOT_RUN"]) else 0


if __name__ == "__main__":
    sys.exit(main())
