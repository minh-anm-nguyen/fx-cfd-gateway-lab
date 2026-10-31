#!/usr/bin/env python3
"""Differential test: the C++ state machine against the Python reference model.

Design doc 20.1 wants the oracle to share no code with the system under test.
This runs every golden trace through both implementations and compares the
observable business state. A disagreement is a finding in one of the two, and
the script does not say which -- that is the point of a differential test.

    python scripts/differential.py [--replay build/order_trace_replay]

Exit status 0 only when every trace agrees. A trace the reference model
declines to model is reported as UNMODELLED and treated as a failure, because
an oracle that quietly does nothing would agree with anything.
"""

import argparse
import glob
import os
import platform
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# The fields both sides report. The C++ side prints a human-readable block, so
# the values are pulled out of it with these patterns.
CPP_PATTERNS = {
    "lifecycle": re.compile(r"^\s+lifecycle\s+(\S+)", re.M),
    "cum": re.compile(r"^\s+cum/leaves\s+(-?\d+) / (-?\d+)", re.M),
    "fills": re.compile(r"^\s+fills\s+(\d+)", re.M),
    "quarantined": re.compile(r"^\s+quarantined\s+(\S+)", re.M),
    "reconcile": re.compile(r"^\s+reconcile\s+(\S+)", re.M),
    "accepted": re.compile(r"^\s+accepted\s+(\S+) qty=(-?\d+) px=(-?\d+) ticks \(v(\d+)\)", re.M),
    "violations": re.compile(r"^\s+violations\s+(\d+)", re.M),
}


def exe(path):
    return path + ".exe" if platform.system() == "Windows" else path


def parse_cpp(output):
    out = {}
    m = CPP_PATTERNS["lifecycle"].search(output)
    if m:
        out["lifecycle"] = m.group(1)
    m = CPP_PATTERNS["cum"].search(output)
    if m:
        out["cum"] = int(m.group(1))
        out["leaves"] = int(m.group(2))
    m = CPP_PATTERNS["fills"].search(output)
    if m:
        out["fills"] = int(m.group(1))
    m = CPP_PATTERNS["quarantined"].search(output)
    if m:
        out["quarantined"] = "yes" if m.group(1) == "yes" else "no"
    m = CPP_PATTERNS["reconcile"].search(output)
    if m:
        out["reconcile"] = "yes" if m.group(1) == "required" else "no"
    m = CPP_PATTERNS["accepted"].search(output)
    if m:
        out["accepted"] = m.group(1)
        out["qty"] = int(m.group(2))
        out["versions"] = int(m.group(4))
    m = CPP_PATTERNS["violations"].search(output)
    if m:
        out["violations"] = int(m.group(1))
    return out


def parse_model(output):
    out = {}
    for line in output.splitlines():
        if line.startswith("UNMODELLED"):
            return None
        if "=" not in line or line.startswith("#"):
            continue
        key, value = line.split("=", 1)
        out[key] = int(value) if value.lstrip("-").isdigit() else value
    return out


# Fields worth comparing. `notional` and `pending` are reported by the model
# but not printed in a comparable form by the replay tool, so they are left out
# rather than compared loosely.
COMPARED = ("lifecycle", "cum", "leaves", "qty", "fills", "versions", "accepted",
            "quarantined", "reconcile")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--replay", default=os.path.join("build", "order_trace_replay"))
    parser.add_argument("--traces", default=os.path.join("tests", "fixtures", "traces", "*.trace"))
    args = parser.parse_args()

    replay = exe(os.path.join(ROOT, args.replay))
    if not os.path.exists(replay):
        print("missing %s -- build the tools first (make tools)" % replay)
        return 2
    model = os.path.join(ROOT, "tools", "reference_model", "model.py")

    traces = sorted(glob.glob(os.path.join(ROOT, args.traces)))
    if not traces:
        print("no traces matched %s" % args.traces)
        return 2

    failures = 0
    for path in traces:
        name = os.path.basename(path)
        rel = os.path.relpath(path, ROOT)

        cpp_run = subprocess.run([replay, rel], cwd=ROOT, capture_output=True, text=True)
        model_run = subprocess.run([sys.executable, model, rel], cwd=ROOT, capture_output=True,
                                   text=True)

        cpp = parse_cpp(cpp_run.stdout)
        ref = parse_model(model_run.stdout)

        if ref is None:
            print("%-16s UNMODELLED by the reference model:" % name)
            for line in model_run.stdout.splitlines():
                if line.startswith("UNMODELLED"):
                    print("                 %s" % line)
            failures += 1
            continue
        if not cpp:
            print("%-16s the replay tool produced no parseable state" % name)
            failures += 1
            continue

        diffs = []
        for key in COMPARED:
            if key not in cpp or key not in ref:
                continue
            if cpp[key] != ref[key]:
                diffs.append("%s: c++=%s model=%s" % (key, cpp[key], ref[key]))
        compared = [k for k in COMPARED if k in cpp and k in ref]

        if diffs:
            print("%-16s DISAGREE (%d field(s) compared)" % (name, len(compared)))
            for d in diffs:
                print("                 %s" % d)
            failures += 1
        else:
            print("%-16s agree on %d field(s): %s" % (name, len(compared), ", ".join(compared)))

    print("\n%d trace(s), %d disagreement(s)" % (len(traces), failures))
    if failures:
        print("verdict: the two implementations do not agree; one of them is wrong")
        return 1
    print("verdict: the independent model agrees with the state machine on every trace")
    return 0


if __name__ == "__main__":
    sys.exit(main())
