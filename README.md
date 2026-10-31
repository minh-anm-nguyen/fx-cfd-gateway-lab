# FX/CFD FIX Gateway and Execution Engine — a lab reconstruction

An order-entry gateway for FX and CFD trading, built as eight C++20 components
against FIX 4.4, and a ninth project that assembles them into one running
system.

The interesting part is not any single component. It is the assembly: eight
engines written separately, each correct against its own specification,
disagreeing with each other in ways no compiler catches. Two of those
disagreements turned out to be money bugs, and both are described below with
the test that pins them.

**Provenance.** This is a reconstruction for study, not production software and
not anyone's proprietary code. The source material was a CV describing work on
an FX/CFD FIX gateway at Larion; the architecture, SLO targets, schemas, test
matrices and every line of code here are a proposed rebuild. No employer's
source, database schema or broker Rules of Engagement is included, and nothing
here reproduces a system that was actually deployed. The design documents say so
individually and in more detail.

**It never trades.** There is no live broker, no real credentials and no real
endpoint. A startup guard refuses to run unless `TRADING_MODE=SIM_ONLY` and
every configured peer is a loopback address or a bare Docker Compose service
name — a hostname with a dot in it is refused, because a broker behind a VPN
looks exactly like a private address.

---

## The eight components

Each was built standalone, with its own design document in [`docs/`](docs/), its
own build and its own test suite. Together: ~152,000 lines.

| | Component | Owns | Suites |
|---|---|---|---|
| **P01** | [FIX 4.4 Session Engine](p01-fix-session-engine/) | TCP/FIX session, sequence numbers, resend, gap fill | 17 |
| **P02** | [LP Simulator](p02-lp-simulator/) | a counterparty: quote book, matching, its own execution ledger | 12 |
| **P03** | [Order State Machine](p03-order-state-machine/) | order status and the fill ledger, as a pure reducer | 21 |
| **P04** | [Order Routing & Reconciliation](p04-order-routing-reconciliation/) | command identity, FIX mapping, correlation, unknown outcomes | 23 |
| **P05** | [Pre-trade Risk & Exposure](p05-pretrade-risk-engine/) | risk budget, exposure, reservations | 8 |
| **P06** | [Durable Journaling & Recovery](p06-durable-journaling/) | the atomic transaction, durable inbox and outbox, checkpoints | 23 |
| **P07** | [Low-Latency Execution Engine](p07-low-latency-execution-engine/) | bounded queues, fixed-size pools, thread topology, measurement | 15 |
| **P08** | [Integration, Replay & Failure Testing](p08-integration/) | the assembled lab, the independent oracle, the release gate | 11 |

P08 is not a wrapper around the other eight. It is the integration plus an
independent verification plane, deliberately built from code that shares no
paths with what it checks. [Its README](p08-integration/README.md) is the
detailed one: every seam, every decision, and why.

## What runs today

One order, end to end, through the real components:

```
contracts: READY
isolation: READY
booted, mode=NORMAL
accepted: command=K-1 chain=ord-1 cl_ord_id=O-1
evidence: INTENT_RECORDED
  round 0: sent=1 | applied=1

P06  binding: command=K-1 original_seq=1 written=1
P05  firm gross notional=1111000 (minor units)
P03  lifecycle=WORKING cum_qty=0 leaves=10000 revision=2
P04  delivery=LP_ACKED business=ACKNOWLEDGED
     evidence now: LP_ACCEPTED
     may release reservation=0 may send original=0

EI-09/JI-19 one original per command: held
```

The last three lines are the part worth reading. P04 tracks twelve delivery
states; the contract asks which of six kinds of *evidence* the gateway holds.
`LP_ACKED` maps to `LP_ACCEPTED`, and at that rung the gateway refuses to
release the risk reservation — not until a fill is reconciled — and refuses to
send another original for the same command. Those answers come from code, not
from a comment.

And the same gateway with the in-process counterparty removed: P01's real
session over TCP to P02 as a **separate process**.

```
P01  session FIX.4.4:TRADER01:SIMLP01:DEV  ACTIVE
P04  accepted K-1 chain=ord-1 cl_ord_id=O-1
P01  bridge delivered=1 unconvertible=0
P06  sequences allocated=1 unconfirmed=0
P04  alert: inbound quarantined: SecurityQuarantine
```

The LP accepted the order and created a chain for it. Its acknowledgement came
back over the socket, was stripped of the session envelope and persisted. Then
P04 quarantined it — correctly: that report carries no `Account` tag, so nothing
in it proves the execution belongs to the account that owns the order.

### Real, and standing in

| | In-process gateway | Over TCP |
|---|---|---|
| P04 routing | real | real |
| P05 risk | real | real |
| P06 journal | real | real |
| P03 order state | real | real |
| P01 session | in-tree stand-in | **real, on a socket** |
| P02 counterparty | in-tree stand-in | **real, separate process** |
| P07 threads | not wired (single thread, by choice) | not wired |

P07's queue and pool are exercised on two threads by their own test rather than
in the demo, because a smoke test that is also a concurrency test tells you
neither thing.

## What the assembly found

**A fill above the order's own limit price was being credited.** Given a buy
limited at 1.10000 and an execution printed at 1.10001, P04's in-tree reducer
applied it and reported the order Filled for the full quantity. P03's
`order_core` refuses it and holds the quantity at what the order authorised. P03
owns the fill ledger, so P03 is right. Installing it on the write path is the
fix, and the test asserts both sides — if P04's reducer is ever corrected, that
test fails and says so out loud.

Finding it needed an oracle sharing no code with what it checks. Writing a
second reducer would have been the obvious way; it was not necessary, because
P04 had already written one as a stand-in, independently, against the same FIX
rules. That 477-line reducer is now the oracle.

**Five representations of a price meet in one binary.** P02 holds ticks plus a
per-symbol format, P03 a mantissa and its own scale per value, P04 int64 plus a
per-symbol digit count, P05 exact rationals, P07 int64 against a compile-time
constant. None converts to another for free, and P07's is binding because it
cannot be reconfigured without a recompile. A startup gate refuses to run if
they disagree.

Smaller disagreements, each handled explicitly at the seam rather than cast
away:

- `router::Side` is `{Buy=0, Sell=1}`; `risk::Side` is `{Buy=1, Sell=2}`. The
  obvious cast turns a sell order into a buy.
- Verdict expiry is wall-clock microseconds on one side and steady-clock
  nanoseconds on the other. A duration crosses that boundary; an instant does
  not.
- P04 quotes `EUR/USD`; P02, P03 and P05 all key on `EURUSD`. P04 routes `ACC1`;
  P05's accounts are `ACC-A`, `ACC-B`, `ACC-C`.
- P04 ships only a clock pinned to a fixed lab epoch, which is right for its own
  determinism and rejected by a live peer as a stale `TransactTime`.

## Build and run

Requires a C++20 compiler, CMake 3.20+ and SQLite3. Developed with GCC 16 and
MSYS2 UCRT64 on Windows; the code targets Linux equally.

Each component builds on its own:

```sh
cmake -S p05-pretrade-risk-engine -B build/p05 && cmake --build build/p05
ctest --test-dir build/p05 --output-on-failure
```

The assembled lab consumes all eight where they live, with no vendoring:

```sh
cmake -S p08-integration -B build/p08 -G Ninja
cmake --build build/p08 --parallel
ctest --test-dir build/p08 --output-on-failure    # 11 integration tests
./build/p08/gateway                               # one order, end to end
```

`ctest` exports `TRADING_MODE=SIM_ONLY` per test; running a binary by hand
without it is refused, which is the point.

For the two-process run, start the counterparty first and point the gateway at
it:

```sh
./build/p02/lp_simulator --config p02-lp-simulator/configs/lp.example.yaml --port 19901
./build/p08/interop_gateway --session-config <p01.toml> --state-dir <dir>
```

Both sides keep durable state, so both have to start clean together. Resetting
one and not the other makes the gateway open with a sequence number the peer has
already passed, and the peer refuses the session — that is FIX working, not a
bug.

## Status

The system runs. What remains is coverage and operations rather than blockers:
the Docker Compose topology, a scenario DSL, the rest of the crash-window
matrix, the 600-case what-if catalogue and the open-loop benchmark.
[`p08-integration/README.md`](p08-integration/README.md) tracks it per
milestone, with what is done and what each remaining piece needs.

## Reading it

Start with [`docs/Integration_Replay_Failure_Testing_System_Design.md`](docs/Integration_Replay_Failure_Testing_System_Design.md)
for what the whole thing is meant to prove, then
[`p08-integration/README.md`](p08-integration/README.md) for what it actually
does and every decision taken at a seam. The design documents are written in
Vietnamese with English technical terms; the code and its comments are in
English.

## License

None yet. All rights reserved by the author until one is added.
