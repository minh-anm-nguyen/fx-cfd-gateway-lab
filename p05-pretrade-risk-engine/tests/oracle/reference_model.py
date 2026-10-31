#!/usr/bin/env python3
"""Independent reference oracle for the P05 risk engine (spec section 24.1).

This recomputes every exported number from the specification's definitions
using `fractions.Fraction` and Python's arbitrary-precision integers. It shares
no code with the C++ engine on purpose: a bug duplicated in both would
otherwise make them agree, and the differential check would prove nothing.

What it recomputes, per account and symbol:

    P  = signed confirmed position from the fill events
    B  = sum over live BUY chains of max(0, accepted - cum) and,
         while a replace is in flight, the max of that and max(0, proposed - cum)
    S  = the same for SELL chains
    E  = max(|P + B|, |P - S|)                        (section 10.1)
    G  = |P| + B + S
    notional_minor = ceil(units * risk_price * tick * multiplier * fx * 10^scale)
    margin_proxy   = ceil(account gross notional * margin rate)

and the conservative inputs:

    risk_price = ceil(max(highest raw limit price on the symbol, ask)
                      * (10000 + buffer_bps) / 10000)
    fx_upper   = ask of the quote->account edge * (10000 + fx_buffer_bps) / 10000

A cancel request deliberately has no effect on B or S (RI-04), and a terminal
cancel takes the chain's remainder to zero while leaving the position (RI-18).

Usage:
    python reference_model.py [path/to/model_traces.json]

Exit status is 0 when every exported number matches, 1 otherwise.
"""

from __future__ import annotations

import json
import sys
from dataclasses import dataclass, field
from fractions import Fraction
from pathlib import Path

DEFAULT_ARTIFACT = Path("test-artifacts/model_traces.json")


def ceil_fraction(value: Fraction) -> int:
    """Ceiling of a nonnegative rational, as integer arithmetic."""
    if value < 0:
        raise ValueError(f"a risk charge may not be negative: {value}")
    return -((-value.numerator) // value.denominator)


def to_fraction(obj: dict) -> Fraction:
    return Fraction(int(obj["num"]), int(obj["den"]))


def envelope(position: int, buys: int, sells: int) -> tuple[int, int]:
    """Returns (net envelope, gross proxy) in base units."""
    if buys < 0 or sells < 0:
        raise ValueError(f"pending quantities must be nonnegative: {buys}, {sells}")
    net = max(abs(position + buys), abs(position - sells))
    gross = abs(position) + buys + sells
    return net, gross


@dataclass
class Chain:
    account: str
    symbol: str
    side: str
    accepted: int
    accepted_price: int
    cum: int = 0
    proposed: int | None = None
    proposed_price: int | None = None
    pending_cancel: bool = False
    terminal: bool = False

    def effective_remaining(self) -> int:
        if self.terminal:
            return 0
        old_branch = max(0, self.accepted - self.cum)
        new_branch = 0 if self.proposed is None else max(0, self.proposed - self.cum)
        # RI-06: the conservative branch is the maximum, never the proposal alone.
        return max(old_branch, new_branch)

    def effective_price(self) -> int:
        if self.proposed_price is None:
            return self.accepted_price
        return max(self.accepted_price, self.proposed_price)


@dataclass
class Model:
    chains: dict[tuple[str, str], Chain] = field(default_factory=dict)
    positions: dict[tuple[str, str], int] = field(default_factory=dict)

    def apply(self, step: dict) -> None:
        op = step["op"]
        if op == "new":
            key = (step["account"], step["chain"])
            self.chains[key] = Chain(
                account=step["account"],
                symbol=step["symbol"],
                side=step["side"],
                accepted=int(step["units"]),
                accepted_price=int(step["price"]),
            )
            return

        key = (step["account"], step["chain"])
        chain = self.chains.get(key)
        if chain is None:
            raise KeyError(f"{op} for an unknown chain {key}")

        if op == "fill":
            units = int(step["units"])
            ceiling = max(chain.accepted, chain.proposed or 0)
            if chain.cum + units > ceiling:
                raise ValueError(
                    f"fill of {units} takes {key} past its reserved ceiling {ceiling}"
                )
            chain.cum += units
            sign = 1 if chain.side == "BUY" else -1
            position_key = (chain.account, chain.symbol)
            self.positions[position_key] = (
                self.positions.get(position_key, 0) + sign * units
            )
        elif op == "replace":
            if chain.proposed is not None:
                raise ValueError(f"{key} already has a pending replace")
            chain.proposed = int(step["units"])
            chain.proposed_price = int(step["price"])
        elif op == "replace_accept":
            if chain.proposed is None:
                raise ValueError(f"{key} has no proposal to promote")
            chain.accepted = chain.proposed
            chain.accepted_price = chain.proposed_price or chain.accepted_price
            chain.proposed = None
            chain.proposed_price = None
        elif op == "replace_reject":
            chain.proposed = None
            chain.proposed_price = None
        elif op == "cancel_request":
            # RI-04: recorded, with no effect on the held quantity.
            chain.pending_cancel = True
        elif op == "cancel_final":
            chain.terminal = True
            chain.pending_cancel = False
            chain.proposed = None
            chain.proposed_price = None
        else:
            raise ValueError(f"unknown op {op!r}")

    def position(self, account: str, symbol: str) -> int:
        return self.positions.get((account, symbol), 0)

    def pending(self, account: str, symbol: str, side: str) -> int:
        return sum(
            c.effective_remaining()
            for c in self.chains.values()
            if c.account == account and c.symbol == symbol and c.side == side
        )

    def highest_live_price(self, symbol: str) -> int:
        """Highest raw limit price across every live chain on the symbol.

        The engine revalues a symbol from raw limit prices rather than from an
        already-buffered one, so re-running the valuation is idempotent.
        """
        prices = [
            c.effective_price()
            for c in self.chains.values()
            if c.symbol == symbol and not c.terminal
        ]
        return max(prices) if prices else 0

    def accounts(self) -> list[str]:
        return sorted({c.account for c in self.chains.values()})

    def symbols_for(self, account: str) -> list[str]:
        """Symbols the engine reports a row for: anything still carrying state.

        A row survives while a chain is live (its working-order count matters)
        or while the position is non-zero; a fully empty row is pruned.
        """
        out = set()
        for c in self.chains.values():
            if c.account != account:
                continue
            if not c.terminal:
                out.add(c.symbol)
        for (acct, symbol), units in self.positions.items():
            if acct == account and units != 0:
                out.add(symbol)
        return sorted(out)


class Valuation:
    """Conservative price and FX bounds, from the section 9 rules."""

    def __init__(self, policy: dict, market: dict):
        self.policy = policy
        self.market = market
        self.minor_scale = int(policy["minor_scale"])
        self.account_currency = policy["account_currency"]
        self.margin_rate = to_fraction(policy["margin_rate"])
        self.fx_buffer_bps = int(policy["fx_upper_buffer_bps"])

    def risk_price_ticks(self, symbol: str, reference_price: int) -> int:
        spec = self.policy["instruments"][symbol]
        ask = int(self.market["quotes"][symbol]["ask"])
        base = max(reference_price, ask)
        buffer_bps = int(spec["risk_price_buffer_bps"])
        return ceil_fraction(Fraction(base * (10000 + buffer_bps), 10000))

    def fx_upper(self, symbol: str) -> Fraction:
        spec = self.policy["instruments"][symbol]
        quote_currency = spec["quote_currency"]
        if quote_currency == self.account_currency:
            # An identity conversion carries no spread, so no buffer either.
            return Fraction(1, 1)
        for edge in self.market["fx"]:
            if edge["from"] == quote_currency and edge["to"] == self.account_currency:
                # The unfavourable side of a direct edge is the ask.
                raw = to_fraction(edge["ask"])
                return raw * Fraction(10000 + self.fx_buffer_bps, 10000)
            if edge["from"] == self.account_currency and edge["to"] == quote_currency:
                # Inverting: the unfavourable direction is the bid.
                raw = 1 / to_fraction(edge["bid"])
                return raw * Fraction(10000 + self.fx_buffer_bps, 10000)
        raise KeyError(f"no FX path for {quote_currency} -> {self.account_currency}")

    def notional_minor(self, units: int, symbol: str, reference_price: int) -> int:
        if units < 0:
            raise ValueError("notional over negative units")
        if units == 0:
            return 0
        spec = self.policy["instruments"][symbol]
        exact = (
            Fraction(units)
            * Fraction(self.risk_price_ticks(symbol, reference_price))
            * to_fraction(spec["tick"])
            * to_fraction(spec["multiplier"])
            * self.fx_upper(symbol)
            * Fraction(10**self.minor_scale)
        )
        # RI-10: round up, always.
        return ceil_fraction(exact)

    def margin_proxy_minor(self, gross_notional_minor: int) -> int:
        return ceil_fraction(Fraction(gross_notional_minor) * self.margin_rate)


def check_trace(trace: dict, valuation: Valuation) -> list[str]:
    """Replays one trace and compares every exported number."""
    problems: list[str] = []
    seed = trace["seed"]
    model = Model()

    for index, step in enumerate(trace["steps"]):
        try:
            model.apply(step)
        except Exception as exc:  # noqa: BLE001 - reported, never swallowed
            problems.append(f"seed {seed} step {index}: {exc}")
            return problems

    engine = trace["engine"]
    expected_firm = 0

    for account in model.accounts():
        engine_account = engine["accounts"].get(account)
        expected_account_gross = 0

        for symbol in model.symbols_for(account):
            position = model.position(account, symbol)
            buys = model.pending(account, symbol, "BUY")
            sells = model.pending(account, symbol, "SELL")
            net_units, gross_units = envelope(position, buys, sells)
            reference_price = model.highest_live_price(symbol)
            net_minor = valuation.notional_minor(net_units, symbol, reference_price)
            gross_minor = valuation.notional_minor(
                gross_units, symbol, reference_price
            )
            expected_account_gross += gross_minor

            if engine_account is None:
                problems.append(f"seed {seed}: the engine has no account {account}")
                break
            reported = engine_account["symbols"].get(symbol)
            if reported is None:
                if position == 0 and buys == 0 and sells == 0:
                    # A fully empty row is pruned by both sides.
                    continue
                problems.append(
                    f"seed {seed}: the engine has no row for {account}/{symbol} "
                    f"(oracle: P={position} B={buys} S={sells})"
                )
                continue

            for label, expected, actual in (
                ("P", position, reported["position_units"]),
                ("B", buys, reported["pending_buy_units"]),
                ("S", sells, reported["pending_sell_units"]),
                ("E units", net_units, reported["net_units"]),
                ("G units", gross_units, reported["gross_units"]),
                ("E minor", net_minor, reported["net_notional_minor"]),
                ("G minor", gross_minor, reported["gross_notional_minor"]),
            ):
                if expected != actual:
                    problems.append(
                        f"seed {seed} {account}/{symbol}: {label} engine={actual} "
                        f"oracle={expected}"
                    )

        if engine_account is not None:
            if engine_account["total_gross_notional_minor"] != expected_account_gross:
                problems.append(
                    f"seed {seed} {account}: account gross engine="
                    f"{engine_account['total_gross_notional_minor']} "
                    f"oracle={expected_account_gross}"
                )
            expected_margin = valuation.margin_proxy_minor(expected_account_gross)
            if engine_account["margin_proxy_minor"] != expected_margin:
                problems.append(
                    f"seed {seed} {account}: margin proxy engine="
                    f"{engine_account['margin_proxy_minor']} oracle={expected_margin}"
                )
        expected_firm += expected_account_gross

    if engine["firm_gross_notional_minor"] != expected_firm:
        problems.append(
            f"seed {seed}: firm gross engine={engine['firm_gross_notional_minor']} "
            f"oracle={expected_firm}"
        )

    return problems


def self_test() -> list[str]:
    """Checks the oracle against the worked example in section 10.2."""
    problems: list[str] = []

    net, gross = envelope(10000, 40000, 20000)
    if (net, gross) != (50000, 70000):
        problems.append(f"section 10.2 envelope: got ({net}, {gross})")

    # After a 15,000 BUY fill the envelope must not move.
    net_after, gross_after = envelope(25000, 25000, 20000)
    if (net_after, gross_after) != (net, gross):
        problems.append("section 10.2 transfer invariance broken in the oracle")

    if ceil_fraction(Fraction(1, 10000)) != 1:
        problems.append("ceil_fraction rounds a sub-unit charge to zero")
    if ceil_fraction(Fraction(0)) != 0:
        problems.append("ceil_fraction of zero is not zero")
    try:
        ceil_fraction(Fraction(-1, 2))
        problems.append("ceil_fraction accepted a negative charge")
    except ValueError:
        pass

    return problems


def main(argv: list[str]) -> int:
    artifact = Path(argv[1]) if len(argv) > 1 else DEFAULT_ARTIFACT
    problems = self_test()
    if problems:
        for problem in problems:
            print(f"ORACLE SELF-TEST FAILED: {problem}")
        return 1

    if not artifact.exists():
        print(f"artifact not found: {artifact}")
        print("run the exporter first: risk_tests --filter=model")
        return 1

    with artifact.open("r", encoding="utf-8") as handle:
        data = json.load(handle)

    valuation = Valuation(data["policy"], data["market"])
    traces = data["traces"]
    if not traces:
        print("the artifact contains no traces")
        return 1

    total_steps = 0
    for trace in traces:
        total_steps += len(trace["steps"])
        problems.extend(check_trace(trace, valuation))

    if problems:
        print(f"DIFFERENTIAL FAILURES: {len(problems)}")
        for problem in problems[:40]:
            print(f"  {problem}")
        if len(problems) > 40:
            print(f"  ... and {len(problems) - 40} more")
        return 1

    print(
        f"ORACLE OK: {len(traces)} traces, {total_steps} steps, "
        "every exported quantity and notional matches"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
