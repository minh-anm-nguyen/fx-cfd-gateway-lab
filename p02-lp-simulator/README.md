# Project 02 — FX/CFD Liquidity Provider Simulator (FIX 4.4 acceptor)

Implementation of `docs/LP_Simulator_System_Design.md`. C++20, SQLite, FIX 4.4
**acceptor** — the counterparty side that Project 01's initiator connects to.

> **Lab system.** Not connected to a real liquidity provider, not certified
> against any counterparty, and not production-ready. Passing the gate below is
> a lab result, which the design doc (§19, §21.2) is explicit is not the same
> thing as production readiness.

## What it does

Accepts a FIX 4.4 session from one authorised counterparty, validates
`NewOrderSingle(35=D)`, `OrderCancelRequest(35=F)` and
`OrderCancelReplaceRequest(35=G)`, maintains order chains against a synthetic
quote book with finite liquidity, and answers with `ExecutionReport(35=8)`,
`OrderCancelReject(35=9)` and `BusinessMessageReject(35=j)`.

Everything is durable and single-writer: one business decision is one atomic
transaction, and no report reaches a socket before the state that justifies it
is committed.

**In the v1 profile:** Limit orders, DAY time in force, one symbol set, one
session, one counterparty firm, scripted or automatic fill triggers.
**Not implemented:** IOC/FOK/Market, TLS, multiple firms, multiple concurrent
sessions, business-day rollover. These are reported as `NOT_IMPLEMENTED` by the
test report rather than quietly passing — see *Honest gaps* below.

## Build

Requires a C++20 compiler and SQLite3 development headers.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j
ctest --test-dir build --output-on-failure
```

There is also `scripts/build.sh` for fast incremental iteration (it compiles
only what changed and skips the CMake reconfigure); CMake + CTest remains the
documented harness.

Build options:

| Option | Default | Effect |
|---|---|---|
| `LPSIM_ENABLE_FAILPOINTS` | `ON` | Compiles in the W1–W10 crash cutpoints. The crash suite needs them: a crash test whose cutpoint cannot be proven is scored INVALID, not PASS (M-15). |
| `LPSIM_SANITIZE` | empty | `address` or `thread`. Never both in one binary. |

## Run

```sh
# the acceptor, bound to loopback
./build/lp_simulator --config configs/lp.example.yaml --db ./var/lp.db

# a scripted scenario, run three times to prove it is reproducible
./build/scenario_runner --scenario scenarios/cancel_fill_race.yaml --repeat 3 --trace

# every suite, then the matrix report
./scripts/run_tests.sh --build --csv test-artifacts/matrix.csv
```

The simulator refuses to start with the unsafe capabilities enabled on anything
but a loopback bind, and refuses a configuration with a blank CompID, a
non-positive heartbeat, a zero queue cap, an unknown key, or an order type it
does not implement.

## Layout

```
include/lp/        headers, one per concern
src/               implementations
apps/
  lp_simulator/    the acceptor process (TCP)
  scenario_runner/ executes a scenario file and reports its trace hash
tests/
  unit/            wire, quote, reports, scheduler, store, business, session, model
  integration/     recovery, playbooks, crash
  support/         test harness: fake transport, scripted client, fixtures
scenarios/         the playbooks of design doc §17, as executable files
configs/           example simulator profiles
scripts/           build.sh, run_tests.sh, report.py
test-artifacts/    per-suite JSON verdicts and the CSV matrix
```

## How the durability works

Four transaction boundaries (design doc §10.2), and the order of them is the
whole point:

| | What commits | Why it is separate |
|---|---|---|
| **TX-A** | inbound `(session, epoch, MsgSeqNum)` + durable inbox | A frame that arrived is recorded before anything acts on it, so a crash cannot lose the fact that it arrived or cause it to be processed twice. |
| **TX-B** | request registry, order chain, quote consumption, fill ledger, outbox rows, scheduler resolution | One business decision, one atomic commit. **No socket write happens inside it.** |
| **TX-C** | `send_once(event_id)` → one outbound sequence number + the journaled frame | An outbox event can never own a second sequence number. The design doc (§3.2, ADR-08) notes a stock QuickFIX sender does not give you this; it has to be built and proven, which the W5/W6 crash tests do. |
| **TX-D** | dispatch bookkeeping | Losing it is harmless: the row stays bound to its original sequence and is re-sent unchanged. |

Startup recovery recomputes `CumQty` and `AvgPx` from the fill ledger and
compares them against the stored snapshot. A disagreement **halts** rather than
repairing itself, because silently trusting either side is how a rolled-back
snapshot reaches a client as a real order state.

## Determinism

Races are decided by `(due_us, priority_class, insertion_seq)` and by nothing
else — never by thread timing or wall-clock time. Everything that can race with
everything else shares one priority class, which is what lets
`cancel_fill_race.yaml` and `cancel_wins_race.yaml` differ only in event order
and still produce different, repeatable outcomes.

`scenario_runner --repeat n` runs a scenario n times from clean stores and fails
unless every run produces the same canonical trace hash.

## Test results

Run `./scripts/run_tests.sh` and read the report. At the time of writing:

```
test cases   310
PASS           302   (P0: 269, P1: 33)
FAIL             0
BLOCKED          2   environment, not counted as passing
NOT_IMPLEMENTED  6   out of v1 scope, not counted as passing

GATE: in-scope P0 PASS
```

Twelve suites; 213 of the design doc's 278 defined scenario rows have an
executable test; all ten crash windows are exercised by killing a real child
process at a failpoint that writes a marker proving where it died; and the
live socket checks (`scripts/smoke_tcp.py`, run by CTest as `test_smoke`)
cover the real listener, including A-08, which needs two actual sockets.

The coverage number counts matrix rows that have a test bearing their id, so it
understates the playbooks: most of §17 is exercised under the matrix row it
cites rather than under a `PB-` id, and §16.11 defines rows J-01…J-10 *as* the
crash windows W1…W10.

## Honest gaps

The design doc insists these be stated rather than buried, so:

- **Power loss is not tested.** `synchronous=FULL` guarantees what the
  filesystem and the drive honour. The crash suites kill processes; proving more
  needs real hardware. Reported as `BLOCKED` (J-18b, J-18c).
- **No interop run against an independent FIX engine.** M-01 and PB-18 call for
  QuickFIX/C++ as a second peer to catch a bug shared by both sides of a
  single-author codebase. The scripted raw peer here is written by the same
  author as the acceptor, so it cannot fully substitute. This is the largest
  open item.
- **IOC, FOK, Market, TLS, multiple firms and multiple sessions do not exist.**
  Where the profile parses these values it does so only to refuse them for a
  named reason. Reported as `NOT_IMPLEMENTED`.
- **No soak or benchmark numbers.** §18.2 layer 8 asks for 1h/6h/24h runs and
  latency distributions. Nothing here measures throughput or latency, and no
  performance claim is made.
- **Group K is the thinnest area.** 8 of 20 rows. The faults that need a real
  network in the path — packet reordering, RST mid-frame, latency injection —
  have no proxy here; what is covered is what a controllable transport can
  express (short writes, a peer that stops reading, coalescing, splitting).
- **A finite suite proves nothing about every interleaving.** 310 cases passing
  is evidence, not proof. The property and model-based suites sample and
  exhaust small state spaces respectively; neither covers production scale.

## Relationship to Project 01

P02 is the **acceptor**, P01 the **initiator**. They keep independent stores,
independent sequence state and independent business namespaces, and the FIX
codec here is deliberately a separate implementation from P01's: ADR-01 and
M-01 want a bug shared by both sides to still be catchable, which one shared
codec would hide.

The outbound contract P02 relies on — `send_once(event_id, payload)` binding an
event to exactly one sequence number — is implemented and tested here. The
design doc is careful that this must not be assumed of P01 or of any library
until it is demonstrated there too.
