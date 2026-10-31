#include "jrnl/schema.hpp"

#include <string>

#include "jrnl/sql.hpp"

namespace jrnl {

const std::string_view kSqliteSchema = R"SQL(
-- ---------------------------------------------------------------------------
-- Metadata
-- ---------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS schema_meta (
  key   TEXT PRIMARY KEY,
  value TEXT NOT NULL
);

-- ---------------------------------------------------------------------------
-- Journal. Append-only; the authority every other table is derived from.
-- ---------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS stream_heads (
  tenant_id       TEXT    NOT NULL,
  stream_id       TEXT    NOT NULL,
  current_version INTEGER NOT NULL DEFAULT 0 CHECK (current_version >= 0),
  state_hash      TEXT    NOT NULL,
  reducer_version TEXT    NOT NULL,
  fencing_token   INTEGER NOT NULL,
  PRIMARY KEY (tenant_id, stream_id)
);

CREATE TABLE IF NOT EXISTS journal_transactions (
  tenant_id            TEXT    NOT NULL,
  tx_id                TEXT    NOT NULL,
  expected_event_count INTEGER NOT NULL CHECK (expected_event_count > 0),
  members_digest       TEXT    NOT NULL,
  created_at           INTEGER NOT NULL,
  PRIMARY KEY (tenant_id, tx_id)
);

CREATE TABLE IF NOT EXISTS journal_events (
  tenant_id       TEXT    NOT NULL,
  stream_id       TEXT    NOT NULL,
  stream_version  INTEGER NOT NULL CHECK (stream_version > 0),
  event_id        TEXT    NOT NULL,
  tx_id           TEXT    NOT NULL,
  event_ordinal   INTEGER NOT NULL CHECK (event_ordinal >= 0),
  event_type      TEXT    NOT NULL,
  schema_version  INTEGER NOT NULL CHECK (schema_version > 0),
  reducer_version TEXT    NOT NULL,
  policy_version  INTEGER NOT NULL,
  payload         TEXT    NOT NULL,
  canonical_hash  TEXT    NOT NULL,
  prev_state_hash TEXT    NOT NULL,
  state_hash      TEXT    NOT NULL,
  -- Audit metadata only. Section 8 note 8: created_at never decides replay
  -- order; stream_version does.
  created_at      INTEGER NOT NULL,
  PRIMARY KEY (tenant_id, stream_id, stream_version),
  UNIQUE (tenant_id, event_id),
  UNIQUE (tenant_id, tx_id, event_ordinal),
  FOREIGN KEY (tenant_id, tx_id)
    REFERENCES journal_transactions (tenant_id, tx_id)
);

CREATE INDEX IF NOT EXISTS idx_journal_tx ON journal_events (tenant_id, tx_id);

-- Defence in depth for JI-02. The doc is explicit that a trigger does not
-- stop a superuser; it stops an ordinary mistake, and the integrity verifier
-- is what catches the rest.
CREATE TRIGGER IF NOT EXISTS journal_events_append_only_update
BEFORE UPDATE ON journal_events
BEGIN
  SELECT RAISE(ABORT, 'journal_events is append-only');
END;

CREATE TRIGGER IF NOT EXISTS journal_events_append_only_delete
BEFORE DELETE ON journal_events
BEGIN
  SELECT RAISE(ABORT, 'journal_events is append-only');
END;

-- ---------------------------------------------------------------------------
-- Projections. Derived from the journal inside the same transaction.
-- ---------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS orders (
  tenant_id      TEXT    NOT NULL,
  order_chain_id TEXT    NOT NULL,
  account_id     TEXT    NOT NULL,
  risk_domain    TEXT    NOT NULL,
  status         TEXT    NOT NULL,
  order_qty      INTEGER NOT NULL CHECK (order_qty >= 0),
  cum_qty        INTEGER NOT NULL CHECK (cum_qty >= 0),
  leaves_qty     INTEGER NOT NULL CHECK (leaves_qty >= 0),
  state          TEXT    NOT NULL,
  state_hash     TEXT    NOT NULL,
  stream_version INTEGER NOT NULL,
  updated_tx     TEXT    NOT NULL,
  PRIMARY KEY (tenant_id, order_chain_id),
  -- The database half of JI-08. The reducer refuses an overfill first; this
  -- stops a projection write that somehow disagreed with it. SQLite requires
  -- table constraints after every column, so it sits here rather than beside
  -- the columns it talks about.
  CHECK (cum_qty + leaves_qty <= order_qty)
);

CREATE TABLE IF NOT EXISTS fills (
  tenant_id        TEXT    NOT NULL,
  lp_namespace     TEXT    NOT NULL,
  semantic_exec_id TEXT    NOT NULL,
  order_chain_id   TEXT    NOT NULL,
  last_qty         INTEGER NOT NULL CHECK (last_qty > 0),
  last_px          INTEGER NOT NULL CHECK (last_px >= 0),
  applied_tx       TEXT    NOT NULL,
  created_at       INTEGER NOT NULL,
  PRIMARY KEY (tenant_id, lp_namespace, semantic_exec_id)
);

CREATE TABLE IF NOT EXISTS risk_domains (
  tenant_id      TEXT    NOT NULL,
  domain_id      TEXT    NOT NULL,
  limit_amount   INTEGER NOT NULL CHECK (limit_amount >= 0),
  reserved       INTEGER NOT NULL CHECK (reserved >= 0),
  exposure       INTEGER NOT NULL CHECK (exposure >= 0),
  revision       INTEGER NOT NULL CHECK (revision >= 0),
  state          TEXT    NOT NULL,
  state_hash     TEXT    NOT NULL,
  stream_version INTEGER NOT NULL,
  updated_tx     TEXT    NOT NULL,
  PRIMARY KEY (tenant_id, domain_id),
  -- JI-09 at the database level: the committed state may never show more
  -- committed than the ceiling allows.
  CHECK (reserved + exposure <= limit_amount)
);

CREATE TABLE IF NOT EXISTS risk_reservations (
  tenant_id      TEXT    NOT NULL,
  domain_id      TEXT    NOT NULL,
  reservation_id TEXT    NOT NULL,
  account_id     TEXT    NOT NULL,
  order_chain_id TEXT    NOT NULL,
  amount         INTEGER NOT NULL CHECK (amount >= 0),
  state          TEXT    NOT NULL CHECK (state IN ('Held','Released','Consumed')),
  updated_tx     TEXT    NOT NULL,
  PRIMARY KEY (tenant_id, domain_id, reservation_id),
  FOREIGN KEY (tenant_id, domain_id) REFERENCES risk_domains (tenant_id, domain_id)
);

-- ---------------------------------------------------------------------------
-- Dedup and intent records
-- ---------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS inbox_receipts (
  tenant_id     TEXT    NOT NULL,
  session_key   TEXT    NOT NULL,
  session_epoch INTEGER NOT NULL,
  receipt_id    INTEGER NOT NULL,
  payload_hash  TEXT    NOT NULL,
  apply_state   TEXT    NOT NULL CHECK (apply_state IN ('APPLIED','NOOP','QUARANTINED')),
  tx_id         TEXT    NOT NULL,
  received_at   INTEGER NOT NULL,
  PRIMARY KEY (tenant_id, session_key, session_epoch, receipt_id)
);

CREATE TABLE IF NOT EXISTS command_intents (
  tenant_id           TEXT    NOT NULL,
  account_id          TEXT    NOT NULL,
  command_id          TEXT    NOT NULL,
  order_chain_id      TEXT    NOT NULL,
  canonical_hash      TEXT    NOT NULL,
  risk_reservation_id TEXT,
  decision            TEXT    NOT NULL,
  result              TEXT    NOT NULL,
  created_tx          TEXT    NOT NULL,
  created_at          INTEGER NOT NULL,
  PRIMARY KEY (tenant_id, account_id, command_id)
);

CREATE TABLE IF NOT EXISTS command_outbox (
  outbox_id             TEXT    PRIMARY KEY,
  tenant_id             TEXT    NOT NULL,
  account_id            TEXT    NOT NULL,
  command_id            TEXT    NOT NULL,
  session_key           TEXT    NOT NULL,
  original_payload_hash TEXT    NOT NULL,
  original_payload      TEXT    NOT NULL,
  state                 TEXT    NOT NULL CHECK (state IN
                          ('READY','CLAIMED','BOUND','UNKNOWN','CONFIRMED','HELD')),
  p01_original_seq      INTEGER,
  claim_owner           TEXT,
  claim_token           INTEGER,
  claim_expires_at      INTEGER,
  attempts              INTEGER NOT NULL DEFAULT 0,
  -- Set, and committed, BEFORE the dispatcher calls P01. Section 10 is
  -- emphatic that a claim timing out does not prove the send never happened;
  -- this column is the only thing that can distinguish the two cases, and it
  -- decides whether an expired claim returns to READY or becomes UNKNOWN.
  send_attempted        INTEGER NOT NULL DEFAULT 0 CHECK (send_attempted IN (0,1)),
  last_error            TEXT,
  created_tx            TEXT    NOT NULL,
  created_at            INTEGER NOT NULL,
  UNIQUE (tenant_id, account_id, command_id),
  FOREIGN KEY (tenant_id, account_id, command_id)
    REFERENCES command_intents (tenant_id, account_id, command_id)
);

CREATE INDEX IF NOT EXISTS idx_outbox_ready ON command_outbox (state, claim_expires_at);

CREATE TABLE IF NOT EXISTS business_exec_keys (
  tenant_id        TEXT    NOT NULL,
  lp_namespace     TEXT    NOT NULL,
  semantic_exec_id TEXT    NOT NULL,
  fingerprint      TEXT    NOT NULL,
  order_chain_id   TEXT    NOT NULL,
  applied_tx       TEXT    NOT NULL,
  created_at       INTEGER NOT NULL,
  PRIMARY KEY (tenant_id, lp_namespace, semantic_exec_id)
);

-- ---------------------------------------------------------------------------
-- Ownership, recovery and audit
-- ---------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS owner_fences (
  tenant_id   TEXT    NOT NULL,
  domain_id   TEXT    NOT NULL,
  token       INTEGER NOT NULL CHECK (token > 0),
  lease_owner TEXT    NOT NULL,
  lease_until INTEGER NOT NULL,
  PRIMARY KEY (tenant_id, domain_id)
);

CREATE TABLE IF NOT EXISTS recovery_cases (
  tenant_id      TEXT    NOT NULL,
  case_id        TEXT    NOT NULL,
  order_chain_id TEXT    NOT NULL,
  reason_code    TEXT    NOT NULL,
  evidence_hash  TEXT,
  status         TEXT    NOT NULL,
  opened_at      INTEGER NOT NULL,
  resolved_at    INTEGER,
  approvals      TEXT    NOT NULL DEFAULT '',
  PRIMARY KEY (tenant_id, case_id)
);

CREATE INDEX IF NOT EXISTS idx_cases_open ON recovery_cases (tenant_id, status);

CREATE TABLE IF NOT EXISTS snapshots (
  tenant_id           TEXT    NOT NULL,
  stream_id           TEXT    NOT NULL,
  stream_version      INTEGER NOT NULL,
  schema_version      INTEGER NOT NULL,
  reducer_version     TEXT    NOT NULL,
  serialized_state    TEXT    NOT NULL,
  content_hash        TEXT    NOT NULL,
  state_hash          TEXT    NOT NULL,
  journal_anchor_hash TEXT    NOT NULL,
  verified            INTEGER NOT NULL DEFAULT 0 CHECK (verified IN (0,1)),
  verified_at         INTEGER,
  created_at          INTEGER NOT NULL,
  PRIMARY KEY (tenant_id, stream_id, stream_version)
);

-- Hash-chained so that a removed or edited row is visible. Section 18 is
-- clear that this is tamper evidence inside the trust domain, not tamper
-- resistance against whoever owns the database.
CREATE TABLE IF NOT EXISTS audit_log (
  audit_id   INTEGER PRIMARY KEY AUTOINCREMENT,
  tenant_id  TEXT    NOT NULL,
  at         INTEGER NOT NULL,
  category   TEXT    NOT NULL,
  detail     TEXT    NOT NULL,
  data       TEXT    NOT NULL,
  tx_id      TEXT,
  prev_hash  TEXT    NOT NULL,
  entry_hash TEXT    NOT NULL
);

CREATE INDEX IF NOT EXISTS idx_audit_tenant ON audit_log (tenant_id, audit_id);
)SQL";

int read_schema_version(Db& db) {
    Stmt s;
    if (db.prepare("SELECT value FROM schema_meta WHERE key='schema_version';", &s) !=
        StoreError::None) {
        return -1;
    }
    bool row = false;
    if (s.step(&row) != StoreError::None || !row) return -1;
    const std::string v = s.col_text(0);
    try {
        return std::stoi(v);
    } catch (...) {
        return -1;
    }
}

StoreError apply_schema(Db& db, std::string* why) {
    const int existing = read_schema_version(db);
    if (existing > 0 && existing != kSchemaVersion) {
        if (why != nullptr) {
            *why = "database schema version " + std::to_string(existing) +
                   " does not match engine version " + std::to_string(kSchemaVersion);
        }
        return StoreError::SchemaMismatch;
    }

    StoreError e = db.exec(kSqliteSchema);
    if (e != StoreError::None) {
        if (why != nullptr) *why = "cannot create schema: " + db.last_error();
        return e;
    }
    if (existing < 0) {
        e = db.exec("INSERT OR REPLACE INTO schema_meta(key,value) VALUES('schema_version','" +
                    std::to_string(kSchemaVersion) + "');");
        if (e != StoreError::None) {
            if (why != nullptr) *why = "cannot record schema version: " + db.last_error();
            return e;
        }
    }
    return StoreError::None;
}

}  // namespace jrnl
