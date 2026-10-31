# Verification status

Measured on this machine, not estimated. Reproduce with `python scripts/ci.py`.

- Host: Windows 11, x86-64, GCC 16.1.0 (MSYS2 UCRT64), `-std=c++20 -O1 -g`
- Store: `MemoryStore` (`durability_mode() == IN_MEMORY`)
- Date of this run: 2026-10-30

## Scenario suite

| | Count |
|---|---:|
| Registered cases | 378 |
| Passed | 374 |
| Failed | **0** |
| Blocked (reported, never counted as passes) | 4 |

The 378 comprise the design document's 340-case what-if catalog (groups A..Q,
twenty cases each), its 26 fault-injection playbooks (PB-01..PB-26), 8
property-based tests over generated histories (PROP-01..PROP-08) and 4 bounded
model-checking runs (MODEL-01..MODEL-04).

`scripts/report.py` parses 366 case IDs out of the design document (the 340
catalog rows plus the 26 playbook headings) and reports **no catalog case
missing from the run**: 366 of 366 are covered, 362 passing and 4 blocked. The
12 PROP and MODEL cases are extra coverage beyond the catalog and are listed as
such.

Per-group wall clock, single-threaded:

| Group | Time | Group | Time |
|---|---:|---|---:|
| A new order and first ack | 1.3 s | K corrections | 1.3 s |
| B lifecycle and precedence | 1.6 s | L durability windows | 0.8 s |
| C fill math | 0.9 s | M isolation and risk | 0.8 s |
| D fixed point | 0.8 s | N concurrency and pressure | ~150 s |
| E identity and lineage | 0.7 s | O adversarial input | 8.4 s |
| F idempotency | 1.0 s | P config and versioning | 3.2 s |
| G ordering and late events | 0.5 s | Q observability | 20 s |
| H cancel flow | 1.1 s | PB playbooks | 2.7 s |
| I replace flow | 0.5 s | PROP properties | 3.7 s |
| J adversarial interleavings | 0.5 s | MODEL state exploration | 1.6 s |

Group N dominates because N-17 drives a 50k-event burst across 500 chains.

## Bounded model check

In the suite, at depth 7:

```
states=1813 transitions=3279 deepest=7
committed=1928 quarantined=419 duplicate=707 rejected=217 held=8
PASS: no invariant violation inside the bound
```

Standalone, at depth 8 (`./build/order_model_check --max-depth 8 --order-qty 3
--max-fill 2`):

```
states=6233 transitions=12441 deepest=8
committed=6506 quarantined=1866 duplicate=3120 rejected=941 held=8
PASS: no invariant violation inside the bound
```

Every reachable state inside the bound was visited and checked against all
twenty-four invariants. Each decision category is non-empty, so the alphabet is
wide enough for the result to mean something: a search that only ever committed
would prove nothing about the refusal paths.

## Independent oracles

All three layers the design document asks for, except the one that needs a
project that does not exist yet:

1. **Journal oracle** (in-suite): re-derives CumQty and notional by walking the
   committed journal and re-parsing the raw event text. Agrees with the
   aggregate in every case that asserts it.
2. **Python reference model** (`tools/reference_model/model.py`): a second
   implementation, written by hand from the FIX 4.4 semantics rather than
   transliterated. `python scripts/differential.py` compares it against the C++
   on every golden trace: **8 traces, 0 disagreements**, agreeing on up to 9
   observables each (lifecycle, cum, leaves, accepted qty, fill count, version
   count, accepted ClOrdID, quarantine flag, reconciliation flag).
3. **External peer oracle**: needs P02. Not available.

## Golden traces

`./build/order_trace_replay tests/fixtures/traces/*.trace` — 8 of 8 PASS with 0
assertion failures. The traces cover J-01 (cancel against a fill), B-07
(replace preserving CumQty), A-18 (lost acknowledgement), C-06 (overfill),
K-02 (unsupported bust), L-10 (crash window W09), H-14 (fill exhausts quantity
while a cancel is pending) and I-20 (CumQty across three versions).

## Blocked cases, and why

All four are P0 and all four are blocked because the evidence lives in a
project that has not been built. None is counted as a pass, and
`scripts/report.py` treats each as a release blocker.

| Case | What it needs |
|---|---|
| M-11 | The P05 reservation transaction contract. P03 emits the release intent with a stable key, but there is no inbound risk event to reconcile a provisional release against. |
| M-14 | The P05 inbound contract. There is no risk-originated event in the v1 event model, so there is nothing to feed a stale revision through. |
| O-14 | P01's framer. A corrupt CheckSum or BodyLength is rejected before a business event exists; P03 never sees a frame. |
| O-15 | P01's session authentication. P03 has no CompID in its event model. The adjacent guarantee P03 does make — an authentic report from the wrong counterparty cannot mutate a chain — is O-16, which passes. |

## Release gates (design doc section 21)

| Gate | Status | Basis |
|---|---|---|
| **G0** design review | **Met for what this project owns** | The v1 Rules of Engagement are pinned in `Policy` and documented field by field in `config/order_core.yaml`; I01..I24 are executable; the P01/P04/P06 contracts exist as ports (`IAtomicCommit`, `EffectIntent`, the durable inbox). Freezing the RoE *with* P02 and P04 needs those projects. No FIX 4.2 ExecType behaviour leaked: B-13 and PB-20 assert it. |
| **G1** pure reducer correctness | **Met, except the sanitizer matrix** | All applicable P0 in A..K pass. Fixed-point boundary cases pass (C-15, C-16, D-10, D-20). The independent ledger oracle matches every valid event and there are no false terminal decisions. The model checker explores its whole bound with zero violations. The sanitizer matrix has not run here: see below. |
| **G2** integration with P01/P02/P04 | **Not satisfiable yet** | Those projects do not exist. What P03 can show on its own it does: unknown statuses and unsupported corrections always fail closed, duplicate replay and `PossResend` are no-ops, and nothing claims exactly-once across independent network and storage domains. |
| **G3** durable recovery | **Not met** | All sixteen crash windows W00..W15 are injected and every case asserts the faultpoint actually fired, but against an in-memory store. The claim needs P06 with a real PostgreSQL transaction, WAL and fsync. L-20 does show 100 restarts over one journal producing one canonical hash, and Q-17 shows 1000 recovery cycles leaking no bounded resource. |
| **G4** capacity and release | **Not met** | Depends on G2 and G3. Also outstanding: a 30 to 60 minute soak on pinned hardware (`scripts/ci.py --soak-seconds` reports a trend, it does not gate), a security review of the P01 credential boundary, and a recovery runbook. |

`scripts/report.py` reconciles the catalog ID set parsed out of the design
document against what actually ran and exits non-zero while any P0 is missing,
blocked or failing. With four P0 cases blocked, the gate reports **NOT
satisfied**, which is the honest result: 374 cases passing is not the same
claim as a release.

## Performance

Reported, never asserted against a target. The CV's 95-microsecond
receive-to-route figure belongs to a different system and is not reused.

Q-08 prints a measured `normalized event -> state commit` distribution. On this
host, over 803 samples on a chain whose ledger grows to 800 fills:

```
p50=206us  p95=634us  p99=6.3ms  max=17.0ms
```

Those numbers are dominated by the aggregate copy inside the commit, not by the
reducer: the commit stores the proposed state into the journal record and the
aggregate row, so cost grows with the ledger length. That is the quadratic
single-chain limitation listed below, visible as latency. They also come from a
`-O1 -g` build on Windows, which is not a benchmark configuration. Treat them
as a sanity check that the commit path is in the expected order of magnitude
and as evidence that the limitation is real, nothing more.

## Known limitations in the implementation

- **A single chain with a very long ledger is quadratic.** The reducer copies
  the aggregate, so one order accumulating many thousands of fills costs
  O(n²). The design document's answer is a bounded in-memory cache over a
  persisted ledger (section 15); until then the throughput case is tested
  across many chains (N-17) and the single-chain case is bounded (N-18, N-19).
- **The blind-retry guard is a heuristic.** F-08 is enforced by comparing an
  *intent fingerprint* (firm, account, symbol, side, quantity, price) against
  chains whose outcome is unknown. A real deployment should carry an explicit
  client retry marker instead of inferring that two New commands mean the same
  order.
- **Cross-firm ClOrdID correlation is ambiguous by construction.** A peer
  report carries no firm or business day, so if one ClOrdID is live on two
  chains the report is quarantined as ambiguous rather than attached to a
  guess. In a real deployment the session identifies the firm.
- **Per-account base currency is not modelled.** M-15 tests the mechanism that
  would protect it — the policy version and scale pinned per accepted version —
  rather than the field itself.

## Roadmap milestones (design doc section 22)

| Milestone | Status |
|---|---|
| M1 strong types, checked fixed-point math, symbols and ticks | Done. `math.hpp`, `decimal.hpp`, `policy.hpp`; groups C and D. |
| M2 aggregate, version chain, reducer for New/Ack/Reject | Done. Groups A and B. |
| M3 fill ledger, ExecID idempotency, invariant checker | Done. Groups C and F; I01..I24 in `invariants.cpp`. |
| M4 pending cancel and 35=9 handling | Done. Group H. |
| M5 pending replace, accepted-version rules, concurrent commands | Done. Groups I and J. |
| M6 status, unknown outcome, quarantine, reconciliation intents | Done. Groups G and Q; `SystemEvent`. |
| M7 P01/P02/P04 integration plus an independent oracle and runner | **Partial.** The independent oracle (Python) and the scenario runner exist and are green. The P01/P02/P04 integration cannot start until those projects do. |
| M8 P06 atomic commit, W00..W15 crash injection, CI and release gates | **Partial.** All sixteen windows are injected and asserted, CI and the gate report exist, and replay is hash-stable across 100 restarts — but against `MemoryStore`, not P06. |

The design document calls M1..M6 the MVP for this project. That is complete:
the library builds and the suite runs standalone. Crash-safe integration is
explicitly not claimed, because M8 is not finished.

## Sanitizers

Not run here, and the reason is the toolchain rather than a choice: this MSYS2
UCRT64 installation ships no `libasan`, `libubsan` or `libtsan`, so
`-fsanitize=address`, `-fsanitize=undefined` and `-fsanitize=thread` all fail at
the link step. The `make asan`, `make ubsan` and `make tsan` targets exist and
are deliberately three separate binaries (ASan and TSan must never share one,
per design doc 20.5); they are a Linux CI job and G1 is not complete without
them.

What was available and did run is a hardened-libstdc++ build, `make hardened`,
which turns on `_GLIBCXX_ASSERTIONS`: bounds and iterator checks inside the
standard containers, no runtime library required. It is not a substitute for
ASan — it sees nothing outside the containers — but it does catch the container
misuse that a layout or ODR mismatch produces, which is exactly the defect
described below. Result: **374 passed, 0 failed, 4 blocked**, identical to the
normal build, with no assertion triggered.

## Notes from the build

One defect worth recording because it is the kind that hides: the Makefile
originally generated header dependencies for the library and the test suite but
not for the `tools/` and `apps/` objects. A struct layout change therefore
relinked the tools against a stale object compiled under the old layout, and
`order_trace_replay` corrupted its own memory and crashed in a destructor —
with every test still green, because the suite's dependencies were tracked
correctly. The fix is in `Makefile` (`DEPS` now covers every object); the
symptom is the reason `scripts/ci.py` builds `all tools` together rather than
trusting an incremental state.
