#!/usr/bin/env python3
"""Assemble the what-if matrix report from the suite artifacts.

Reference: design doc 16.1, 18.4 and 19.

The rules this script enforces are the ones the design doc is emphatic about,
and they are the reason the report is generated rather than written by hand:

  * a BLOCKED or NOT_IMPLEMENTED case is NOT a pass. Both are listed separately
    with their stated reason, and neither counts towards a gate (16.1, M-07);
  * a P0 failure fails the run. So does a P0 case that did not run at all;
  * the report states how many matrix rows are covered, rather than implying
    the matrix is complete.

Usage:
    python scripts/report.py [--artifacts DIR] [--csv FILE] [--quiet]

Exit code 0 when every P0 case that ran passed and none is missing, 1 otherwise.
"""

import argparse
import collections
import glob
import json
import os
import sys

# The thirteen groups of design doc 16, plus the playbooks of section 17. The
# defined counts come from the document itself.
GROUPS = [
    ("A", "Connection, FIX acceptor, session identity", 20),
    ("B", "Application parse, validation, reject semantics", 20),
    ("C", "New orders, request identity, idempotency", 20),
    ("D", "Quotes, matching, price, liquidity", 20),
    ("E", "Partial fills, accounting, ExecutionReport", 20),
    ("F", "Cancel and OrderCancelReject", 20),
    ("G", "Replace, chaining, modification", 20),
    ("H", "Races, scheduler, time, interleavings", 20),
    ("I", "FIX resend, application resend, outbound reports", 20),
    ("J", "Durable store, crashes, restart, consistency", 20),
    ("K", "Transport chaos, bad time, peer stalls", 20),
    ("L", "Capacity, security, observability, operations", 20),
    ("M", "Interop, test quality, reproducibility, release", 20),
    ("PB", "Fault-injection playbooks (design doc 17)", 18),
]

# The crash windows W1..W10 are NOT a separate group: design doc 16.11 defines
# rows J-01..J-10 as "kill at W1".."kill at W10", so they are counted under J.
# Several playbooks are likewise exercised under the matrix row they cite
# rather than under a PB id, which is why the PB count below understates them.

VERDICTS = ("PASS", "FAIL", "BLOCKED", "NOT_IMPLEMENTED", "NOT_RUN")


def group_of(case_id):
    """Map a case id onto a matrix group and row.

    Ids carry a suffix when one matrix row needs several tests, for example
    D-19b alongside D-19, or J-08s for the store-level half of J-08. Any
    trailing lowercase letters are stripped so those count as covering the same
    row rather than inflating the coverage number.
    """
    def row_of(text):
        return text.rstrip("abcdefghijklmnopqrstuvwxyz")

    if case_id.startswith("PB-"):
        return "PB", row_of(case_id[3:])
    if len(case_id) > 2 and case_id[1] == "-":
        return case_id[0], row_of(case_id[2:])
    return "?", case_id


def load(artifacts):
    cases = []
    files = sorted(glob.glob(os.path.join(artifacts, "*.json")))
    if not files:
        print("no artifacts found in %s - run the suites first" % artifacts,
              file=sys.stderr)
        return None, []
    for path in files:
        with open(path, "r", encoding="utf-8") as handle:
            try:
                doc = json.load(handle)
            except ValueError as exc:
                print("cannot parse %s: %s" % (path, exc), file=sys.stderr)
                return None, []
        suite = doc.get("suite", os.path.basename(path))
        for case in doc.get("cases", []):
            case["suite"] = suite
            cases.append(case)
    return files, cases


def main():
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--artifacts", default="test-artifacts")
    parser.add_argument("--csv", default=None)
    parser.add_argument("--quiet", action="store_true")
    args = parser.parse_args()

    files, cases = load(args.artifacts)
    if files is None:
        return 2

    by_verdict = collections.Counter(c.get("verdict", "NOT_RUN") for c in cases)
    by_priority = collections.defaultdict(collections.Counter)
    for case in cases:
        by_priority[case.get("priority", "?")][case.get("verdict", "NOT_RUN")] += 1

    rows = collections.defaultdict(set)
    for case in cases:
        group, row = group_of(case.get("id", ""))
        rows[group].add(row)

    print("=" * 78)
    print("Project 02 - LP Simulator: what-if matrix report")
    print("=" * 78)
    print("suites       %d (%s)" % (
        len(files), ", ".join(os.path.basename(f) for f in files)))
    print("test cases   %d" % len(cases))
    print()

    print("%-18s %6s %6s %6s %6s" % ("verdict", "all", "P0", "P1", "P2"))
    for verdict in VERDICTS:
        total = by_verdict.get(verdict, 0)
        if total == 0 and verdict == "NOT_RUN":
            continue
        print("%-18s %6d %6d %6d %6d" % (
            verdict, total,
            by_priority["P0"].get(verdict, 0),
            by_priority["P1"].get(verdict, 0),
            by_priority["P2"].get(verdict, 0)))
    print()

    print("matrix coverage by group")
    print("%-4s %-52s %6s %8s" % ("grp", "area", "rows", "defined"))
    total_rows = 0
    total_defined = 0
    for key, title, defined in GROUPS:
        covered = len(rows.get(key, ()))
        total_rows += covered
        total_defined += defined
        print("%-4s %-52s %6d %8d" % (key, title[:52], covered, defined))
    unknown = rows.get("?", set())
    if unknown:
        print("%-4s %-52s %6d %8s" % ("?", "unrecognised case ids", len(unknown), "-"))
    print("%-4s %-52s %6d %8d" % ("", "total", total_rows, total_defined))
    print()
    print("Coverage counts matrix ROWS exercised by at least one test, keyed by the")
    print("case id. It is not a claim that a row is fully explored, and it")
    print("UNDERSTATES the playbooks: most of section 17 is exercised under the")
    print("matrix row it cites rather than under a PB id (PB-01 under J-04, PB-04")
    print("under C-03, PB-05 under C-04, PB-11 under J-11, PB-13 under I-08,")
    print("PB-14 under L-03 and K-15). The crash windows W1..W10 are rows")
    print("J-01..J-10 by the design doc's own definition. The document defines")
    print("%d scenarios; %d rows have an executable test here." % (total_defined, total_rows))
    print()

    failures = [c for c in cases if c.get("verdict") == "FAIL"]
    if failures:
        print("FAILURES")
        for case in failures:
            print("  %-10s %-4s %-10s %s" % (
                case.get("id"), case.get("priority"), case.get("suite"),
                case.get("title")))
            if case.get("detail"):
                print("             %s" % case["detail"])
        print()

    blocked = [c for c in cases if c.get("verdict") == "BLOCKED"]
    if blocked:
        print("BLOCKED - cannot run in this environment, NOT counted as passing")
        for case in blocked:
            print("  %-10s %-4s %s" % (
                case.get("id"), case.get("priority"), case.get("title")))
            if case.get("detail") and not args.quiet:
                print("             %s" % case["detail"])
        print()

    not_impl = [c for c in cases if c.get("verdict") == "NOT_IMPLEMENTED"]
    if not_impl:
        print("NOT IMPLEMENTED - outside the v1 profile, NOT counted as passing")
        for case in not_impl:
            print("  %-10s %-4s %s" % (
                case.get("id"), case.get("priority"), case.get("title")))
            if case.get("detail") and not args.quiet:
                print("             %s" % case["detail"])
        print()

    # Release gate, per design doc 19: every in-scope P0 must pass.
    p0 = by_priority["P0"]
    p0_fail = p0.get("FAIL", 0)
    p0_not_run = p0.get("NOT_RUN", 0)

    print("=" * 78)
    print("release gate (design doc 19): every in-scope P0 case must PASS")
    print("  P0 passed            %d" % p0.get("PASS", 0))
    print("  P0 failed            %d" % p0_fail)
    print("  P0 not run           %d" % p0_not_run)
    print("  P0 blocked           %d  (environment, see above)" % p0.get("BLOCKED", 0))
    print("  P0 not implemented   %d  (out of v1 scope, see above)"
          % p0.get("NOT_IMPLEMENTED", 0))

    ok = p0_fail == 0 and p0_not_run == 0
    print()
    if ok:
        print("GATE: in-scope P0 PASS")
        print()
        print("What this does NOT establish, stated because the design doc insists")
        print("on it (16.1, 21.2): a finite suite does not prove the system survives")
        print("every interleaving or every correlated fault; the blocked and")
        print("not-implemented rows above are open; and passing a lab gate is")
        print("neither production readiness nor counterparty certification.")
    else:
        print("GATE: FAILED")
    print("=" * 78)

    if args.csv:
        with open(args.csv, "w", encoding="utf-8", newline="") as handle:
            handle.write("id,priority,verdict,suite,seconds,title,detail\n")
            for case in sorted(cases, key=lambda c: c.get("id", "")):
                def esc(value):
                    return '"%s"' % str(value).replace('"', '""')
                handle.write(",".join([
                    esc(case.get("id", "")),
                    esc(case.get("priority", "")),
                    esc(case.get("verdict", "")),
                    esc(case.get("suite", "")),
                    "%.4f" % float(case.get("seconds", 0.0)),
                    esc(case.get("title", "")),
                    esc(case.get("detail", "")),
                ]) + "\n")
        print("csv written to %s" % args.csv)

    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
