#!/usr/bin/env python3
"""Aggregate suite artifacts into the P01 test report.

Every suite writes one JSON file with a verdict per what-if matrix scenario.
This script turns those files into a single Markdown report and exits non-zero
when a P0 scenario failed, which is the release condition in design doc section
18: Project 01 is complete only when 100 percent of P0 passes.

BLOCKED is a real verdict, not a pass. It means the scenario could not run here
and the reason is recorded, so a reader can judge the gap instead of guessing.
"""

import argparse
import glob
import json
import os
import platform
import subprocess
import sys
from collections import Counter
from datetime import datetime, timezone

# Precedence when the same scenario appears in more than one build. FAIL always
# dominates. Otherwise a build that actually RAN the scenario supersedes one that
# could not: BLOCKED means "not runnable in that build", not "not runnable".
PRECEDENCE = {"FAIL": 0, "PASS": 1, "BLOCKED": 2, "NOT_RUN": 3}


# Release gates from design doc section 18. Each gate owns a set of scenario id
# prefixes; a gate passes when nothing in it failed. Blocked scenarios are
# counted separately and listed, because a gate that passes with blocked
# evidence is not the same as a gate that passes outright.
GATES = [
    ("A", "Parser and wire safety",
     ("B-", "GF-", "SAN-", "J-04", "J-05", "J-10")),
    ("B", "Session protocol correctness",
     ("C-", "D-", "E-0", "E-1", "E-2", "F-", "H-", "PB-05", "PB-06", "PB-07", "PB-11")),
    ("C", "Crash consistency",
     ("G-", "PB-01", "PB-02", "PB-03", "PB-04", "PB-12", "PROP-03", "I-02s", "K-10")),
    ("D", "Security and bounded resources",
     ("J-", "I-0", "I-1", "K-11", "K-12", "PB-11")),
    ("E", "System demonstration",
     ("E2E-", "L-", "PROP-01", "PROP-02", "PROP-04", "K-0", "K-14")),
]


def gate_rows(cases):
    rows = []
    for name, title, prefixes in GATES:
        matched = [c for c in cases
                   if any(c.get("id", "").startswith(p) for p in prefixes)]
        counts = Counter(c.get("verdict", "?") for c in matched)
        failed = counts["FAIL"]
        status = "FAIL" if failed else "PASS"
        rows.append((name, title, status, counts["PASS"], failed, counts["BLOCKED"],
                     len(matched)))
    return rows


def load(artifact_dir):
    cases = []
    for path in sorted(glob.glob(os.path.join(artifact_dir, "*.json"))):
        with open(path, encoding="utf-8") as handle:
            try:
                data = json.load(handle)
            except json.JSONDecodeError as exc:
                print(f"warning: {path} is not valid JSON: {exc}", file=sys.stderr)
                continue
        suite = data.get("suite", os.path.basename(path))
        for case in data.get("cases", []):
            case["suite"] = suite
            cases.append(case)
    return cases


def sort_key(case):
    ident = case.get("id", "")
    head, _, tail = ident.partition("-")
    try:
        number = int("".join(ch for ch in tail if ch.isdigit()) or 0)
    except ValueError:
        number = 0
    return (head, number, tail)


def git_describe():
    try:
        out = subprocess.run(["git", "rev-parse", "--short", "HEAD"],
                             capture_output=True, text=True, timeout=5)
        if out.returncode == 0:
            return out.stdout.strip()
    except (OSError, subprocess.SubprocessError):
        pass
    return "not a git repository"


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--artifacts", action="append", default=None,
                        help="artifact directory; repeat to merge several builds")
    parser.add_argument("--out", default=os.path.join("docs", "P01_TEST_REPORT.md"))
    parser.add_argument("--priority", default=None, help="only report this priority")
    args = parser.parse_args()

    artifact_dirs = args.artifacts or [os.path.join("build", "test-artifacts")]
    merged = {}
    for directory in artifact_dirs:
        for case in load(directory):
            case["build"] = directory
            key = case.get("id", "")
            previous = merged.get(key)
            if previous is None or PRECEDENCE[case.get("verdict", "NOT_RUN")] <                     PRECEDENCE[previous.get("verdict", "NOT_RUN")]:
                merged[key] = case
    cases = list(merged.values())
    if not cases:
        print(f"no artifacts found in {artifact_dirs}; run ctest first", file=sys.stderr)
        return 2

    if args.priority:
        cases = [c for c in cases if c.get("priority") == args.priority]
    cases.sort(key=sort_key)

    by_priority = {}
    for case in cases:
        by_priority.setdefault(case.get("priority", "?"), Counter())[case.get("verdict", "?")] += 1
    totals = Counter(c.get("verdict", "?") for c in cases)

    failures = [c for c in cases if c.get("verdict") == "FAIL"]
    blocked = [c for c in cases if c.get("verdict") == "BLOCKED"]
    p0_failures = [c for c in failures if c.get("priority") == "P0"]

    lines = []
    lines.append("# Project 01 test report")
    lines.append("")
    lines.append("Generated by `scripts/report.py` from the per-suite artifacts in")
    lines.append("`" + "`, `".join(artifact_dirs) + "`. Each row is one scenario from the "
                 "what-if matrix in")
    lines.append("`FIX_Session_Engine_System_Design.md` section 15, or one of the playbooks in")
    lines.append("section 16.")
    lines.append("")
    lines.append("| field | value |")
    lines.append("|---|---|")
    lines.append(f"| generated (UTC) | {datetime.now(timezone.utc):%Y-%m-%d %H:%M:%S} |")
    lines.append(f"| host | {platform.system()} {platform.release()} ({platform.machine()}) |")
    lines.append(f"| python | {platform.python_version()} |")
    lines.append(f"| revision | {git_describe()} |")
    lines.append(f"| artifact sources | {', '.join(artifact_dirs)} |")
    lines.append("")

    lines.append("## Summary")
    lines.append("")
    lines.append("| priority | PASS | FAIL | BLOCKED | NOT_RUN | total |")
    lines.append("|---|---:|---:|---:|---:|---:|")
    for priority in sorted(by_priority):
        counts = by_priority[priority]
        total = sum(counts.values())
        lines.append(f"| {priority} | {counts['PASS']} | {counts['FAIL']} | "
                     f"{counts['BLOCKED']} | {counts['NOT_RUN']} | {total} |")
    lines.append(f"| **all** | **{totals['PASS']}** | **{totals['FAIL']}** | "
                 f"**{totals['BLOCKED']}** | **{totals['NOT_RUN']}** | **{len(cases)}** |")
    lines.append("")
    verdict = "PASS" if not p0_failures else "FAIL"
    lines.append(f"**Release condition (100 percent of P0 passing): {verdict}**")
    if p0_failures:
        lines.append("")
        for case in p0_failures:
            lines.append(f"- {case['id']}: {case.get('detail', 'no detail')}")
    lines.append("")

    lines.append("## Release gates")
    lines.append("")
    lines.append("From design doc section 18. A gate passes when nothing assigned to it")
    lines.append("failed. Blocked evidence is counted separately on purpose: a gate carried by")
    lines.append("blocked scenarios is weaker than one carried by executed ones, and the reader")
    lines.append("should be able to see which is which.")
    lines.append("")
    lines.append("| gate | scope | status | PASS | FAIL | BLOCKED | scenarios |")
    lines.append("|---|---|---|---:|---:|---:|---:|")
    for name, title, status, passed, failed, blocked_count, total in gate_rows(cases):
        lines.append(f"| {name} | {title} | **{status}** | {passed} | {failed} "
                     f"| {blocked_count} | {total} |")
    lines.append("")

    if blocked:
        lines.append("## Blocked scenarios and why")
        lines.append("")
        lines.append("These did not run here. The reason is part of the evidence: a blocked")
        lines.append("scenario is an admitted gap, never a silent pass.")
        lines.append("")
        lines.append("| id | priority | scenario | reason |")
        lines.append("|---|---|---|---|")
        for case in blocked:
            lines.append(f"| {case['id']} | {case.get('priority','?')} | {case.get('title','')} "
                         f"| {case.get('detail','')} |")
        lines.append("")

    lines.append("## Every scenario")
    lines.append("")
    lines.append("| id | priority | verdict | suite | scenario |")
    lines.append("|---|---|---|---|---|")
    for case in cases:
        lines.append(f"| {case.get('id','')} | {case.get('priority','')} "
                     f"| {case.get('verdict','')} | {case.get('suite','')} "
                     f"| {case.get('title','')} |")
    lines.append("")

    os.makedirs(os.path.dirname(args.out) or ".", exist_ok=True)
    with open(args.out, "w", encoding="utf-8") as handle:
        handle.write("\n".join(lines) + "\n")

    print(f"{len(cases)} scenarios: {totals['PASS']} pass, {totals['FAIL']} fail, "
          f"{totals['BLOCKED']} blocked, {totals['NOT_RUN']} not run")
    print(f"report written to {args.out}")
    if p0_failures:
        print(f"RELEASE BLOCKED: {len(p0_failures)} P0 scenarios failed", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
