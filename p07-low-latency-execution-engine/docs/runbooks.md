# Operator runbooks

The failure matrix and recovery procedures of design doc §21, as they apply to
this implementation. Each entry says what the engine does by itself, what it
will not do without an operator, and which test holds the behaviour.

The rule behind all of them, from doc §4: when safety can no longer be proven,
the engine gives up throughput, availability or both. It does not trade journal
or risk correctness for a better number.

---

## Reading the engine's state

```bash
./build/latency_fault_harness --scenario=fill_cycle --count=10
```

prints the mode, the mode transition log with a reason for each change, the
owner, store and dispatch counters, the outcome accounting, the self-check and
the business invariant check. That output is the first thing to capture in any
incident.

The fields that decide what to do next:

| Field | Meaning |
|---|---|
| `mode` | `WARMUP`→`NORMAL`→`WATCH`→`SHED_NEW`→`RECON_ONLY`→`HALTED` |
| `owner.unknown` | commits with no proven outcome; non-zero forces `RECON_ONLY` |
| `dispatch.ambiguous` | outbox rows that may already be at the LP |
| `dispatch.duplicate_original_attempts` | **must be zero**; non-zero is a hard block |
| `store.discarded` | torn journal records dropped on replay |
| `self check` | queue invariant, pool leaks, business state, accounting |

---

## Mode escalations the engine performs itself

### Queue sustained above the shed watermark → `SHED_NEW`

New orders are refused with `RejectedOverload`. Cancels still go through, since
they reduce exposure. Accepted execution receipts are still taken, or deferred
with `RetryLaterNoAdmission` if there is no capacity — never dropped.

Recovery is automatic once occupancy falls below the entry threshold by the
hysteresis margin and stays there for the dwell period. There is nothing to do
except find out why the consumer fell behind.

Tests: `LL-065`, `FI-09`.

### Consumer stalled, pool exhausted → `SHED_NEW`

Same shape. `pool_free` at zero with queue space available means the pool is the
binding constraint, which usually means handles are being leased and not
released. `pool.live` is the leak count.

Tests: `LL-083`, `FI-05`.

### Store unhealthy, or an unresolved unknown commit → `RECON_ONLY`

Immediate, with no dwell period. No new original may be issued. Receipts are
still processed, because they are the evidence that resolves the unknown.

**This does not clear itself.** See the reconciliation procedure below.

Tests: `LL-068`, `FI-14`, `FI-30`.

### Integrity violation → `HALTED`

A corrupted ring counter, an ownership-guard violation, or a business state that
fails its own invariant check. Immediate, no dwell. Nothing proceeds.

Leaving `HALTED` returns to `WARMUP`, so preflight and the replay check run
again before anything is admitted. It requires both
`mark_reconciliation_complete(true)` and `authorize_operator(true)`.

Tests: `LL-067`, `LL-074`, `LL-040`.

---

## Procedure: an unresolved `COMMIT_UNKNOWN`

The transaction may or may not have committed. Doc §13 and LI-17 forbid reading
it as a failure, because reissuing the business command would risk a duplicate
order.

1. Stop new admissions. The engine has already done this by entering
   `RECON_ONLY`.
2. Preserve the evidence: the journal file and the harness output. Do not
   restart yet.
3. Resolve by idempotency key, not by reissuing:
   `JournalStore::lookup_by_idempotency(key)`. The key is derived from the
   event identity, so it is recomputable from the event itself rather than
   remembered.
   - **Found, `Committed`** → the transaction did commit. The effect is in the
     state and the outbox row is `Ready`: the original may be sent exactly once.
   - **Not found** → it did not commit. Nothing happened; the command may be
     offered again under the same identity.
4. Only when every outstanding key is resolved: `mark_reconciliation_complete`,
   then `authorize_operator`. The engine resumes at `WATCH`, not `NORMAL`.

Never resolve this on a timer. `LL-073` is the case that proves it cannot be.

---

## Procedure: an ambiguous send

`dispatch.ambiguous` is non-zero, or an outbox row is `Ambiguous` after a
restart with `attempts > 0` and no `Sent` marker.

The LP may already be working the order. The engine will not send a second
original and will not mint a new `ClOrdID` (LI-18, ADR-09).

1. Establish what the LP has, from its side: an execution report, a drop copy,
   or an order-status request through P01.
2. **The LP has it** → `Dispatcher::reconcile(outbox_id, true)`. The row settles
   as `Sent` and the order proceeds normally.
3. **The LP does not have it** → `reconcile(outbox_id, false)` deliberately
   returns false and changes nothing. Re-offering the *same* durable record
   under the *same* `ClOrdID` is the only safe retry, and it is an
   operator-authorized action, not something the dispatcher does on its own.

A timeout is not evidence. It never converts to either answer.

Tests: `FI-16`, `LL-509`, `CW-19` to `CW-22`.

---

## Procedure: restart after a crash

1. Do not delete the journal. It is the authority for everything that survived.
2. Start a new engine over the same `journal_path`. `preflight()` replays the
   log, rebuilds the projection, fill ledger, risk reservations and outbox, and
   runs the invariant check.
3. If `preflight()` returns false, the replayed state failed its own invariants.
   Stop. Do not admit anything. This is a `HALTED`-class condition and needs
   P06 integrity sign-off.
4. Check the outbox. Rows with `attempts > 0` and no `Sent` marker come back as
   `Ambiguous`, not `Ready`, and belong to the ambiguous-send procedure above.
5. Verify the state hash against the pre-crash value if you have it. A replay
   of the same committed log must produce the same state (LI-39).
6. `store.discarded` non-zero means a torn record was dropped from the tail.
   That is correct and expected: the transaction it belonged to never
   committed. A bad record in the *middle* of a log would be corruption, and
   the replay stops at the first one rather than skipping past it.

Tests: `LL-441`, `LL-405`, `CW-11` to `CW-18`, `FI-32`, `LL-M07`.

---

## Procedure: a failover or a suspected second writer

1. Promote the new owner with `promote_to_epoch(n)`, which fences the store at
   `n`.
2. Anything the previous owner had in flight becomes an unresolved unknown
   rather than being declared done or failed — the engine does this itself, and
   it is why a failover lands in `RECON_ONLY`.
3. A receipt, event or outbox claim carrying an older epoch is refused at every
   layer: the owner, the store and the dispatcher each check it.
4. Work the reconciliation procedure for every key the promotion surfaced.

A local fence is sufficient for the single-host scope this project covers. It is
**not** sufficient for multi-host HA, which needs external fencing or a lease —
out of scope here, and stated rather than implied.

Tests: `LL-002`, `LL-015`, `FI-20`, `FI-33`.

---

## Procedure: affinity or placement drift

`plan.accepted` false after startup, or `plan_still_valid` reporting drift.

The allowed CPU set changed under the process — a cpuset change or a CPU
hotplug. Business processing is still correct on whatever CPU the OS chose, so
the engine does not stop. What is invalid is the *benchmark*: the placement it
would be attributed to no longer exists.

1. Mark the run invalid. Do not quote its latency figures.
2. Re-read the topology and rebuild the plan.
3. `unpin_current_thread` restores the process-wide mask if a rollback is
   wanted.

Never ignore an `EPERM` or `EINVAL` from a pin: they mean different things and
the operator needs to know which happened.

Tests: `LL-151`, `LL-153`, `LL-154`, `FI-22` (detection half only).

---

## What the engine will never do on its own

Listed because each one is something an operator might expect, and must not:

- reissue a business command after an unknown commit or an ambiguous send;
- release a risk reservation against a timeout rather than evidence;
- leave `RECON_ONLY` or `HALTED` without an explicit operator authorization;
- drop an execution report P01 has already accepted, under any load;
- set `SCHED_FIFO`, change IRQ affinity, or enable huge pages or `mlockall`;
- reload a capacity or watermark configuration in place — a new configuration
  requires a new engine, so a half-applied capacity cannot exist (`FI-34`);
- claim a latency result from a build with failpoints compiled in.
