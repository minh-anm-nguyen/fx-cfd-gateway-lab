# Project 01 — FIX 4.4 Session Engine

Triển khai của `docs/FIX_Session_Engine_System_Design.md` (ở repo gốc:
`D:\trading\prj-1\docs\FIX_Session_Engine_System_Design.md`).

**C++20 · FIX 4.4 initiator · SQLite WAL · Linux và Windows · không TLS trong v1**

Engine này làm **session layer** và chỉ session layer: TCP, framing, Logon,
heartbeat, sequence persistence, resend, GapFill, durable inbox/outbox, recovery
sau crash. Nó **không** diễn giải business semantics của application message —
đó là Project 03/04.

## Điều engine này bảo đảm, và điều nó không

| Bảo đảm được | Không bảo đảm |
|---|---|
| Một original sequence number không bao giờ được cấp hai lần trong một epoch | Exactly-once business execution |
| Không frame nào ra socket trước khi commit thành công | Peer đã nhận bytes, chỉ vì `send()` trả về thành công |
| Application message được deliver đúng thứ tự `MsgSeqNum`, mỗi sequence đúng một inbox row | Sink chỉ nhận đúng một lần — delivery là **at-least-once** |
| Disconnect không reset sequence; reset cần dual control + audit | GapFill đã phục hồi được nội dung bị skip |
| Mọi queue/buffer có hard cap, vượt cap có hành vi xác định | Chịu được mọi lỗi phần cứng (power-loss nằm ngoài phạm vi lab) |

`ACCEPTED_DURABLE` của một outbound message nghĩa là **intent đã bền**, không
phải counterparty đã nhận. Chủ đích: API không có hàm nào báo "đã giao".

## Build và test

Yêu cầu: CMake ≥ 3.20, compiler C++20, SQLite3 dev, Ninja (khuyến nghị).

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j
ctest --test-dir build --output-on-failure

# Toàn bộ matrix + sinh báo cáo docs/P01_TEST_REPORT.md
scripts/run_matrix.sh

# Crash windows + fuzz dài
scripts/run_crash_fuzz.sh --iterations 1000000 --seed 20261030

# Undefined-behaviour checks (trap mode, không cần libubsan)
scripts/run_sanitizers.sh --ubsan-trap
# ASan/TSan: dùng trên Linux, xem SAN-02/SAN-03 trong báo cáo
scripts/run_sanitizers.sh --asan

# Soak
scripts/run_soak.sh --hours 1
```

Golden binary fixtures (`fixtures/`, delimiter là byte `0x01` thật) được sinh
bởi `scripts/make_fixtures.py` — cài đặt BodyLength/CheckSum **thứ ba**, độc lập
với cả `src/codec.cpp` và `tests/support/peer_wire.hpp`.

## Chạy thử hai tiến trình

```bash
# Terminal 1: scriptable peer
./build/fix_test_acceptor --port 19878 --heartbeat 5

# Terminal 2: engine
./build/fix_client --config config/session.example.toml \
    --duration-s 30 --send-every-ms 500 \
    --metrics-out metrics.txt --audit-out audit.log

# Trạng thái và kiểm tra tính nhất quán của store
./build/fix_admin_cli status --config config/session.example.toml
./build/fix_admin_cli verify --config config/session.example.toml
# Reset cần hai người phê duyệt khác nhau
./build/fix_admin_cli reset --config config/session.example.toml \
    --approver alice --approver bob --reason "agreed weekly reset"
```

`fix_test_acceptor` có các công tắc gây lỗi để tái hiện nhóm A/C/D của matrix
với tiến trình thật: `--suppress-logon`, `--logout-on-logon`, `--wrong-sender`,
`--logon-seq-ahead N`, `--unsolicited-reset`, `--drop-after N`, `--byte-drip N`.

## Bản đồ source

| Thành phần | File | Trách nhiệm |
|---|---|---|
| Wire constants | `include/fix/wire.hpp`, `src/wire.cpp` | Tags, MsgType, checksum, phân loại session vs application |
| Framer | `src/framer.cpp` | TCP stream → frame; BodyLength là untrusted input |
| Codec | `src/codec.cpp` | Decode strict; encode tự tính `9`/`10`; rebuild retransmission |
| Store | `src/sqlite_store.cpp` | Hai transaction nguyên tử; integrity checksum; audit |
| Ownership | `src/ownership.cpp` | OS file lock — single-host, **không đủ cho HA nhiều host** |
| Resend planner | `src/resend_planner.cpp` | Pure function: replay app, coalesce GapFill, fail closed khi thiếu history |
| Session engine | `src/session_actor.cpp` | State machine 9 trạng thái, sequencing, timers, recovery |
| Transport | `src/tcp_connector.cpp` | Winsock/POSIX; partial write; connection generation |
| Failpoints | `src/failpoint.cpp` | Inject lỗi tại đúng biên durability (chỉ build lab) |

Mọi side effect đi qua interface (`ITransport`, `IConnectionController`,
`IApplicationSink`, `ISessionStore`, `IClock`, `ISocketOps`). Nhờ đó toàn bộ
matrix chạy được bằng `on_connected / on_bytes / on_timer / on_disconnect` với
virtual clock, không cần network — và các test vẫn đi qua đúng code path thật.

## Safety invariants I-01..I-12

| ID | Nội dung | Thực thi ở đâu | Test |
|---|---|---|---|
| I-01 | Một writer cho một session identity | `OwnershipLock` + lease row | C-18, G-21, K-14 |
| I-02 | `NextNumIn/Out` không giảm trong một epoch | Precondition trong store | I-02s, PROP-01 |
| I-03 | Không deliver trước khi validate + commit | `handle_frame` → `commit_inbound_and_dispatch` | Nhóm B, C-09 |
| I-04 | Deliver theo thứ tự, mỗi sequence một inbox row | PRIMARY KEY + future buffer | E-05, E-08, PB-03 |
| I-05 | Không dùng lại sequence cho original; resend giữ seq gốc + `43=Y` + `122` | `send_retransmission` | F-09, F-10, PROP-04 |
| I-06 | Commit trước, ghi socket sau | `send_original` | PB-01, PB-04, I-09 |
| I-07 | Không coi inbound là processed nếu chưa commit | `commit_inbound_and_dispatch` | G-05, G-08, I-03 |
| I-08 | Không âm thầm GapFill qua app message | Planner fail closed; policy inbound | F-16, F-20, PB-07 |
| I-09 | Mọi buffer có hard cap | Framer, future buffer, inbox, tx queue | E-03, E-14, I-01, I-05 |
| I-10 | `ACTIVE` chỉ sau logon + đồng bộ | `maybe_finish_synchronising` | C-01, C-11, K-09 |
| I-11 | Session recovery ≠ business exactly-once | Delivery at-least-once, sink idempotent | G-07, L-06, F-26 |
| I-12 | Không tự reset state vì restart/ngày mới | Reset chỉ qua dual control | C-14, H-07, H-09, L-14 |

## Bằng chứng kiểm thử

`docs/P01_TEST_REPORT.md` được sinh từ artifact JSON của từng suite; mỗi dòng là
một scenario ID của matrix. Verdict gồm `PASS`, `FAIL`, `BLOCKED` (không chạy
được ở đây, **kèm lý do**) và `NOT_RUN`. `BLOCKED` không phải pass.

Các lớp test (theo doc §17.3):

1. **Unit + golden fixtures** — `tests/unit/`: framer, codec, store, planner, 36 fixture nhị phân.
2. **Model-based** — `tests/property/`: reference model + kiểm invariant sau **mỗi** event.
3. **Property-based** — fragmentation tùy ý cho cùng byte stream phải cho cùng kết quả.
4. **Fuzz** — `tests/fuzz/`: corpus + mutation, cross-check với checker độc lập.
5. **Crash injection** — `tests/crash/`: child process bị kill tại đúng failpoint, parent mở lại store và kiểm chứng.
6. **Interop** — `tests/scenario/`: TCP loopback thật, peer viết độc lập. QuickFIX: xem L-01 trong báo cáo.
7. **Chaos** — 1000 lần flap kết nối, 100 lần restart, resend storm.
8. **Benchmark** — chưa có; latency/throughput là mục backlog, không khẳng định số liệu nào.

## Giới hạn đã biết

- **TLS chưa triển khai.** `tls_enabled = true` làm **dừng startup**, không bao giờ im lặng rớt về plaintext.
- **Ownership chỉ cho một host.** OS file lock không chặn được hai host; HA cần external fencing/lease — ngoài phạm vi.
- **Power-loss không được kiểm.** Crash test terminate process không unwinding; durability dưới mức fsync cần VM/hardware (G-09).
- **Interop với QuickFIX chưa chạy** (không có trong môi trường này). Peer tự viết không thể tự chứng minh compliance.
- **ASan/TSan không có runtime trên toolchain này**; thay bằng UB trap mode (SAN-01), còn SAN-02/SAN-03 ghi nhận là BLOCKED.
- **Chưa có benchmark.** Không có số latency/throughput nào được tuyên bố; con số p99 95 µs trong CV thuộc span đo khác và không áp dụng ở đây.
- **Một session mỗi tiến trình.** Multi-session là đường mở rộng, chưa làm.
