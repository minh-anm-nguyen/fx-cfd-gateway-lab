# Golden FIX 4.4 fixtures

Each pair of files is one FIX 4.4 **application field block**:

- `*.bin` — the bytes, with real `0x01` (SOH) separators
- `*.txt` — the same fields with SOH shown as `|`, plus a line saying what the
  frame is for

## These are not TCP frames

Design doc 9.3 is explicit about this, and it is worth repeating where the files
live: a block here has **no standard header or trailer**. There is no
`BeginString(8)`, no `BodyLength(9)`, no `MsgSeqNum(34)`, no `SenderCompID(49)`,
no `SendingTime(52)`, no `TargetCompID(56)` and no `CheckSum(10)`. Those belong
to P01 (design doc 5.1 and 9.1), and P04 never attaches them.

So do not pipe one of these into a socket and expect an acceptor to take it.
They are the application payload that P04 produces or consumes, which is exactly
the layer this project owns.

## They are used, not just documented

`tests/unit/test_fixtures.cpp` makes every file load-bearing:

- `U-20` encodes `D`, `F`, `G` and `H` with the router's own encoder and compares
  the result to `new/`, `cancel/`, `replace/` and `status/` **byte for byte**. If
  the encoder changes, this fails.
- `U-21` parses every file under `execution/` and `reject/` with the real parser
  and runs the pinned dictionary over it. All of them must be accepted.
- `U-22` parses every file under `invalid/` and requires each to be refused
  **with the specific error that file was built to trigger** — not merely
  rejected for some reason.
- `U-23` replays `execution/trade_partial.bin` through the whole router into a
  live chain and checks the resulting fill, then replays
  `invalid/trade_missing_lastqty.bin` and checks that nothing moved.

A missing fixture fails the test rather than being skipped, so deleting one
cannot look like a green suite.

## Layout

| Directory | Contents |
|---|---|
| `new/`, `cancel/`, `replace/`, `status/` | what the router sends: `35=D`, `35=F`, `35=G`, `35=H` |
| `execution/` | valid `35=8` reports: New, Trade (partial and filling), Pending Cancel, Canceled, Pending Replace, Replaced, Rejected, Order Status |
| `reject/` | valid `35=9` OrderCancelReject, answering an `F`, answering a `G`, and the unknown-order case where `37=NONE` is permitted |
| `invalid/` | one deliberate defect each: a Trade with no `LastQty`, no `LastPx`, an exponent price, a locale price, a negative `LastQty`, the FIX 4.2 `ExecType=1`, a `35=9` with no `434`, a duplicated tag 11, an unagreed tag above 5000 |

## The values

All synthetic, and they match the lab fixture in design doc 21.0: account
`ACC1`, symbol `EUR/USD` with tick `0.00001`, LP `LP_SIM_01`, order `O-1` for
100,000 at `1.08500`, fill `EX-1` of 40,000, cancel `C-2`, replace `R-2`.
Timestamps are FIX `UTCTimestamp` with milliseconds —
`20261030-08:00:00.000` — which is the format the lab profile pins.

Two details worth noticing, because both are rules rather than arbitrary values:

- `execution/order_status.bin` carries `17=0`. FIX 4.4 permits that sentinel on
  an `ExecType=I` snapshot, and R-I08 forbids using it as a business execution
  identity. The dedup ledger never contains it.
- `replace/golden_replace.bin` has `38=80000`, the whole intended order size.
  It is never `LeavesQty`, which is the mistake design doc 9.1 calls out.
