#!/usr/bin/env python3
"""An independent reference model of the order state machine.

Design doc 20.1 asks for three layers of oracle and warns that reusing the
system's own reducer makes the oracle worthless. This is layer two: written
in a different language, from the FIX 4.4 semantics and the pinned Rules of
Engagement in the design document, by hand -- not transliterated from the C++.

It is deliberately naive. It keeps no cached totals, recomputes CumQty from its
fill list every time it is asked, and uses integer arithmetic throughout. Where
it does not know what to do it says so, so it cannot agree with the C++ by
accident of silence.

    python tools/reference_model/model.py tests/fixtures/traces/J-01.trace

Prints one `key=value` line per observable, which scripts/differential.py
compares against the C++ replay tool.
"""

import sys
from fractions import Fraction

TICK = Fraction(1, 100000)      # EURUSD, the only symbol the fixtures use
QTY_STEP = 1000


def to_ticks(text):
    """Exact decimal text to an integer tick count, or None if inadmissible."""
    if not text:
        return None
    body = text[1:] if text[0] in "+-" else text
    negative = text[0] == "-"
    if not body or body.count(".") > 1:
        return None
    for ch in body.replace(".", ""):
        if ch not in "0123456789":
            return None
    if "." in body:
        whole, frac = body.split(".")
        if not whole or not frac:
            return None
    else:
        whole, frac = body, ""
    value = Fraction(int(whole + frac) if (whole + frac) else 0, 10 ** len(frac))
    if negative:
        value = -value
    quotient = value / TICK
    if quotient.denominator != 1:
        return None                      # off tick: refused, never rounded
    return int(quotient)


def to_units(text):
    if not text:
        return None
    try:
        if "." in text:
            return None
        return int(text)
    except ValueError:
        return None


class Order:
    """One order chain, as this model understands it."""

    def __init__(self):
        self.exists = False
        self.base = "preparing"          # preparing | sent_unknown | working | done
        self.done_reason = None
        self.versions = []               # [{"cl": str, "qty": int, "px": int}]
        self.fills = []                  # [{"exec": str, "qty": int, "px": int, "cl": str}]
        self.seen = {}                   # exec id -> tuple of business fields
        self.pending = None              # {"kind", "cl", "orig", "qty", "px", "unknown"}
        self.peer_pending = None         # "cancel" | "replace" | None
        self.quarantined = False
        self.reconcile = False
        self.unknown_outcome = False     # the order's own disposition
        self.unsupported = []            # anything this model declined to model

    # --- derived -----------------------------------------------------------
    @property
    def cum(self):
        return sum(f["qty"] for f in self.fills)

    @property
    def accepted(self):
        return self.versions[-1] if self.versions else None

    @property
    def qty(self):
        return self.accepted["qty"] if self.accepted else 0

    @property
    def leaves(self):
        if self.base == "done":
            return 0
        if not self.accepted:
            return 0
        return max(0, self.qty - self.cum)

    @property
    def lifecycle(self):
        if self.quarantined:
            return "QUARANTINED"
        if self.base == "done":
            return {"filled": "FILLED", "canceled": "CANCELED", "expired": "EXPIRED",
                    "rejected": "REJECTED", "done_for_day": "DONE_FOR_DAY"}[self.done_reason]
        if self.reconcile or self.unknown_outcome:
            return "RECONCILIATION_REQUIRED"
        if self.peer_pending == "cancel" or (self.pending and self.pending["kind"] == "cancel"):
            return "PENDING_CANCEL"
        if self.peer_pending == "replace" or (self.pending and self.pending["kind"] == "replace"):
            return "PENDING_REPLACE"
        if self.base == "preparing":
            return "LOCAL_PREPARING"
        if self.base == "sent_unknown":
            return "NEW_SENT_UNKNOWN"
        return "PART_FILLED" if self.cum > 0 else "WORKING"

    def quarantine(self, why):
        self.quarantined = True
        self.reconcile = True
        self.unsupported.append(why)


def fingerprint(fields):
    return tuple(fields)


def apply(order, verb, args, kv):
    """Applies one trace line. Returns a short decision string."""

    def kvget(key, default=""):
        return kv.get(key, default)

    if verb == "policy":
        return "noop"

    if verb == "new":
        if order.exists:
            return "reject:collision"
        qty = to_units(kvget("qty"))
        px = to_ticks(kvget("px"))
        if qty is None or qty <= 0 or qty % QTY_STEP:
            return "reject:qty"
        if px is None or px <= 0:
            return "reject:px"
        order.exists = True
        order.base = "preparing"
        order.pending = {"kind": "new", "cl": args[0], "orig": "", "qty": qty, "px": px,
                         "unknown": False}
        return "applied"

    if verb == "dispatch":
        if not order.pending:
            return "reject:no-request"
        if order.pending["kind"] == "new" and order.base == "preparing":
            order.base = "sent_unknown"
        return "applied"

    if verb == "timeout":
        if not order.pending:
            return "reject:no-request"
        order.pending["unknown"] = True
        order.reconcile = True
        if order.pending["kind"] == "new":
            order.unknown_outcome = True
        return "applied:unknown"

    if verb == "disconnect":
        if order.pending:
            order.pending["unknown"] = True
            order.reconcile = True
        return "applied"

    if verb == "reconcile":
        order.reconcile = True
        return "applied"

    if verb in ("resolve", "override"):
        if "evidence" not in kv and not any(a == "evidence" for a in args):
            return "reject:not-authorized"
        if verb == "override" and "dual" not in kv and not any(a == "dual" for a in args):
            return "reject:not-authorized"
        status = kvget("status")
        if status == "CANCELED":
            order.base, order.done_reason = "done", "canceled"
            order.pending, order.peer_pending = None, None
        elif status == "REJECTED":
            if order.fills:
                order.quarantine("resolution claims rejected with fills")
                return "quarantine"
            order.base, order.done_reason = "done", "rejected"
            order.pending = None
        elif status == "WORKING":
            order.base = "working"
            order.pending, order.peer_pending = None, None
        elif status == "FILLED":
            if order.cum != order.qty:
                order.quarantine("resolution claims filled without the ledger")
                return "quarantine"
            order.base, order.done_reason = "done", "filled"
        else:
            return "reject:unknown-status"
        order.reconcile = False
        order.unknown_outcome = False
        order.quarantined = False
        return "applied"

    if verb == "ack":
        cl = args[0]
        if order.versions:
            return "duplicate:late"
        if not order.pending or order.pending["kind"] != "new":
            order.quarantine("ack with no New outstanding")
            return "quarantine"
        if cl != order.pending["cl"]:
            order.quarantine("ack names a different request")
            return "quarantine"
        order.versions.append({"cl": cl, "qty": order.pending["qty"], "px": order.pending["px"]})
        order.base = "working"
        order.pending = None
        order.unknown_outcome = False
        order.reconcile = False
        return "applied"

    if verb == "fill":
        exec_id = args[0]
        cl = kvget("cl")
        qty = to_units(kvget("qty"))
        px = to_ticks(kvget("px"))
        shape = fingerprint([exec_id, cl, kvget("qty"), kvget("px"), kvget("cum"),
                             kvget("leaves")])
        if exec_id in order.seen:
            if order.seen[exec_id] == shape:
                return "duplicate"
            order.quarantine("same ExecID, different business fields")
            return "quarantine"
        if not order.versions:
            return "held:awaiting-correlation"
        if order.base == "done":
            order.seen[exec_id] = shape
            order.quarantine("trade after a terminal state")
            return "quarantine"
        if cl not in [v["cl"] for v in order.versions]:
            order.seen[exec_id] = shape
            order.quarantine("trade on an unknown version")
            return "quarantine"
        if cl != order.accepted["cl"]:
            order.seen[exec_id] = shape
            order.quarantine("trade on a superseded version")
            return "quarantine"
        if qty is None or qty <= 0 or qty % QTY_STEP:
            order.seen[exec_id] = shape
            order.quarantine("bad LastQty")
            return "quarantine"
        if px is None or px <= 0:
            order.seen[exec_id] = shape
            order.quarantine("bad LastPx")
            return "quarantine"
        if px > order.accepted["px"]:           # buy only, which is all the fixtures use
            order.seen[exec_id] = shape
            order.quarantine("fill above the buy limit")
            return "quarantine"
        if order.cum + qty > order.qty:
            order.seen[exec_id] = shape
            order.quarantine("overfill")
            return "quarantine"
        reported_cum = to_units(kvget("cum")) if kvget("cum") else None
        if reported_cum is not None and reported_cum != order.cum + qty:
            order.seen[exec_id] = shape
            order.quarantine("reported CumQty disagrees with the ledger")
            return "quarantine"
        reported_leaves = to_units(kvget("leaves")) if kvget("leaves") else None
        if reported_leaves is not None and reported_leaves != order.qty - (order.cum + qty):
            order.seen[exec_id] = shape
            order.quarantine("reported LeavesQty disagrees")
            return "quarantine"

        order.seen[exec_id] = shape
        order.fills.append({"exec": exec_id, "qty": qty, "px": px, "cl": cl})
        if order.unknown_outcome:
            order.unknown_outcome = False
        if order.cum == order.qty:
            order.base, order.done_reason = "done", "filled"
            if order.pending:
                order.reconcile = True
        return "applied"

    if verb == "report":
        kind = args[0]
        cl = args[1] if len(args) > 1 else ""
        exec_id = kvget("exec", "E-%s-%s" % (kind, cl))
        shape = fingerprint([kind, exec_id, cl, kvget("cum"), kvget("leaves"), kvget("qty"),
                             kvget("px"), kvget("ref"), kvget("to")])
        key = exec_id if kind != "cancel_reject" else "rej:%s:%s" % (cl, kvget("to", "1"))
        if key in order.seen:
            if order.seen[key] == shape:
                return "duplicate"
            order.quarantine("same report key, different fields")
            return "quarantine"

        def record():
            order.seen[key] = shape

        if kind == "pending_cancel":
            if order.base == "done":
                return "duplicate:late"
            if not order.pending or order.pending["kind"] != "cancel":
                record()
                order.quarantine("unsolicited pending cancel")
                return "quarantine"
            record()
            order.peer_pending = "cancel"
            return "applied"

        if kind == "canceled":
            if order.base == "done":
                record()
                if order.done_reason == "canceled":
                    return "duplicate"
                order.quarantine("final cancel after another terminal")
                return "quarantine"
            if not order.pending or order.pending["kind"] != "cancel":
                record()
                order.quarantine("final cancel with no request")
                return "quarantine"
            reported_cum = to_units(kvget("cum")) if kvget("cum") else None
            if reported_cum is not None and reported_cum != order.cum:
                record()
                order.quarantine("final cancel disagrees about CumQty")
                return "quarantine"
            record()
            order.base, order.done_reason = "done", "canceled"
            order.pending, order.peer_pending = None, None
            order.reconcile = False
            return "applied"

        if kind == "pending_replace":
            if order.base == "done":
                return "duplicate:late"
            if cl in [v["cl"] for v in order.versions]:
                return "duplicate:late"
            if not order.pending or order.pending["kind"] != "replace":
                record()
                order.quarantine("unsolicited pending replace")
                return "quarantine"
            record()
            order.peer_pending = "replace"
            return "applied"

        if kind == "replaced":
            if order.base == "done":
                record()
                order.quarantine("replace after a terminal state")
                return "quarantine"
            if not order.pending or order.pending["kind"] != "replace":
                record()
                order.quarantine("replace ack with no request")
                return "quarantine"
            if cl != order.pending["cl"]:
                record()
                order.quarantine("replace ack names a different request")
                return "quarantine"
            qty = to_units(kvget("qty")) if kvget("qty") else order.pending["qty"]
            px = to_ticks(kvget("px")) if kvget("px") else order.pending["px"]
            if qty is None or px is None:
                record()
                order.quarantine("replace ack carries an inadmissible field")
                return "quarantine"
            if qty < order.cum:
                record()
                order.quarantine("replace ack below CumQty")
                return "quarantine"
            record()
            order.versions.append({"cl": cl, "qty": qty, "px": px})
            order.pending, order.peer_pending = None, None
            order.reconcile = False
            return "applied"

        if kind == "cancel_reject":
            to = kvget("to", "1")
            if to not in ("1", "2"):
                record()
                order.quarantine("cancel reject without tag 434")
                return "quarantine"
            if not order.pending:
                record()
                order.quarantine("cancel reject with no request")
                return "quarantine"
            want = "cancel" if to == "1" else "replace"
            if order.pending["kind"] != want:
                record()
                order.quarantine("cancel reject answers the wrong request")
                return "quarantine"
            record()
            order.pending, order.peer_pending = None, None
            if kvget("reason") == "1":
                order.unknown_outcome = True
                order.reconcile = True
            return "applied"

        if kind in ("expired", "done_for_day"):
            if order.base == "done":
                record()
                order.quarantine("terminal report after another terminal")
                return "quarantine"
            record()
            order.base = "done"
            order.done_reason = "expired" if kind == "expired" else "done_for_day"
            order.pending, order.peer_pending = None, None
            return "applied"

        if kind == "rejected":
            if order.fills:
                record()
                order.quarantine("reject after a recorded fill")
                return "quarantine"
            if order.pending and order.pending["kind"] != "new" and cl == order.pending["cl"]:
                record()
                order.quarantine("ExecType=8 answering a cancel or replace request")
                return "quarantine"
            record()
            order.base, order.done_reason = "done", "rejected"
            order.pending = None
            return "applied"

        if kind == "status":
            reported_cum = to_units(kvget("cum")) if kvget("cum") else None
            if reported_cum is not None and reported_cum != order.cum:
                record()
                order.quarantine("status disagrees with the ledger")
                return "quarantine"
            record()
            return "applied"

        if kind in ("trade_correct", "trade_cancel"):
            ref = kvget("ref")
            if not ref:
                record()
                order.quarantine("correction without ExecRefID")
                return "quarantine"
            if ref not in [f["exec"] for f in order.fills]:
                record()
                order.quarantine("correction names an unknown execution")
                return "quarantine"
            record()
            order.quarantine("corrections are unsupported in v1")
            return "quarantine"

        order.unsupported.append("report kind %s" % kind)
        return "unmodelled"

    if verb in ("cancel", "replace"):
        cl = args[0]
        orig = kvget("orig")
        if order.base == "done" or order.quarantined:
            return "reject:terminal"
        if not order.versions:
            return "reject:unsafe-dependent"
        if order.unknown_outcome:
            return "reject:unsafe-dependent"
        if order.pending:
            return "reject:pending-mutation"
        if orig != order.accepted["cl"]:
            return "reject:lineage"
        if verb == "cancel":
            order.pending = {"kind": "cancel", "cl": cl, "orig": orig, "qty": 0, "px": 0,
                             "unknown": False}
            return "applied"
        qty = to_units(kvget("qty")) if kvget("qty") else order.qty
        px = to_ticks(kvget("px")) if kvget("px") else order.accepted["px"]
        if qty is None or px is None:
            return "reject:field"
        if qty < order.cum:
            return "reject:qty-below-cum"
        if qty == order.cum and order.cum > 0:
            return "reject:zero-working-leaves"
        order.pending = {"kind": "replace", "cl": cl, "orig": orig, "qty": qty, "px": px,
                         "unknown": False}
        return "applied"

    if verb in ("crash", "recover", "drain"):
        # Crash and recovery are properties of the storage boundary, which this
        # model does not have. A deterministic model's state is unchanged by
        # them, which is itself the thing the differential test checks.
        return "noop"

    if verb == "assert":
        return "noop"

    order.unsupported.append("verb %s" % verb)
    return "unmodelled"


def observe(order):
    out = {
        "lifecycle": order.lifecycle,
        "cum": order.cum,
        "leaves": order.leaves,
        "qty": order.qty,
        "fills": len(order.fills),
        "versions": len(order.versions),
        "accepted": order.accepted["cl"] if order.accepted else "",
        "pending": order.pending["cl"] if order.pending else "none",
        "quarantined": "yes" if order.quarantined else "no",
        "reconcile": "yes" if order.reconcile else "no",
    }
    if order.fills:
        total = sum(f["qty"] * f["px"] for f in order.fills)
        out["notional"] = total
    return out


def run(path):
    order = Order()
    decisions = []
    with open(path, encoding="utf-8") as handle:
        for raw in handle:
            line = raw.split("#", 1)[0].strip()
            if not line:
                continue
            tokens = line.split()
            verb = tokens[0]
            args = [t for t in tokens[1:] if "=" not in t]
            kv = dict(t.split("=", 1) for t in tokens[1:] if "=" in t)
            decisions.append("%s:%s" % (verb, apply(order, verb, args, kv)))
    return order, decisions


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 2
    order, decisions = run(argv[1])
    if order.unsupported and any(u.startswith(("verb ", "report kind ")) for u in order.unsupported):
        # Refusing to guess is the point: an oracle that quietly does nothing
        # would agree with anything.
        for u in order.unsupported:
            if u.startswith(("verb ", "report kind ")):
                print("UNMODELLED %s" % u)
        return 3
    for key, value in observe(order).items():
        print("%s=%s" % (key, value))
    if "--trace" in argv:
        for d in decisions:
            print("# %s" % d)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
