# P05 acceptance report

Measured on GCC 16.1 (MSYS2 UCRT), Windows 11, `RelWithDebInfo`, 8 logical
cores. Reproduce with `scripts/run-tests.sh` from the project root.

```
risk_tests      267 cases, 0 failures
  unit          129    arithmetic, rationals, hashing, journal format, market data,
                       valuation, policy, config loader, store semantics
  scenario       94    admission, lifecycle, operations, the scenario fixtures
  crash          18    CW-01..CW-20 in-process plus out-of-process hard exits
  concurrency    11    shared-cap contention, idempotency, fencing, backpressure
  chaos           8    the playbooks no other suite injects, plus the coverage map
  property        6    P-01..P-16 over seeded conformant and adversarial traces
  model           3    trace export for the independent oracle
oracle          6 traces / 313 steps, every exported figure matches
scenarios       8 files, 0 failures
```

---

## 1. Release gates (§26)

| Gate | Verdict | Evidence |
|---|---|---|
| **G0** Mathematical core | **PASS** | `unit_arithmetic`, `unit_wide`, `unit_rational`, `unit_valuation`, `unit_exposure`. The §10.2 worked example is reproduced exactly (E=50,000 units → 5,500,000 minor; G=70,000 → 7,700,000; margin 385,000). `notional_is_never_below_the_exact_rational_value` checks 2,000 random inputs against exact integer arithmetic. |
| **G1** In-memory deterministic | **PASS** | `property_suite` over 8 seeds × 80 steps, asserting P-01..P-16 after every accepted event against a separately written model, plus `the_same_seed_produces_the_same_state_hash`. |
| **G2** Atomic admission | **PASS for the single-node journal; the P06 contract is not implemented** | `RiskStore::commit` is the only mutator and takes hold + intent + audit + dedup together, so the §23.2 dual write is not expressible. CW-01..CW-13 pass in-process and out-of-process. The spec's own gate stands: without a PostgreSQL P06 boundary this is `IN_MEMORY_LAB` plus a single-node append-only journal, and no multi-node claim is made. |
| **G3** Fill / release | **PASS** | `scenario_lifecycle`, 39 cases: transfer invariance, duplicate `ExecID` no-ops, contradictory `ExecID` quarantine, no early release on pending cancel or pending replace, replace promotion, the §23.3 table and the full §24.5 scenario. |
| **G4** Crash / fencing | **PASS for the implemented store** | All 20 crash windows, plus a child process that exits via `_Exit` at CW-06/10/11/12/13/14/16/18/19 so the journal is read exactly as it reached the device. `repeating_the_same_crash_is_deterministic` runs the same window three times and requires an identical recovered state hash. Fencing: `two_coordinators_cannot_both_write`, and the token is itself durable, so a restart cannot resurrect a stale writer. |
| **G5** Security / policy | **PASS** | `scenario_ops`, 22 cases: dual approval for policy changes and for reconciliation, role separation (a trader cannot touch the kill switch), tenant and account isolation, authorisation checked *before* the idempotency lookup so command ids cannot be probed, token forgery across five mutation shapes, and a self-verifying audit chain. |
| **G6** Capacity / overload | **PASS with the numbers published below** | Hard caps are enforced and refusals are typed: `the_admission_concurrency_cap_is_enforced`, `the_backlog_cap_stops_new_risk`, `resource_caps_bound_the_state`, `the_admission_rate_limit_is_enforced`. No safety violation at saturation — every BUSY answer commits nothing. |
| **G7** End-to-end integration | **NOT CLAIMED** | There is no P01 FIX session, no P02 LP simulator and no P03 reducer in this repository. The engine is driven through the P03/P04 *event contract* (`VerifiedOrderEvent`), which is the right seam, but a real `New → partial fill → G/F → reconnect → reconcile → restart` run across six processes has not been demonstrated. |

---

## 2. Safety invariants (§2.3)

All 32 are enforced in code and covered by at least one case. The mechanism
matters more than the test, so both are named.

| | Invariant | Mechanism | Case |
|---|---|---|---|
| RI-01 | Reservation before send | the outbox intent only exists inside a committed transaction | `happy_path_commits_hold_intent_and_audit_together`; every suite asserts zero orphan intents |
| RI-02 | Stable command identity | `(tenant, account, command_id)` maps to one canonical hash | `same_id_different_payload_is_a_conflict`, `concurrent_conflicting_payloads_change_nothing` |
| RI-03 | Single accounting | the `risk_applied_events` dedup key | `a_duplicate_exec_id_is_a_no_op`, `the_same_fill_from_many_threads_applies_once` |
| RI-04 | No early cancel release | `pending_cancel` is not part of `risk_effective_remaining` | `a_pending_cancel_does_not_release_anything` |
| RI-05 | No early replace release | the remainder is the max of both branches | `replace_down_keeps_the_old_branch` |
| RI-06 | Conservative pending replace | max branch in quantity *and* valuation | `replace_up_reserves_the_maximum_branch`, `a_price_only_replace_upward_reserves_the_worse_price` |
| RI-07 | No double count on fill | position and remainder move in one delta on one row | `a_partial_fill_transfers_without_changing_the_envelope` |
| RI-08 | Net envelope | `net_envelope_units` | `spec_worked_example_10_2`, `transfer_invariance_holds_for_every_split` |
| RI-09 | Gross fallback, no cross-account netting | accounts are summed | `accounts_are_summed_never_netted`, `the_gross_proxy_can_be_the_binding_cap` |
| RI-10 | Monotone bounding | every narrowing is a ceiling | `notional_never_rounds_down`, `margin_proxy_rounds_up`, the Python oracle |
| RI-11 | No unbounded market | market orders refused at policy validation | `rejects_unsupported_safety_flags` |
| RI-12 | No stale data admission | monotonic TTLs on quote, FX and equity | `ttl_boundary_is_exclusive`, `a_stale_feed_denies_and_leaves_existing_holds_alone` |
| RI-13 | Checked arithmetic | `checked.hpp` / `wide.hpp`; no raw `+`/`*` on money | `unit_arithmetic` (9 cases), `notional_reports_overflow_instead_of_wrapping` |
| RI-14 | Identity isolation | the chain key includes tenant and account | `tenants_and_accounts_are_isolated` |
| RI-15 | Limit sum on one snapshot | domain lock plus a revision CAS | `a_shared_budget_is_never_oversubscribed`, `revision_cas_rejects_a_stale_expectation` |
| RI-16 | Deterministic policy | a canonical policy hash pinned per decision | `hash_changes_with_every_decision_relevant_field`, `the_same_seed_produces_the_same_state_hash` |
| RI-17 | Rejected New release | a reject after fills is contradictory | `a_new_reject_after_fills_quarantines` |
| RI-18 | Terminal after proof | the terminal report's cumulative must match | `a_final_cancel_with_the_wrong_cumulative_quarantines` |
| RI-19 | Idempotent event effects | same key plus same hash is a no-op; a different hash quarantines | `the_same_exec_id_with_different_content_quarantines` |
| RI-20 | A snapshot is not a fill | `StatusSnapshot` can only move a chain to UNKNOWN | `a_status_snapshot_never_creates_a_fill` |
| RI-21 | Correct/bust gate | `TradeCorrect` and `TradeBust` quarantine | `trade_correct_and_bust_are_refused` |
| RI-22 | No risk bypass | an HMAC token verified against durable state | `a_token_cannot_be_forged_or_reused_across_identities` |
| RI-23 | Commit atomicity | one `commit()`; the projection is cross-checked against the ledger | `cw05_to_cw11_roll_the_transaction_back_on_disk`; the cross-check runs after every property step |
| RI-24 | Fail closed on persistence | a commit failure is a HOLD and grants nothing | `a_storage_write_failure_grants_no_budget`, `a_lost_commit_ack_is_durable_and_recoverable` |
| RI-25 | No split brain | fencing enforced at the store, and durable | `a_fenced_writer_racing_the_new_leader_commits_nothing` |
| RI-26 | No silent auto-release | no timeout path mutates a hold | `an_unknown_outcome_keeps_the_worst_case_hold`, `a_day_rollover_releases_nothing` |
| RI-27 | Breach policy | a post-event breach forces NO_NEW in the same transaction | `a_mark_to_market_breach_forces_no_new` |
| RI-28 | Kill switch semantics | the mode is read on the serialised state | `kill_switch_modes_block_risk_increase`, `a_kill_switch_racing_admissions_is_linearised` |
| RI-29 | No double margin debit | margin is a function of the gross projection, not a running tally | `spec_worked_example_10_2` (margin 385,000 derived from G) |
| RI-30 | Versioned data | every audit event carries policy and snapshot versions | `every_mutation_is_audited_and_the_chain_is_unbroken` |
| RI-31 | Bounded faults | concurrency, backlog, working-order and rate caps | `the_admission_concurrency_cap_is_enforced` and three others |
| RI-32 | No unchecked recovery | admission starts closed; recovery opens it | `admission_is_closed_until_recovery_opens_it`, `recovery_keeps_admission_closed_while_anything_is_unresolved` |

---

## 3. Crash windows (§23.1) and playbooks (§22)

All 20 windows are named in `Failpoint` and listed by `risk_cli failpoints`.
`chaos_coverage.every_playbook_has_a_home` asserts the count is 20 and that
every window carries a `CW-nn` identifier, so the enumeration cannot drift away
from the spec.

- **In-process:** CW-01..CW-04 leave no trace; CW-05..CW-11 roll the whole
  transaction back and the journal confirms it; CW-12 is durable but unacked and
  recoverable by command id; CW-13..CW-16 keep the hold when dispatch is
  uncertain; CW-17..CW-19 never double-count an event; CW-20 releases exactly
  once.
- **Out-of-process:** a child exits via `_Exit` at CW-06/10/11/12/13/14/16/18/19.
  The harness first runs the child *without* a failpoint and checks the journal
  really contains the reservation, so "the child never started" cannot pass as
  "the child crashed where we asked".

All 30 playbooks map to a specific case via
`chaos_coverage.every_playbook_has_a_home`. The ones needing a fault no other
suite injects live in `tests/chaos/`: PB-07 (notional overflow), PB-11 (a corrupt
record is dropped whole), PB-23 (storage write failure), PB-26 (missing journal
tail, checked against six truncation points), PB-29 (backpressure plus a poison
event) and PB-30 (compound chaos across a restart).

---

## 4. Measured lab figures (§25)

Lab numbers from the environment at the top of this file. §25.2's conditions
apply: these include the full admission path through commit, and the engine is
single-writer per risk domain by design.

| Workload | Accepted/s | p50 | p95 | p99 | p99.9 | max |
|---|---:|---:|---:|---:|---:|---:|
| 50,000 admissions, in-memory, 1 writer | 6,740 | 200 µs | 200 µs | 500 µs | 5 ms | 10.0 ms |
| 20,000 admissions, durable journal (fsync per commit), 1 writer | 770 | 2 ms | 2 ms | 5 ms | 20 ms | 199 ms |
| 50,000 admissions, in-memory, 8 writers | 6,170 | 1 ms | 5 ms | 20 ms | 50 ms | 47.7 ms |

Scaling with the number of live working orders, in-memory, single writer:

| Live orders | Wall | Accepted/s |
|---:|---:|---:|
| 2,000 | 0.40 s | 5,035 |
| 8,000 | 1.28 s | 6,251 |
| 32,000 | 5.56 s | 5,760 |
| 100,000 | 17.4 s | 5,744 |

Flat, which is what §25.1 requires of the 100k-working-orders workload. Getting
there meant fixing two real quadratic paths that this benchmark found, both
recorded in [ADR.md](ADR.md) as ADR-08 and ADR-09: the state hash re-hashed
every record on every commit, and `commit()` copied the entire domain state in
order to be able to roll back. Before those fixes, 8,000 admissions took 298
seconds.

Eight writers give no throughput gain and a worse tail — exactly what §15.2
predicts for one writer per risk domain. Sharding by a domain that shares no cap
is the scale path; a shared global cap would need a formal allocator.

Every bench run ends by re-checking the projection against the ledger and
verifying the audit chain, and `risk_cli bench` exits non-zero if either fails:
a throughput number measured on an inconsistent engine is not a measurement.

---

## 5. What this does **not** demonstrate

Stated plainly, because a green suite invites the opposite conclusion.

1. **No P06 PostgreSQL adapter.** Durability here is a single-node append-only
   journal with `fsync` per commit. The spec's §6 gate therefore still binds
   G2/G4/G7: nothing about multi-node or multi-region consistency is claimed.
2. **No real end-to-end run.** There is no FIX session, LP simulator or order
   reducer in this repository. The P03/P04 event contract is honoured as a seam,
   which is the useful part, but G7 is untested.
3. **The state hash is a divergence detector, not a commitment.** Each
   collection contributes an additive multiset digest so that a commit is O(1)
   rather than O(records). That is order-independent and incremental, and it
   detects a missing, altered or extra record — but sums are malleable, so it
   would not resist an adversary who controls record contents. Integrity against
   a tampered journal comes from the per-record CRC and the all-or-nothing
   framing instead.
4. **Sanitizers have not been run here.** `RISK_SANITIZE_ADDRESS` and
   `RISK_SANITIZE_THREAD` exist and are mutually exclusive as §24.4 requires,
   but the MSYS2 toolchain used for these numbers ships neither ASan nor TSan
   for this target. The concurrency cases are barrier-started rather than
   timing-dependent, which raises the odds of catching a race but is not a
   substitute for TSan.
5. **No soak test.** §25.1 asks for 24–48 hours with RSS, FD and WAL tracking.
   The longest run here is seconds.
6. **The spec's 400-case matrix is not enumerated one-for-one.** The 326 P0 rows
   are grouped behaviours, and this suite covers them by mechanism: 267 cases,
   several of which sweep a table of variants, plus 8 seeded property traces and
   the differential oracle. Where a numbered item has its own case it is named
   above; where several rows share one mechanism, one case covers the mechanism.
   That is a reasoned mapping, not the literal 326.
7. **The margin figure is a lab proxy.** `ceil(gross notional × rate)`. It is not
   a broker margin model, carries no hedge offsets, and must not be compared
   with one.
8. **Two numbers in the spec's own example config contradict each other.** §19.3
   pairs a 100,000-unit maximum order with a 10,000,000 minor-unit notional cap,
   but 100,000 EUR at the lab price is about 11,110,000 cents, so no order of
   maximum size could ever have passed. `configs/risk_policy.lab.yaml` raises
   the notional cap to 15,000,000 and the quantity cap to 200,000 (the §24.5
   scenario needs a 150,000-unit replace) and says so in its header.
