# Project 03 — FX/CFD Order State Machine

Client-side order state machine for a FIX 4.4 FX/CFD gateway, built to
[`docs/Order_State_Machine_System_Design.md`](../docs/Order_State_Machine_System_Design.md).
It consumes *normalized business events* and owns the canonical answer to
"what is true about this order". It is not a FIX session engine (that is P01),
not an LP matching engine (P02), not a router (P04), not a risk engine (P05),
and not a durable journal (P06).

C++20, no third-party dependencies, no network I/O.

## What it guarantees

The design document states twenty-four safety invariants and a 340-case
adversarial what-if catalog. Both are implemented as executable checks, not as
prose:

- **A pure reducer.** `reduce(previous, event, policy)` is a function of its
  arguments: no clock, no randomness, no socket, no storage, no logging side
  effect. Everything it wants the world to do leaves as an `EffectIntent`, and
  the caller acts on those intents only after the atomic commit succeeds.
- **Invariants are gates, not logs.** `check_invariants` runs on the *proposed*
  state. A proposal that violates one is never committed; the chain is
  quarantined with the evidence instead.
- **Integers everywhere.** Quantities are base units, prices are tick counts,
  notional is a checked 128-bit product. There is no `double`, no `strtod`, and
  no silent rounding — an off-tick price is refused, not repaired. Averages are
  kept as an exact rational and rounded once, half-to-even, at the output.
- **Pending is not done.** Pending Cancel is not Canceled, Pending Replace is
  not Replaced, and a timeout is an observation about us rather than a decision
  by the counterparty. Confirmed business state and operational uncertainty are
  separate fields; `lifecycle()` derives one view for reporting, but nothing
  inside the reducer decides anything from it.
- **Executed quantity is never lost.** CumQty accumulates across accepted
  versions, a cancel never reverses a fill, and a status report can never
  create one.
- **Contradictions stop the chain.** Overfill, a conflicting duplicate ExecID,
  a cross-account or cross-peer report, an obsolete or FIX 4.2 ExecType, an
  unsupported trade correction, a cumulative figure that disagrees with the
  ledger: each one quarantines the affected chain with its evidence and leaves
  every other chain working.

## What it does not guarantee

- **Crash safety is not proved here.** The suite runs against `MemoryStore`,
  which implements the atomic-commit contract and lets all sixteen crash
  windows be injected, but whose `durability_mode()` is `IN_MEMORY`. Release
  gate G3 needs P06 with a real PostgreSQL transaction, WAL and fsync behind
  it. Nothing in this repository should be read as evidence for it.
- **No end-to-end FIX integration.** P01, P02 and P04 do not exist yet, so the
  cases that can only be proved inside them (frame checksum, session identity,
  byte-level fuzz through the whole pipeline, an external peer oracle) are
  reported as BLOCKED with the reason, never as passes.
- **No exposure enforcement.** P03 emits risk intents with stable identities
  and never claims to enforce an account limit; that is P05's job, serialised
  per account.
- **No performance claim.** Latency is measured and printed; it is not asserted
  against a target. The CV's 95-microsecond receive-to-route figure belongs to
  a different system and is not reused here.
- **One chain with a very long ledger is quadratic.** The reducer copies the
  aggregate, so a single order accumulating many thousands of fills costs
  O(n²). That is the honest price of a pure reducer at this stage; the design
  doc's answer is a bounded in-memory cache over a persisted ledger
  (section 15). Throughput is therefore tested across many chains, not one.

## Layout

```
include/order_core/   math, decimal, policy, ids, events, aggregate,
                      invariants, reducer, failpoint, store, engine
src/                  their implementations
tests/support/        the tiny_test harness and the scenario fixture
tests/unit/           catalog groups A..K and M, P, Q
tests/failure/        groups L, N, O (crash windows, pressure, adversarial)
tests/integration/    the PB-01..PB-26 fault-injection playbooks
tests/property/       property-based tests over generated histories
tests/model/          bounded exhaustive state exploration
tests/fixtures/traces golden traces in the scenario DSL
tools/order_trace_replay   replays a trace and verifies its assertions
tools/order_model_check    standalone bounded model checker
tools/reference_model      independent Python oracle (a second implementation)
apps/order_lab_cli         hand-driving the state machine
scripts/ci.py              one command for the whole verification run
scripts/report.py          release gate: catalog coverage vs what actually ran
scripts/differential.py    C++ against the Python reference model
config/order_core.yaml     the reference configuration, field by field
```

## Building and running

Needs a C++20 compiler. There is nothing to fetch.

```bash
# everything: build, suite, model check, traces, gate report
python scripts/ci.py

# or piecemeal
make -j8 all tools
./build/order_core_tests                  # the whole catalog
./build/order_core_tests --filter=J-      # one group
./build/order_core_tests --list           # every registered case id
./build/order_model_check --max-depth 8
./build/order_trace_replay tests/fixtures/traces/J-01.trace
python scripts/differential.py            # C++ against the Python oracle
python scripts/report.py test-artifacts/order_core.json

# sanitizers, deliberately separate binaries: ASan and TSan must not share one
make asan
make tsan
```

CMake is also supported (`cmake -S . -B build-cmake -DORDER_CORE_ENABLE_TESTS=ON`
then `ctest`), with one ctest entry per catalog group so a failing group is
named in CI output.

Every run writes `test-artifacts/order_core.json`: one record per case with its
catalog id, priority, verdict and failure detail. `scripts/report.py` reconciles
that against the ID set parsed out of the design document and fails if a P0 is
missing, blocked or failing — a skipped P0 counted as a pass is the release
blocker the design doc names by name.

**`scripts/ci.py` exits 1 today, and that is the correct result.** Every test
passes, but four P0 cases are BLOCKED on P01 and P05, so the release gate is
not satisfied. A green test run and a satisfied gate are different claims, and
the script refuses to conflate them. The stage output tells you which it is.

## Three independent oracles

The design document warns that an oracle sharing the system's own reducer is
worthless, so there are three:

1. **A journal oracle** inside the suite, which re-derives CumQty and notional
   by walking the committed journal and re-parsing the raw event text. It
   shares no cached field and no ledger vector with the aggregate.
2. **A Python reference model** (`tools/reference_model/model.py`), written by
   hand from the FIX 4.4 semantics and the pinned Rules of Engagement rather
   than transliterated from the C++. It keeps no cached totals and refuses to
   guess: anything it does not model is reported as `UNMODELLED` and treated as
   a failure, so it cannot agree by accident of silence.
   `scripts/differential.py` compares the two implementations on every golden
   trace.
3. **An external peer oracle**, which needs P02. Not available, so the cases
   that depend on it are BLOCKED with that reason.

## Verification status

See [`docs/VERIFICATION.md`](docs/VERIFICATION.md) for the measured results,
the gate-by-gate status against design doc section 21, and the list of blocked
cases with the reason each one is blocked.

## Reading the code

Start at [`include/order_core/reducer.hpp`](include/order_core/reducer.hpp) for
the six decision outcomes, then
[`include/order_core/aggregate.hpp`](include/order_core/aggregate.hpp) for the
two state layers and why one enum is not enough. `src/reducer.cpp` is the
transition table, and every branch names the what-if case it exists for.
[`include/order_core/invariants.hpp`](include/order_core/invariants.hpp) maps
I01..I24 onto executable checks.
