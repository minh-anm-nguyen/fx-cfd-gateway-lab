#include "lp/sqlite_store.hpp"

#include <sqlite3.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "lp/failpoint.hpp"
#include "lp/ids.hpp"
#include "lp/validator.hpp"

namespace lpsim {

namespace {

// ---------------------------------------------------------------------------
// Small RAII wrapper over a prepared statement. Every query in this file goes
// through it so that an early return cannot leak a statement handle.
// ---------------------------------------------------------------------------
class Stmt {
public:
    Stmt(sqlite3* db, const char* sql) : db_(db) {
        rc_ = sqlite3_prepare_v2(db, sql, -1, &st_, nullptr);
    }
    ~Stmt() {
        if (st_ != nullptr) sqlite3_finalize(st_);
    }
    Stmt(const Stmt&) = delete;
    Stmt& operator=(const Stmt&) = delete;

    bool ok() const { return rc_ == SQLITE_OK && st_ != nullptr; }
    int rc() const { return rc_; }
    sqlite3_stmt* get() { return st_; }

    void bind_text(int i, std::string_view v) {
        sqlite3_bind_text(st_, i, v.data(), static_cast<int>(v.size()), SQLITE_TRANSIENT);
    }
    void bind_blob(int i, std::string_view v) {
        sqlite3_bind_blob(st_, i, v.data(), static_cast<int>(v.size()), SQLITE_TRANSIENT);
    }
    void bind_i64(int i, std::int64_t v) { sqlite3_bind_int64(st_, i, v); }

    int step() { return sqlite3_step(st_); }

    std::string text(int col) const {
        const unsigned char* p = sqlite3_column_text(st_, col);
        const int n = sqlite3_column_bytes(st_, col);
        return p == nullptr ? std::string() : std::string(reinterpret_cast<const char*>(p),
                                                          static_cast<std::size_t>(n));
    }
    std::string blob(int col) const {
        const void* p = sqlite3_column_blob(st_, col);
        const int n = sqlite3_column_bytes(st_, col);
        return p == nullptr ? std::string()
                            : std::string(static_cast<const char*>(p),
                                          static_cast<std::size_t>(n));
    }
    std::int64_t i64(int col) const { return sqlite3_column_int64(st_, col); }

private:
    sqlite3* db_ = nullptr;
    sqlite3_stmt* st_ = nullptr;
    int rc_ = SQLITE_ERROR;
};

StoreError classify(int rc) {
    switch (rc) {
        case SQLITE_OK:
        case SQLITE_DONE:
        case SQLITE_ROW:
            return StoreError::None;
        case SQLITE_BUSY:
        case SQLITE_LOCKED:
            return StoreError::Busy;
        case SQLITE_FULL:
        case SQLITE_NOMEM:
            return StoreError::Full;
        case SQLITE_IOERR:
        case SQLITE_PERM:
        case SQLITE_READONLY:
        case SQLITE_CANTOPEN:
            return StoreError::IoError;
        case SQLITE_CORRUPT:
        case SQLITE_NOTADB:
            return StoreError::Corrupt;
        case SQLITE_CONSTRAINT:
            return StoreError::Conflict;
        default:
            return StoreError::Unknown;
    }
}

const char* kSchema = R"SQL(
CREATE TABLE IF NOT EXISTS schema_versions (
  version    INTEGER PRIMARY KEY,
  applied_at TEXT NOT NULL
);

CREATE TABLE IF NOT EXISTS session_state (
  session_id      TEXT PRIMARY KEY,
  sender_comp_id  TEXT NOT NULL,
  target_comp_id  TEXT NOT NULL,
  epoch           INTEGER NOT NULL,
  next_in         INTEGER NOT NULL,
  next_out        INTEGER NOT NULL,
  clean_shutdown  INTEGER NOT NULL DEFAULT 0
);

-- TX-A: FIX inbound identity and the durable application inbox.
CREATE TABLE IF NOT EXISTS inbound_inbox (
  session_id  TEXT NOT NULL,
  epoch       INTEGER NOT NULL,
  msg_seq_num INTEGER NOT NULL,
  raw_frame   BLOB NOT NULL,
  delivered   INTEGER NOT NULL DEFAULT 0,
  received_at TEXT NOT NULL,
  PRIMARY KEY(session_id, epoch, msg_seq_num)
);

-- Business request identity, independent of the FIX session epoch (I-16).
CREATE TABLE IF NOT EXISTS client_requests (
  firm           TEXT NOT NULL,
  business_day   TEXT NOT NULL,
  cl_ord_id      TEXT NOT NULL,
  request_type   TEXT NOT NULL,
  canonical_hash TEXT NOT NULL,
  order_id       TEXT,
  outcome_state  TEXT NOT NULL,
  reject_reason  TEXT NOT NULL DEFAULT 'None',
  PRIMARY KEY(firm, business_day, cl_ord_id)
);

CREATE TABLE IF NOT EXISTS order_chains (
  order_id            TEXT PRIMARY KEY,
  firm                TEXT NOT NULL,
  business_day        TEXT NOT NULL,
  original_cl_ord_id  TEXT NOT NULL,
  current_cl_ord_id   TEXT NOT NULL,
  pending             TEXT NOT NULL,
  pending_cl_ord_id   TEXT NOT NULL,
  pending_qty         INTEGER NOT NULL,
  pending_limit_ticks INTEGER NOT NULL,
  symbol              TEXT NOT NULL,
  side                TEXT NOT NULL,
  ord_type            TEXT NOT NULL,
  tif                 TEXT NOT NULL,
  original_qty        INTEGER NOT NULL,
  effective_qty       INTEGER NOT NULL,
  limit_ticks         INTEGER NOT NULL,
  cum_qty             INTEGER NOT NULL,
  leaves_qty          INTEGER NOT NULL,
  avg_px_ticks        INTEGER NOT NULL,
  base_state          TEXT NOT NULL,
  version             INTEGER NOT NULL,
  accepted_at         TEXT NOT NULL,
  terminal_at         TEXT NOT NULL
);

CREATE TABLE IF NOT EXISTS fill_ledger (
  fill_id         TEXT PRIMARY KEY,
  exec_id         TEXT NOT NULL UNIQUE,
  order_id        TEXT NOT NULL,
  quote_version   INTEGER NOT NULL,
  liquidity_slice TEXT NOT NULL,
  qty             INTEGER NOT NULL,
  px_ticks        INTEGER NOT NULL,
  event_id        TEXT NOT NULL UNIQUE,
  created_at      TEXT NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_fill_order ON fill_ledger(order_id);

CREATE TABLE IF NOT EXISTS report_outbox (
  event_id          TEXT PRIMARY KEY,
  order_id          TEXT,
  report_type       TEXT NOT NULL,
  canonical_payload BLOB NOT NULL,
  payload_hash      TEXT NOT NULL,
  exec_id           TEXT,
  dispatch_state    TEXT NOT NULL,
  outbound_seq      INTEGER,
  session_epoch     INTEGER,
  created_at        TEXT NOT NULL,
  insertion         INTEGER NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_outbox_state ON report_outbox(dispatch_state, insertion);

-- One outbound sequence number per outbox event, enforced by the schema and
-- not only by the code path (I-15).
CREATE TABLE IF NOT EXISTS session_outbound_mapping (
  session_id         TEXT NOT NULL,
  epoch              INTEGER NOT NULL,
  event_id           TEXT NOT NULL UNIQUE,
  msg_seq_num        INTEGER NOT NULL,
  original_fix_frame BLOB NOT NULL,
  created_at         TEXT NOT NULL,
  PRIMARY KEY(session_id, epoch, msg_seq_num)
);

CREATE TABLE IF NOT EXISTS quote_versions (
  symbol        TEXT NOT NULL,
  version       INTEGER NOT NULL,
  bid_ticks     INTEGER NOT NULL,
  ask_ticks     INTEGER NOT NULL,
  bid_available INTEGER NOT NULL,
  ask_available INTEGER NOT NULL,
  valid_from_ms INTEGER NOT NULL,
  expires_at_ms INTEGER NOT NULL,
  fixture_id    TEXT NOT NULL,
  is_current    INTEGER NOT NULL,
  PRIMARY KEY(symbol, version)
);

CREATE TABLE IF NOT EXISTS scheduled_events (
  event_id      TEXT PRIMARY KEY,
  due_us        INTEGER NOT NULL,
  kind          TEXT NOT NULL,
  insertion     INTEGER NOT NULL,
  order_id      TEXT NOT NULL DEFAULT '',
  request_id    TEXT NOT NULL DEFAULT '',
  qty           INTEGER NOT NULL DEFAULT 0,
  px_ticks      INTEGER NOT NULL DEFAULT 0,
  quote_version INTEGER NOT NULL DEFAULT 0,
  quote_payload TEXT NOT NULL DEFAULT '',
  fault_name    TEXT NOT NULL DEFAULT '',
  fault_repeats INTEGER NOT NULL DEFAULT 1,
  executed      INTEGER NOT NULL DEFAULT 0
);

CREATE TABLE IF NOT EXISTS allocators (
  name         TEXT PRIMARY KEY,
  next_counter INTEGER NOT NULL
);

CREATE TABLE IF NOT EXISTS audit_log (
  id   INTEGER PRIMARY KEY AUTOINCREMENT,
  line TEXT NOT NULL
);
)SQL";

// --- QuoteSlice serialisation for scheduled_events -------------------------
std::string pack_quote(const QuoteSlice& q) {
    std::string s;
    s.append(q.symbol).push_back('\x1f');
    s.append(std::to_string(q.version)).push_back('\x1f');
    s.append(std::to_string(q.bid_ticks)).push_back('\x1f');
    s.append(std::to_string(q.ask_ticks)).push_back('\x1f');
    s.append(std::to_string(q.bid_available)).push_back('\x1f');
    s.append(std::to_string(q.ask_available)).push_back('\x1f');
    s.append(std::to_string(q.valid_from_ms)).push_back('\x1f');
    s.append(std::to_string(q.expires_at_ms)).push_back('\x1f');
    s.append(q.source_fixture_id);
    return s;
}

bool unpack_quote(const std::string& s, QuoteSlice* q) {
    std::vector<std::string> parts;
    std::size_t start = 0;
    while (true) {
        const std::size_t p = s.find('\x1f', start);
        if (p == std::string::npos) {
            parts.push_back(s.substr(start));
            break;
        }
        parts.push_back(s.substr(start, p - start));
        start = p + 1;
    }
    if (parts.size() < 9) return false;
    q->symbol = parts[0];
    q->version = std::strtoull(parts[1].c_str(), nullptr, 10);
    q->bid_ticks = std::strtoll(parts[2].c_str(), nullptr, 10);
    q->ask_ticks = std::strtoll(parts[3].c_str(), nullptr, 10);
    q->bid_available = std::strtoll(parts[4].c_str(), nullptr, 10);
    q->ask_available = std::strtoll(parts[5].c_str(), nullptr, 10);
    q->valid_from_ms = std::strtoll(parts[6].c_str(), nullptr, 10);
    q->expires_at_ms = std::strtoll(parts[7].c_str(), nullptr, 10);
    q->source_fixture_id = parts[8];
    return true;
}

// --- enum round-tripping ---------------------------------------------------
BaseState base_from(const std::string& s) {
    if (s == "NEW") return BaseState::New;
    if (s == "PARTIALLY_FILLED") return BaseState::PartiallyFilled;
    if (s == "FILLED") return BaseState::Filled;
    if (s == "CANCELED") return BaseState::Canceled;
    if (s == "EXPIRED") return BaseState::Expired;
    return BaseState::Rejected;
}

Pending pending_from(const std::string& s) {
    if (s == "PENDING_CANCEL") return Pending::Cancel;
    if (s == "PENDING_REPLACE") return Pending::Replace;
    return Pending::None;
}

RequestType request_from(const std::string& s) {
    if (s == "CANCEL") return RequestType::Cancel;
    if (s == "REPLACE") return RequestType::Replace;
    return RequestType::New;
}

RequestOutcome outcome_from(const std::string& s) {
    if (s == "ACCEPTED") return RequestOutcome::Accepted;
    if (s == "REJECTED") return RequestOutcome::Rejected;
    return RequestOutcome::Pending;
}

SendState send_state_from(const std::string& s) {
    if (s == "SEQ_ASSIGNED") return SendState::SeqAssigned;
    if (s == "SENT") return SendState::Sent;
    return SendState::Pending;
}

ReportType report_from(const std::string& s) {
    if (s == "EXEC_TRADE") return ReportType::ExecTrade;
    if (s == "EXEC_CANCELED") return ReportType::ExecCanceled;
    if (s == "EXEC_REPLACED") return ReportType::ExecReplaced;
    if (s == "EXEC_PENDING_CANCEL") return ReportType::ExecPendingCancel;
    if (s == "EXEC_PENDING_REPLACE") return ReportType::ExecPendingReplace;
    if (s == "EXEC_REJECTED") return ReportType::ExecRejected;
    if (s == "EXEC_EXPIRED") return ReportType::ExecExpired;
    if (s == "CANCEL_REJECT") return ReportType::CancelReject;
    if (s == "BUSINESS_REJECT") return ReportType::BusinessReject;
    return ReportType::ExecNew;
}

EventKind kind_from(const std::string& s) {
    if (s == "QUOTE_UPDATE") return EventKind::QuoteUpdate;
    if (s == "INJECT_FILL") return EventKind::InjectFill;
    if (s == "FINALIZE_CANCEL") return EventKind::FinalizeCancel;
    if (s == "FINALIZE_REPLACE") return EventKind::FinalizeReplace;
    if (s == "EXPIRE") return EventKind::Expire;
    if (s == "FAULT_TRIGGER") return EventKind::FaultTrigger;
    return EventKind::AutoMatch;
}

BizReject reject_from(const std::string& s) {
    // Only the reasons that can be persisted on a registry row need to round
    // trip; anything unrecognised degrades to a generic refusal rather than
    // being silently read as "no rejection".
    static const std::pair<const char*, BizReject> kMap[] = {
        {"None", BizReject::None},
        {"MissingRequiredField", BizReject::MissingRequiredField},
        {"AmbiguousDuplicateTag", BizReject::AmbiguousDuplicateTag},
        {"IdTooLong", BizReject::IdTooLong},
        {"UnknownSymbol", BizReject::UnknownSymbol},
        {"DisabledSymbol", BizReject::DisabledSymbol},
        {"BadQuantity", BizReject::BadQuantity},
        {"BadPrice", BizReject::BadPrice},
        {"OffTick", BizReject::OffTick},
        {"OffLotStep", BizReject::OffLotStep},
        {"UnsupportedOrdType", BizReject::UnsupportedOrdType},
        {"UnsupportedTimeInForce", BizReject::UnsupportedTimeInForce},
        {"UnsupportedMsgType", BizReject::UnsupportedMsgType},
        {"DuplicateRequest", BizReject::DuplicateRequest},
        {"ConflictingRequest", BizReject::ConflictingRequest},
        {"UnknownOrder", BizReject::UnknownOrder},
        {"StaleOrigClOrdId", BizReject::StaleOrigClOrdId},
        {"TooLate", BizReject::TooLate},
        {"AlreadyPending", BizReject::AlreadyPending},
        {"QtyBelowCumQty", BizReject::QtyBelowCumQty},
        {"ImmutableAttribute", BizReject::ImmutableAttribute},
        {"StaleTransactTime", BizReject::StaleTransactTime},
        {"FutureTransactTime", BizReject::FutureTransactTime},
        {"UnknownAccount", BizReject::UnknownAccount},
        {"OrderCapReached", BizReject::OrderCapReached},
        {"AdmissionStopped", BizReject::AdmissionStopped},
        {"NotActive", BizReject::NotActive},
        {"CrossFirm", BizReject::CrossFirm},
    };
    for (const auto& kv : kMap) {
        if (s == kv.first) return kv.second;
    }
    return BizReject::ConflictingRequest;
}

}  // namespace

// ---------------------------------------------------------------------------

SqliteStore::~SqliteStore() { close(); }

StoreError SqliteStore::exec(const char* sql) {
    char* err = nullptr;
    const int rc = sqlite3_exec(db_, sql, nullptr, nullptr, &err);
    if (err != nullptr) sqlite3_free(err);
    return classify(rc);
}

StoreError SqliteStore::begin() {
    if (fp::Action::ReturnError == LPSIM_FP(fp::point::kStoreBeginTxn))
        return StoreError::Full;
    return exec("BEGIN IMMEDIATE");
}

StoreError SqliteStore::commit() {
    if (fp::Action::ReturnError == LPSIM_FP(fp::point::kStoreCommit)) {
        rollback();
        return StoreError::Full;
    }
    return exec("COMMIT");
}

void SqliteStore::rollback() { exec("ROLLBACK"); }

StoreError SqliteStore::last_error() const { return classify(sqlite3_errcode(db_)); }

StoreError SqliteStore::open(const StoreConfig& cfg) {
    close();
    cfg_ = cfg;

    // Exclusive ownership first: a second instance must be refused before it
    // has a chance to write anything (J-12).
    if (!lock_.acquire(cfg.path + ".lock")) return StoreError::NotOwner;

    const int rc = sqlite3_open_v2(cfg.path.c_str(), &db_,
                                   SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr);
    if (rc != SQLITE_OK) {
        if (db_ != nullptr) {
            sqlite3_close(db_);
            db_ = nullptr;
        }
        lock_.release();
        return classify(rc);
    }

    sqlite3_busy_timeout(db_, 3000);
    exec("PRAGMA foreign_keys=ON");
    if (cfg.wal) exec("PRAGMA journal_mode=WAL");
    exec(cfg.synchronous_full ? "PRAGMA synchronous=FULL" : "PRAGMA synchronous=NORMAL");

    const StoreError se = write_schema();
    if (se != StoreError::None) {
        close();
        return se;
    }
    open_ = true;
    return StoreError::None;
}

StoreError SqliteStore::write_schema() {
    StoreError e = exec(kSchema);
    if (e != StoreError::None) return e;

    // Version check before anything reads a table whose shape may have changed.
    {
        Stmt q(db_, "SELECT MAX(version) FROM schema_versions");
        if (!q.ok()) return StoreError::Corrupt;
        if (q.step() == SQLITE_ROW) {
            const std::int64_t v = q.i64(0);
            if (v != 0 && v != kSchemaVersion) return StoreError::SchemaMismatch;
        }
    }
    {
        Stmt ins(db_, "INSERT OR IGNORE INTO schema_versions(version, applied_at) VALUES(?,?)");
        if (!ins.ok()) return StoreError::Corrupt;
        ins.bind_i64(1, kSchemaVersion);
        ins.bind_text(2, "bootstrap");
        if (ins.step() != SQLITE_DONE) return last_error();
    }
    // Session row, created on first open with the configured identity.
    {
        Stmt ins(db_,
                 "INSERT OR IGNORE INTO session_state"
                 "(session_id, sender_comp_id, target_comp_id, epoch, next_in, next_out,"
                 " clean_shutdown) VALUES(?,?,?,1,1,1,1)");
        if (!ins.ok()) return StoreError::Corrupt;
        ins.bind_text(1, cfg_.session_id);
        ins.bind_text(2, cfg_.sender_comp_id);
        ins.bind_text(3, cfg_.target_comp_id);
        if (ins.step() != SQLITE_DONE) return last_error();
    }
    {
        static const char* kNames[] = {"order_id", "exec_id", "fill_id", "event_id"};
        static const std::int64_t kStart[] = {1001, 1, 1, 1};
        for (int i = 0; i < 4; ++i) {
            Stmt ins(db_, "INSERT OR IGNORE INTO allocators(name, next_counter) VALUES(?,?)");
            if (!ins.ok()) return StoreError::Corrupt;
            ins.bind_text(1, kNames[i]);
            ins.bind_i64(2, kStart[i]);
            if (ins.step() != SQLITE_DONE) return last_error();
        }
    }
    return StoreError::None;
}

void SqliteStore::close() {
    if (db_ != nullptr) {
        if (open_) {
            // A clean close records itself, so recovery can tell an orderly
            // shutdown from a kill and report unclean_shutdown honestly.
            Stmt up(db_, "UPDATE session_state SET clean_shutdown=1 WHERE session_id=?");
            if (up.ok()) {
                up.bind_text(1, cfg_.session_id);
                up.step();
            }
        }
        sqlite3_close(db_);
        db_ = nullptr;
    }
    open_ = false;
    lock_.release();
}

// ---------------------------------------------------------------------------
// TX-A
// ---------------------------------------------------------------------------

StoreError SqliteStore::commit_inbound(const InboundIdentity& id, std::string_view raw_frame,
                                       bool needs_delivery, bool* duplicate) {
    *duplicate = false;
    if (db_ == nullptr) return StoreError::IoError;

    // Bounded inbox. A full inbox stops reading; it never drops a row that was
    // already committed (I-17, L-02).
    {
        Stmt q(db_, "SELECT COUNT(*) FROM inbound_inbox WHERE delivered=0");
        if (!q.ok()) return StoreError::Corrupt;
        if (q.step() == SQLITE_ROW &&
            static_cast<std::size_t>(q.i64(0)) >= cfg_.max_inbox)
            return StoreError::Full;
    }

    StoreError e = begin();
    if (e != StoreError::None) return e;

    {
        Stmt q(db_,
               "SELECT 1 FROM inbound_inbox WHERE session_id=? AND epoch=? AND msg_seq_num=?");
        if (!q.ok()) {
            rollback();
            return StoreError::Corrupt;
        }
        q.bind_text(1, id.session_id);
        q.bind_i64(2, static_cast<std::int64_t>(id.epoch));
        q.bind_i64(3, static_cast<std::int64_t>(id.msg_seq_num));
        if (q.step() == SQLITE_ROW) {
            // Already committed: a session retransmission (43=Y) or a replayed
            // frame. Nothing changes and no business delivery is due (I-02).
            rollback();
            *duplicate = true;
            return StoreError::None;
        }
    }
    {
        Stmt ins(db_,
                 "INSERT INTO inbound_inbox"
                 "(session_id, epoch, msg_seq_num, raw_frame, delivered, received_at)"
                 " VALUES(?,?,?,?,?,?)");
        if (!ins.ok()) {
            rollback();
            return StoreError::Corrupt;
        }
        ins.bind_text(1, id.session_id);
        ins.bind_i64(2, static_cast<std::int64_t>(id.epoch));
        ins.bind_i64(3, static_cast<std::int64_t>(id.msg_seq_num));
        ins.bind_blob(4, raw_frame);
        // A session-level message is delivered on arrival: there is no business
        // consumer that could still owe an acknowledgement for it.
        ins.bind_i64(5, needs_delivery ? 0 : 1);
        ins.bind_text(6, "");
        if (ins.step() != SQLITE_DONE) {
            const StoreError err = last_error();
            rollback();
            return err;
        }
    }
    // NextNumIn advances in the same transaction as the inbox row. Advancing it
    // separately is what creates the "sequence moved but the message is gone"
    // window the design doc rules out.
    {
        Stmt up(db_,
                "UPDATE session_state SET next_in=? , clean_shutdown=0 WHERE session_id=?");
        if (!up.ok()) {
            rollback();
            return StoreError::Corrupt;
        }
        up.bind_i64(1, static_cast<std::int64_t>(id.msg_seq_num + 1));
        up.bind_text(2, id.session_id);
        if (up.step() != SQLITE_DONE) {
            const StoreError err = last_error();
            rollback();
            return err;
        }
    }

    if (fp::Action::ReturnError == LPSIM_FP(fp::point::kTxABeforeCommit)) {
        rollback();
        return StoreError::Full;
    }
    e = commit();
    if (e != StoreError::None) return e;
    LPSIM_FP(fp::point::kTxAAfterCommit);
    return StoreError::None;
}

StoreError SqliteStore::pending_inbox(
    std::vector<std::pair<std::uint64_t, std::string>>* out) {
    out->clear();
    Stmt q(db_,
           "SELECT msg_seq_num, raw_frame FROM inbound_inbox"
           " WHERE session_id=? AND delivered=0 ORDER BY epoch, msg_seq_num");
    if (!q.ok()) return StoreError::Corrupt;
    q.bind_text(1, cfg_.session_id);
    while (q.step() == SQLITE_ROW) {
        out->emplace_back(static_cast<std::uint64_t>(q.i64(0)), q.blob(1));
    }
    return StoreError::None;
}

// ---------------------------------------------------------------------------
// TX-B
// ---------------------------------------------------------------------------

StoreError SqliteStore::commit_business(const BusinessTxn& txn) {
    if (db_ == nullptr) return StoreError::IoError;

    // Admission check before opening the transaction: once the outbox backlog
    // is at its cap the simulator stops producing new business effects rather
    // than growing without bound (L-03, I-17).
    if (!txn.outbox.empty() && count_outbox_pending() >= cfg_.max_pending_outbox)
        return StoreError::Full;

    StoreError e = begin();
    if (e != StoreError::None) return e;

    auto fail = [&](StoreError err) {
        rollback();
        return err;
    };

    // --- request registry --------------------------------------------------
    for (const RegistryRow& r : txn.registry) {
        Stmt ins(db_,
                 "INSERT INTO client_requests"
                 "(firm, business_day, cl_ord_id, request_type, canonical_hash, order_id,"
                 " outcome_state, reject_reason) VALUES(?,?,?,?,?,?,?,?)"
                 " ON CONFLICT(firm, business_day, cl_ord_id) DO UPDATE SET"
                 " outcome_state=excluded.outcome_state, order_id=excluded.order_id,"
                 " reject_reason=excluded.reject_reason");
        if (!ins.ok()) return fail(StoreError::Corrupt);
        ins.bind_text(1, r.key.firm);
        ins.bind_text(2, r.key.business_day);
        ins.bind_text(3, r.key.cl_ord_id);
        ins.bind_text(4, to_string(r.type));
        ins.bind_text(5, std::to_string(r.canonical_hash));
        ins.bind_text(6, r.order_id);
        ins.bind_text(7, to_string(r.outcome));
        ins.bind_text(8, to_string(r.reject));
        if (ins.step() != SQLITE_DONE) return fail(last_error());
    }

    // --- order chains ------------------------------------------------------
    for (const OrderChain& c : txn.chains) {
        Stmt ins(db_,
                 "INSERT INTO order_chains"
                 "(order_id, firm, business_day, original_cl_ord_id, current_cl_ord_id,"
                 " pending, pending_cl_ord_id, pending_qty, pending_limit_ticks, symbol,"
                 " side, ord_type, tif, original_qty, effective_qty, limit_ticks, cum_qty,"
                 " leaves_qty, avg_px_ticks, base_state, version, accepted_at, terminal_at)"
                 " VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)"
                 " ON CONFLICT(order_id) DO UPDATE SET"
                 " current_cl_ord_id=excluded.current_cl_ord_id, pending=excluded.pending,"
                 " pending_cl_ord_id=excluded.pending_cl_ord_id,"
                 " pending_qty=excluded.pending_qty,"
                 " pending_limit_ticks=excluded.pending_limit_ticks,"
                 " effective_qty=excluded.effective_qty, limit_ticks=excluded.limit_ticks,"
                 " cum_qty=excluded.cum_qty, leaves_qty=excluded.leaves_qty,"
                 " avg_px_ticks=excluded.avg_px_ticks, base_state=excluded.base_state,"
                 " version=excluded.version, terminal_at=excluded.terminal_at");
        if (!ins.ok()) return fail(StoreError::Corrupt);
        int i = 1;
        ins.bind_text(i++, c.order_id);
        ins.bind_text(i++, c.firm);
        ins.bind_text(i++, c.business_day);
        ins.bind_text(i++, c.original_cl_ord_id);
        ins.bind_text(i++, c.current_cl_ord_id);
        ins.bind_text(i++, to_string(c.pending));
        ins.bind_text(i++, c.pending_cl_ord_id);
        ins.bind_i64(i++, c.pending_qty);
        ins.bind_i64(i++, c.pending_limit_ticks);
        ins.bind_text(i++, c.symbol);
        ins.bind_text(i++, to_string(c.side));
        ins.bind_text(i++, to_string(c.ord_type));
        ins.bind_text(i++, to_string(c.tif));
        ins.bind_i64(i++, c.original_qty);
        ins.bind_i64(i++, c.effective_qty);
        ins.bind_i64(i++, c.limit_ticks);
        ins.bind_i64(i++, c.cum_qty);
        ins.bind_i64(i++, c.leaves_qty);
        ins.bind_i64(i++, c.avg_px_ticks);
        ins.bind_text(i++, to_string(c.base));
        ins.bind_i64(i++, static_cast<std::int64_t>(c.version));
        ins.bind_text(i++, c.accepted_at_utc);
        ins.bind_text(i++, c.terminal_at_utc);
        if (ins.step() != SQLITE_DONE) return fail(last_error());
    }

    // --- quote consumption -------------------------------------------------
    // Deducting liquidity and writing the fill row are the same transaction, so
    // W10 cannot leave consumed inventory with no fill behind it.
    for (const QuoteConsumption& q : txn.quote_consumptions) {
        const char* sql = (q.taker_side == Side::Buy)
                              ? "UPDATE quote_versions SET ask_available=ask_available-?"
                                " WHERE symbol=? AND version=? AND ask_available>=?"
                              : "UPDATE quote_versions SET bid_available=bid_available-?"
                                " WHERE symbol=? AND version=? AND bid_available>=?";
        Stmt up(db_, sql);
        if (!up.ok()) return fail(StoreError::Corrupt);
        up.bind_i64(1, q.qty);
        up.bind_text(2, q.symbol);
        up.bind_i64(3, static_cast<std::int64_t>(q.version));
        up.bind_i64(4, q.qty);
        if (up.step() != SQLITE_DONE) return fail(last_error());
        // The WHERE clause guards the inventory floor in SQL, so a logic error
        // upstream cannot drive availability negative (invariant I-12).
        if (sqlite3_changes(db_) != 1) return fail(StoreError::Inconsistent);
    }

    if (fp::Action::ReturnError == LPSIM_FP(fp::point::kTxBAfterQuoteConsume))
        return fail(StoreError::Full);

    // --- quote updates -----------------------------------------------------
    for (const QuoteSlice& q : txn.quote_updates) {
        {
            Stmt up(db_, "UPDATE quote_versions SET is_current=0 WHERE symbol=?");
            if (!up.ok()) return fail(StoreError::Corrupt);
            up.bind_text(1, q.symbol);
            if (up.step() != SQLITE_DONE) return fail(last_error());
        }
        Stmt ins(db_,
                 "INSERT INTO quote_versions"
                 "(symbol, version, bid_ticks, ask_ticks, bid_available, ask_available,"
                 " valid_from_ms, expires_at_ms, fixture_id, is_current)"
                 " VALUES(?,?,?,?,?,?,?,?,?,1)");
        if (!ins.ok()) return fail(StoreError::Corrupt);
        int i = 1;
        ins.bind_text(i++, q.symbol);
        ins.bind_i64(i++, static_cast<std::int64_t>(q.version));
        ins.bind_i64(i++, q.bid_ticks);
        ins.bind_i64(i++, q.ask_ticks);
        ins.bind_i64(i++, q.bid_available);
        ins.bind_i64(i++, q.ask_available);
        ins.bind_i64(i++, q.valid_from_ms);
        ins.bind_i64(i++, q.expires_at_ms);
        ins.bind_text(i++, q.source_fixture_id);
        if (ins.step() != SQLITE_DONE) return fail(last_error());
    }

    // --- fills -------------------------------------------------------------
    for (const Fill& f : txn.fills) {
        Stmt ins(db_,
                 "INSERT INTO fill_ledger"
                 "(fill_id, exec_id, order_id, quote_version, liquidity_slice, qty,"
                 " px_ticks, event_id, created_at) VALUES(?,?,?,?,?,?,?,?,?)");
        if (!ins.ok()) return fail(StoreError::Corrupt);
        int i = 1;
        ins.bind_text(i++, f.fill_id);
        ins.bind_text(i++, f.exec_id);
        ins.bind_text(i++, f.order_id);
        ins.bind_i64(i++, static_cast<std::int64_t>(f.quote_version));
        ins.bind_text(i++, f.liquidity_slice);
        ins.bind_i64(i++, f.qty);
        ins.bind_i64(i++, f.px_ticks);
        ins.bind_text(i++, f.event_id);
        ins.bind_text(i++, f.created_at_utc);
        // A duplicate ExecID or event_id trips the UNIQUE constraint and rolls
        // the whole decision back: no partial state, no second trade (E-14).
        if (ins.step() != SQLITE_DONE) return fail(last_error());
    }

    // --- outbox ------------------------------------------------------------
    for (const OutboxEntry& o : txn.outbox) {
        const std::string payload = serialize_body(o.body);
        Stmt ins(db_,
                 "INSERT INTO report_outbox"
                 "(event_id, order_id, report_type, canonical_payload, payload_hash,"
                 " exec_id, dispatch_state, outbound_seq, session_epoch, created_at,"
                 " insertion) VALUES(?,?,?,?,?,?,?,NULL,NULL,?,"
                 "(SELECT COALESCE(MAX(insertion),0)+1 FROM report_outbox))");
        if (!ins.ok()) return fail(StoreError::Corrupt);
        int i = 1;
        ins.bind_text(i++, o.event_id);
        ins.bind_text(i++, o.order_id);
        ins.bind_text(i++, to_string(o.type));
        ins.bind_blob(i++, payload);
        ins.bind_text(i++, std::to_string(canonical_hash64(payload)));
        ins.bind_text(i++, o.exec_id);
        ins.bind_text(i++, to_string(SendState::Pending));
        ins.bind_text(i++, o.created_at_utc);
        if (ins.step() != SQLITE_DONE) return fail(last_error());
    }

    // --- scheduler ---------------------------------------------------------
    for (const std::string& id : txn.executed_event_ids) {
        Stmt up(db_, "UPDATE scheduled_events SET executed=1 WHERE event_id=?");
        if (!up.ok()) return fail(StoreError::Corrupt);
        up.bind_text(1, id);
        if (up.step() != SQLITE_DONE) return fail(last_error());
    }
    for (const ScheduledEvent& ev : txn.new_events) {
        Stmt ins(db_,
                 "INSERT INTO scheduled_events"
                 "(event_id, due_us, kind, insertion, order_id, request_id, qty, px_ticks,"
                 " quote_version, quote_payload, fault_name, fault_repeats, executed)"
                 " VALUES(?,?,?,?,?,?,?,?,?,?,?,?,0)"
                 " ON CONFLICT(event_id) DO NOTHING");
        if (!ins.ok()) return fail(StoreError::Corrupt);
        int i = 1;
        ins.bind_text(i++, ev.event_id);
        ins.bind_i64(i++, ev.due_us);
        ins.bind_text(i++, to_string(ev.kind));
        ins.bind_i64(i++, static_cast<std::int64_t>(ev.insertion));
        ins.bind_text(i++, ev.order_id);
        ins.bind_text(i++, ev.request_id);
        ins.bind_i64(i++, ev.qty);
        ins.bind_i64(i++, ev.px_ticks);
        ins.bind_i64(i++, static_cast<std::int64_t>(ev.quote_version));
        ins.bind_text(i++, pack_quote(ev.quote));
        ins.bind_text(i++, ev.fault_name);
        ins.bind_i64(i++, ev.fault_repeats);
        if (ins.step() != SQLITE_DONE) return fail(last_error());
    }

    // --- inbound acknowledgement -------------------------------------------
    for (std::uint64_t seq : txn.acked_inbound_seqs) {
        Stmt up(db_,
                "UPDATE inbound_inbox SET delivered=1"
                " WHERE session_id=? AND msg_seq_num=?");
        if (!up.ok()) return fail(StoreError::Corrupt);
        up.bind_text(1, cfg_.session_id);
        up.bind_i64(2, static_cast<std::int64_t>(seq));
        if (up.step() != SQLITE_DONE) return fail(last_error());
    }

    // --- allocators --------------------------------------------------------
    struct AllocUpdate {
        const char* name;
        std::uint64_t value;
    };
    const AllocUpdate allocs[] = {{"order_id", txn.next_order_counter},
                                  {"exec_id", txn.next_exec_counter},
                                  {"fill_id", txn.next_fill_counter},
                                  {"event_id", txn.next_event_counter}};
    for (const AllocUpdate& a : allocs) {
        if (a.value == 0) continue;
        // MAX() so that a stale in-memory value can never move a counter back.
        Stmt up(db_,
                "UPDATE allocators SET next_counter=MAX(next_counter, ?) WHERE name=?");
        if (!up.ok()) return fail(StoreError::Corrupt);
        up.bind_i64(1, static_cast<std::int64_t>(a.value));
        up.bind_text(2, a.name);
        if (up.step() != SQLITE_DONE) return fail(last_error());
    }

    for (const std::string& line : txn.audit) {
        Stmt ins(db_, "INSERT INTO audit_log(line) VALUES(?)");
        if (!ins.ok()) return fail(StoreError::Corrupt);
        ins.bind_text(1, line);
        if (ins.step() != SQLITE_DONE) return fail(last_error());
    }

    {
        Stmt up(db_, "UPDATE session_state SET clean_shutdown=0 WHERE session_id=?");
        if (up.ok()) {
            up.bind_text(1, cfg_.session_id);
            up.step();
        }
    }

    if (fp::Action::ReturnError == LPSIM_FP(fp::point::kTxBBeforeCommit))
        return fail(StoreError::Full);

    const StoreError ce = commit();
    if (ce != StoreError::None) return ce;
    LPSIM_FP(fp::point::kTxBAfterCommit);
    return StoreError::None;
}

// ---------------------------------------------------------------------------
// TX-C
// ---------------------------------------------------------------------------

StoreError SqliteStore::send_once(const std::string& event_id, const CanonicalBody& body,
                                  const FrameEncoder& encode, SendOnceResult* out) {
    if (db_ == nullptr) return StoreError::IoError;
    *out = SendOnceResult{};

    const std::string payload = serialize_body(body);
    const std::string hash = std::to_string(canonical_hash64(payload));

    StoreError e = begin();
    if (e != StoreError::None) return e;

    std::string stored_hash;
    std::string dispatch;
    std::int64_t existing_seq = 0;
    {
        Stmt q(db_,
               "SELECT payload_hash, dispatch_state, COALESCE(outbound_seq,0)"
               " FROM report_outbox WHERE event_id=?");
        if (!q.ok()) {
            rollback();
            return StoreError::Corrupt;
        }
        q.bind_text(1, event_id);
        if (q.step() != SQLITE_ROW) {
            // No committed intent for this event. Issuing a sequence number now
            // would create a report the business state does not justify (J-17).
            rollback();
            return StoreError::Inconsistent;
        }
        stored_hash = q.text(0);
        dispatch = q.text(1);
        existing_seq = q.i64(2);
    }

    if (stored_hash != hash) {
        // Same event identity, different body. Sending either version would be
        // a lie about what was committed (I-13).
        rollback();
        return StoreError::Conflict;
    }

    if (send_state_from(dispatch) != SendState::Pending && existing_seq > 0) {
        // Already bound to a sequence number: return the journaled frame
        // unchanged. NextNumOut does NOT move again (I-12, I-15, PB-02).
        Stmt q(db_,
               "SELECT original_fix_frame FROM session_outbound_mapping WHERE event_id=?");
        if (!q.ok()) {
            rollback();
            return StoreError::Corrupt;
        }
        q.bind_text(1, event_id);
        if (q.step() != SQLITE_ROW) {
            rollback();
            return StoreError::Inconsistent;
        }
        out->msg_seq_num = static_cast<std::uint64_t>(existing_seq);
        out->frame = q.blob(0);
        out->newly_assigned = false;
        rollback();  // read-only path, nothing to commit
        return StoreError::None;
    }

    std::int64_t epoch = 1;
    std::int64_t next_out = 1;
    {
        Stmt q(db_, "SELECT epoch, next_out FROM session_state WHERE session_id=?");
        if (!q.ok()) {
            rollback();
            return StoreError::Corrupt;
        }
        q.bind_text(1, cfg_.session_id);
        if (q.step() != SQLITE_ROW) {
            rollback();
            return StoreError::Inconsistent;
        }
        epoch = q.i64(0);
        next_out = q.i64(1);
    }

    const std::uint64_t seq = static_cast<std::uint64_t>(next_out);
    const std::string frame = encode(seq);

    {
        Stmt ins(db_,
                 "INSERT INTO session_outbound_mapping"
                 "(session_id, epoch, event_id, msg_seq_num, original_fix_frame, created_at)"
                 " VALUES(?,?,?,?,?,'')");
        if (!ins.ok()) {
            rollback();
            return StoreError::Corrupt;
        }
        ins.bind_text(1, cfg_.session_id);
        ins.bind_i64(2, epoch);
        ins.bind_text(3, event_id);
        ins.bind_i64(4, static_cast<std::int64_t>(seq));
        ins.bind_blob(5, frame);
        if (ins.step() != SQLITE_DONE) {
            const StoreError err = last_error();
            rollback();
            return err;
        }
    }
    {
        Stmt up(db_,
                "UPDATE report_outbox SET dispatch_state=?, outbound_seq=?, session_epoch=?"
                " WHERE event_id=?");
        if (!up.ok()) {
            rollback();
            return StoreError::Corrupt;
        }
        up.bind_text(1, to_string(SendState::SeqAssigned));
        up.bind_i64(2, static_cast<std::int64_t>(seq));
        up.bind_i64(3, epoch);
        up.bind_text(4, event_id);
        if (up.step() != SQLITE_DONE) {
            const StoreError err = last_error();
            rollback();
            return err;
        }
    }
    {
        Stmt up(db_, "UPDATE session_state SET next_out=? WHERE session_id=?");
        if (!up.ok()) {
            rollback();
            return StoreError::Corrupt;
        }
        up.bind_i64(1, static_cast<std::int64_t>(seq + 1));
        up.bind_text(2, cfg_.session_id);
        if (up.step() != SQLITE_DONE) {
            const StoreError err = last_error();
            rollback();
            return err;
        }
    }

    if (fp::Action::ReturnError == LPSIM_FP(fp::point::kTxCBeforeCommit)) {
        rollback();
        return StoreError::Full;
    }
    e = commit();
    if (e != StoreError::None) return e;
    LPSIM_FP(fp::point::kTxCAfterCommit);

    out->msg_seq_num = seq;
    out->frame = frame;
    out->newly_assigned = true;
    return StoreError::None;
}

StoreError SqliteStore::mark_sent(const std::string& event_id) {
    if (db_ == nullptr) return StoreError::IoError;
    if (fp::Action::ReturnError == LPSIM_FP(fp::point::kTxDBeforeCommit))
        return StoreError::Full;
    Stmt up(db_,
            "UPDATE report_outbox SET dispatch_state=? WHERE event_id=? AND outbound_seq"
            " IS NOT NULL");
    if (!up.ok()) return StoreError::Corrupt;
    up.bind_text(1, to_string(SendState::Sent));
    up.bind_text(2, event_id);
    if (up.step() != SQLITE_DONE) return last_error();
    // TX-D never touches next_out, so losing it cannot cause a second original
    // sequence number for the same event (design doc 10.2).
    return sqlite3_changes(db_) == 1 ? StoreError::None : StoreError::Inconsistent;
}

StoreError SqliteStore::take_admin_seq(std::uint64_t* seq) {
    if (db_ == nullptr) return StoreError::IoError;
    StoreError e = begin();
    if (e != StoreError::None) return e;

    std::int64_t next_out = 1;
    {
        Stmt q(db_, "SELECT next_out FROM session_state WHERE session_id=?");
        if (!q.ok()) {
            rollback();
            return StoreError::Corrupt;
        }
        q.bind_text(1, cfg_.session_id);
        if (q.step() != SQLITE_ROW) {
            rollback();
            return StoreError::Inconsistent;
        }
        next_out = q.i64(0);
    }
    {
        Stmt up(db_, "UPDATE session_state SET next_out=? WHERE session_id=?");
        if (!up.ok()) {
            rollback();
            return StoreError::Corrupt;
        }
        up.bind_i64(1, next_out + 1);
        up.bind_text(2, cfg_.session_id);
        if (up.step() != SQLITE_DONE) {
            const StoreError err = last_error();
            rollback();
            return err;
        }
    }
    e = commit();
    if (e != StoreError::None) return e;
    *seq = static_cast<std::uint64_t>(next_out);
    return StoreError::None;
}

StoreError SqliteStore::pending_outbox(std::vector<OutboxEntry>* out) {
    out->clear();
    Stmt q(db_,
           "SELECT event_id, order_id, report_type, canonical_payload, exec_id,"
           " dispatch_state, COALESCE(outbound_seq,0), COALESCE(session_epoch,0), created_at"
           " FROM report_outbox WHERE dispatch_state!=? ORDER BY insertion");
    if (!q.ok()) return StoreError::Corrupt;
    q.bind_text(1, to_string(SendState::Sent));
    while (q.step() == SQLITE_ROW) {
        OutboxEntry e;
        e.event_id = q.text(0);
        e.order_id = q.text(1);
        e.type = report_from(q.text(2));
        if (!deserialize_body(q.blob(3), &e.body)) return StoreError::Corrupt;
        e.exec_id = q.text(4);
        e.send_state = send_state_from(q.text(5));
        e.outbound_seq = static_cast<std::uint64_t>(q.i64(6));
        e.session_epoch = static_cast<std::uint64_t>(q.i64(7));
        e.created_at_utc = q.text(8);
        out->push_back(std::move(e));
    }
    return StoreError::None;
}

StoreError SqliteStore::outbound_history(
    std::uint64_t begin_seq, std::uint64_t end_seq,
    std::vector<std::pair<std::uint64_t, std::string>>* out) {
    out->clear();
    if (end_seq < begin_seq) return StoreError::Inconsistent;

    Stmt q(db_,
           "SELECT msg_seq_num, original_fix_frame FROM session_outbound_mapping"
           " WHERE session_id=? AND msg_seq_num>=? AND msg_seq_num<=?"
           " ORDER BY msg_seq_num");
    if (!q.ok()) return StoreError::Corrupt;
    q.bind_text(1, cfg_.session_id);
    q.bind_i64(2, static_cast<std::int64_t>(begin_seq));
    q.bind_i64(3, static_cast<std::int64_t>(end_seq));
    while (q.step() == SQLITE_ROW) {
        out->emplace_back(static_cast<std::uint64_t>(q.i64(0)), q.blob(1));
    }
    return StoreError::None;
}

StoreError SqliteStore::load_session_state(SessionState* out) {
    Stmt q(db_,
           "SELECT session_id, sender_comp_id, target_comp_id, epoch, next_in, next_out"
           " FROM session_state WHERE session_id=?");
    if (!q.ok()) return StoreError::Corrupt;
    q.bind_text(1, cfg_.session_id);
    if (q.step() != SQLITE_ROW) return StoreError::Inconsistent;
    out->session_id = q.text(0);
    out->sender_comp_id = q.text(1);
    out->target_comp_id = q.text(2);
    out->epoch = static_cast<std::uint64_t>(q.i64(3));
    out->next_in = static_cast<std::uint64_t>(q.i64(4));
    out->next_out = static_cast<std::uint64_t>(q.i64(5));
    return StoreError::None;
}

StoreError SqliteStore::apply_coordinated_reset(std::uint64_t new_epoch) {
    StoreError e = begin();
    if (e != StoreError::None) return e;
    Stmt up(db_,
            "UPDATE session_state SET epoch=?, next_in=1, next_out=1 WHERE session_id=?");
    if (!up.ok()) {
        rollback();
        return StoreError::Corrupt;
    }
    up.bind_i64(1, static_cast<std::int64_t>(new_epoch));
    up.bind_text(2, cfg_.session_id);
    if (up.step() != SQLITE_DONE) {
        const StoreError err = last_error();
        rollback();
        return err;
    }
    // Nothing else is touched. Orders, fills, the request registry and the
    // ExecID allocator all survive a sequence reset (design doc 3.4, I-15).
    return commit();
}

std::size_t SqliteStore::count_fills() const {
    Stmt q(db_, "SELECT COUNT(*) FROM fill_ledger");
    if (!q.ok()) return 0;
    return q.step() == SQLITE_ROW ? static_cast<std::size_t>(q.i64(0)) : 0;
}

std::size_t SqliteStore::count_outbox_pending() const {
    Stmt q(db_, "SELECT COUNT(*) FROM report_outbox WHERE dispatch_state!='SENT'");
    if (!q.ok()) return 0;
    return q.step() == SQLITE_ROW ? static_cast<std::size_t>(q.i64(0)) : 0;
}

// ---------------------------------------------------------------------------
// Recovery
// ---------------------------------------------------------------------------

StoreError SqliteStore::load_allocators(RecoveryReport* out) {
    Stmt q(db_, "SELECT name, next_counter FROM allocators");
    if (!q.ok()) return StoreError::Corrupt;
    while (q.step() == SQLITE_ROW) {
        const std::string name = q.text(0);
        const std::uint64_t v = static_cast<std::uint64_t>(q.i64(1));
        if (name == "order_id") out->next_order_counter = v;
        else if (name == "exec_id") out->next_exec_counter = v;
        else if (name == "fill_id") out->next_fill_counter = v;
        else if (name == "event_id") out->next_event_counter = v;
    }
    return StoreError::None;
}

RecoveryReport SqliteStore::recover_and_verify() {
    RecoveryReport r;
    if (db_ == nullptr) {
        r.halt = HaltReason::StoreCorrupt;
        r.detail = "store not open";
        return r;
    }

    // Step 1: structural integrity. A database SQLite itself calls corrupt is
    // not something to repair in place.
    {
        Stmt q(db_, "PRAGMA integrity_check");
        if (!q.ok()) {
            r.halt = HaltReason::StoreCorrupt;
            r.detail = "integrity_check failed to run";
            return r;
        }
        if (q.step() == SQLITE_ROW && q.text(0) != "ok") {
            r.halt = HaltReason::StoreCorrupt;
            r.detail = "integrity_check: " + q.text(0);
            return r;
        }
    }

    // Step 2: identity. A snapshot restored onto a host with a different
    // profile must refuse to start rather than trade under the wrong CompIDs
    // (A-18, J-20).
    {
        Stmt q(db_,
               "SELECT sender_comp_id, target_comp_id, clean_shutdown FROM session_state"
               " WHERE session_id=?");
        if (!q.ok()) {
            r.halt = HaltReason::StoreCorrupt;
            r.detail = "session_state unreadable";
            return r;
        }
        q.bind_text(1, cfg_.session_id);
        if (q.step() != SQLITE_ROW) {
            r.halt = HaltReason::StoreCorrupt;
            r.detail = "session_state missing for " + cfg_.session_id;
            return r;
        }
        if (q.text(0) != cfg_.sender_comp_id || q.text(1) != cfg_.target_comp_id) {
            r.halt = HaltReason::IdentityMismatch;
            r.detail = "stored CompIDs do not match the configured profile";
            return r;
        }
        r.unclean_shutdown = q.i64(2) == 0;
    }

    if (load_session_state(&r.session) != StoreError::None) {
        r.halt = HaltReason::StoreCorrupt;
        r.detail = "session_state unreadable";
        return r;
    }

    // Step 3: fills, and the per-order totals recomputed from them.
    std::map<std::string, NotionalAccumulator> recomputed;
    std::set<std::string> exec_ids;
    std::set<std::string> fill_event_ids;
    {
        Stmt q(db_,
               "SELECT fill_id, exec_id, order_id, quote_version, liquidity_slice, qty,"
               " px_ticks, event_id, created_at FROM fill_ledger ORDER BY fill_id");
        if (!q.ok()) {
            r.halt = HaltReason::StoreCorrupt;
            r.detail = "fill_ledger unreadable";
            return r;
        }
        while (q.step() == SQLITE_ROW) {
            Fill f;
            f.fill_id = q.text(0);
            f.exec_id = q.text(1);
            f.order_id = q.text(2);
            f.quote_version = static_cast<std::uint64_t>(q.i64(3));
            f.liquidity_slice = q.text(4);
            f.qty = q.i64(5);
            f.px_ticks = q.i64(6);
            f.event_id = q.text(7);
            f.created_at_utc = q.text(8);

            if (!exec_ids.insert(f.exec_id).second) {
                r.halt = HaltReason::DuplicateExecId;
                r.detail = "duplicate ExecID " + f.exec_id;
                return r;
            }
            fill_event_ids.insert(f.event_id);
            if (!recomputed[f.order_id].add(f.qty, f.px_ticks)) {
                r.halt = HaltReason::LedgerMismatch;
                r.detail = "weighted notional overflow on " + f.order_id;
                return r;
            }
            r.fills.push_back(std::move(f));
        }
    }

    // Step 4: order chains, compared against the recomputed ledger.
    {
        Stmt q(db_,
               "SELECT order_id, firm, business_day, original_cl_ord_id, current_cl_ord_id,"
               " pending, pending_cl_ord_id, pending_qty, pending_limit_ticks, symbol,"
               " side, ord_type, tif, original_qty, effective_qty, limit_ticks, cum_qty,"
               " leaves_qty, avg_px_ticks, base_state, version, accepted_at, terminal_at"
               " FROM order_chains ORDER BY order_id");
        if (!q.ok()) {
            r.halt = HaltReason::StoreCorrupt;
            r.detail = "order_chains unreadable";
            return r;
        }
        while (q.step() == SQLITE_ROW) {
            OrderChain c;
            int i = 0;
            c.order_id = q.text(i++);
            c.firm = q.text(i++);
            c.business_day = q.text(i++);
            c.original_cl_ord_id = q.text(i++);
            c.current_cl_ord_id = q.text(i++);
            c.pending = pending_from(q.text(i++));
            c.pending_cl_ord_id = q.text(i++);
            c.pending_qty = q.i64(i++);
            c.pending_limit_ticks = q.i64(i++);
            c.symbol = q.text(i++);
            {
                const std::string side_s = q.text(i++);
                c.side = (side_s == "SELL") ? Side::Sell : Side::Buy;
            }
            i++;  // ord_type: v1 is Limit only
            c.ord_type = OrdType::Limit;
            {
                const std::string tif_s = q.text(i++);
                c.tif = (tif_s == "DAY") ? Tif::Day : Tif::Day;
            }
            c.original_qty = q.i64(i++);
            c.effective_qty = q.i64(i++);
            c.limit_ticks = q.i64(i++);
            c.cum_qty = q.i64(i++);
            c.leaves_qty = q.i64(i++);
            c.avg_px_ticks = q.i64(i++);
            c.base = base_from(q.text(i++));
            c.version = static_cast<std::uint64_t>(q.i64(i++));
            c.accepted_at_utc = q.text(i++);
            c.terminal_at_utc = q.text(i++);

            // The ledger is the authority. A snapshot that disagrees means
            // either a rolled-back restore or a corrupted row, and neither is
            // safe to trade on (E-13, J-14, J-17).
            const auto it = recomputed.find(c.order_id);
            const Units ledger_cum = it == recomputed.end() ? 0 : it->second.qty();
            const Ticks ledger_avg = it == recomputed.end() ? 0 : it->second.avg_px_ticks();
            if (ledger_cum != c.cum_qty) {
                r.halt = HaltReason::LedgerMismatch;
                r.detail = "CumQty mismatch on " + c.order_id + ": snapshot " +
                           format_qty(c.cum_qty) + " ledger " + format_qty(ledger_cum);
                return r;
            }
            if (ledger_avg != c.avg_px_ticks) {
                r.halt = HaltReason::LedgerMismatch;
                r.detail = "AvgPx mismatch on " + c.order_id;
                return r;
            }
            if (c.cum_qty > c.effective_qty) {
                r.halt = HaltReason::LedgerMismatch;
                r.detail = "CumQty exceeds OrderQty on " + c.order_id;
                return r;
            }
            if (c.leaves_qty != c.expected_leaves()) {
                r.halt = HaltReason::InvariantViolated;
                r.detail = "LeavesQty inconsistent on " + c.order_id;
                return r;
            }
            r.chains.push_back(std::move(c));
        }
    }

    // Step 5: every fill must have the outbox row that reports it. A fill with
    // no report is a trade the client can never learn about (J-17).
    for (const std::string& ev : fill_event_ids) {
        Stmt q(db_, "SELECT 1 FROM report_outbox WHERE event_id=?");
        if (!q.ok()) {
            r.halt = HaltReason::StoreCorrupt;
            r.detail = "report_outbox unreadable";
            return r;
        }
        q.bind_text(1, ev);
        if (q.step() != SQLITE_ROW) {
            r.halt = HaltReason::InvariantViolated;
            r.detail = "fill event " + ev + " has no outbox row";
            return r;
        }
    }

    if (pending_outbox(&r.outbox) != StoreError::None) {
        r.halt = HaltReason::StoreCorrupt;
        r.detail = "outbox unreadable";
        return r;
    }

    // Step 6: outbound history must not run past NextNumOut, and every
    // sequence-assigned row must have its frame journaled.
    {
        Stmt q(db_,
               "SELECT COALESCE(MAX(msg_seq_num),0) FROM session_outbound_mapping"
               " WHERE session_id=?");
        if (!q.ok()) {
            r.halt = HaltReason::StoreCorrupt;
            r.detail = "outbound mapping unreadable";
            return r;
        }
        q.bind_text(1, cfg_.session_id);
        if (q.step() == SQLITE_ROW) {
            const std::uint64_t max_seq = static_cast<std::uint64_t>(q.i64(0));
            if (max_seq + 1 > r.session.next_out) {
                // A restored snapshot whose NextNumOut sits below a sequence
                // number already issued would reuse that number (J-14, I-11).
                r.halt = HaltReason::StaleSnapshot;
                r.detail = "NextNumOut " + std::to_string(r.session.next_out) +
                           " is behind issued sequence " + std::to_string(max_seq);
                return r;
            }
        }
    }
    for (const OutboxEntry& o : r.outbox) {
        if (o.send_state == SendState::Pending) continue;
        Stmt q(db_,
               "SELECT msg_seq_num FROM session_outbound_mapping WHERE event_id=?");
        if (!q.ok()) {
            r.halt = HaltReason::StoreCorrupt;
            r.detail = "outbound mapping unreadable";
            return r;
        }
        q.bind_text(1, o.event_id);
        if (q.step() != SQLITE_ROW) {
            r.halt = HaltReason::MissingOutboundHistory;
            r.detail = "event " + o.event_id + " claims a sequence with no journaled frame";
            return r;
        }
        if (static_cast<std::uint64_t>(q.i64(0)) != o.outbound_seq) {
            r.halt = HaltReason::SequenceConflict;
            r.detail = "event " + o.event_id + " disagrees with its journaled sequence";
            return r;
        }
    }

    // Step 7: allocators against the high-water marks actually in use.
    if (load_allocators(&r) != StoreError::None) {
        r.halt = HaltReason::StoreCorrupt;
        r.detail = "allocators unreadable";
        return r;
    }
    {
        IdAllocator order_alloc = make_order_id_allocator(r.next_order_counter);
        IdAllocator exec_alloc = make_exec_id_allocator(r.next_exec_counter);
        IdAllocator fill_alloc = make_fill_id_allocator(r.next_fill_counter);
        IdAllocator event_alloc = make_event_id_allocator(r.next_event_counter);

        std::uint64_t max_order = 0, max_exec = 0, max_fill = 0, max_event = 0;
        for (const OrderChain& c : r.chains) {
            if (const auto n = order_alloc.parse_counter(c.order_id))
                max_order = std::max(max_order, *n);
        }
        for (const Fill& f : r.fills) {
            if (const auto n = exec_alloc.parse_counter(f.exec_id))
                max_exec = std::max(max_exec, *n);
            if (const auto n = fill_alloc.parse_counter(f.fill_id))
                max_fill = std::max(max_fill, *n);
        }
        {
            Stmt q(db_, "SELECT event_id FROM report_outbox");
            if (q.ok()) {
                while (q.step() == SQLITE_ROW) {
                    if (const auto n = event_alloc.parse_counter(q.text(0)))
                        max_event = std::max(max_event, *n);
                }
            }
        }

        const bool ok_order = order_alloc.adopt_highwater(max_order);
        const bool ok_exec = exec_alloc.adopt_highwater(max_exec);
        const bool ok_fill = fill_alloc.adopt_highwater(max_fill);
        const bool ok_event = event_alloc.adopt_highwater(max_event);

        r.next_order_counter = order_alloc.next_counter();
        r.next_exec_counter = exec_alloc.next_counter();
        r.next_fill_counter = fill_alloc.next_counter();
        r.next_event_counter = event_alloc.next_counter();

        if (!ok_order || !ok_exec || !ok_fill || !ok_event) {
            r.halt = HaltReason::AllocatorRollback;
            r.detail = "an identifier counter sits at or below an identifier already issued";
            return r;
        }
    }

    // Step 8: quotes, registry and the scheduler work list.
    {
        Stmt q(db_,
               "SELECT symbol, version, bid_ticks, ask_ticks, bid_available, ask_available,"
               " valid_from_ms, expires_at_ms, fixture_id FROM quote_versions"
               " WHERE is_current=1 ORDER BY symbol");
        if (q.ok()) {
            while (q.step() == SQLITE_ROW) {
                QuoteSlice s;
                int i = 0;
                s.symbol = q.text(i++);
                s.version = static_cast<std::uint64_t>(q.i64(i++));
                s.bid_ticks = q.i64(i++);
                s.ask_ticks = q.i64(i++);
                s.bid_available = q.i64(i++);
                s.ask_available = q.i64(i++);
                s.valid_from_ms = q.i64(i++);
                s.expires_at_ms = q.i64(i++);
                s.source_fixture_id = q.text(i++);
                r.quotes.push_back(std::move(s));
            }
        }
    }
    {
        Stmt q(db_,
               "SELECT firm, business_day, cl_ord_id, request_type, canonical_hash,"
               " COALESCE(order_id,''), outcome_state, reject_reason FROM client_requests");
        if (q.ok()) {
            while (q.step() == SQLITE_ROW) {
                RegistryRow row;
                row.key.firm = q.text(0);
                row.key.business_day = q.text(1);
                row.key.cl_ord_id = q.text(2);
                row.type = request_from(q.text(3));
                row.canonical_hash = std::strtoull(q.text(4).c_str(), nullptr, 10);
                row.order_id = q.text(5);
                row.outcome = outcome_from(q.text(6));
                row.reject = reject_from(q.text(7));
                r.registry.push_back(std::move(row));
            }
        }
    }
    {
        Stmt q(db_,
               "SELECT event_id, due_us, kind, insertion, order_id, request_id, qty,"
               " px_ticks, quote_version, quote_payload, fault_name, fault_repeats, executed"
               " FROM scheduled_events ORDER BY due_us, insertion");
        if (q.ok()) {
            while (q.step() == SQLITE_ROW) {
                ScheduledEvent e;
                int i = 0;
                e.event_id = q.text(i++);
                e.due_us = q.i64(i++);
                e.kind = kind_from(q.text(i++));
                e.insertion = static_cast<std::uint64_t>(q.i64(i++));
                e.order_id = q.text(i++);
                e.request_id = q.text(i++);
                e.qty = q.i64(i++);
                e.px_ticks = q.i64(i++);
                e.quote_version = static_cast<std::uint64_t>(q.i64(i++));
                unpack_quote(q.text(i++), &e.quote);
                e.fault_name = q.text(i++);
                e.fault_repeats = static_cast<int>(q.i64(i++));
                const bool executed = q.i64(i++) != 0;
                if (executed) r.executed_event_ids.push_back(e.event_id);
                else r.pending_events.push_back(std::move(e));
            }
        }
    }
    {
        Stmt q(db_, "SELECT COUNT(*) FROM inbound_inbox WHERE delivered=0");
        if (q.ok() && q.step() == SQLITE_ROW)
            r.pending_inbox = static_cast<std::size_t>(q.i64(0));
    }

    r.ok = true;
    r.halt = HaltReason::None;
    return r;
}

// ---------------------------------------------------------------------------
// Test seams
// ---------------------------------------------------------------------------

StoreError SqliteStore::test_force_fill_cum_qty(const std::string& order_id, Units cum_qty) {
    Stmt up(db_, "UPDATE order_chains SET cum_qty=? WHERE order_id=?");
    if (!up.ok()) return StoreError::Corrupt;
    up.bind_i64(1, cum_qty);
    up.bind_text(2, order_id);
    return up.step() == SQLITE_DONE ? StoreError::None : last_error();
}

StoreError SqliteStore::test_delete_outbox_row(const std::string& event_id) {
    Stmt d(db_, "DELETE FROM report_outbox WHERE event_id=?");
    if (!d.ok()) return StoreError::Corrupt;
    d.bind_text(1, event_id);
    return d.step() == SQLITE_DONE ? StoreError::None : last_error();
}

StoreError SqliteStore::test_delete_registry_row(const BusinessKey& key) {
    Stmt d(db_,
           "DELETE FROM client_requests WHERE firm=? AND business_day=? AND cl_ord_id=?");
    if (!d.ok()) return StoreError::Corrupt;
    d.bind_text(1, key.firm);
    d.bind_text(2, key.business_day);
    d.bind_text(3, key.cl_ord_id);
    return d.step() == SQLITE_DONE ? StoreError::None : last_error();
}

StoreError SqliteStore::test_set_allocator(const std::string& name,
                                          std::uint64_t next_counter) {
    Stmt up(db_, "UPDATE allocators SET next_counter=? WHERE name=?");
    if (!up.ok()) return StoreError::Corrupt;
    up.bind_i64(1, static_cast<std::int64_t>(next_counter));
    up.bind_text(2, name);
    return up.step() == SQLITE_DONE ? StoreError::None : last_error();
}

StoreError SqliteStore::test_corrupt_outbound_frame(std::uint64_t seq,
                                                    const std::string& frame) {
    Stmt up(db_,
            "UPDATE session_outbound_mapping SET original_fix_frame=?"
            " WHERE session_id=? AND msg_seq_num=?");
    if (!up.ok()) return StoreError::Corrupt;
    up.bind_blob(1, frame);
    up.bind_text(2, cfg_.session_id);
    up.bind_i64(3, static_cast<std::int64_t>(seq));
    return up.step() == SQLITE_DONE ? StoreError::None : last_error();
}

StoreError SqliteStore::test_delete_outbound_history(std::uint64_t seq) {
    Stmt d(db_,
           "DELETE FROM session_outbound_mapping WHERE session_id=? AND msg_seq_num=?");
    if (!d.ok()) return StoreError::Corrupt;
    d.bind_text(1, cfg_.session_id);
    d.bind_i64(2, static_cast<std::int64_t>(seq));
    return d.step() == SQLITE_DONE ? StoreError::None : last_error();
}

}  // namespace lpsim
