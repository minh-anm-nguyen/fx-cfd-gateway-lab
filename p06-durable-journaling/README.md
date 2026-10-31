# Project 06 — Durable Journaling & Recovery Engine

An implementation of `docs/Durable_Journaling_Recovery_System_Design.md`: an
append-only domain journal, one atomic unit of work spanning the journal, the
dedup keys, the order and risk projections and the outbound intents,
deterministic replay with verified snapshots, a fenced outbox over a
stable-send boundary, and a recovery supervisor that will not enable dispatch
until it can show why that is safe.

## What runs here, and what does not

The design targets PostgreSQL 17. This machine has no PostgreSQL, no libpq and
no Docker, so:

- The **runnable backend is SQLite** with `journal_mode=WAL` and
  `synchronous=FULL`, which is the closest equivalent of the durability
  profile section 17.1 pins. The store logic is written against an
  engine-neutral interface (`include/jrnl/sql.hpp`) rather than against SQLite.
- `migrations/0001_core.sql` carries the **PostgreSQL 17 schema**, kept
  deliberately close to `src/schema.cpp` so the two stay comparable. Where the
  engines genuinely differ the reason is commented in both files.
- Every scenario that needs PostgreSQL's own WAL, replication or
  point-in-time recovery is reported **BLOCKED with its reason** in
  `docs/P06_TEST_REPORT.md`. None of them is counted as a pass.

That split is the honest line: what is tested is P06's decision logic, not
PostgreSQL's storage machinery.

## Layout

```
include/jrnl/      canon  hash  domain  store  replay  snapshot  outbox
                   stable_send  recovery  retention  config  metrics
src/               the implementations
migrations/        0001_core.sql, the PostgreSQL 17 target schema
configs/           recovery.yaml, the durability and gating floor
apps/              replay_verify  recon_cli  journal_bench
tests/             unit  crash  scenario  property  playbook
fixtures/          golden canonical encodings
scripts/           run_unit  run_matrix  run_crash_campaign
                   restore_drill  run_soak  run_sanitizers  report.py
docs/              P06_TEST_REPORT.md, generated from the suite artifacts
```

## Building and testing

Needs CMake 3.20+, Ninja, a C++20 compiler and sqlite3 development files. On
the MSYS2 UCRT64 toolchain used here the sqlite3 DLL lives beside the
compiler, so the compiler prefix has to be on `PATH` when the tests run; the
scripts handle that.

```sh
./scripts/run_unit.sh            # configure, build, ctest, write the report
./scripts/run_matrix.sh --priority P0 --seed 20261030
./scripts/run_crash_campaign.sh --iterations 200
./scripts/restore_drill.sh
./scripts/run_soak.sh --hours 1
./scripts/run_sanitizers.sh      # trapping UB checks; see the note below
```

`run_sanitizers.sh` uses trapping undefined-behaviour checks rather than
UBSan proper, because mingw-w64 ships no `libubsan`. That is weaker, and the
script says so rather than implying a sanitizer ran.

## Design decisions worth knowing before reading the code

**A TxPlan carries events, not projections.** The store runs the pure
reducers itself, inside the transaction, and writes what they produce. That
is what makes "the projection equals a replay of the journal" (JI-11, JI-12)
true by construction rather than by convention, and it is why a test can
corrupt a projection row and watch the verifier find it.

**Risk is measured in quantity units.** A new order reserves its full
quantity, a fill consumes the filled quantity and leaves the rest held, a
terminal outcome releases what remains. Integers throughout, so "the risk
ledger equals the sum of unique verified fills" is an equality a test can
assert rather than a tolerance.

**The session store is a separate database file.** Section 6 names this as
the open integration blocker: if P01's store is not P06's store, no
transaction spans them and the contract degrades to a durable
`(session, command_id, payload_hash) -> original sequence` mapping written
before the socket write, plus at-least-once inbound delivery. Modelling it as
two files is what makes crash windows W13 to W21 reproducible instead of
asserted.

**`send_attempted` is committed before P01 is called.** Section 10 says a
claim timing out does not prove the send never happened. That column is the
only thing that can tell the two cases apart, and it decides whether an
expired claim returns to READY or becomes UNKNOWN.

**A quarantine commits; a rejection rolls back.** A contradictory receipt is
recorded as QUARANTINED with a case and an audit line, because discarding the
evidence would leave the receipt to be redelivered for ever and lose the only
record that the contradiction happened.

## Measured on this host, not a target

`journal_bench` prints commit and replay rates for whatever machine it runs
on and labels them as such. Section 19.1 is explicit that a number measured
anywhere other than the deployment target is not an SLO, so none is claimed.

One scaling characteristic the benchmark did surface and that a reader should
know: a risk domain aggregate holds every reservation it has ever issued, so
its canonical state grows with the number of orders in the domain and each
inbound report re-serialises all of it. At a few hundred reservations this is
already the dominant cost of applying a fill. Compacting terminal reservations
out of the aggregate would fix it, but it changes reducer semantics and
therefore needs a new reducer version and an upcaster (JI-37). It is recorded
here rather than changed late.

## Test results

`docs/P06_TEST_REPORT.md` is generated from the per-suite artifacts and is
the evidence for section 27's release gates. BLOCKED is a real verdict there,
with a reason, never a silent pass.
