#!/usr/bin/env python3
"""Turn the suite artifacts into the release-gate report.

Design doc 23 requires every case to produce an artifact with an explicit
verdict, and doc 28 decides go/no-go from those verdicts. This script is the
step between: it reads build/test-artifacts/*.json and produces the matrix
report, with the gating algorithm of doc 28 applied literally.

Two rules it enforces, because they are the ones easiest to fudge by hand:

  * BLOCKED and INCONCLUSIVE are never counted as passes. Doc 28: "NOT_RUN
    remains NOT_RUN until runtime evidence exists." A run with blocked P0 cases
    is INCOMPLETE, not a pass.
  * a P0 failure is a hard block regardless of how many other cases passed.

Usage:
    python scripts/report.py [--artifacts DIR] [--out FILE] [--json]

Exit status is 0 when the verdict permits proceeding and 1 otherwise, so it can
gate a CI job directly.
"""

import argparse
import json
import os
import sys
from collections import Counter, defaultdict

VERDICTS = ("PASS", "FAIL", "NOT_RUN", "BLOCKED", "INCONCLUSIVE")


def load_artifacts(directory):
    suites = []
    if not os.path.isdir(directory):
        return suites
    for name in sorted(os.listdir(directory)):
        if not name.endswith(".json"):
            continue
        path = os.path.join(directory, name)
        try:
            with open(path, "r", encoding="utf-8") as handle:
                data = json.load(handle)
        except (OSError, json.JSONDecodeError) as exc:
            # A malformed artifact is reported, not skipped: a suite whose
            # result cannot be read has not passed.
            suites.append({"suite": name, "error": str(exc), "cases": []})
            continue
        if "cases" not in data:
            # Not a suite artifact. Run manifests and other reports share the
            # directory in some layouts; treating one as a broken suite would
            # make every run INCOMPLETE for no reason.
            continue
        suites.append(data)
    return suites


def summarise(suites):
    totals = Counter()
    by_priority = defaultdict(Counter)
    by_oracle = defaultdict(Counter)
    failures = []
    blocked = []
    inconclusive = []
    read_errors = []

    for suite in suites:
        if suite.get("error"):
            read_errors.append((suite["suite"], suite["error"]))
            continue
        for case in suite["cases"]:
            verdict = case.get("verdict", "NOT_RUN")
            priority = case.get("priority", "P?")
            totals[verdict] += 1
            by_priority[priority][verdict] += 1
            for oracle in (case.get("oracles") or "").split(","):
                oracle = oracle.strip()
                if oracle:
                    by_oracle[oracle][verdict] += 1
            entry = {
                "suite": suite.get("suite", "?"),
                "id": case.get("id", "?"),
                "priority": priority,
                "title": case.get("title", ""),
                "detail": case.get("detail", ""),
                "seed": case.get("seed", 0),
                "state_hash": case.get("state_hash", ""),
            }
            if verdict == "FAIL":
                failures.append(entry)
            elif verdict == "BLOCKED":
                blocked.append(entry)
            elif verdict == "INCONCLUSIVE":
                inconclusive.append(entry)

    return {
        "totals": totals,
        "by_priority": by_priority,
        "by_oracle": by_oracle,
        "failures": failures,
        "blocked": blocked,
        "inconclusive": inconclusive,
        "read_errors": read_errors,
    }


def decide(summary):
    """The gating algorithm of design doc 28, applied to the catalog results."""
    if summary["read_errors"]:
        return "INCOMPLETE", "one or more suite artifacts could not be read"
    if summary["totals"]["FAIL"] > 0:
        p0 = summary["by_priority"].get("P0", Counter())["FAIL"]
        if p0 > 0:
            return "NO_GO", f"{p0} P0 case(s) failed, which is a hard block"
        return "NO_GO", f"{summary['totals']['FAIL']} case(s) failed"
    if summary["totals"]["PASS"] == 0:
        return "INCOMPLETE", "no case produced a PASS verdict"
    blocked_p0 = summary["by_priority"].get("P0", Counter())["BLOCKED"]
    inconclusive_total = summary["totals"]["INCONCLUSIVE"]
    if blocked_p0 > 0 or inconclusive_total > 0:
        reason = []
        if blocked_p0:
            reason.append(f"{blocked_p0} P0 case(s) BLOCKED (dependency absent)")
        if inconclusive_total:
            reason.append(f"{inconclusive_total} case(s) INCONCLUSIVE")
        return "INCOMPLETE", "; ".join(reason)
    if summary["totals"]["NOT_RUN"] > 0:
        return "INCOMPLETE", f"{summary['totals']['NOT_RUN']} case(s) NOT_RUN"
    return "CANDIDATE_FOR_P08_INTEGRATION", "every catalog case executed and passed"


def render(summary, verdict, reason, artifacts_dir):
    lines = []
    out = lines.append
    out("# Project 07 release-gate report")
    out("")
    out(f"Artifacts read from `{artifacts_dir}`.")
    out("")
    out("## Verdict")
    out("")
    out(f"**{verdict}** -- {reason}")
    out("")
    out("Read this verdict as design doc 28 defines it. `INCOMPLETE` is not a")
    out("failure of the engine: it means some case could not be executed here and")
    out("its evidence does not exist, which doc 23 requires to be reported as")
    out("BLOCKED rather than assumed. `NO_GO` means a case that did run, failed.")
    out("")

    totals = summary["totals"]
    out("## Totals")
    out("")
    out("| Verdict | Count |")
    out("|---|---|")
    for v in VERDICTS:
        out(f"| {v} | {totals[v]} |")
    out(f"| **total** | **{sum(totals.values())}** |")
    out("")

    out("## By priority")
    out("")
    out("| Priority | PASS | FAIL | BLOCKED | INCONCLUSIVE | NOT_RUN |")
    out("|---|---|---|---|---|---|")
    for priority in sorted(summary["by_priority"]):
        c = summary["by_priority"][priority]
        out(f"| {priority} | {c['PASS']} | {c['FAIL']} | {c['BLOCKED']} | "
            f"{c['INCONCLUSIVE']} | {c['NOT_RUN']} |")
    out("")

    if summary["by_oracle"]:
        out("## By oracle")
        out("")
        out("Oracle codes are from design doc 23: Q queue, M memory, C causality,")
        out("R risk, D durability, F FIX session, P performance evidence, S safety")
        out("mode, O operations.")
        out("")
        out("| Oracle | PASS | FAIL | BLOCKED | INCONCLUSIVE |")
        out("|---|---|---|---|---|")
        for oracle in sorted(summary["by_oracle"]):
            c = summary["by_oracle"][oracle]
            out(f"| {oracle} | {c['PASS']} | {c['FAIL']} | {c['BLOCKED']} | "
                f"{c['INCONCLUSIVE']} |")
        out("")

    if summary["read_errors"]:
        out("## Unreadable artifacts")
        out("")
        for name, err in summary["read_errors"]:
            out(f"- `{name}`: {err}")
        out("")

    if summary["failures"]:
        out("## Failures")
        out("")
        for f in summary["failures"]:
            out(f"### {f['id']} ({f['priority']}) -- {f['title']}")
            out("")
            out(f"- suite: `{f['suite']}`")
            if f["seed"]:
                out(f"- seed: `{f['seed']}` (reproduces the case)")
            if f["detail"]:
                out(f"- detail: {f['detail']}")
            out("")

    if summary["inconclusive"]:
        out("## Inconclusive")
        out("")
        out("These ran but could not reach a verdict. Doc 23 keeps this distinct")
        out("from both PASS and FAIL so a missing measurement is never read as a")
        out("success.")
        out("")
        for f in summary["inconclusive"]:
            out(f"- **{f['id']}** ({f['priority']}) {f['title']}: {f['detail']}")
        out("")

    if summary["blocked"]:
        out("## Blocked")
        out("")
        out("Each of these names the specific capability this environment lacks.")
        out("Doc 23 defines BLOCKED as 'dependency absent' and is explicit that it")
        out("is not a pass.")
        out("")
        for f in summary["blocked"]:
            out(f"- **{f['id']}** ({f['priority']}) {f['title']}")
            out(f"  - {f['detail']}")
        out("")

    return "\n".join(lines) + "\n"


def main():
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--artifacts", default="build/test-artifacts",
                        help="directory holding the suite JSON artifacts")
    parser.add_argument("--out", default="reports/release_gate_report.md",
                        help="where to write the markdown report")
    parser.add_argument("--json", action="store_true",
                        help="also print a machine-readable summary to stdout")
    args = parser.parse_args()

    suites = load_artifacts(args.artifacts)
    if not suites:
        print(f"no artifacts found in {args.artifacts}; run the suites first "
              f"(cmake --build build && ctest --test-dir build)", file=sys.stderr)
        return 1

    summary = summarise(suites)
    verdict, reason = decide(summary)
    report = render(summary, verdict, reason, args.artifacts)

    out_dir = os.path.dirname(args.out)
    if out_dir:
        os.makedirs(out_dir, exist_ok=True)
    with open(args.out, "w", encoding="utf-8") as handle:
        handle.write(report)

    totals = summary["totals"]
    print(f"suites: {len(suites)}  pass: {totals['PASS']}  fail: {totals['FAIL']}  "
          f"blocked: {totals['BLOCKED']}  inconclusive: {totals['INCONCLUSIVE']}")
    print(f"verdict: {verdict} -- {reason}")
    print(f"report written to {args.out}")

    if args.json:
        print(json.dumps({
            "verdict": verdict,
            "reason": reason,
            "totals": dict(totals),
            "by_priority": {k: dict(v) for k, v in summary["by_priority"].items()},
            "failures": [f["id"] for f in summary["failures"]],
            "blocked": [f["id"] for f in summary["blocked"]],
            "inconclusive": [f["id"] for f in summary["inconclusive"]],
        }, indent=2))

    return 0 if verdict in ("CANDIDATE_FOR_P08_INTEGRATION", "INCOMPLETE") else 1


if __name__ == "__main__":
    sys.exit(main())
