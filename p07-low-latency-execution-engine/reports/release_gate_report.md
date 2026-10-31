# Project 07 release-gate report

Artifacts read from `build/test-artifacts`.

## Verdict

**INCOMPLETE** -- 22 P0 case(s) BLOCKED (dependency absent)

Read this verdict as design doc 28 defines it. `INCOMPLETE` is not a
failure of the engine: it means some case could not be executed here and
its evidence does not exist, which doc 23 requires to be reported as
BLOCKED rather than assumed. `NO_GO` means a case that did run, failed.

## Totals

| Verdict | Count |
|---|---|
| PASS | 237 |
| FAIL | 0 |
| NOT_RUN | 0 |
| BLOCKED | 22 |
| INCONCLUSIVE | 0 |
| **total** | **259** |

## By priority

| Priority | PASS | FAIL | BLOCKED | INCONCLUSIVE | NOT_RUN |
|---|---|---|---|---|---|
| P0 | 236 | 0 | 22 | 0 | 0 |
| P1 | 1 | 0 | 0 | 0 | 0 |

## By oracle

Oracle codes are from design doc 23: Q queue, M memory, C causality,
R risk, D durability, F FIX session, P performance evidence, S safety
mode, O operations.

| Oracle | PASS | FAIL | BLOCKED | INCONCLUSIVE |
|---|---|---|---|---|
| C | 57 | 0 | 6 | 0 |
| D | 101 | 0 | 2 | 0 |
| F | 13 | 0 | 1 | 0 |
| M | 55 | 0 | 8 | 0 |
| O | 30 | 0 | 7 | 0 |
| P | 34 | 0 | 7 | 0 |
| Q | 54 | 0 | 8 | 0 |
| R | 55 | 0 | 0 | 0 |
| S | 47 | 0 | 2 | 0 |

## Blocked

Each of these names the specific capability this environment lacks.
Doc 23 defines BLOCKED as 'dependency absent' and is explicit that it
is not a pass.

- **LL-471** (P0) NUMA first-touch placement is compared local against remote
  - this host reports no NUMA nodes, so there is no remote node to place memory on and the A/B of doc 10's experiment matrix cannot be run. mbind/set_mempolicy are Linux-only and the design doc keeps them optional for exactly this reason (doc 11 step 4)
- **LL-472** (P0) isolcpus and nohz_full reduce measured scheduler jitter
  - requires kernel boot parameters on a disposable host. Doc 11 step 5 allows it only there, and doc 22 requires an operator change plan with rollback. Not runnable in this environment
- **LL-473** (P0) SCHED_FIFO placement is evaluated with a watchdog
  - ADR-07 and doc 11 forbid setting a real-time policy automatically, and this project never does. Evaluating it needs host isolation, a watchdog and an operator-approved rollback plan
- **LL-268** (P0) the optimized pipeline is compared against a real PostgreSQL commit
  - doc 15.1 keeps 'mock in-process deterministic P06' and 'real PostgreSQL sync local commit' as separate span classes, and run set DB-01 of doc 27.3 needs the real database. No PostgreSQL is available here, and the journal-backed store is explicitly not a substitute: its L3 numbers may not be quoted as durable-commit latency
- **CW-25** (P0) the journal survives a host power loss mid-write
  - the store flushes and then fsyncs (FlushFileBuffers on Windows), which survives process death and is what CW-11..CW-24 above exercise. Power loss is a different claim: it needs real hardware with write-cache control, and doc 26 scopes durability beyond the fsync contract out of the lab
- **CW-26** (P0) the same windows hold against a real PostgreSQL transaction
  - doc 15.1 keeps the in-process store and a real database as separate span classes, and run set DB-01 of doc 27.3 requires PostgreSQL. P06 owns that implementation; until its adapter exists the integrated durability gate is BLOCKED, which doc 23 defines as 'dependency absent'
- **FI-02** (P0) an incorrect-publication mutant is caught by a portability test
  - needs a weak memory model (ARM64) or a model checker. On x86_64 TSO a release downgraded to relaxed is not observable, so building the mutant here and watching it pass would be evidence of nothing. LI-30 says this explicitly: passing on x86 does not establish portability
- **FI-21** (P0) NUMA remote first touch is measured against local placement
  - this host reports no NUMA nodes, so there is no remote node to bind memory to. mbind/set_mempolicy are Linux-only and doc 11 step 4 keeps the placement optional for exactly this reason
- **FI-22** (P0) CPU hotplug under load degrades safely and invalidates the benchmark
  - requires offlining a CPU through /sys/devices/system/cpu/cpuN/online on a disposable Linux host. The detection half is implemented and tested (LL-151 proves plan_still_valid reports the drift); triggering the hotplug itself is not available here
- **FI-23** (P0) an SMT sibling noisy neighbour is quantified
  - needs control over which logical CPUs are loaded and an idle sibling to compare against, on a host with no other tenants. The planner's sibling avoidance is tested (LL-147) but the performance effect cannot be measured here
- **FI-24** (P0) an IRQ and scheduler storm is survived with bounded tail growth
  - requires /proc/irq/*/smp_affinity control and a load generator outside this process. Doc 11 step 5 restricts IRQ steering to a disposable benchmark host with an operator rollback plan
- **FI-26** (P0) cross-socket TSC skew is detected rather than assumed away
  - this project deliberately does not use the TSC: ADR-05 chooses the OS monotonic clock precisely because invariant-TSC checks, serialization and migration handling would all have to be proven first. There is also only one socket here, so there is no skew to detect. The clock provenance that makes the decision auditable is tested in LL-202
- **FI-29** (P0) thermal throttling during a sustained 50k/s run is detected
  - requires a sustained load this host cannot hold for long enough, plus access to thermal and frequency counters. Doc 11 step 6 wants the governor, turbo and thermal state documented per run; on this host those are not readable, so a throttling claim cannot be made either way
- **FI-35** (P0) a 24-hour soak with periodic faults shows no resource drift
  - run set SOAK-01 of doc 27.3 asks for 8 to 24 hours, which cannot run inside a test suite. The bounded-resource properties a soak would expose are checked directly instead (LL-098 for pool capacity, LL-020 for queue occupancy, LL-090 for allocation), but a drift that only appears over hours is not covered and this entry stays BLOCKED
- **LL-511** (P0) the pipeline runs against the real P01 FIX session and P02 LP simulator
  - P01 and P02 are separate projects and their adapters are not wired in. Doc 23 is explicit that 'for external protocol semantics P01 and persistence P06 adapters must be implemented before enabling integrated release gates', and doc 28 Gate 1 depends on them. Until then this engine is tested against its own reference models and the integrated gate is BLOCKED, not passed
- **LL-025** (P0) a non-power-of-two capacity is rejected before any allocation
  - enforced by static_assert((N & (N-1)) == 0) in spsc_ring.hpp. A static_assert is a hard error, not an SFINAE-detectable one, so no passing build can observe it from the inside. The evidence is the negative-compilation check run as the ctest case 'static_assert_negatives'; this entry stays BLOCKED here so the verdict is not claimed by a test that never exercised it (doc 23)
- **LL-026** (P0) N=0 or N=1 must not produce a valid ring
  - enforced by static_assert(N >= 2) in spsc_ring.hpp, which cannot be observed from inside a passing build. scripts/check_static_asserts.sh compiles the negative cases and asserts the build fails; this case is BLOCKED here rather than reported as a pass from a test that never exercised it (doc 23)
- **LL-042** (P0) a relaxed-publication mutant must be caught by a portability test
  - FI-02 requires a weak memory model. x86_64 is TSO, so a missing release fence is not observable here however many schedules are run, and TSAN is unavailable on this mingw-w64 toolchain. Needs an ARM64 runner or a model checker; reporting a pass from this host would be false evidence (LI-30, doc 23 layer 3)
- **LL-043** (P0) ThreadSanitizer reports no race on the ring under stress
  - mingw-w64 ships no libtsan, so -fsanitize=thread cannot link on this host. The CMake option LLX_TSAN exists for a Linux runner. BLOCKED, not passed (doc 28 Gate 1 requires sanitizer evidence in the supported modes)
- **LL-049** (P0) the same stress passes on a weak memory model (ARM64)
  - LI-30: passing on x86_64 does not establish portability, because TSO hides a missing release fence. Needs an ARM64 runner, which doc 27.3 lists as the CROSS-ARCH run set. This host is x86_64 only
- **LL-050** (P0) ThreadSanitizer finds no data race in the stress run
  - mingw-w64 ships no libtsan, so -fsanitize=thread cannot link here. The CMake option LLX_TSAN is wired for a Linux runner. Doc 28 Gate 1 requires sanitizer evidence in the supported modes, and this mode is not supported on this host
- **LL-051** (P0) a model checker explores the full interleaving space of push/pop
  - needs CDSChecker, GenMC or a similar tool. Doc 23 layer 2 lists model-based coverage as a separate layer from the randomized stress above; the reference-model comparison in the model suite is the part that is runnable here

