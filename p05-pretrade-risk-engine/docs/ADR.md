# Architecture decision records

Decisions actually made while building this, with the alternative that was
rejected and what would make us revisit. The spec's §28.1 proposes a set of
ADRs; these are the ones the code reflects.

---

## ADR-01 — One commit boundary, expressed in the type system

**Decision.** `RiskStore::commit(CommitRequest)` is the only mutator, and a
`TransactionData` carries the command record, the reservation, the chain index,
the event dedup key, the projection, the audit event and the router outbox
intent together.

**Why.** §23.2's dual-write counterexample is not a bug you fix by being
careful; it is a bug you avoid by not providing the API. There is deliberately
no `insert_reservation()` and no `append_outbox()`, so a caller cannot write the
hold and then fail to write the intent.

**Rejected.** Separate repositories with a coordinating service. That is what
the spec warns against, and it would have forced a compensation protocol we
could not prove.

**Revisit when** a P06 PostgreSQL adapter arrives. The interface is already the
right shape for it: `CommitRequest` maps onto one `SERIALIZABLE` transaction.

---

## ADR-02 — The outstanding quantity is a definition, not a counter

**Decision.** `Reservation::risk_effective_remaining()` returns
`max(max(0, accepted − cum), max(0, proposed − cum))`, and zero once the chain
is terminal. Nothing increments or decrements a running total.

**Why.** Most of the spec's hardest invariants collapse into arithmetic.
"A pending cancel releases nothing" (RI-04) holds because `pending_cancel` does
not appear in the expression. "A fill does not change the envelope" (RI-07)
holds because `P + B` is invariant when `f` moves from one to the other.
"A replace reserves the worst branch" (RI-06) *is* the `max`.

**Rejected.** Incrementing `pending_buy_units` on reserve and decrementing on
fill. Every double-count and early-release bug in this class of system lives in
the gap between those two updates.

**Consequence.** The committed projection is a materialised view of this
definition. ADR-09 explains why it is maintained by delta and how that is kept
honest.

---

## ADR-03 — "We cannot prove it" is a third answer

**Decision.** Three outcomes, not two: `ALLOW_COMMITTED`, `DENY_DEFINITIVE`, and
`HOLD_RETRYABLE` for anything uncertain. `reason_is_retryable()` is the single
place that classifies, and `record_denial()` refuses to persist a retryable
reason as a decision.

**Why.** A `DENY` tells the caller the command is definitely not at the LP. For
a lost commit acknowledgement that statement is false, and acting on it is how
you get a duplicate order. Writing such a denial down would be worse still: a
later retry of the same command id would be told "no" for a command that may
already be working.

**Notable case.** A fenced writer returns HOLD, not DENY. Nothing was written —
but the writer is no longer the owner, so it cannot speak definitively about a
command another owner may already have decided.

---

## ADR-04 — Exact rationals, never floating point

**Decision.** Tick sizes, contract multipliers, FX rates and margin rates are
`Rational{num, den}`. A valuation accumulates in `RatioAcc`, a 128-bit exact
fraction that reduces by gcd as it goes, and narrows once at the end with a
ceiling.

**Why.** §8 forbids floating point on the risk path, and the reason is RI-10: a
single rounding in the wrong direction understates risk by a sub-unit amount
that then accumulates. Accumulating exactly and rounding once, upward, makes
"the charge is never below the true value" checkable — which the Python oracle
does, independently, with `fractions.Fraction`.

**Rejected.** Fixed-point `int64` with a chosen scale. Picking a scale that
survives `units × ticks × tick_size × multiplier × fx × 10^minor` without
overflow or precision loss is a per-instrument judgement call, and getting it
wrong is silent.

---

## ADR-05 — Freshness lives on the monotonic axis

**Decision.** Every TTL is measured against `steady_clock`; the wall clock is
recorded for audit only. An ingest timestamp *ahead* of now is refused as
`ClockRollback` rather than treated as fresh.

**Why.** §9.1 is explicit, and PB-28 is the reason: an NTP step backwards would
otherwise make an expired quote look current. Two tests pin it —
`wall_clock_movement_does_not_affect_freshness` and
`a_wall_clock_rollback_does_not_revive_stale_data`.

**Consequence.** The TTL boundary is exclusive: at exactly `ttl` the record is
expired. PB-04 advances a virtual clock to that boundary and requires a denial,
so a `<=` would fail the suite.

---

## ADR-06 — An append-only journal, not SQLite

**Decision.** A single file of length-prefixed, CRC-protected records, one per
commit, flushed to the device before the acknowledgement. Replay applies the
prefix of fully-written records and reports the torn tail.

**Why.** It gives genuine all-or-nothing durability with no external dependency,
which matters because the crash tests need a child process that can be killed at
an exact instant and a journal that can then be read byte-for-byte. A
half-written record is indistinguishable from a crash mid-append, and both
resolve the same way: discard it. Adding SQLite would have put a library's own
WAL between the test and the thing being tested.

**Rejected.** SQLite (an extra dependency, and its durability would be what is
under test rather than ours) and a snapshot-plus-delta scheme (a snapshot is
only safe once its checksum and the journal tail are verified, per §16.3; not
worth it at this scale).

**Limit.** Single node. No claim beyond that, per §1.3.

---

## ADR-07 — Crash windows are code, not timing

**Decision.** The twenty windows of §23.1 are `Failpoint` enumerators reached by
an explicit call on the commit path. A test arms one with an action
(`commit_fail`, `commit_ack_lost`, `fence_reject`, `abort`, or a hook).

**Why.** A crash test built from sleeps and races proves whatever the scheduler
felt like that run. Naming the window makes the test deterministic, makes a
failure say which row of §23.1 broke, and lets the same window be driven
in-process *and* by a child process that leaves via `_Exit`.

**Supporting decision.** The harness launches the child through
`cmd /s /c "..."` and links the runtime statically. Both exist because a child
that fails to start looks exactly like a child that crashed where we asked, and
that false pass is worse than no test. The harness proves itself first by
running the child without a failpoint and checking the journal really contains
the reservation.

---

## ADR-08 — An incremental multiset digest for the state hash

**Decision.** Each keyed collection (reservations, commands, applied events,
outbox) keeps a cached per-record digest and a running sum modulo 2^256. The
state hash combines those sums, their sizes, the domain state, the policy hash
and the projection.

**Why.** The state hash is taken twice per commit, to stamp the audit chain's
before and after. Hashing every record each time made a commit O(records) in
SHA-256 operations and the workload quadratic, which §25.1 rules out: 8,000
admissions took 298 seconds. The benchmark found this, not a review.

**Trade-off, stated because it is a real one.** An additive multiset hash is
order-independent and incremental, but weaker than a single SHA-256 over the
whole serialised set, because sums are malleable. It is used here as a
*divergence detector* — proving a replayed journal reproduces the live state and
that no mutation went unaudited — not as a commitment that resists an adversary
who controls record contents. Integrity against a tampered journal comes from
the per-record CRC and the all-or-nothing framing.

**Revisit if** the audit trail ever has to survive an untrusted writer. Then it
needs a proper accumulator or a Merkle structure.

---

## ADR-09 — The projection is updated by delta and cross-checked from scratch

**Decision.** The commit path computes the new projection by applying one
reservation's delta to one row (`apply_reservation_delta`). The from-scratch
derivation over the whole ledger remains as
`RiskCoordinator::cross_check_projection()`, and the property suite runs it
after *every* step.

**Why.** ADR-02's derive-from-the-ledger approach is the right definition but
the wrong hot path: it made every admission O(live reservations). Together with
ADR-08, and with removing the full-state copy that `commit()` used to make in
order to be able to roll back, this took admission throughput from 27/s at 8,000
live orders to a flat ~5,700/s at 100,000.

**What makes it safe.** The spec asks for a materialised projection *and* a
cross-check against the ledger (§16.1's note, RI-23), which is exactly this
shape. The guarantee changes from "drift is impossible" to "drift is detected
immediately", and the detector runs on 8 seeds × 80 steps, in every scenario,
and after every crash recovery.

**Supporting decision.** Removing the full-state copy meant evaluating the
in-transaction crash windows *before* any mutation, instead of aborting part-way
through a copy that was going to be discarded. The observable result is
identical — on a blocked transaction nothing is visible and nothing is durable,
which is what each window asserts — and all twenty window tests pass unchanged.

---

## ADR-10 — The reference oracle is in another language

**Decision.** `tests/oracle/reference_model.py` recomputes every exported
quantity, envelope, notional and margin figure from the specification's
definitions, using `fractions.Fraction`. The C++ exporter writes the traces and
the engine's numbers; the oracle replays them independently.

**Why.** §24.1 is right that a reference model sharing code with the system
under test can carry the same bug and agree with it. The property suite's
in-binary model catches structural mistakes — a double count, a release without
evidence — but both halves are one reading of the spec. The Python side is a
second reading, written from the document rather than from the code, and it
checks the parts most likely to go subtly wrong: the ceiling direction, the
unfavourable FX side, and which branch a pending replace is valued at.

**Rejected.** TLA+. It would model the state machine well and say nothing about
the rounding, which is where this engine's arithmetic risk actually lives.

---

## ADR-11 — Raw limit prices are stored alongside the pinned valuation

**Decision.** A reservation keeps the raw `accepted_limit_price_ticks` (and the
proposed one) as well as the buffered `Valuation` that approved it. Revaluation
reads the raw prices.

**Why.** Found while building, not designed up front. Revaluing from the stored
*buffered* price re-applies the instrument's buffer on every pass, so a symbol's
bound creeps upward every time any order touches it. Conservative, but it means
two equivalent states value differently and the commit-time freshness re-check
can never match. Raw prices make revaluation idempotent.

---

## ADR-12 — A strict, hand-written config parser

**Decision.** A small YAML subset parser that rejects tabs, odd indentation,
anchors, aliases, block sequences, duplicate keys, non-integer values for
integer fields, and — most importantly — any key it does not recognise.

**Why.** The dangerous failure for a risk config is not a parse error; it is a
typo that silently leaves a cap at its default. `max_firm_gross_minr` must be an
error, not a zero cap that denies everything or an unlimited one that allows
everything. A general YAML library would accept it and move on.

**Cost.** The scenario DSL had to be line-oriented rather than a YAML list,
because the parser refuses sequences. One syntax serves both files, and the
examples in §24.5 translate directly.
