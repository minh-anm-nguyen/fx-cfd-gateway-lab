# Architecture decision record

The ten decisions of design doc §30, as implemented, plus what each one actually
cost once it was built and tested. The design doc states the decisions; this
file records the consequences, including the four that only became visible when
a test failed.

---

## ADR-01 — One owner per risk domain

**Decision.** A single thread owns the ordering and hot state of a risk domain.
`OwnerActor` assigns `domain_seq` and the store enforces it.

**Rejected.** Arbitrary per-order mutation from many threads. It increases
contention on a shared firm limit and makes the cancel-cannot-overtake-a-fill
property a matter of timing rather than structure.

**Cost.** A hot domain is a bottleneck, and `owner_shards > 1` is refused by
config validation until the P05/P06 shared-limit proof exists (doc §30 open
question 3). `LL-469` is that refusal.

---

## ADR-02 — Fixed-capacity SPSC with explicit full handling

**Decision.** `SpscRing` is bounded, refuses when full, and never overwrites.
The producer is told and decides.

**Rejected.** An unbounded queue. It absorbs bursts, which is exactly the
problem: it converts a visible backpressure signal into an invisible heap and
eventually an OOM, and it hides the risk that caused the burst.

**Cost.** The producer has to handle a refusal on every push, and the asymmetry
between an unadmitted command and accepted evidence has to be encoded at every
call site. `LL-071` is the case that pins it.

**What it cost in practice.** The ownership guard that enforces LI-03 caught a
bug in this project's own test code: an early draft of `LL-028` popped from the
producer thread and was correctly refused as `WrongConsumer`. The guard works.

---

## ADR-03 — Durable commit before an original dispatch

**Decision.** An outbound `35=D/F/G` becomes eligible only after the store has
atomically written inbox, journal, projection, fill ledger, risk reservation,
command intent and outbox. `Engine::pump_dispatch` reads only committed rows.

**Rejected.** Speculative send. It is the fastest option and it is unsafe on
rollback or crash.

**Cost.** The durable commit is on the critical path to the wire, and no amount
of optimization elsewhere removes it. `CW-11` to `CW-16` are the evidence.

**What it cost in practice.** `CW-17` found a real bug. `run_until_idle`
evaluated the service mode, then `step()` processed an event whose commit came
back `UNKNOWN`, and the dispatcher in that same iteration still saw the stale
`NORMAL` mode and put the original on the wire — an LI-16 and LI-18 violation.
Eligibility is now decided inside `pump_dispatch` from current health, never
from a mode read earlier in the same pass.

---

## ADR-04 — Bounded preallocated pool with generation-tagged handles

**Decision.** `(epoch, slab, slot, generation)`. A stale generation is
`INVALID_HANDLE` and counted. A slot at the end of its generation range is
retired rather than wrapped, so an old handle can never alias a new object.

**Rejected.** Raw pointer handles. They cannot distinguish a live object from a
slot that has been reused, which is the ABA case of `FI-06`.

**Cost.** Retirement shrinks capacity over a very long run, which is visible in
`PoolStats::retired`. `LL-088` exercises it with an 8-bit generation.

**What it cost in practice.** `LL-045` found that `Pool::get()` mutated
non-atomic counters and read non-atomic slot fields, so resolving a handle on a
consumer thread raced the owner's `lease()`. Doc §9 explicitly has handles
travelling across a queue, so resolution has to be thread-safe: the slot's
generation and liveness are now atomic, and the rejection counters with them.
The threading contract is written down in `pool.hpp` rather than left implicit.

---

## ADR-05 — Monotonic clock with transparent span definitions

**Decision.** `clock_gettime(CLOCK_MONOTONIC_RAW)` on Linux, `steady_clock`
elsewhere, with the source, resolution and measured overhead recorded in every
manifest. Seven spans `L0`–`L6`, each named for what it measures.

**Rejected.** TSC/RDTSCP. It is faster, and using it would require proving
invariant TSC, serialization, cross-core stability and migration handling
first. None of that is free, and a cycle count without calibration is not a
nanosecond.

**Cost.** A clock reading costs more than a TSC read, which `micro_bench`
prints next to the things it measures. `LL-206` reports `INCONCLUSIVE` rather
than a number if the clock turns out too coarse for the spans.

---

## ADR-06 — Open loop for offered load, closed loop as a diagnostic

**Decision.** `replay_benchmark` schedules arrival *k* at `start + k·interval`
regardless of how the system is doing, and records the time from the scheduled
arrival to the outcome.

**Rejected.** Closed loop alone. It stops offering while the system is slow, so
the slow period contributes almost no samples and the tail disappears.
`LL-250` and `FI-27` demonstrate the difference on the same underlying data.

**Cost.** The generator busy-waits for the next due instant, which burns a core.
At microsecond intervals there is no sleep primitive fine enough, and a sleep
would dominate what is being measured.

---

## ADR-07 — Affinity optional and host-specific by default

**Decision.** `attempt_pinning` is off. When enabled, the planner builds from
the allowed CPU intersection, reserves at least one housekeeping CPU, pins, and
reads the mask back from the OS. A failure is recorded and invalidates the
benchmark; it does not stop the engine.

**Rejected.** Always-on pinning. It conflicts with cpuset, SMT, IRQ steering and
whatever else shares the host, and a pin that silently did not take is worse
than no pin because the benchmark gets attributed to a placement that never
happened.

**Cost.** Pinning has to be opted into per host, so a default run carries no
placement claim. `SCHED_FIFO` is never set automatically, and the three cases
that would evaluate it are `BLOCKED` with the reason stated.

---

## ADR-08 — Separate clean-performance, profiling and sanitizer builds

**Decision.** Two libraries from one source tree: `llx` with failpoints and the
ownership guard, `llx_measured` without. `LLX_ASAN` and `LLX_TSAN` cannot be
enabled together. The manifest records which variant ran, and
`performance_claim_admissible()` returns false for a failpoint build.

**Rejected.** One build for everything. The failpoint registry takes a mutex, so
a fault-injection build cannot produce a timing, and quoting one p99 from a run
that had all three concerns active confounds the result.

**Cost.** The test suites and the benchmark link different libraries, so every
change has to compile cleanly in both configurations.

---

## ADR-09 — Never issue a fresh original on an ambiguous send

**Decision.** After a write whose outcome is unknown the outbox row becomes
`Ambiguous`, the attempt count is durable, and no second original is issued
under any identifier. A restart sees `attempts > 0` with no `Sent` marker and
leaves the row to reconciliation.

**Rejected.** Treating a lost acknowledgement as a failed send. It is the
intuitive reading and it risks a duplicate order at the LP, which is the worst
outcome available.

**Cost.** An order can sit in `Ambiguous` indefinitely until evidence arrives.
That is the intended behaviour: `Dispatcher::reconcile` requires evidence, and
`FI-16`, `CW-19` to `CW-22` hold the line.

---

## ADR-10 — `RECON_ONLY` on integrity or ownership uncertainty

**Decision.** An unresolved `COMMIT_UNKNOWN`, an unhealthy store or ownership
doubt moves the engine to `RECON_ONLY` immediately, with no dwell period. An
integrity violation goes straight to `HALTED`. Leaving `RECON_ONLY` needs the
reconciliation finished *and* an operator decision; it never happens on a timer.

**Rejected.** Continuing to trade. It optimizes availability at the cost of
safety, which inverts the project's priority order.

**Cost.** A single unknown commit stops new business for the domain until
someone resolves it. `FI-14`, `FI-36` and `LL-073` are the cases.

---

## Added decision: a dedicated session-control channel

Not in the design doc's ADR register, because it only became necessary once the
tests ran.

**Problem.** `LI-19` requires FIX session control to stay serviced however busy
the business lane is, and doc §12 names the mechanism: "dedicated reserved
processing budget". With a single shared ring, a business flood filled it and
the next heartbeat was refused with `RejectedOverload`. `FI-09` and `FI-10`
failed, and they were right to: losing a heartbeat loses the session, and with
it the resend path that recovers everything else.

**Decision.** Session control has its own small bounded channel
(`kControlCapacity = 256`), drained ahead of the business queue. The watermarks
are computed from the business queue only, because a full control channel is a
session problem and not a reason to shed business work. If even the control
channel is full the answer is a deferral, never a rejection.

**Cost.** One more ring, and a control event that has been serviced now counts
as a terminal outcome in the accounting — without that the denominator did not
balance, which is what the second half of `FI-09` caught.

---

## Added decision: `invariant_ok()` reads the counter pair with a stability check

**Problem.** `LL-043` runs a third observer thread alongside the producer and
consumer. It reported invariant violations that had not happened: reading `head`
and then `tail` as two separate loads can give a torn pair, because the consumer
may advance `tail` past the `head` value just read, and the modular difference
then looks far larger than the capacity.

**Decision.** `invariant_ok()` reads head, tail, head again, and only compares
when head did not move — so `tail` was read while head held that value, which
makes the pair consistent. An observation that cannot be completed returns true
rather than reporting a violation, because an unprovable integrity violation
would halt the engine (doc §21), and halting on a bad *reading* would be worse
than the problem it claims to have found.

**Cost.** The check retries, so it stays off the hot path. `size()` remains
documented as approximate for an outside observer and is clamped to the
capacity, which is enough for a watermark decision.

---

## Added decision: the store serializes the durable path, and the outbox is indexed

Two consequences of doc §7 putting the commit sequencing on T2 and the outbox
handoff on T3, both of which only appeared once the threaded runtime and the
benchmark ran.

**The store needs a lock.** The owner commits while the dispatcher claims, and
both append to the same record log. Two threads pushing to one `std::vector` is
heap corruption, and that is what an intermittent `0xC0000374` in `LL-442`
turned out to be — it passed five runs in a row and crashed on the sixth.
`JournalStore` now takes an internal mutex for every operation touching the
record log, the outbox or the file. The cost is nothing worth measuring: the
commit it guards already performs an fsync. `state()` stays unlocked, because
only `commit()` mutates the projection and only the owner thread commits and
reduces.

The same investigation moved three other things off the shared path. The mode
controller is evaluated by the owner thread alone and published through an
atomic, because its transition log and non-atomic mode were being written from
two threads. The owner publishes its health inputs through atomics instead of
having another thread walk its maps. And `Engine::health()` no longer runs the
full business-state audit — walking every order and fill on every health
evaluation was a performance bug as well as a threading one; that check now
lives in `preflight`, `self_check` and the deterministic driver.

**The outbox needs an index.** `claim_next` scanned from row 0 every time, so
dispatch was O(rows²) over a run. The benchmark measured 2,041 completed/s
against 20,000/s offered, with a p50 of 1.24 seconds. A monotone
next-ready hint plus an id-to-position map took it to 19,227/s and a p50 of
6.5 µs. Rows are appended in id order and only ever move Ready → Claimed, so
the hint can never skip a claimable row.

Worth noting how this was found. No test failed: every correctness case passed
throughout, because they all process tens or hundreds of events where the
quadratic term is invisible. It took an open-loop benchmark at a target rate to
expose it, which is precisely the argument doc §15 and ADR-06 make for
scheduling arrivals rather than offering them on completion.

---

## Added decision: a rejected commit returns its sequence number

**Problem.** The owner assigns `domain_seq` before committing. When the store
refused the commit — a stale epoch, an out-of-order sequence, an injected stall
— the number was consumed but never recorded, so every later event for that
domain failed the store's causal check and the domain was wedged. It looked
like a correctness failure and was really bookkeeping.

**Decision.** `OwnerActor::finish_commit` returns the sequence number on
`Rejected`, and only when it is still the highest one handed out. Safe because
at most one commit per domain is in flight. `UNKNOWN` does *not* return it: that
record is durable, so the number was legitimately consumed.

**Cost.** None, beyond the invariant that the in-flight cap stays at one per
domain. `FI-13` is the regression test: after an injected stall the engine must
accept the next order rather than refuse it forever.
