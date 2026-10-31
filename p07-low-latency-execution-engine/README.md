# Project 07 — FX/CFD Low-Latency Execution Engine

Implementation of [docs/Low_Latency_Execution_Engine_System_Design.md](../docs/Low_Latency_Execution_Engine_System_Design.md).

C++20, CMake, no external dependencies. The design targets Linux; it builds and
runs on Windows with mingw-w64 too, and every platform-specific fallback is
reported rather than hidden — see **Platform honesty** below.

The project's own priority order, from the design doc, decides every trade-off
in here:

```
Correctness > Durability/Risk > Bounded Behavior > Tail Latency > Peak Throughput
```

An engine that is fast but loses a fill, releases risk early or sends a
duplicate order does not pass. That is why the queue refuses rather than
overwrites, why nothing reaches the wire before a durable commit, and why a
measurement that cannot be trusted is reported as `NO_PERFORMANCE_CLAIM`
instead of as a number.

## Build and test

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure
python scripts/report.py           # writes reports/release_gate_report.md
```

Two library variants are built from the same sources, because ADR-08 requires a
clean performance run, a profiling run and a sanitizer run to be separate
builds:

| Target | Failpoints | Ownership guard | Used by |
|---|---|---|---|
| `llx` | compiled in | compiled in | every test suite, `latency_fault_harness` |
| `llx_measured` | absent | absent | `replay_benchmark`, `micro_bench` |

The failpoints are what make the 36 fault playbooks and 24 crash windows
reachable at all, so the tests need them. They also put a branch and a mutex on
the hot path, so a measurement taken against `llx` is not a measurement — the
manifest records which variant ran and refuses a performance claim from the
wrong one.

Sanitizer builds:

```bash
cmake -S . -B build-asan -DLLX_ASAN=ON      # ASan + UBSan
cmake -S . -B build-tsan -DLLX_TSAN=ON      # needs libtsan: not on mingw-w64
cmake -S . -B build-ubtrap -DLLX_UBSAN_TRAP=ON   # trapping UB, no runtime needed
```

`LLX_ASAN` and `LLX_TSAN` together are a configure error: doc §23 layer 3
forbids linking both runtimes into one binary.

## Running it

```bash
# Open-loop benchmark with the baseline correctness comparison.
./build/replay_benchmark --rate=50000 --count=200000 --repeats=5

# One fault playbook by hand, with the evidence printed.
./build/latency_fault_harness --scenario=unknown_commit \
    --failpoint=store.commit_lost_ack=error
./build/latency_fault_harness --list-failpoints

# Primitive costs. Not the pipeline's latency.
./build/micro_bench
```

## What is here

```
include/llx/            the library
  spsc_ring.hpp         §8  bounded SPSC, acquire/release, no overwrite
  pool.hpp              §9  fixed-size pools, generation-tagged handles
  event.hpp             §18 canonical event metadata, fixed-point qty/px
  domain.hpp            §23 reference P03 reducer and P05 risk model
  store.hpp             §13 P06 atomic-commit port + journal-backed store
  dispatch.hpp          §13 outbox dispatch, send-ambiguity policy
  backpressure.hpp      §12 service modes, lane policy, admission credits
  owner.hpp             §7  the single-owner actor
  engine.hpp            §6  wiring, thread roles, lifecycle
  reference_pipeline.hpp §23 the deliberately plain baseline
  clock.hpp             §14 monotonic clock with provenance
  histogram.hpp         §14 histograms with retained raw counts
  telemetry.hpp         §14 bounded sampling, coverage validity
  affinity.hpp          §11 topology discovery, placement, verified pinning
  config.hpp            §20 profile parsing and validation
  manifest.hpp          §27 run manifest and the doc §28 gate algorithm
  failpoint.hpp         §25 deterministic fault injection
apps/
  replay_benchmark/     open-loop benchmark, writes reports/manifest-*.json
  latency_fault_harness/ operator tool for one scenario + one fault
tests/
  unit/ concurrency/ model/ differential/ integration/ fault/ crash/ benchmark/
configs/                five profiles, including one deliberately invalid
scripts/
  report.py             artifacts -> release-gate report
  check_static_asserts.sh  negative-compilation evidence for LL-025/LL-026
docs/
  adr.md                the ten ADRs, with what each one actually cost
  benchmark_methodology.md  how to produce an admissible measurement
  runbooks.md           the operator procedures of §21
```

## The three rules that shape the code

**Nothing reaches the wire before a durable commit.** `Engine::offer` publishes,
`step` decides and commits, `pump_dispatch` sends — in that order, as separate
stages. The dispatcher reads only committed outbox rows, under the current
fencing epoch. `LL-001` and `CW-16` are the cases that hold this.

**An ambiguous send is not a failed send.** After a write whose outcome is
unknown the outbox row becomes `Ambiguous`, no second original is issued, and no
new `ClOrdID` is minted. A timeout never converts into success or failure
(`LI-18`, `ADR-09`, `FI-16`, `CW-22`).

**An unadmitted command may be refused; accepted evidence may not be dropped.**
A `New` that has not reached durable admission gets `RejectedOverload`. An
execution report P01 already accepted gets `RetryLaterNoAdmission`, which
obliges the caller to hold it while the engine degrades. There is no outcome in
the enumeration that means "dropped" (`LI-20`, `LI-21`, `LL-071`).

## Platform honesty

The design targets Linux. This tree runs on Windows too, and every gap is
reported in the run manifest instead of being papered over:

| Capability | Linux | This host (Windows/mingw-w64) |
|---|---|---|
| Clock | `clock_gettime(CLOCK_MONOTONIC_RAW)` | `steady_clock`; manifest sets `target_platform=false` |
| Affinity | `sched_setaffinity` + read-back | `SetThreadAffinityMask`; read-back is the weaker set-twice idiom |
| cpuset / cgroup | `cpuset.cpus.effective` | unavailable; `capability.cpuset_cgroup=false` |
| SMT map | `/sys` sibling lists | `GetLogicalProcessorInformationEx` |
| NUMA | `mbind` / first-touch A/B | unavailable; the NUMA cases are `BLOCKED` |
| TSAN | available | no `libtsan`; those cases are `BLOCKED` |
| Durability | fsync | `FlushFileBuffers`; survives process death, not power loss |

`LI-28` and `LI-29` forbid comparing a measurement taken here with a Linux
baseline, and the manifest's clock note says so in the artifact itself.

## On the CV's 95 µs

The design doc's source note records a CV figure: p99 receive-to-route improved
from 310 µs to 95 µs at 50k messages/s, measured as an in-process replay on an
8-core Linux host. Nothing in this repository reproduces or compares against
that number, and `LI-29` is the reason: the span definition, the fixture and the
hardware are all different. `replay_benchmark` prints its own spans with their
own definitions and writes a note into every manifest saying exactly this.

## Test results on this host

15 ctest targets: 14 suites plus the negative-compilation checks. All pass, in
about 90 seconds.

| | |
|---|---|
| PASS | 237 |
| FAIL | 0 |
| BLOCKED | 22 |
| INCONCLUSIVE | 0 |
| **verdict** | **`INCOMPLETE` — 22 P0 cases BLOCKED (dependency absent)** |

Coverage against the design doc's catalogs: all **24** crash windows `CW-01`
to `CW-24`, **28 of 36** fault playbooks `FI-01` to `FI-36`, and cases drawn
from 20 of the 26 what-if groups. Full breakdown by priority and oracle in
[reports/release_gate_report.md](reports/release_gate_report.md), regenerated by
`scripts/report.py`.

The verdict is computed, never asserted. `INCOMPLETE` is the expected outcome
here and it is not a failure of the engine: it means 22 P0 cases are `BLOCKED`
because this host cannot run them — no ARM64 for the weak-memory cases, no
`libtsan`, no NUMA, no CPU-hotplug or IRQ control, no PostgreSQL for the
`DB-01` durable-commit span, no 24-hour window for the soak, and no P01/P02
adapters for the integrated gate. Doc §28 is explicit that `NOT_RUN` stays
`NOT_RUN` until runtime evidence exists, and every blocked case names the
specific capability it needs rather than being skipped.

Measured pipeline throughput, Release build, open-loop at a 20k/s target:
**19,227 completed/s**, p50 scheduled-arrival-to-outcome **6.5 µs**, accounting
balanced, zero lost samples, and the optimized pipeline's committed state hash
equal to the baseline's in every run. The per-span figures, the spreads and the
caveats that bound them are in
[docs/benchmark_methodology.md](docs/benchmark_methodology.md).

### Four bugs the tests found

Worth listing, because they are the argument for writing the catalog rather
than trusting the code:

- **`CW-17`** — inside one `run_until_idle` pass the service mode was evaluated
  *before* `step()` learned a commit had come back `UNKNOWN`, so the dispatcher
  put an original on the wire for a transaction whose outcome was unproven. An
  LI-16 and LI-18 violation. Dispatch eligibility is now decided at dispatch
  time.
- **`LL-045`** — `Pool::get()` mutated non-atomic counters and read non-atomic
  slot fields, so resolving a handle on the consumer thread raced the owner's
  `lease()`. Doc §9 has handles travelling across a queue, so resolution has to
  be thread-safe.
- **`FI-09` / `FI-10`** — a business flood filled the shared ring and the next
  FIX heartbeat was refused with `RejectedOverload`, which is exactly what
  LI-19 forbids. Session control now has a dedicated channel.
- **`LL-442`** — an intermittent heap corruption (`0xC0000374`, five clean runs
  then a crash): the owner and the dispatcher appended to the same journal
  vector. The durable path is now serialized.

And one the benchmark found that no test could: `claim_next` rescanned the
outbox from row 0 on every call, making dispatch quadratic. Every correctness
case passed throughout, because they process tens of events where the quadratic
term is invisible. It took an open-loop run at a target rate to expose it —
2,041 completed/s against 20,000 offered, p50 1.24 *seconds*. That is the case
for ADR-06 in one sentence.

## Scope

P07 owns the runtime: queues, pools, the owner actor, backpressure, telemetry
and placement. It does not own FIX session semantics (P01), the LP (P02), the
order reducer (P03), routing (P04), risk policy (P05) or durability (P06), and
`LI-01` forbids it from changing any of them.

What it carries instead are *reference models* of P03, P05 and P06, which is
what doc §23 prescribes: the differential suite replays one event stream through
the baseline and the optimized pipeline and compares the committed state hash,
so a difference can only have come from the parts P07 changed. The real adapters
are not wired in, and the integrated release gates stay `BLOCKED` until they
are — stated in `LL-511` rather than quietly skipped.
