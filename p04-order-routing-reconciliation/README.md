# Project 04 — FX/CFD Order Routing & Execution Reconciliation

A C++20 implementation of the design in
[`../docs/Order_Routing_Execution_Reconciliation_System_Design.md`](../docs/Order_Routing_Execution_Reconciliation_System_Design.md):
a client-side order router and FIX 4.4 application adapter that admits
idempotent commands, pins one route per account, encodes `D`/`F`/`G` (and `H`
behind a capability gate), validates and correlates inbound `8`/`9`, hands
business events to a single-owner order state machine, and coordinates
reconciliation when the outcome is not known.

## What is here

| Path | Contents |
|---|---|
| `include/router/`, `src/` | the router: config and ROE, FIX 4.4 codec, normalizer and correlation, admission, outbox and dispatch, reconciliation, atomic store, audit, metrics, failpoints, invariant oracle |
| `include/deps/`, `src/p0*.cpp` | the dependency boundaries: P01 session engine with `StableSendV1`, P02 LP simulator, P03 order state machine, P05 risk |
| `apps/router_cli/` | `order_router_cli` — drive an order by hand, inspect chains, commands, cases, wire frames, audit, invariants |
| `apps/recon_operator_cli/` | `recon_operator_cli` — the operator console: list cases, print an escalation bundle, attempt a resolution |
| `tests/` | 437 scenarios: the full what-if matrix A–R, the 29 playbooks, the 20 crash windows, property walks, load and soak |
| `fixtures/fix44/` | golden application field blocks, used by the tests rather than described |
| `configs/roe_lab.yaml` | the Rules of Engagement snapshot in written form |
| `test-artifacts/` | evidence: per-case verdicts, the report, the latency and saturation figures |

## Build and run

```bash
make -j4                       # library, test binary, both CLIs
make test                      # the whole suite, writes test-artifacts/all.json
python scripts/report.py       # the coverage and release-gate report
```

Or with CMake:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j4
ctest --test-dir build --output-on-failure    # one entry per matrix section
```

Selecting scenarios:

```bash
./build/router_test.exe --list
./build/router_test.exe --filter=matrix-H          # one section
./build/router_test.exe --filter=CW-13             # one case
./build/router_test.exe --filter=playbook --priority=P0
./build/router_test.exe --artifact=test-artifacts/all.json
```

A quick look at the engine:

```bash
./build/order_router_cli.exe demo      # a full lifecycle, printed
./build/recon_operator_cli.exe         # stages an incident, then type 'help'
```

Warnings are errors on every translation unit. The test binary is statically
linked so it runs without the toolchain on `PATH`. Run the suite from the
project root: the fixture tests read `fixtures/fix44/` relative to the working
directory.

## Current state

The suite runs 437 scenarios. The last full run on this host: **431 pass, 0
fail, 6 BLOCKED**, with every P0 case that can run here passing. The blocked
cases and the two unmet release gates are listed with their reasons in
`test-artifacts/report.md` and summarised below.

This is lab evidence, not a production certification — the design document says
so and it stays true here.

### What is proven, and against what

The router is exercised against in-process implementations of its four
dependencies, each with its own durable image that survives a simulated crash:

- **P01** implements the `StableSendV1` contract the design document proposes
  but P01 does not yet have: `(session, command_id)` maps to at most one
  original sequence number, forever. Every issue-once claim in sections E, O and
  the crash windows is proven *against that contract*. `PB-03` runs the same
  router against a P01 build with the contract switched off and proves it
  refuses to claim the property instead of approximating it.
- **P02** is a deliberately separate implementation: its own field splitter, its
  own book, its own arithmetic, sharing no code with `router::fix` or
  `router::p03`. A one-sided parser or ledger bug shows up as a disagreement.
- **P03** is the single owner of order status and the fill ledger. The router has
  no code path that writes either.
- **P05** is a fake with scripted verdicts and idempotent reservations, and is
  not presented as a risk engine.

All twenty-six safety invariants (`R-I01`..`R-I26`) are checked by an oracle
that reads the persisted state, and it runs after **every step of every
scenario**, not just at the end. The one exception is stated where it is made:
the long random walks check the chain, ledger and dedup invariants every step
and the command-table ones on a fixed cadence plus in full at the end, because
that table only grows and scanning all of it per step is quadratic.

### What is not proven

Six cases are BLOCKED rather than passed, because passing them would need
something this host does not have:

| Case | Needs |
|---|---|
| `OR-R-16`, `PB-27` | a third-party FIX 4.4 engine (QuickFIX or a vendor acceptor) for genuine interop. The P02 cross-check is a second implementation, not an independent vendor. |
| `OR-P-12` | a real scheduler and a contended core |
| `OR-P-14` | a real thread pool to exhaust |
| `OR-P-15` | real sockets and files (P01 owns both) |
| `OR-P-17` | a day of wall clock plus RSS and FD sampling; `LOAD-01` is the scaled deterministic equivalent |

Two release gates are also unmet and say so in the report:

- **Sanitizers did not run.** `ASAN=1` is wired through the Makefile to
  `-fsanitize=address,undefined`, but neither sanitizer runtime links on this
  MinGW-w64 UCRT64 toolchain. That evidence needs a Linux or clang leg.
- **Third-party interop** is the blocked work above.

Where a scenario names a figure this harness cannot reach in a test that
finishes — 100,000 queued commands, 10,000 quarantined orphans — the configured
bound is lowered and then exceeded, so the same code path runs. Each such case
states the substitution in its own comment; nothing reports a benchmark it did
not measure.

### Measured on this host

From `test-artifacts/perf-*.json`, wall clock, one thread, `-O1`, debug build:

| Path | Rate | p50 | p95 | p99 |
|---|---:|---:|---:|---:|
| command admitted to intent persisted (`LOAD-02`) | 39,900/s | 23 µs | 32 µs | 65 µs |
| durable inbound to P03 commit (`LOAD-03`) | 32,700/s | 26 µs | 69 µs | 103 µs |

The saturation knee is exactly the configured queue bound — 500 accepted, 1,500
refused with `OVERLOADED`, nothing silently dropped, and the whole backlog
drains afterwards with one original send per accepted command.

Two cautions. These are debug-build figures on a single core with an in-memory
store; they are not the SLO, and design doc 2.3 is explicit that the CV's
`p99 95 µs / 50k msg/s` belongs to a different system. And the router's own
latency histograms are fed from the virtual clock, so they read zero here by
construction — they prove the instrumentation is wired, not how fast anything
is. The `wall_us` block is the real measurement, and the artifact says so.

## How the lab is built

Three things that make the matrix testable:

**The disks are separate from the process.** `StoreImage`, `p01::SessionImage`
and `p02::LpImage` are plain data that outlive the `Router`, `Store` and
`SessionEngine` objects. A crash destroys the process side and keeps the images;
a restart builds a new process side against them. That is what makes the twenty
crash windows mean something — if the router kept its state in the same object
as its store, a "restart" would prove nothing.

**Nothing sleeps.** A `VirtualClock` separates monotonic time (timeouts, backoff,
reconciliation budgets) from UTC wall time (`TransactTime`, `SendingTime`). A
scenario advances time explicitly, so a fault schedule is reproducible instead
of race-prone, and a wall-clock jump provably cannot disturb a timeout.

**Faults are named points, not timing tricks.** `FailpointRegistry` arms a named
spot in the code — `after_send_binding`, `inside_apply_txn`, `commit_ack_lost` —
on a counted schedule. The names map one-to-one to the crash windows in the
design document.

### A note on "concurrent"

Several matrix cases describe concurrent workers. The engine is a single owner
per chain by construction, so those races are modelled by interleaving the steps
explicitly, and where the real protection is a database constraint (`OR-L-13`)
by driving two transactions at the store directly. That is stated in the test
file rather than dressed up as a multi-threaded run, and the cases that genuinely
need threads are BLOCKED above.

## Bugs this suite found

Recorded because they are the reason for the work, and because each one is now
a test:

- A fill arriving after a confirmed cancel was applied, leaving a cancelled
  order with a non-zero remainder. Found by the random walk at a known seed; now
  held as a contradiction.
- A `LastPx` of zero passed both the dictionary and the reducer, putting a fill
  in the ledger that nothing justified.
- A replayed `ExecType=5` was re-validated against the lineage it had itself
  established, turning a harmless duplicate into a conflict. Dedup now
  short-circuits an already-applied execution before lineage re-validation.
- A status snapshot set a business outcome on the command, so a chain could look
  acknowledged with no acknowledgement behind it.
- A fill naming an unaccepted replace version was credited silently.
- An `OrderCancelReject` was recorded as an acknowledgement rather than a
  rejection of the request it answers.
- Hold and quarantine mutations were not journaled, so a replay diverged from
  the live state whenever anything had been held.
- A lost commit acknowledgement released the risk reservation of a command that
  had in fact been committed.
- `MsgSeqNum` uniqueness was checked without the session epoch, so a legitimate
  day-rollover reset looked like a double original send.
- Admission counted the whole outbox and the whole quarantine table per command,
  which made it quadratic; found by the throughput measurement, not by a test.

## Reading the output

Two distinctions the output keeps, because the design document insists on them:

```
command=K1 chain=ord-1 clordid=O-1 route=LP_SIM_01 delivery=ACCEPTED_DURABLE business=UNKNOWN
```

`ACCEPTED_DURABLE` means the intent is on disk. It does **not** mean the LP has
the order. And `UNKNOWN_TRANSPORT` means nobody knows whether the bytes arrived
— it is not a rejection, and it never becomes one by timing out.
