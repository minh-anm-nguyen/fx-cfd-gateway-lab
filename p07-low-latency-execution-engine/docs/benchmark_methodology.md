# Benchmark methodology

How to produce a measurement from this engine that is admissible, and how to
tell when one is not. Design doc §15 and §27 are the source; this file is the
operational version.

The short form: a number on its own is not a result. A result is a number plus
the span it belongs to, the denominator it was computed over, the environment it
was taken in, and evidence that the run was correct. `replay_benchmark` produces
all five or it reports `NO_PERFORMANCE_CLAIM`.

## Before you measure anything

1. **Pick the span.** The seven spans of doc §3 measure different things and may
   not be added, averaged or substituted for one another.

   | Span | From | To |
   |---|---|---|
   | `L0` | validated inbound event | route intent eligible |
   | `L1` | enqueue | owner dequeue |
   | `L2` | owner starts | decision produced |
   | `L3` | transaction begins | durable commit confirmed |
   | `L4` | durable outbox confirmed | safe-to-send record exists |
   | `L5` | execution receipt | committed P03/P05 update |
   | `L6` | scheduled arrival | reconciled outcome |

   `L3` contains a disk sync. `L2` does not. A p99 that blends them is not a
   p99 of anything.

2. **Use the measured build.** `replay_benchmark` links `llx_measured`: no
   failpoints, no ownership guard. Check the manifest says
   `failpoints_compiled_in: false`. It will not let you claim otherwise —
   `performance_claim_admissible()` is false for a lab build.

3. **Use a release build.** `-DCMAKE_BUILD_TYPE=Release`. `micro_bench` prints a
   warning if you forgot; the debug build is several times slower and the ratio
   is not constant across the primitives, so a debug A/B can invert.

4. **Freeze the environment.** Compiler, flags, kernel, CPU topology, cpuset,
   governor, container limits. The manifest records what it can read; what it
   cannot read it marks unavailable rather than guessing.

## Running it

```bash
cmake -S . -B build-rel -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-rel
./build-rel/replay_benchmark --rate=50000 --count=200000 --repeats=5
```

Every run writes `reports/manifest-<run_id>.json`. Read the `verdict` field
before reading any latency figure.

### The arguments that change what the result means

- `--rate` is the *offered* rate, not the achieved one. The report prints both
  and never conflates them.
- `--repeats=5` is the minimum worth quoting. All runs are printed, with the
  spread; doc §15.2 step 4 requires reporting every run, not the best.
- `--closed-loop` is a diagnostic. It under-reports the tail by construction,
  and the manifest records `arrival_mode: closed_loop` so the result cannot be
  presented as an open-loop one.
- `--no-baseline` skips the correctness comparison. Gate 4 cannot pass without
  it: a performance result from a run whose state differs from the baseline is
  not a result.
- `--capacity` sweeps 256/1024/8192/65536. All four produce identical business
  state (`LL-265`), so a difference between them is a performance difference
  and nothing else.

## The six traps, and what stops each one here

| Trap (doc §15.3) | What stops it |
|---|---|
| Dead-code elimination | every measured loop consumes its result through `do_not_optimize` and prints a checksum |
| Coordinated omission | arrivals are scheduled, not offered on completion; the sample is scheduled-arrival-to-outcome |
| Observer effect | measured build has no failpoints; `micro_bench` prints the clock's own cost next to the figures |
| Denominator cheating | `OutcomeCounters` keeps offered, admitted, completed, rejected, held, duplicate, timed-out separately, and `balanced()` must hold |
| Unfair comparison | baseline and variant replay the same fixture hash; the config hash differs if any setting differs |
| Causal violation | a route intent is not an eligible send; `L4` starts only after the commit |

## Reading the verdict

`decide_verdict` implements doc §28's algorithm literally:

- **`INCOMPLETE`** — something was `BLOCKED` or `NOT_RUN`. Not a failure of the
  engine. It is the honest state of a run that could not execute every gate,
  which is the normal outcome on a host without ARM64, `libtsan`, NUMA or
  PostgreSQL.
- **`NO_GO`** — a case that ran, failed, or an invariant was violated. Hard
  block.
- **`NO_PERFORMANCE_CLAIM`** — the engine is correct but the measurement is not
  sound: lost samples, unbalanced accounting, too few samples for the quantile,
  or a failpoint build.
- **`OPTIMIZATION_REJECTED`** — a regression beyond the accepted budget.
- **`CANDIDATE_FOR_P08_INTEGRATION`** — every gate satisfied.

## Quantiles

The histogram keeps raw bucket counts and emits them in the manifest, so a
distribution can be re-binned later. The reported percentile is the upper bound
of the bucket it falls in: exact below 64 ns, and at most 1.6% above the true
value above that.

`sufficient_for(p)` refuses a quantile the sample count cannot support — about
1,000 samples for p99 and 10,000 for p99.9. A p99.9 from n=1,000 is not a
number, and the benchmark says so in the output rather than printing it
unqualified.

## The workload matrix of doc §27.3

| Run set | What it needs | Status here |
|---|---|---|
| `MICRO-01` | 5 × 2 min | `micro_bench`, runnable |
| `SUST-01` | 5 × 5 min per load | `replay_benchmark --repeats=5`, runnable |
| `BURST-01` | 30 min burst sweep | partially: the burst shapes are in the fixture mix, the 30-minute window is not |
| `DB-01` | real PostgreSQL | **BLOCKED** — no database. The journal-backed store is not a substitute and its `L3` must not be quoted as a durable-commit latency |
| `SOAK-01` | 8–24 h | **BLOCKED** — the bounded-resource properties a soak would expose are checked directly (`LL-098`, `LL-020`, `LL-090`), but hour-scale drift is not covered |
| `CHAOS-01` | 36 FI + 24 CW with seeds | runnable: the fault and crash suites, 28 of 36 and 24 of 24 |
| `CROSS-ARCH` | x86_64 + ARM64 | **BLOCKED** — x86_64 only, so the weak-memory cases cannot be run |

## Measured on this host

Release build, GCC 16.1 (mingw-w64 UCRT64), 8 logical CPUs, Windows. Primitive
costs from `micro_bench`, median of 7 repetitions. **These are not the
pipeline's latency** and must not be added to a span figure.

| Primitive | median ns/op | spread |
|---|---|---|
| `SpscRing` push+pop, same thread | 11.2 | 630% |
| `SpscChannel` direct (template) | 34.0 | 133% |
| `SpscChannel` via `IQueueChannel` (virtual) | 23.6 | 173% |
| `Pool` lease+release (preallocated) | 19.9 | 20% |
| `new`/`delete` (malloc baseline) | 94.8 | 53% |
| `MonotonicClock::now_ns` | 72.3 | 72% |
| `reduce()` a new order | 25.6 | 122% |

Three findings, two of which answer open questions in the design doc.

**The pool is about 4.8× cheaper than `new`/`delete`** (19.9 against 94.8 ns),
with by far the tightest spread of anything measured. That is the measured
evidence doc §16 demands for the "preallocated pools" row, and it supports
ADR-04 on this host.

Getting that number required fixing the benchmark first. An earlier version
measured `new`/`delete` at **0.47 ns per pair**, which is impossible: C++14
onward permits a compiler to elide a `new`/`delete` pair whose allocation does
not escape, and GCC did. `do_not_optimize` on the *value read through* the
pointer was not enough — the pointer itself has to escape. This is trap one of
doc §15.3 appearing inside this project's own benchmark, and it is why every
measured loop here also prints a checksum.

**The virtual channel is not measurably more expensive than the direct
template.** Doc §18 warns that virtual dispatch on the hot path is a cost to be
measured rather than assumed away, and leaves open whether the indirection
should be removed. On this host it should not: the virtual form measured
*faster* than the template one, which at spreads of 133% and 173% means the
difference is below the noise floor. Removing `IQueueChannel` would cost the
runtime capacity sweep and buy nothing demonstrable. That conclusion is
host-specific and the measurement should be repeated on the target Linux box.

**The clock costs more than most of what it would measure.** A reading is 72 ns;
`reduce()` is 26 ns and a ring round trip is 11 ns. So an `L2` span on this host
is dominated by its own instrumentation, and the `L1`/`L2` figures
`replay_benchmark` reports are an upper bound that includes two clock readings
rather than a measurement of the work. `LL-206` passes — the clock is not too
coarse to be usable — but anyone quoting an `L2` p99 from this host has to say
this. On Linux with `CLOCK_MONOTONIC_RAW` the reading is typically well under
30 ns and the ratio improves, which is another reason the target platform
matters.

The 20–630% spreads are themselves a result: this is a shared desktop with no
CPU isolation, no pinning and other tenants. Doc §15.2 requires reporting every
repetition rather than the best, which is why the table carries the spread
column and `micro_bench` prints all seven runs.

### The pipeline

`replay_benchmark --rate=20000 --count=20000 --repeats=3`, Release, memory-only
journal, no pinning. Spans in nanoseconds, p50 and p99 from the run with the
median p99.

| Span | p50 | p99 | n |
|---|---|---|---|
| `L1` queue wait | 403 | 1,503 | 19,226 |
| `L2` decision compute | 607 | 2,527 | 19,226 |
| `L3` durable admission | 2,111 | 20,735 | 19,226 |
| `L0` receive to route | 3,711 | 2,588,671 | 19,226 |
| `L5` reconciliation | 4,543 | 2,555,903 | 3,311 |
| scheduled arrival to outcome | 6,527 | 2,654,207 | 20,000 |

Goodput 19,227 completed/s against 20,000/s offered; the 774-event difference is
the fixture's deliberately invalid events, counted as `rejected_invalid`.
Accounting balanced, zero lost samples, baseline state hash matched in all three
runs.

The p99 across the three runs was 2.65 ms, 2.65 ms and 11.67 ms — a 339%
spread. That is the honest headline: on an unisolated shared desktop the tail is
dominated by whatever else the machine is doing, and no amount of care inside
the engine changes it. The p50 figures are stable; the p99 is not, and a p99
from this host should not be quoted as a property of the engine.

Note also that `L3` (2.1 µs) is three times `L2` (0.6 µs) even with a
memory-only journal. On a real database it would be orders of magnitude larger.
That ratio is the whole argument of doc §13: the durable commit dominates, so
optimizing the compute path further buys very little, and `DB-01` is the run
that would actually matter.

### What this benchmark found

Both of these were real bugs in the engine, found by running the measurement
rather than by reading the code.

**`claim_next` was O(rows).** It scanned the outbox from row 0 on every call, so
dispatch was quadratic in the number of orders. The first run of this benchmark
reported 2,041 completed/s against 20,000/s offered, with a p50 of 1.24
*seconds* and 68,039 lost telemetry samples. Fixing it to advance a monotone
hint and look up `mark_*` through an index map took goodput to 19,227/s and the
p50 to 6.5 µs — about 190× on the throughput.

This is the case for doc §15's insistence on open-loop arrivals. A closed-loop
generator would have offered the next event only after the previous one
completed, measured a per-event latency that looked unremarkable, and never
revealed that the engine could not keep up with its offered rate at all.

**The harness itself was losing samples.** The benchmark drained the telemetry
lane only at the end of a run, so the 8,192-sample ring overflowed and every
run reported incomplete coverage. The gate correctly refused a performance
claim, but for a reason that was an artefact of the harness. It now drains as it
goes, and `lost_samples` is zero.

## What must never be compared

The design doc's source note records a CV figure: p99 receive-to-route 310 µs →
95 µs at 50k msg/s, in-process replay, 8-core Linux. `LI-29` forbids comparing
anything here with it unless the span definition *and* the workload are
equivalent, and they are not — different fixture, different hardware, different
span boundaries. Every manifest this engine writes carries a note saying so, so
the comparison cannot be made accidentally downstream.
