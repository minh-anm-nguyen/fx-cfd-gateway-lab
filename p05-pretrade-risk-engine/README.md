# P05 — FX/CFD Pre-trade Risk & Exposure Reservation Engine

A client-side, conservative admission layer for an FX/CFD order flow. Before a
router may emit a risk-increasing `35=D` (New) or `35=G` (Replace), it must hold
a **durable reservation** issued by this engine — or a refusal with a typed
reason. After partial fills, cancel/replace races, lost acknowledgements and
restarts, the position and the outstanding commitment are counted exactly once.

Built from `docs/Pretrade_Risk_Exposure_Reservation_System_Design.md`. That
document is a learning reconstruction, and so is this: **every limit, rate and
price here is a lab figure.** It is not a broker risk policy, it is not wired to
any real venue, and `ALLOW_COMMITTED` means budget was reserved on the client —
not that a liquidity provider will accept or fill anything.

---

## What it does

| Area | Behaviour |
|---|---|
| **Admission** | `reserve_new` / `reserve_replace`, idempotent per `(tenant, account, command_id)`, one durable decision each |
| **Exposure** | `E = max(abs(P+B), abs(P-S))` and `G = abs(P)+B+S` per account-symbol, in base units, with conservative money conversion |
| **Limits** | per request, per chain, account-symbol, account, firm/global, rate, plus a kill switch |
| **Lifecycle** | partial fills, cancel (which releases nothing until confirmed), replace reserved at the worst branch, unknown outcomes, quarantine |
| **Durability** | one commit boundary covering the risk ledger, command dedup, event dedup, projection, audit and the router outbox |
| **Recovery** | append-only journal replay, torn-tail detection, and a gate that keeps admission closed until the state is verified |

### Deliberately out of scope

Market/stop/OCO orders, hedging or cross-margin accounts, real broker margin,
multi-region HA, and a PostgreSQL adapter. The spec's §6 gate applies: until a
P06 transaction boundary exists, only the single-node modes below are claimed.

---

## Build and run

Requirements: CMake ≥ 3.20, a C++20 compiler, and Python 3 for the differential
oracle. Developed and tested with GCC 16.1 (MSYS2 UCRT) on Windows 11; the code
is portable C++20 with a portable fallback for 128-bit arithmetic.

```sh
cmake -S . -B build -G Ninja
cmake --build build

# everything: 267 cases, the Python oracle, and the 8 scenario files
scripts/run-tests.sh

# one area
scripts/run-tests.sh unit
scripts/run-tests.sh crash

# or through ctest
cd build && ctest --output-on-failure
```

`scripts/run-tests.sh` sets the two environment variables the suite needs
(`RISK_CRASH_CHILD`, `RISK_MODEL_ARTIFACT`) and runs from the project root,
which the scenario and fixture tests rely on.

### Binaries

```sh
# inspect, validate, recover, measure
build/risk_cli validate-policy configs/risk_policy.lab.yaml
build/risk_cli inspect  --journal risk.journal
build/risk_cli recover  --journal risk.journal
build/risk_cli failpoints          # the 20 named crash windows
build/risk_cli bench --orders 50000 [--writers 8] [--journal path]

# run deterministic scenarios
build/risk_scenario fixtures/*.scenario
```

---

## Layout

```
include/risk/         public headers
  checked.hpp wide.hpp rational.hpp    checked integer and exact rational maths
  types.hpp hash.hpp clock.hpp         identifiers, canonical hashing, clocks
  policy.hpp config.hpp                the immutable ruleset and its loader
  market.hpp valuation.hpp             versioned feeds, freshness, conservative bounds
  exposure.hpp                         P/B/S, the E/G envelope, the limit hierarchy
  reservation.hpp                      the reservation record and state machine
  store.hpp memory_store.hpp
  journal_store.hpp                    the single commit boundary
  coordinator.hpp                      the single writer per risk domain
  recovery.hpp audit.hpp
  metrics.hpp failpoint.hpp
  scenario.hpp                         lab environment, fixtures, scenario DSL
src/                  implementations (~12.2k lines including headers)
apps/risk_cli         inspect / validate / recover / bench
apps/risk_scenario    scenario file runner
tests/                ~8.5k lines: unit, property, model, scenario, crash,
                      concurrency, chaos, plus the Python oracle
fixtures/*.scenario   8 deterministic scenarios
configs/              the lab policy, regenerable from risk_cli print-policy
docs/                 ACCEPTANCE.md (gate report), ADR.md (decisions)
```

---

## How the safety argument is structured

Three ideas carry most of the weight. Each is enforced by a mechanism rather
than a convention, and each has a test that fails if the mechanism is removed.

**1. One commit boundary.** `RiskStore::commit` is the only mutator, and it
takes the hold, the command record, the event dedup key, the projection, the
audit event and the router intent together. There is no API that writes the
hold without the intent, so the dual-write failure in §23.2 is not expressible.
The twenty named crash windows (`CW-01`..`CW-20`) are positions inside that
path; tests arm them in-process and, for the hard cases, in a child process
that leaves via `_Exit`, so the journal is observed exactly as it reached the
device.

**2. The outstanding quantity is defined, not accumulated.** A reservation's
contribution is `max(0, accepted − cum)` and, while a replace is in flight, the
maximum of that and `max(0, proposed − cum)`. A cancel request is recorded but
does not enter that expression at all, so "a pending cancel releases nothing"
is arithmetic rather than a rule someone has to remember. The committed
projection is maintained by delta for speed, and
`RiskCoordinator::cross_check_projection()` re-derives it from the whole
reservation ledger; the property suite runs that cross-check after *every*
step, so drift is caught immediately.

**3. Not knowing is not a denial.** A refusal is `DENY_DEFINITIVE` only when
the engine can prove nothing was reserved and nothing was sent. Anything
uncertain — a lost commit acknowledgement, a fenced writer, an unresolved
outcome — is `HOLD_RETRYABLE`, and a hold is never written down as a durable
decision. There is no `force_release`: the only path out of `UNKNOWN` or
`QUARANTINE` is `reconcile_with_evidence`, which needs verified quantities and
two distinct principals.

### The independent oracle

The property suite compares the engine against a differently-shaped model in
the same binary, which catches structural mistakes but not a misreading of the
spec. The real differential check is `tests/oracle/reference_model.py`: it
recomputes every exported quantity, envelope, notional and margin figure from
the specification's definitions using `fractions.Fraction`, shares no code with
the engine, and is run both by `scripts/run-tests.sh` and by the `oracle` ctest
entry.

---

## Status

All 267 cases, the 8 scenario files and the Python oracle pass. See
[docs/ACCEPTANCE.md](docs/ACCEPTANCE.md) for the gate-by-gate report, the
invariant and playbook coverage maps, the measured lab throughput, and — just
as importantly — the list of things this project does **not** demonstrate.
