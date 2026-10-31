#!/usr/bin/env python3
"""One entry point for the whole verification run.

    python scripts/ci.py [--build-dir build] [--skip-build] [--soak-seconds 0]

Stages, in order, stopping at the first that fails:

  1. the scenario suite (groups A..Q, the PB playbooks, the property tests and
     the bounded model check)
  2. the standalone bounded model checker, at a wider bound than the suite uses
  3. every golden trace in tests/fixtures/traces, through order_trace_replay
  4. the release-gate report, which reconciles the design doc's catalog against
     what actually ran
  5. optionally a soak, which reports a resident-memory trend rather than
     asserting one

The sanitizer builds are deliberately separate targets (`make asan`, `make
tsan`): ASan and TSan must not share a binary, as the design doc notes in 20.5.
"""

import argparse
import glob
import os
import platform
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def exe(path):
    return path + ".exe" if platform.system() == "Windows" else path


def run(cmd, cwd=ROOT):
    print("\n$ %s" % " ".join(cmd), flush=True)
    return subprocess.run(cmd, cwd=cwd).returncode


def stage(name):
    print("\n" + "=" * 72)
    print("== %s" % name)
    print("=" * 72, flush=True)


def sample_rss_kb(pid):
    try:
        if platform.system() == "Windows":
            out = subprocess.check_output(
                ["tasklist", "/FI", "PID eq %d" % pid, "/FO", "CSV", "/NH"],
                stderr=subprocess.DEVNULL, text=True)
            parts = out.strip().split('","')
            if len(parts) >= 5:
                return int(parts[4].replace('"', "").replace(",", "").replace(" K", ""))
        else:
            with open("/proc/%d/status" % pid, encoding="utf-8") as handle:
                for line in handle:
                    if line.startswith("VmRSS:"):
                        return int(line.split()[1])
    except Exception:
        pass
    return 0


def soak(binary, seconds):
    """Runs the suite in a loop and reports the peak working set.

    This reports a measurement; it does not assert a ceiling. A real soak gate
    belongs in CI on a pinned host, which this script cannot assume.
    """
    stage("soak for %d second(s)" % seconds)
    deadline = time.time() + seconds
    rounds = 0
    peak_kb = 0
    while time.time() < deadline:
        proc = subprocess.Popen([binary, "--artifact=test-artifacts/soak.json"], cwd=ROOT,
                                stdout=subprocess.DEVNULL)
        while proc.poll() is None:
            peak_kb = max(peak_kb, sample_rss_kb(proc.pid))
            time.sleep(0.25)
        if proc.returncode != 0:
            print("soak round %d failed with exit %d" % (rounds, proc.returncode))
            return 1
        rounds += 1
    print("soak: %d round(s), peak resident set %.1f MiB" % (rounds, peak_kb / 1024.0))
    print("note: a trend, not a gate. The design doc asks for 30 to 60 minutes on pinned "
          "hardware before any memory claim is made.")
    return 0


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--build-dir", default="build")
    parser.add_argument("--skip-build", action="store_true")
    parser.add_argument("--soak-seconds", type=int, default=0)
    parser.add_argument("--jobs", default="8")
    args = parser.parse_args()

    build = os.path.join(ROOT, args.build_dir)
    suite = exe(os.path.join(build, "order_core_tests"))
    model = exe(os.path.join(build, "order_model_check"))
    replay = exe(os.path.join(build, "order_trace_replay"))

    if not args.skip_build:
        stage("build")
        if run(["make", "-j" + args.jobs, "all", "tools"]) != 0:
            print("build failed")
            return 1

    for binary in (suite, model, replay):
        if not os.path.exists(binary):
            print("missing %s -- run without --skip-build" % binary)
            return 1

    stage("scenario suite")
    artifact = "test-artifacts/order_core.json"
    if run([suite, "--artifact=" + artifact]) != 0:
        print("the scenario suite reported failures")
        return 1

    stage("bounded model check, wider bound than the suite")
    if run([model, "--max-depth", "8", "--order-qty", "3", "--max-fill", "2"]) != 0:
        print("the model checker found a violation")
        return 1

    stage("golden traces")
    traces = sorted(glob.glob(os.path.join(ROOT, "tests", "fixtures", "traces", "*.trace")))
    if not traces:
        print("no trace fixtures found")
        return 1
    failures = []
    for path in traces:
        if run([replay, os.path.relpath(path, ROOT), "--quiet"]) != 0:
            failures.append(os.path.basename(path))
    if failures:
        print("\ntrace replay failed for: %s" % ", ".join(failures))
        return 1
    print("\n%d trace(s) replayed cleanly" % len(traces))

    stage("release gate report")
    doc = os.path.join(ROOT, "..", "docs", "Order_State_Machine_System_Design.md")
    gate = run([sys.executable, os.path.join("scripts", "report.py"), artifact, doc])

    if args.soak_seconds > 0 and soak(suite, args.soak_seconds) != 0:
        return 1

    print("\n" + "=" * 72)
    if gate != 0:
        print("== result: verification ran, release gate NOT satisfied")
        return 1
    print("== result: all stages passed")
    print("=" * 72)
    return 0


if __name__ == "__main__":
    sys.exit(main())
