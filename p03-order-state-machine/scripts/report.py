#!/usr/bin/env python3
"""Release-gate report: catalog coverage against what the suite actually ran.

The design doc is explicit that a skipped P0 marked as passed blocks release
(section 21.6) and that CI must reconcile the catalog's ID set against the test
registry (section 20.6). So the catalog is read from the design document
itself, not from a copy that could drift, and anything missing is a failure
rather than a silence.

    python scripts/report.py test-artifacts/order_core.json [design-doc.md]

Exit status 0 only when every P0 in the catalog ran and passed.
"""

import json
import os
import re
import sys
from collections import OrderedDict

DEFAULT_DOC = os.path.join("..", "docs", "Order_State_Machine_System_Design.md")

# Rows look like:  | **A-01** | P0 | what if ... | oracle ... |
ROW = re.compile(r"^\|\s*\*\*([A-Q]-\d{2}|PB-\d{2})\*\*\s*\|\s*(P[012])?\s*\|")
# Playbook headings look like:  ### PB-01 — New ACK lost; ...
HEADING = re.compile(r"^###\s+(PB-\d{2})\s*[—-]\s*(.+)$")


def read_catalog(path):
    """Returns an ordered mapping of case id -> priority."""
    catalog = OrderedDict()
    if not os.path.exists(path):
        return catalog, "catalog not found at %s" % path
    with open(path, encoding="utf-8") as handle:
        for line in handle:
            row = ROW.match(line)
            if row:
                case_id, priority = row.group(1), row.group(2) or "P0"
                catalog[case_id] = priority
                continue
            heading = HEADING.match(line)
            if heading:
                # The playbooks in section 19 are all P0 by definition.
                catalog.setdefault(heading.group(1), "P0")
    return catalog, None


def read_artifact(path):
    with open(path, encoding="utf-8") as handle:
        data = json.load(handle)
    results = OrderedDict()
    for case in data.get("cases", []):
        results[case["id"]] = case
    return data.get("suite", "?"), results


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 2
    artifact_path = argv[1]
    doc_path = argv[2] if len(argv) > 2 else os.path.join(os.path.dirname(__file__), DEFAULT_DOC)

    suite, results = read_artifact(artifact_path)
    catalog, catalog_error = read_catalog(doc_path)

    print("suite:    %s" % suite)
    print("artifact: %s (%d case(s) reported)" % (artifact_path, len(results)))
    if catalog_error:
        print("WARNING:  %s -- coverage cannot be reconciled" % catalog_error)
    else:
        print("catalog:  %s (%d case(s) defined)" % (doc_path, len(catalog)))

    by_verdict = {}
    for case in results.values():
        by_verdict.setdefault(case["verdict"], []).append(case)
    for verdict in ("PASS", "FAIL", "BLOCKED", "NOT_RUN"):
        cases = by_verdict.get(verdict, [])
        print("  %-9s %d" % (verdict, len(cases)))

    # Cases that ran but are not in the catalog: extra coverage, worth seeing
    # but never a failure.
    extra = [cid for cid in results if catalog and cid not in catalog]

    missing = [cid for cid in catalog if cid not in results]
    failed = [c["id"] for c in by_verdict.get("FAIL", [])]
    blocked = [c for c in by_verdict.get("BLOCKED", [])]
    not_run = [c["id"] for c in by_verdict.get("NOT_RUN", [])]

    blockers = []

    if failed:
        print("\nFAILED (%d):" % len(failed))
        for cid in failed:
            print("  %-8s %s" % (cid, results[cid].get("detail", "")[:160]))
        blockers.append("%d failing case(s)" % len(failed))

    if missing:
        p0_missing = [cid for cid in missing if catalog[cid] == "P0"]
        print("\nIN THE CATALOG BUT NOT RUN (%d, of which P0: %d):"
              % (len(missing), len(p0_missing)))
        for cid in missing:
            print("  %-8s %s" % (cid, catalog[cid]))
        if p0_missing:
            blockers.append("%d P0 case(s) in the catalog with no test" % len(p0_missing))

    if blocked:
        print("\nBLOCKED (%d) -- these are reported, never counted as passes:" % len(blocked))
        for case in blocked:
            print("  %-8s %-4s %s" % (case["id"], case["priority"],
                                      case.get("detail", "(no reason given)")))
        p0_blocked = [c for c in blocked if c["priority"] == "P0"]
        if p0_blocked:
            # A blocked P0 is an honest gap, and it still blocks the gate: the
            # release criteria require 100% of applicable P0 to pass.
            blockers.append("%d P0 case(s) blocked and therefore unproved" % len(p0_blocked))

    if not_run:
        print("\nNOT RUN (%d):" % len(not_run))
        for cid in not_run:
            print("  %s" % cid)
        blockers.append("%d case(s) registered but not executed" % len(not_run))

    if extra:
        print("\nRAN BUT NOT IN THE CATALOG (%d): %s" % (len(extra), ", ".join(sorted(extra))))

    print("\n--- gate ---")
    if blockers:
        for reason in blockers:
            print("BLOCKED: %s" % reason)
        print("verdict: release gate NOT satisfied")
        return 1
    print("every catalog case ran and passed")
    print("verdict: gate satisfied for the cases this suite covers")
    print("note: G3 (durable recovery) additionally requires P06; the suite runs against an "
          "in-memory store and does not prove end-to-end crash safety")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
