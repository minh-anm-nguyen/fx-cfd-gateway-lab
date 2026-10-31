# P08 — Integration, Deterministic Replay & Failure Testing

The lab where P01–P07 exist as one system, per
[`docs/Integration_Replay_Failure_Testing_System_Design.md`](../docs/Integration_Replay_Failure_Testing_System_Design.md).

P08 is not a wrapper around the other seven. It is two halves: the integration
that makes them run together, and an independent verification plane that is
deliberately built from different code than the thing it checks (§11.1, EI-53).

## Build

```sh
cmake -S . -B build -G Ninja
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

`ctest` exports `TRADING_MODE=SIM_ONLY` per test. Running a binary by hand
without it is refused, which is the point — see the lab guard below. On Windows
the toolchain's libstdc++ and libwinpthread are DLLs beside the compiler, so a
binary run outside `ctest` also needs that directory on `PATH`.

The seven components are consumed where they live, via `add_subdirectory` on
`../p0*`. Nothing is vendored or copied, so a fix in `p03-order-state-machine`
is a fix here, and the version pinning §2 demands is a property of one checkout
rather than of a sync step somebody forgets to run. `FX_COMPONENT_ROOT`
overrides where they are looked for.

Each component's own suites and CLIs are skipped in this build: their
CMakeLists return early unless `<COMPONENT>_BUILD_TOOLS` is set, which defaults
to `PROJECT_IS_TOP_LEVEL`. A standalone build of any component is therefore
unchanged — all 119 of their tests are still there — while this build compiles
libraries only. That default is also what resolves the one hard collision
between the seven: P03 and P05 both define a CMake target named `tiny_test`.

## The gateway on a real socket

`apps/interop_gateway` is the same gateway with P04's in-tree LP removed: P01's
real session over `fixse::TcpConnector`, talking to P02's `lp_simulator` as a
separate process over loopback TCP. §6 is why it matters — P02 is "a peer that
may be faulty or hostile", not a library in the same address space, and until
frames cross a socket none of the network crash windows can be injected at all.

```
isolation (reconciled): READY
P01  session FIX.4.4:TRADER01:SIMLP01:DEV starting, epoch 1 next_in 1 next_out 1
P01  session ACTIVE
P04  accepted K-1 chain=ord-1 cl_ord_id=O-1
     dispatch sent=1 | inbound applied=0 quarantined=0
     dispatch sent=0 | inbound applied=0 quarantined=1
P01  final state STOPPED, next_in 4 next_out 4
P01  bridge delivered=1 unconvertible=0
P06  sequences allocated=1 unconfirmed=0 inbound unacked=0
P04  alert: inbound quarantined: SecurityQuarantine
P04  K-1 delivery=AWAIT_BUSINESS business=UNKNOWN
```

The LP's side of the same run: `order_chains = 1`, and the frame it sent back

```
35=8 37=SIM-1001 11=O-1 17=EX-000001 150=0 39=0 55=EURUSD 54=1 38=10000 14=0 151=10000 6=0.00000
```

So the order was admitted, risk reserved, a sequence bound durably, the bytes
left over a real socket, **a separate process accepted the order and created a
chain for it**, and its acknowledgement came back, was stripped of the session
envelope and persisted into P06.

Then P04 quarantined it, and it is right to. `SecurityQuarantine` is P04's
reason for "wrong account, tenant or CompID", and that report carries no tag 1:
nothing in it proves it belongs to the account that owns the chain. EI-08 wants
an LP response correlated to the right account scope, so failing closed is the
correct answer and P04 names it in an alert rather than applying on a guess.

That is a Rules-of-Engagement disagreement between two components, which is what
an interop run is for. §2 lists "FIX dictionary, RoE" and "account/symbol
policy" among the things that must be pinned, and resolving it is a profile
decision — either P02 emits tag 1 on its reports, or P04's profile is told to
correlate on ClOrdID alone for this peer. It is not a defect in the bridge: the
frame arrived intact and P04 applied its own dictionary to it, finding all eight
required tags present and the account missing.

Three other things the run had to be told, each a real divergence rather than a
setup detail:

- **the session identity.** P04 refuses a route naming a session other than the
  one it is attached to (OR-B-06 — sending one peer's order to another). Its lab
  default names `ROUTER04:LPSIM01:PRIMARY`; P01's config says
  `TRADER01:SIMLP01:DEV`. P01's values are the ones on the wire, so the route is
  reconciled to them and the isolation guard is re-run on the result.
- **the wire symbol.** P02 names its instrument `EURUSD`, as do P03's policy and
  P05's; P04's lab default quotes `EUR/USD`. Before this was set the LP recorded
  the request and created no order, replying `103=1 "unknown symbol"`. P04
  already models it: the client keeps saying `EUR/USD` and the alias table
  carries what goes in tag 55.
- **the clock.** P04 ships only a `VirtualClock` pinned to the lab epoch, which
  is right for its own suite and wrong against a live peer: P02 rejected the
  order with `103=8 "TransactTime too old"`. The interop app defines a real
  clock rather than changing P04's, so P04's determinism is untouched.

A fourth was not a mismatch but a FIX rule doing its job: restarting P01's store
without resetting P02's made the gateway open with MsgSeqNum 1 against a peer
expecting a higher one, and the LP refused the session. Both sides need clean
state together, or neither.

## The gateway

`apps/gateway/main.cpp` runs one New order through the assembled path and prints
what each component ended up believing. `ctest` runs it as `gateway_smoke`.

```
contracts: READY
isolation: READY
booted, mode=NORMAL
accepted: command=K-1 chain=ord-1 cl_ord_id=O-1
evidence: INTENT_RECORDED
  round 0: sent=1 unknown=0 held=0 | applied=1 dedup=0 quarantined=0 orphaned=0 invalid=0

P06  sequences allocated=1 unconfirmed=0 inbound unacked=0
P06  binding: command=K-1 original_seq=1 written=1
P05  stranded=0 scale_mismatches=0 unmapped_symbols=0
P05  firm gross notional=1111000 (minor units)
P04  mode=NORMAL original_sends=1 open_unknown=0 open_recon=0
P04  delivery=LP_ACKED business=ACKNOWLEDGED
     evidence now: LP_ACCEPTED
     may release reservation=0 may send original=0

EI-09/JI-19 one original per command: held
```

What is real in that run and what is not, because a smoke test that overclaims
is worse than none:

| | |
|---|---|
| **P04** | real — admission, routing, correlation, reconciliation |
| **P05** | real — `risk::RiskCoordinator`, through the adapter. The 1,111,000 is budget actually taken, with P05's conservative buffer over the 1,100,000 notional |
| **P06** | real — `jrnl::StableSendStore` holds the issue-once binding and the durable receipts, on SQLite |
| **P03** | **not yet** — P04's in-tree reducer still owns status here; Oracle A compares the two instead |
| **P02** | **not yet** — P04's in-tree `LpSimulator` stands in, in-process, no socket |
| **P07** | **not yet** — one thread, deliberately: a smoke test that is also a concurrency test tells you neither thing |

The last three printed lines are the evidence ladder doing work rather than
decorating. P04 tracks twelve delivery states; §2 asks which of six kinds of
evidence the gateway holds. `LP_ACKED` maps to `LP_ACCEPTED`, and at that rung
the reservation may **not** be released (EI-25 — not until `EXECUTED`) and no
further original may be sent (EI-09). Both answers come out of
`gw::evidence_of` and `gw/evidence.hpp`, not out of a comment.

Two states are worth singling out because they look like progress and are not:
`AwaitBusiness` means the kernel took the bytes and nothing more (EI-04), and
`UnknownTransport` and `Reconciling` both mean the outcome is unknown, which
EI-20 forbids resolving with a timer.

## Ownership, and the three gateway layers

The components were written separately and three of them grew a gateway-shaped
API with its own store and outbox: `jrnl::GatewayService`, `router::Router` and
`order_core::Engine`. §2 and §6.1 settle which one wins where, and the
assembly follows that table rather than picking a favourite:

| Component | Single owner of | Consequence for the assembly |
|---|---|---|
| P01 `fixse` | TCP/FIX session, sequence numbers, wire receipt | P04 inbound attaches to `fix::IApplicationSink::deliver` |
| P02 `lpsim` | LP orders, fills, the independent LP ledger | runs as a **separate process** over FIX — §6 treats it as a peer that may be faulty or hostile, not a sibling library |
| P03 `order_core` | order status and the fill ledger | P04's in-tree reducer leaves the write path |
| P04 `order_router` | command ids, FIX mapping, correlation, reconciliation | keeps admission and correlation, gives up status and risk |
| P05 `risk_engine` | risk budget, exposure, reservations | P04's `FakeRisk` becomes an adapter onto `risk::RiskCoordinator` |
| P06 `jrnl` | the atomic transaction, inbox, outbox, checkpoints | the stores inside P03 and P05 write through it |
| P07 `llx` | queue, pool and thread execution | may not bypass P05 or P06 to go faster |

P04 shipped its own implementations of P01, P02, P03 and P05 in
`include/deps/` and `src/p0*.cpp`, 1355 lines. Taking them off the write path
does not waste them: `src/p03_osm.cpp` is a 477-line order reducer that shares
no code with `order_core`, and `src/p02_lp.cpp` a 563-line LP, which is exactly
the independent reference model §11.1 requires and forbids deriving from the
system under test. They move to `harness/reference_model/` as Oracle A.

## What the contract gate checks

`contract_probe` is M1's gate (§29). It exits 0 on `READY` and 2 on `BLOCKED` —
distinct from 1, so a crash of the probe is never read as a verdict, because
EI-54 forbids a gate from counting `BLOCKED` as a pass.

Linking it is half the check. A header constant proves nothing about linkage,
and with static libraries the linker pulls in only what something references,
so `verify_contracts()` calls one out-of-line function from each of the seven
libraries. A duplicate symbol — two copies of sqlite3, say, since P01, P02 and
P06 each open their own — then fails at link time instead of mid-scenario.

The values it compares come from §2's compatibility matrix:

- **the numeric contract.** Five representations of a price meet in this
  binary and none converts to another for free: P02 holds ticks plus a
  per-symbol `PriceFormat`, P03 a mantissa and its own scale per value, P04
  int64 plus a per-symbol digit count, P05 exact rationals, P07 int64 against
  `llx::kPxScale`. P07's is binding because it is a compile-time constant: a
  symbol quoted to anything but 5 digits is representable in P04 and silently
  wrong in P07. EI-15 requires that to be rejected, not rounded.
- **the three store schemas.** P01, P02 and P06 each own a durable store with
  its own independent `kSchemaVersion`. Nothing else stops one from being
  migrated alone.

## The P04 → P05 seam

`gw::RiskAdmissionAdapter` puts P04's admission port onto the real engine, and
P04's lab fake — whose own header says it "khong duoc trinh bay la risk engine
thuc" — leaves the write path. The two were built apart and disagree in seven
ways, none of which is a compile error and all of which are a wrong number in an
account:

| Disagreement | What a pass-through would do |
|---|---|
| `router::Side` is `{Buy=0, Sell=1}`, `risk::Side` is `{Buy=1, Sell=2}` | **a sell becomes a buy** |
| the port carried no `order_chain_id` | a replace takes a second hold beside the New's instead of resizing it (EI-23) |
| the port sent a replace as a delta; P05 wants the absolute total | two views of one position drift apart |
| the port carried no `price_scale` | P05 reads `price_ticks` at a scale it assumed (EI-15) |
| `valid_until` is wall-clock micros, `dispatch_deadline_mono` is steady-clock nanos | the expiry check compares unrelated epochs, so a stale verdict looks eternally fresh |
| P04's symbol is `"EUR/USD"`, P05's instrument key is `"EURUSD"` | the instrument is not found |
| P05 has no `release()` at all | see below |

The first four needed a field on `router::p05::RiskRequest`, so the port gained
`order_chain_id`, `price_scale` and `total_units_after`, and `admit_new` now
peeks the chain id before asking risk rather than allocating it after.
`FakeRisk` ignores all three, so P04's own suite is unaffected.

**The release.** P05 deliberately offers none: budget falls only to a verified
terminal event or to an audited reconciliation, because EI-25 forbids releasing
on a disconnect, a timeout or an unknown status. `Committed → Final` is refused
by name in `validate_transition` — "a local timeout is not a terminal proof" —
and reaching `Final` through `InFlight` would assert a dispatch that never
happened. P04 is careful about when it releases (only on a commit that refused
outright, never on a lost acknowledgement), but it still has nowhere to send the
call. So the hold stays and the id is surfaced through
`stranded_reservations()`. Over-reserving costs availability; under-reserving
costs money, and §3 ranks business safety above bounded availability. The real
fix is EI-22: once the reservation and the command intent commit in one P06
transaction, a failed commit leaves nothing to hand back and the list cannot
fill.

**The cancel.** `admit_cancel` never consulted risk at all, so P05's
pending-cancel marker was never set and EI-24's headroom was not being held for
fills that can still arrive before the LP acts. `IRiskAdmission` gained a
default-no-op `note_cancel_requested`, called once the cancel commits durably.

`tests/test_risk_seam.cpp` asserts each of these against the real engine,
including that the side cast *would* have been wrong.

## The P04 → P01/P06 seam

P04's session port asks for issue-once keyed by command id — `send_once()`,
`lookup_binding()` — and for a durable inbox it can acknowledge after its own
commit. Its header says that state belongs to P01.

**P01 does not have it.** `fixse::SessionEngine::submit_app_message()` returns
Accepted or a refusal and no sequence number, and `fixse::OutboundRecord` is
`{seq, msg_type, payload, original_sending_time}` with no command id anywhere.
Nothing in P01 can answer "did I already issue this command?" after a restart.
P01 owns the FIX rules, the sequence numbers and the socket — exactly its §2
row, and no more.

**P06 has it, and wrote it for this boundary.** `jrnl/stable_send.hpp` opens by
naming the problem: if P01's session store is a different database from P06's
journal, there is no single transaction across the two, so the contract degrades
to "P01 persists a stable (session, command_id, payload_hash) → original
MsgSeqNum mapping BEFORE the socket write, and delivers inbound receipts at
least once". `jrnl::StableSendStore` is that mapping plus the receipt half, and
§2 gives P06 the journal, the durable inbox and the outbox.

So `gw::SessionAdapter` routes the issue-once question to P06 and leaves the
wire to P01. The two commits that result are not hidden — P06's own header calls
them "exactly the hazard the doc describes", and they are what makes crash
windows W13–W21 reproducible instead of asserted.

| P04 port | P06 `StableSendStore` |
|---|---|
| `send_once(command_id, hash, frame)` | `send_once(session, command, hash, payload)` |
| `lookup_binding(command_id)` | `bound_sequence(session, command, &seq, &hash)` |
| `peek_inbound()` | `undelivered(session, 1)` |
| `ack_inbound_after_commit(id)` | `ack_delivery(session, epoch, id)` |
| `deliver_inbound(frame)` | `persist_raw(RawReceipt)` |
| `epoch()` / `bump_epoch()` | `current_epoch()` / `bump_epoch()` |
| `SessionImage::wire_log` | the store's `wire` callback → P01 |

Three decisions worth knowing:

**`Binding::written` is answered `true`.** P04 reads it as `UnknownTransport`
when true and `AcceptedDurable` — still safe to dispatch — when false. P06 can
count unconfirmed sends per session but cannot say whether *this* key's bytes
were confirmed, so the honest answer is "assume they were". A bound sequence
with unknown write status is precisely `UNKNOWN_OUTCOME`, and EI-34 forbids
resending it; `false` would invite the second original send EI-09 rules out.

**Receipt ids resume past acknowledged rows.** `persist_raw` inserts with
`ON CONFLICT … DO NOTHING` and `undelivered` filters on `acked=0`, so an
acknowledged receipt is invisible through the store's API while its row remains.
Restarting the counter at 1 would make the next insert a silent no-op and an
accepted event would simply stop existing (EI-45). The adapter reads
`MAX(receipt_id)` for the epoch instead of assuming.

**`NOT_READY` touches nothing.** The session state is checked before the store,
because a sequence spent on a message that cannot leave is a hole in the
outbound numbering.

`tests/test_session_seam.cpp` covers these across a simulated restart.

## P03 on the write path

`order_core` now owns order status and the fill ledger, which is what §2 gives
it. `router::Store` takes an `IOrderReducer`, and `gw::OrderCoreReducer`
reduces through `order_core::reduce`.

Two constraints came from P04's own header rather than from the design doc, and
both shaped the result:

**"Stateless: all state is in the ChainState the caller passes in, so the router
can run it inside a transaction and discard the result if the commit fails."**
order_core's aggregate is far richer than P04's projection — a fill ledger, an
accepted-version chain, a seen-report set, quarantine incidents. Keeping it in a
map beside the transaction would leave it ahead of the durable record every time
a commit failed. It rides in `ChainState::reducer_state` instead, as an opaque
`shared_ptr<const void>` that P04 copies and discards with the projection and
never reads. That also keeps P04's build free of `order_core`.

**Replay goes through the same interface.** `replay_chains_from_journal`
re-applies journalled reports, so a restart rebuilds order_core aggregates by
the same path the live write took — which is the only thing that makes an
equivalence proof mean anything (EI-32). It is also why no aggregate serialiser
was needed: P03 is a pure fold, so the events are the state, and P04 already
journals `NormalizedReport` and `CommandIntent`. The pointer does not survive a
process restart, and for this lab that is correct rather than a shortcut —
`StoreImage` is in-process memory. A real database behind it would need the
aggregate serialised, and `canonical_text()` would not do: it deliberately
"excludes observational data", so a round trip through it would lose fields.

What P04 keeps: command ids, FIX mapping, correlation, routing, recon cases.
What it stops deciding: whether a fill counts, and what the order's status is.

**The defect this removes.** The same sequence the oracle caught — a buy limited
at 110000, a fill printed at 110001 — now comes back `Quarantine` with
`cum_qty` held at the 5000 the order authorised. `tests/test_order_write_path.cpp`
asserts both sides of it: order_core refuses, and P04's own reducer (still
reachable as `default_order_reducer()`) still credits the fill and calls the
order Filled. If P04's reducer is ever fixed, that assertion fails and says so.

The test also pins three behaviours worth naming: a redelivered execution is a
`Noop` rather than a second fill (EI-11), an unmapped symbol fails closed
because P03 holds no pinned scale for it, and a price scale that is not the
contract's is refused rather than rescaled (EI-15). One assertion of mine was
wrong and the code was right: after `create()` there is no accepted version,
because a locally admitted New is not an acknowledged order — acceptance comes
from the peer, and P03 sits in `LocalPreparing` until it arrives.

## Oracle A, and what it found

§11.1 forbids checking a reducer with itself: the oracle must be "a small
reference model … không share code paths với P03/P05/P06 reducers". Writing a
second order reducer would be the obvious way to get one, and it was not needed.
P04 already has it — `router::p03::OrderStateMachine`, 477 lines, written
against the same FIX 4.4 rules by whoever built the router, sharing no code with
`order_core`. It was built as a stand-in, and a stand-in written independently
is what an oracle is.

So `harness/reference_model/order_oracle.cpp` implements no reducer. It makes
the two existing ones answerable to the same question:

```
P04 NormalizedReport ──> router::p03::OrderStateMachine::apply ──> ChainState
        │                                                              │
        └──> rendered to FIX text ──> order_core::reduce ──> OrderAggregate
                                                                       │
                              both projected and compared <────────────┘
```

The rendering step is not incidental. P04 holds a report as typed enums and
int64 ticks; P03 parses FIX text and does its own decimal conversion. Going
through text means the two reach their numbers by independent parsers at the
pinned scale — the EI-15 surface, where a scale mismatch has to show up rather
than reach an account.

Two findings, both asserted in `tests/test_order_oracle.cpp`:

**On a clean chain they agree exactly.** Across an acknowledgement and two
fills, `cum_qty`, `leaves_qty` and `terminal` match at every step. `avg_px`
differs by exactly one tick — 109999 against 110000 on a weighted average of
109999.5 — because P04 truncates where P03 rounds. That is a rounding mode, and
the test tolerates one tick and no more.

**P04 credits a fill its own order did not authorise.** Given a buy limited at
110000 and a fill printed at 110001, P04's reducer applies it and reports the
order Filled for 10000. P03 refuses it — its policy carries
`enforce_limit_price_on_fill` (what-if D-14, D-15) — the reduction comes back
QUARANTINED, and `cum_qty` stays at the 5000 it can prove. §2 gives P03 order
status and the fill ledger, so P03 is right and this is a defect in P04's
reducer. It is asserted rather than printed, so fixing P04 makes that case fail
and forces the comment to be revisited.

A third thing the oracle surfaced before it could run at all: **the symbol
naming divergence is systemic.** P04 carries the wire form `"EUR/USD"`; P03 and
P05 both key on `"EURUSD"`. P03's first answer was `REJECT_LOCAL EUR/USD has no
pinned scale`, and every downstream quarantine followed from a chain that never
opened. So the instrument key is supplied at each boundary rather than passed
through — in the risk adapter as a map, here as a parameter.

## P07's queue and pool, on two threads

`gw::ExecutionPipeline` runs admission on a producer thread and everything
single-owner on a consumer thread, connected by `llx::SpscRing` with commands
held in an `llx::Pool`.

`llx::Engine` is deliberately **not** used. Its `InputEnvelope` is P07's own
numeric order model — integer `OrderId`, `SymbolId`, `Qty`, `Px`, its own
`ExecType` — and `step()` processes orders against it. Driving the gateway
through that would make P07 a second implementation of what P03 and P05 already
own, which is what §2 forbids: *"P07 không skip P05/P06 để cải thiện latency."*
So what is used is what §2 actually gives P07 — `Submit/TryPush`,
`BoundedResource`, `LatencySpan` — and nothing that decides what an order means.

The topology follows from the single-owner rule. `router::Router`, `order_core`,
the risk coordinator and the journal are all single-owner, so all of them sit on
the consumer thread and none is reachable from anywhere else. The producer only
leases a pool slot and publishes a handle; the ring carries the handle because
its slot type must be trivially copyable and `CommandEnvelope` holds strings.
A fixed-size pool with handle reuse is what bounds the memory, and bounding it
is the point (LI-02).

A threaded test can honestly assert an invariant, not an interleaving, so that
is what `tests/test_execution_pipeline.cpp` does. On the run recorded here the
pool was the binding constraint and the bound was genuinely met:

```
accepted=16 queue_full=0 pool_exhausted=484 processed=16
```

484 submissions were refused **explicitly** and none was lost (EI-43), and after
the drain `accepted == processed` with an empty ring (EI-45). A second producer
is refused by the ring's ownership guard and recorded in `guard_violations()`
rather than quietly corrupting the counters (EI-42), and its work never reaches
the router. Submitting with no consumer running is refused too: accepting work
nobody will drain would make the accounting a promise the pipeline cannot keep.

## EI-22, across components

**A correction.** An earlier version of this file said EI-22 was not held. That
was wrong, and the mistake was one of scope. EI-22 asks that "reservation +
command intent + outbox" be written atomically, and P05 already does it: one
`commit(CommitRequest)` carries the command record, the reservation, the outbox
intent, the exposure projection and the audit event into a single CRC-protected
journal record, flushed before the acknowledgement. P05's own crash suite proves
it — for windows CW-05 through CW-11 it reopens the journal and asserts no
reservation and no outbox row survived, and CW-12 asserts that a durable commit
whose acknowledgement was lost stays recoverable by command id.

What P05 cannot check is whether anyone else agrees, and that is the integration
question `tests/test_admission_atomicity.cpp` asks. Arming each of CW-06 through
CW-11 through the real admission path and submitting a New order: the router
refuses, P05 keeps no reservation and no outbox row, **and** no command record
survives in P04, no sequence is bound in P06, and no frame reaches the LP. On
CW-12 — commit durable, acknowledgement lost — the router still fails closed,
the reservation is there (treating it as a rollback and retrying would reserve
twice for one order), and nothing was sent.

What remains outside that transaction, stated so it is not mistaken for a gap:
P06's send binding is a separate commit on purpose. Its own header explains why
— the mapping has to be durable *before* the socket write, so it cannot sit
inside the business transaction that precedes it. P05's journal also says of
itself that it is "not a substitute for the P06 PostgreSQL contract", so the
single-node journal is the sanctioned lab adapter rather than the end state.

`tests/test_two_store_gap.cpp` measures what that separation can produce, in
both directions. They are not symmetric, and the asymmetry is the finding:

| Window | What survives | Cost |
|---|---|---|
| reservation without intent | budget held for an order that does not exist | availability. Nothing reached the wire, so an original may still be sent |
| intent without reservation | durable send binding with no budget behind it | **money**. The LP may be working an order the gateway cannot account for |

The test asserts that both are computable from the two stores alone — a startup
sweep needs no third source of truth — and that the dangerous one cannot be
talked into looking approved: at `UNKNOWN_OUTCOME` nothing may be released
(EI-25), no original may be reissued (EI-34), and the rung cannot step back down
to one that would permit either (EI-20). Only reconciled evidence leaves it.

It also shows P05's journal surviving a restart on its own, with the reservation
still held and still findable by chain — a restart is not evidence that an order
is gone.

## The lab guard

`gw::verify_isolation` is §19's first bullet, and it is deliberately narrow,
because P04 already does most of it: `router::Config::validate()` refuses a
route whose host is absent from the allowlist, at startup rather than at send
time (OR-B-17). Two things it cannot do:

- **nothing validates the allowlist itself.** Add `fix.a-real-broker.com` to
  `allowed_hosts`, point a route at it, and `validate()` passes — the list
  agrees with itself. `tests/test_lab_guard.cpp` asserts that P04 accepts
  exactly that configuration and that this guard refuses it; if P04 ever closes
  the hole, that test starts failing and the check should be deleted.
- **P04 has no `TRADING_MODE`**, so nothing required anyone to opt into a
  simulation. Unset is a refusal, not a default.

A host is a lab host when it cannot route off the lab: a loopback literal, or a
bare name with no dot and no colon, which is how Compose publishes a service.
Private ranges are refused — a broker behind a VPN is indistinguishable from
`10.0.0.5`, and §19 asks for isolated, not merely private.

This is the app-flag half, and §19 says so in the same sentence: it can refuse
to connect, which is cheap and testable, but it cannot constrain a stray script
or a process that opens its own socket. The probe prints that caveat so `READY`
is not read as more than it proves.

## The evidence ladder

`include/gw/evidence.hpp` is §2's six-state ambiguity contract in code. Four
components already have a private notion of an unknown outcome — 
`order_core::Reason::UnknownOutcome`, `router::RoutingError::UnknownOutcomeBlocked`,
P05's own, and a metric string — and P01, P05 and P07 disagree on what a
`DeliveryState` or a `CommitOutcome` is. None of them can see the whole ladder,
so every adapter between two components has to say which rung it is claiming.

The three predicates are where the invariants live: `may_send_original` is true
only at `INTENT_RECORDED` (EI-09, EI-34), `may_release_reservation` only at
`EXECUTED` (EI-25, which rules out releasing on a disconnect or a timeout), and
`advances_to` lets any rung fall to `UNKNOWN_OUTCOME` but lets nothing leave it
except on reconciled evidence, because EI-20 says a timeout is not evidence.

## Status

| Milestone (§29) | State |
|---|---|
| M1 contracts + isolated Compose lab | superbuild, contract gate and endpoint-isolation guard done; Compose topology and RoE version pinning outstanding |
| M2 synthetic CLI + golden FIX fixtures | the gateway runs one order end to end, in process and over a real socket to P02; a driving CLI, golden fixtures and the tag-1 RoE reconciliation outstanding |
| M3 independent oracle + canonical hashes | Oracle A done, and the defect it found is now fixed by putting P03 on the write path; canonical hashes and oracles B, C and D outstanding |
| M4 scenario DSL + deterministic scheduler | not started |
| M5 failpoint adapters + 32 crash windows | P05's admission windows CW-06..CW-12 are driven through the assembled gateway; the FIX/TCP, storage and process-kill windows outstanding |
| M6 reconciliation, PITR, owner fencing | the two-store crash windows are measured and the unsafe one is detectable; PITR, fencing drills and the recovery path itself outstanding |
| M7 open-loop benchmark, saturation, soak | the bounded hand-off and its refusal accounting are in place; open-loop load generation and percentile evidence outstanding |
| M8 600-case matrix + 40 playbooks | not started |

Of the adapters that put the ownership table into effect, P04→P05 and
P04→P01/P06 are done. P04→P03 — taking P04's reducer off the write path and
moving it to Oracle A — and the single P06 transaction that wraps an admission
(EI-22, which is also what retires `stranded_reservations()`) are the next
pieces. Nothing in `harness/` exists yet beyond its directories, and there is no
gateway binary: the components are assembled and two seams are proven, but no
order has yet travelled the whole path.

Per §1.3 this lab never trades real money and never connects to a live broker,
and per EI-55 it must hold no live credentials or endpoints.
