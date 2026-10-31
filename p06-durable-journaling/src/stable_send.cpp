#include "jrnl/stable_send.hpp"

#include "jrnl/failpoint.hpp"

namespace jrnl {
namespace {

constexpr std::string_view kSchema = R"SQL(
CREATE TABLE IF NOT EXISTS session_state (
  session_key TEXT PRIMARY KEY,
  next_seq    INTEGER NOT NULL DEFAULT 1 CHECK (next_seq > 0),
  epoch       INTEGER NOT NULL DEFAULT 1 CHECK (epoch > 0)
);

-- The stable mapping. The UNIQUE on (session_key, original_seq) is the
-- structural half of JI-19: one sequence number cannot serve two commands.
CREATE TABLE IF NOT EXISTS send_once (
  session_key  TEXT    NOT NULL,
  command_id   TEXT    NOT NULL,
  payload_hash TEXT    NOT NULL,
  original_seq INTEGER NOT NULL,
  payload      TEXT    NOT NULL,
  wire_written INTEGER NOT NULL DEFAULT 0 CHECK (wire_written IN (0,1)),
  created_at   INTEGER NOT NULL,
  PRIMARY KEY (session_key, command_id),
  UNIQUE (session_key, original_seq)
);

CREATE TABLE IF NOT EXISTS inbound_raw (
  session_key   TEXT    NOT NULL,
  session_epoch INTEGER NOT NULL,
  receipt_id    INTEGER NOT NULL,
  raw           TEXT    NOT NULL,
  raw_hash      TEXT    NOT NULL,
  delivered     INTEGER NOT NULL DEFAULT 0 CHECK (delivered IN (0,1)),
  acked         INTEGER NOT NULL DEFAULT 0 CHECK (acked IN (0,1)),
  received_at   INTEGER NOT NULL,
  PRIMARY KEY (session_key, session_epoch, receipt_id)
);
)SQL";

}  // namespace

StableSendStore::~StableSendStore() = default;

StoreError StableSendStore::open(const Options& opt, std::unique_ptr<StableSendStore>* out,
                                 std::string* why) {
    auto s = std::unique_ptr<StableSendStore>(new StableSendStore());

    Db::Options dopt;
    dopt.path = opt.path;
    dopt.create_if_missing = true;
    // Same durability profile as the journal. A session store that loses its
    // sequence mapping on a crash would let the engine mint a second
    // original send, which is the one thing JI-19 forbids.
    dopt.journal_mode = "WAL";
    dopt.synchronous = "FULL";
    StoreError e = Db::open(dopt, &s->db_, why);
    if (e != StoreError::None) return e;

    e = s->db_->exec(kSchema);
    if (e != StoreError::None) {
        if (why != nullptr) *why = "cannot create P01 schema: " + s->db_->last_error();
        return e;
    }

    if (opt.clock != nullptr) {
        s->clock_ = opt.clock;
    } else {
        s->owned_clock_ = std::make_unique<SystemClock>();
        s->clock_ = s->owned_clock_.get();
    }
    s->wire_ = opt.wire;
    *out = std::move(s);
    return StoreError::None;
}

SendBinding StableSendStore::send_once(const SessionKey& session, const CommandId& command,
                                       const Digest& payload_hash, const std::string& payload) {
    SendBinding b;

    std::uint64_t seq = 0;
    bool already_bound = false;
    bool already_written = false;
    {
        std::uint64_t existing_seq = 0;
        Digest existing_hash{};
        if (bound_sequence(session, command, &existing_seq, &existing_hash)) {
            if (!(existing_hash == payload_hash)) {
                // The same command id carrying different bytes. Allocating a
                // second sequence would be issuing a second order (JI-05).
                b.conflict = true;
                b.detail = "stable send key already bound to a different payload";
                return b;
            }
            seq = existing_seq;
            already_bound = true;
            Stmt q;
            if (db_->prepare("SELECT wire_written FROM send_once WHERE session_key=?1 AND "
                             "command_id=?2;",
                             &q) == StoreError::None) {
                q.bind_text(1, session);
                q.bind_text(2, command);
                bool row = false;
                if (q.step(&row) == StoreError::None && row) already_written = q.col_i64(0) != 0;
            }
        }
    }

    if (!already_bound) {
        // Allocate and persist the sequence BEFORE any byte leaves. This is
        // the whole point of the contract: after a crash, the mapping tells
        // us which sequence this command owns, so recovery never needs to
        // invent a new one.
        TxGuard tx(*db_);
        StoreError e = tx.begin();
        if (e != StoreError::None) {
            b.ambiguous = true;
            b.detail = "cannot begin session transaction";
            return b;
        }

        Stmt rd;
        e = db_->prepare("SELECT next_seq FROM session_state WHERE session_key=?1;", &rd);
        if (e != StoreError::None) {
            b.ambiguous = true;
            return b;
        }
        rd.bind_text(1, session);
        bool row = false;
        if (rd.step(&row) != StoreError::None) {
            b.ambiguous = true;
            return b;
        }
        seq = row ? rd.col_u64(0) : 1;
        rd.reset();

        Stmt ins;
        e = db_->prepare(
            "INSERT INTO send_once(session_key,command_id,payload_hash,original_seq,payload,"
            "wire_written,created_at) VALUES(?1,?2,?3,?4,?5,0,?6);",
            &ins);
        if (e != StoreError::None) {
            b.ambiguous = true;
            return b;
        }
        ins.bind_text(1, session);
        ins.bind_text(2, command);
        ins.bind_hex(3, payload_hash);
        ins.bind_u64(4, seq);
        ins.bind_text(5, payload);
        ins.bind_i64(6, clock_->now_ms());
        e = ins.run();
        if (e != StoreError::None) {
            b.conflict = e == StoreError::Conflict;
            b.ambiguous = !b.conflict;
            b.detail = "cannot persist stable send mapping: " + db_->last_error();
            return b;
        }

        Stmt up;
        e = db_->prepare(
            "INSERT INTO session_state(session_key,next_seq,epoch) VALUES(?1,?2,1) "
            "ON CONFLICT(session_key) DO UPDATE SET next_seq=excluded.next_seq;",
            &up);
        if (e != StoreError::None) {
            b.ambiguous = true;
            return b;
        }
        up.bind_text(1, session);
        up.bind_u64(2, seq + 1);
        e = up.run();
        if (e != StoreError::None) {
            b.ambiguous = true;
            return b;
        }

        e = tx.commit();
        if (e != StoreError::None) {
            // The mapping may or may not be durable. Reporting bound here
            // would be a guess, so the caller is told it is ambiguous.
            b.ambiguous = true;
            b.detail = "stable send mapping commit failed";
            return b;
        }
    }

    b.bound = true;
    b.original_seq = seq;

    // W14: the sequence is durable and nothing has been written yet.
    if (JRNL_FP(fp::point::kAfterSeqPersisted) == fp::Action::ReturnError) {
        b.ambiguous = true;
        b.detail = "crashed after sequence was persisted, before the write";
        return b;
    }

    if (already_written) {
        // Re-presenting a key whose bytes already went out does not send
        // them again from here. FIX session resend is the protocol-level
        // answer, and it reuses this same sequence (JI-19).
        return b;
    }

    // W15: a partial write. The peer may hold a fragment; nothing can be
    // concluded about whether it will be parsed.
    if (JRNL_FP(fp::point::kDuringSocketWrite) == fp::Action::ReturnError) {
        b.ambiguous = true;
        b.detail = "partial write on the session socket";
        return b;
    }

    bool wrote = true;
    if (wire_) wrote = wire_(session, seq, payload);
    if (!wrote) {
        b.ambiguous = true;
        b.detail = "session write reported failure";
        return b;
    }

    // W16: the bytes went out and the confirmation was lost. JI-17 forbids
    // treating a successful write as evidence of anything at the LP, and the
    // lost confirmation makes even the write unprovable from here.
    if (JRNL_FP(fp::point::kAfterSocketWrite) == fp::Action::ReturnError) {
        b.ambiguous = true;
        b.detail = "write completed but the confirmation was lost";
        return b;
    }

    Stmt mark;
    if (db_->prepare("UPDATE send_once SET wire_written=1 WHERE session_key=?1 AND "
                     "command_id=?2;",
                     &mark) == StoreError::None) {
        mark.bind_text(1, session);
        mark.bind_text(2, command);
        mark.run();
    }
    return b;
}

bool StableSendStore::bound_sequence(const SessionKey& session, const CommandId& command,
                                     std::uint64_t* seq, Digest* hash) const {
    Stmt s;
    if (const_cast<Db&>(*db_).prepare(
            "SELECT original_seq,payload_hash FROM send_once WHERE session_key=?1 AND "
            "command_id=?2;",
            &s) != StoreError::None) {
        return false;
    }
    s.bind_text(1, session);
    s.bind_text(2, command);
    bool row = false;
    if (s.step(&row) != StoreError::None || !row) return false;
    if (seq != nullptr) *seq = s.col_u64(0);
    if (hash != nullptr && !s.col_hex(1, hash)) return false;
    return true;
}

std::uint64_t StableSendStore::allocated_sequences(const SessionKey& session) const {
    Stmt s;
    if (const_cast<Db&>(*db_).prepare(
            "SELECT COUNT(DISTINCT original_seq) FROM send_once WHERE session_key=?1;", &s) !=
        StoreError::None) {
        return 0;
    }
    s.bind_text(1, session);
    bool row = false;
    if (s.step(&row) != StoreError::None || !row) return 0;
    return s.col_u64(0);
}

std::size_t StableSendStore::unconfirmed_sends(const SessionKey& session) const {
    Stmt s;
    if (const_cast<Db&>(*db_).prepare(
            "SELECT COUNT(*) FROM send_once WHERE session_key=?1 AND wire_written=0;", &s) !=
        StoreError::None) {
        return 0;
    }
    s.bind_text(1, session);
    bool row = false;
    if (s.step(&row) != StoreError::None || !row) return 0;
    return static_cast<std::size_t>(s.col_i64(0));
}

StoreError StableSendStore::persist_raw(const RawReceipt& r) {
    Stmt s;
    StoreError e = db_->prepare(
        "INSERT INTO inbound_raw(session_key,session_epoch,receipt_id,raw,raw_hash,delivered,"
        "acked,received_at) VALUES(?1,?2,?3,?4,?5,0,0,?6) "
        "ON CONFLICT(session_key,session_epoch,receipt_id) DO NOTHING;",
        &s);
    if (e != StoreError::None) return e;
    s.bind_text(1, r.session_key);
    s.bind_u64(2, r.session_epoch);
    s.bind_u64(3, r.receipt_id);
    s.bind_text(4, r.raw);
    s.bind_hex(5, r.raw_hash);
    s.bind_i64(6, clock_->now_ms());
    e = s.run();
    if (e == StoreError::None) {
        // W18: raw is durable at P01, nothing has been handed to P06 yet.
        JRNL_FP(fp::point::kAfterRawReceiptPersisted);
    }
    return e;
}

std::vector<RawReceipt> StableSendStore::undelivered(const SessionKey& session,
                                                     std::size_t max) const {
    std::vector<RawReceipt> out;
    Stmt s;
    // Note the condition: not acked, rather than not delivered. A message
    // that was handed over but never acknowledged is redelivered, which is
    // what "at least once" means and what JI-26 relies on.
    if (const_cast<Db&>(*db_).prepare(
            "SELECT session_epoch,receipt_id,raw,raw_hash,delivered FROM inbound_raw WHERE "
            "session_key=?1 AND acked=0 ORDER BY session_epoch,receipt_id LIMIT ?2;",
            &s) != StoreError::None) {
        return out;
    }
    s.bind_text(1, session);
    s.bind_i64(2, static_cast<std::int64_t>(max == 0 ? 1000000 : max));
    while (true) {
        bool row = false;
        if (s.step(&row) != StoreError::None || !row) break;
        RawReceipt r;
        r.session_key = session;
        r.session_epoch = s.col_u64(0);
        r.receipt_id = s.col_u64(1);
        r.raw = s.col_text(2);
        if (!s.col_hex(3, &r.raw_hash)) continue;
        r.delivered = s.col_i64(4) != 0;
        out.push_back(std::move(r));
    }
    return out;
}

StoreError StableSendStore::mark_delivered(const SessionKey& session, std::uint64_t epoch,
                                           std::uint64_t receipt_id) {
    Stmt s;
    StoreError e = db_->prepare(
        "UPDATE inbound_raw SET delivered=1 WHERE session_key=?1 AND session_epoch=?2 AND "
        "receipt_id=?3;",
        &s);
    if (e != StoreError::None) return e;
    s.bind_text(1, session);
    s.bind_u64(2, epoch);
    s.bind_u64(3, receipt_id);
    return s.run();
}

StoreError StableSendStore::ack_delivery(const SessionKey& session, std::uint64_t epoch,
                                         std::uint64_t receipt_id) {
    // W21 sits immediately before this: P06 has committed, the ack has not
    // been recorded, and the receipt will therefore be redelivered.
    if (JRNL_FP(fp::point::kBeforeDeliveryAck) == fp::Action::ReturnError) {
        return StoreError::IoError;
    }
    Stmt s;
    StoreError e = db_->prepare(
        "UPDATE inbound_raw SET acked=1, delivered=1 WHERE session_key=?1 AND session_epoch=?2 "
        "AND receipt_id=?3;",
        &s);
    if (e != StoreError::None) return e;
    s.bind_text(1, session);
    s.bind_u64(2, epoch);
    s.bind_u64(3, receipt_id);
    return s.run();
}

std::size_t StableSendStore::unacked_count(const SessionKey& session) const {
    Stmt s;
    if (const_cast<Db&>(*db_).prepare(
            "SELECT COUNT(*) FROM inbound_raw WHERE session_key=?1 AND acked=0;", &s) !=
        StoreError::None) {
        return 0;
    }
    s.bind_text(1, session);
    bool row = false;
    if (s.step(&row) != StoreError::None || !row) return 0;
    return static_cast<std::size_t>(s.col_i64(0));
}

std::uint64_t StableSendStore::current_epoch(const SessionKey& session) const {
    Stmt s;
    if (const_cast<Db&>(*db_).prepare("SELECT epoch FROM session_state WHERE session_key=?1;",
                                      &s) != StoreError::None) {
        return 1;
    }
    s.bind_text(1, session);
    bool row = false;
    if (s.step(&row) != StoreError::None || !row) return 1;
    return s.col_u64(0);
}

StoreError StableSendStore::bump_epoch(const SessionKey& session, std::uint64_t* now) {
    const std::uint64_t next = current_epoch(session) + 1;
    Stmt s;
    StoreError e = db_->prepare(
        "INSERT INTO session_state(session_key,next_seq,epoch) VALUES(?1,1,?2) "
        "ON CONFLICT(session_key) DO UPDATE SET epoch=excluded.epoch;",
        &s);
    if (e != StoreError::None) return e;
    s.bind_text(1, session);
    s.bind_u64(2, next);
    e = s.run();
    if (e == StoreError::None && now != nullptr) *now = next;
    return e;
}

}  // namespace jrnl
