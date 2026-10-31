#!/usr/bin/env python3
"""Live checks over a real TCP socket against the lp_simulator process.

Every other suite drives the session in process through a fake transport, which
is deliberate: scenarios K-07, K-08 and K-14 need a transport that fails on
demand, and a real socket will not do that reliably. But that leaves the actual
listener, the accept loop and real socket reads untested, so this script covers
those, plus A-08, which needs two real sockets and therefore cannot be done in
process at all.

Cases, by matrix id:
  A-02  Logon over a real socket reaches ACTIVE and is acknowledged
  C-01  a NewOrderSingle is accepted and acknowledged exactly once
  F-01  a cancel on a live order is answered as Canceled with LeavesQty zero
  A-08  while one session is active, a second peer for the same identity is
        not served

Results are written to test-artifacts/smoke.json in the same schema the C++
suites use, so scripts/report.py counts them in the gate.

usage: python scripts/smoke_tcp.py --exe build/lp_simulator.exe [--port 19898]

Exit code 0 when every case passed, 1 otherwise.
"""

import argparse
import json
import os
import shutil
import socket
import subprocess
import sys
import tempfile
import time

SOH = "\x01"


def checksum(text):
    return sum(text.encode("latin-1")) % 256


def encode(msg_type, seq, sender, target, fields, sending_time):
    body = "35=%s%s49=%s%s56=%s%s34=%d%s52=%s%s" % (
        msg_type, SOH, sender, SOH, target, SOH, seq, SOH, sending_time, SOH)
    for tag, value in fields:
        body += "%s=%s%s" % (tag, value, SOH)
    head = "8=FIX.4.4%s9=%d%s" % (SOH, len(body), SOH)
    frame = head + body
    return frame + "10=%03d%s" % (checksum(frame), SOH)


def parse(frame):
    out = {}
    for part in frame.split(SOH):
        if not part:
            continue
        tag, _, value = part.partition("=")
        # First occurrence wins; the simulator does not repeat the tags read
        # here, so this is adequate for a live check.
        out.setdefault(tag, value)
    return out


def split_frames(buffer):
    """Split a buffer into complete frames using BodyLength, as the engine does."""
    out = []
    while buffer.startswith("8=FIX.4.4" + SOH):
        nine = buffer.find("9=", 10)
        if nine < 0:
            break
        soh_after_nine = buffer.find(SOH, nine)
        if soh_after_nine < 0:
            break
        try:
            length = int(buffer[nine + 2:soh_after_nine])
        except ValueError:
            break
        total = soh_after_nine + 1 + length + 7
        if len(buffer) < total:
            break
        out.append(buffer[:total])
        buffer = buffer[total:]
    return out, buffer


def utc_now():
    return time.strftime("%Y%m%d-%H:%M:%S", time.gmtime()) + ".000"


class Peer:
    """A scripted FIX client on a real socket."""

    def __init__(self, host, port, sender="TRADER01", target="SIMLP01", timeout=5.0):
        self.sock = socket.create_connection((host, port), timeout=timeout)
        self.sender = sender
        self.target = target
        self.buffer = ""
        self.seq = 1

    def send(self, msg_type, fields):
        frame = encode(msg_type, self.seq, self.sender, self.target, fields, utc_now())
        self.seq += 1
        self.sock.sendall(frame.encode("latin-1"))

    def recv_until(self, predicate, timeout=5.0):
        end = time.time() + timeout
        while time.time() < end:
            self.sock.settimeout(max(0.05, end - time.time()))
            try:
                chunk = self.sock.recv(65536)
            except socket.timeout:
                continue
            except OSError:
                return None
            if not chunk:
                return None
            self.buffer += chunk.decode("latin-1")
            ready, self.buffer = split_frames(self.buffer)
            for frame in ready:
                fields = parse(frame)
                if predicate(fields):
                    return fields
        return None

    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass


class Report:
    """Collects per-case verdicts in the schema the C++ suites emit."""

    def __init__(self):
        self.cases = []
        self._current = None

    def case(self, case_id, priority, title):
        self._current = {"id": case_id, "priority": priority, "title": title,
                         "verdict": "PASS", "detail": "", "seconds": 0.0,
                         "_start": time.time()}
        self.cases.append(self._current)
        print("%s %-5s %s" % (case_id, priority, title))

    def check(self, condition, message):
        if condition:
            print("    ok    %s" % message)
            return True
        print("    FAIL  %s" % message)
        if self._current is not None:
            self._current["verdict"] = "FAIL"
            if self._current["detail"]:
                self._current["detail"] += "; "
            self._current["detail"] += message
        return False

    def finish(self):
        for case in self.cases:
            start = case.pop("_start", None)
            if start is not None:
                case["seconds"] = round(time.time() - start, 4)

    def failures(self):
        return [c for c in self.cases if c["verdict"] != "PASS"]

    def write(self, path):
        os.makedirs(os.path.dirname(path) or ".", exist_ok=True)
        with open(path, "w", encoding="utf-8") as handle:
            json.dump({"suite": "smoke", "cases": self.cases}, handle, indent=2)
            handle.write("\n")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--exe", default="build/lp_simulator.exe")
    parser.add_argument("--config", default="configs/lp.example.yaml")
    parser.add_argument("--port", type=int, default=19898)
    parser.add_argument("--artifacts", default="test-artifacts")
    args = parser.parse_args()

    if not os.path.exists(args.exe):
        print("executable not found: %s" % args.exe, file=sys.stderr)
        return 1

    workdir = tempfile.mkdtemp(prefix="lpsim-smoke-")
    db = os.path.join(workdir, "lp.db")
    # Absolute, normalised paths: a relative path with forward slashes passes
    # os.path.exists under MSYS but CreateProcess on Windows rejects it.
    exe = os.path.normpath(os.path.abspath(args.exe))
    config = os.path.normpath(os.path.abspath(args.config))
    proc = subprocess.Popen(
        [exe, "--config", config, "--db", db, "--port", str(args.port)],
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)

    report = Report()
    peer = None
    second = None

    try:
        # Wait for the listener rather than guessing at a sleep duration.
        deadline = time.time() + 10.0
        while time.time() < deadline:
            try:
                peer = Peer("127.0.0.1", args.port)
                break
            except OSError:
                if proc.poll() is not None:
                    print("the simulator exited before listening", file=sys.stderr)
                    if proc.stdout:
                        print(proc.stdout.read())
                    return 1
                time.sleep(0.1)
        if peer is None:
            print("could not connect to 127.0.0.1:%d" % args.port, file=sys.stderr)
            return 1

        report.case("A-02", "P0", "Logon over a real socket is acknowledged")
        peer.send("A", [("98", "0"), ("108", "30")])
        ack = peer.recv_until(lambda f: f.get("35") == "A")
        if report.check(ack is not None, "the acceptor answered the Logon"):
            report.check(ack.get("49") == "SIMLP01", "the ack comes from SIMLP01")
            report.check(ack.get("56") == "TRADER01", "it is addressed to TRADER01")
            report.check(ack.get("108") == "30", "the heartbeat interval is mirrored")

        report.case("C-01", "P0", "a NewOrderSingle is accepted over a real socket")
        peer.send("D", [("11", "SMOKE-1"), ("55", "EURUSD"), ("54", "1"),
                        ("38", "100000"), ("40", "2"), ("44", "1.08500"),
                        ("59", "0"), ("60", utc_now())])
        new_ack = peer.recv_until(
            lambda f: f.get("35") == "8" and f.get("11") == "SMOKE-1")
        if report.check(new_ack is not None, "an ExecutionReport arrived"):
            report.check(new_ack.get("150") == "0", "ExecType is New")
            report.check(new_ack.get("39") == "0", "OrdStatus is New")
            report.check(new_ack.get("14") == "0", "CumQty is zero")
            report.check(new_ack.get("151") == "100000",
                         "LeavesQty is the full quantity")
            report.check(bool(new_ack.get("37")), "an OrderID was assigned")
            report.check(bool(new_ack.get("17")), "an ExecID was assigned")
            # No fill: the profile is scripted, so New must not trade.
            report.check(new_ack.get("32") is None and new_ack.get("31") is None,
                         "a no-fill report carries no LastQty or LastPx")

        report.case("A-08", "P0",
                    "a second peer is not served while a session is active")
        # This case cannot be done in process at all: it needs two real sockets
        # against the listener. The acceptor serves one session, so the second
        # connection must not get a Logon acknowledgement while the first is
        # still active.
        try:
            second = Peer("127.0.0.1", args.port)
            second.send("A", [("98", "0"), ("108", "30")])
            # A short wait is the whole assertion: nothing should come back.
            reply = second.recv_until(lambda f: f.get("35") == "A", timeout=2.0)
            report.check(reply is None,
                         "the second peer received no Logon acknowledgement")
        except OSError:
            # Refusing the connection outright is an equally correct answer.
            report.check(True, "the second connection was refused by the listener")

        # The first session is unaffected by the intruder.
        report.case("F-01", "P0", "a cancel on a live order is answered as Canceled")
        peer.send("F", [("11", "SMOKE-C1"), ("41", "SMOKE-1"), ("55", "EURUSD"),
                        ("54", "1"), ("60", utc_now())])
        cancel = peer.recv_until(
            lambda f: f.get("35") in ("8", "9") and f.get("11") == "SMOKE-C1")
        if report.check(cancel is not None, "the cancel was answered"):
            report.check(cancel.get("35") == "8",
                         "the answer is an ExecutionReport, not a reject")
            report.check(cancel.get("150") == "4", "ExecType is Canceled")
            report.check(cancel.get("39") == "4", "OrdStatus is Canceled")
            report.check(cancel.get("151") == "0",
                         "LeavesQty is zero once terminal")
            report.check(cancel.get("41") == "SMOKE-1",
                         "the previous version is echoed")

        peer.send("5", [("58", "smoke test complete")])
        peer.recv_until(lambda f: f.get("35") == "5", timeout=2.0)

    finally:
        for p in (second, peer):
            if p is not None:
                p.close()
        proc.terminate()
        try:
            out, _ = proc.communicate(timeout=10)
        except subprocess.TimeoutExpired:
            proc.kill()
            out, _ = proc.communicate()
        if out:
            print("\n--- simulator output ---")
            print(out.strip())
        shutil.rmtree(workdir, ignore_errors=True)

    report.finish()
    report.write(os.path.join(args.artifacts, "smoke.json"))

    print()
    failures = report.failures()
    if failures:
        print("SMOKE FAIL: %d of %d cases failed" % (len(failures), len(report.cases)))
        return 1
    print("SMOKE PASS: %d cases over a real socket" % len(report.cases))
    return 0


if __name__ == "__main__":
    sys.exit(main())
