-- P06 core schema, PostgreSQL 17.
--
-- This is the deployment target named in design doc section 8, kept
-- deliberately close to the SQLite schema in src/schema.cpp so that the
-- store logic is portable between them. Where the two engines genuinely
-- differ the reason is commented in both files.
--
-- Differences from the SQLite form, and why:
--
--   * bytea instead of 64-character hex text. SQLite has no bytea; the hex
--     rendering there is a storage detail, not part of the hash preimage.
--   * jsonb instead of canonical TEXT for payloads. The canonical bytes
--     remain the hash preimage either way: jsonb must NOT be re-serialised
--     before hashing, because its key order is its own.
--   * timestamptz instead of epoch milliseconds, for audit columns only.
--     Section 8 note 8 still applies: created_at never decides replay
--     order, stream_version does.
--   * bigserial appears only on audit_log, which is append-only commentary.
--     Section 7's warning stands: no consumer may derive commit order from
--     an id a sequence handed out before COMMIT.
--
-- NOT YET DONE, and required before production (section 8's closing note):
-- role grants, row-level security, partitioning of journal_events by
-- tenant and time, and a migration runner. They are listed here rather
-- than silently omitted.

BEGIN;

CREATE TABLE IF NOT EXISTS schema_meta (
  key   text PRIMARY KEY,
  value text NOT NULL
);
INSERT INTO schema_meta(key, value) VALUES ('schema_version', '1')
  ON CONFLICT (key) DO NOTHING;

-- ---------------------------------------------------------------------------
-- Journal
-- ---------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS stream_heads (
  tenant_id       text   NOT NULL,
  stream_id       text   NOT NULL,
  current_version bigint NOT NULL DEFAULT 0 CHECK (current_version >= 0),
  state_hash      bytea  NOT NULL,
  reducer_version text   NOT NULL,
  fencing_token   bigint NOT NULL,
  PRIMARY KEY (tenant_id, stream_id)
);

CREATE TABLE IF NOT EXISTS journal_transactions (
  tenant_id            text        NOT NULL,
  tx_id                text        NOT NULL,
  expected_event_count int         NOT NULL CHECK (expected_event_count > 0),
  members_digest       bytea       NOT NULL,
  created_at           timestamptz NOT NULL DEFAULT now(),
  PRIMARY KEY (tenant_id, tx_id)
);

CREATE TABLE IF NOT EXISTS journal_events (
  tenant_id       text        NOT NULL,
  stream_id       text        NOT NULL,
  stream_version  bigint      NOT NULL CHECK (stream_version > 0),
  event_id        text        NOT NULL,
  tx_id           text        NOT NULL,
  event_ordinal   int         NOT NULL CHECK (event_ordinal >= 0),
  event_type      text        NOT NULL,
  schema_version  int         NOT NULL CHECK (schema_version > 0),
  reducer_version text        NOT NULL,
  policy_version  bigint      NOT NULL,
  payload         jsonb       NOT NULL,
  canonical_hash  bytea       NOT NULL,
  prev_state_hash bytea       NOT NULL,
  state_hash      bytea       NOT NULL,
  created_at      timestamptz NOT NULL DEFAULT now(),
  PRIMARY KEY (tenant_id, stream_id, stream_version),
  UNIQUE (tenant_id, event_id),
  UNIQUE (tenant_id, tx_id, event_ordinal),
  FOREIGN KEY (tenant_id, tx_id)
    REFERENCES journal_transactions (tenant_id, tx_id)
);

CREATE INDEX IF NOT EXISTS idx_journal_tx ON journal_events (tenant_id, tx_id);

-- Defence in depth for JI-02. Section 8 is explicit that this stops an
-- ordinary mistake, not a superuser; the integrity verifier catches the rest.
CREATE OR REPLACE FUNCTION journal_events_append_only() RETURNS trigger AS $$
BEGIN
  RAISE EXCEPTION 'journal_events is append-only';
END;
$$ LANGUAGE plpgsql;

DROP TRIGGER IF EXISTS journal_events_no_update ON journal_events;
CREATE TRIGGER journal_events_no_update
  BEFORE UPDATE ON journal_events
  FOR EACH ROW EXECUTE FUNCTION journal_events_append_only();

DROP TRIGGER IF EXISTS journal_events_no_delete ON journal_events;
CREATE TRIGGER journal_events_no_delete
  BEFORE DELETE ON journal_events
  FOR EACH ROW EXECUTE FUNCTION journal_events_append_only();

-- ---------------------------------------------------------------------------
-- Projections, derived inside the same transaction as the events
-- ---------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS orders (
  tenant_id      text   NOT NULL,
  order_chain_id text   NOT NULL,
  account_id     text   NOT NULL,
  risk_domain    text   NOT NULL,
  status         text   NOT NULL,
  order_qty      bigint NOT NULL CHECK (order_qty >= 0),
  cum_qty        bigint NOT NULL CHECK (cum_qty >= 0),
  leaves_qty     bigint NOT NULL CHECK (leaves_qty >= 0),
  state          text   NOT NULL,
  state_hash     bytea  NOT NULL,
  stream_version bigint NOT NULL,
  updated_tx     text   NOT NULL,
  PRIMARY KEY (tenant_id, order_chain_id),
  -- The database half of JI-08.
  CHECK (cum_qty + leaves_qty <= order_qty)
);

CREATE TABLE IF NOT EXISTS fills (
  tenant_id        text        NOT NULL,
  lp_namespace     text        NOT NULL,
  semantic_exec_id text        NOT NULL,
  order_chain_id   text        NOT NULL,
  last_qty         bigint      NOT NULL CHECK (last_qty > 0),
  last_px          bigint      NOT NULL CHECK (last_px >= 0),
  applied_tx       text        NOT NULL,
  created_at       timestamptz NOT NULL DEFAULT now(),
  PRIMARY KEY (tenant_id, lp_namespace, semantic_exec_id)
);

CREATE TABLE IF NOT EXISTS risk_domains (
  tenant_id      text   NOT NULL,
  domain_id      text   NOT NULL,
  limit_amount   bigint NOT NULL CHECK (limit_amount >= 0),
  reserved       bigint NOT NULL CHECK (reserved >= 0),
  exposure       bigint NOT NULL CHECK (exposure >= 0),
  revision       bigint NOT NULL CHECK (revision >= 0),
  state          text   NOT NULL,
  state_hash     bytea  NOT NULL,
  stream_version bigint NOT NULL,
  updated_tx     text   NOT NULL,
  PRIMARY KEY (tenant_id, domain_id),
  -- JI-09 at the database level.
  CHECK (reserved + exposure <= limit_amount)
);

CREATE TABLE IF NOT EXISTS risk_reservations (
  tenant_id      text   NOT NULL,
  domain_id      text   NOT NULL,
  reservation_id text   NOT NULL,
  account_id     text   NOT NULL,
  order_chain_id text   NOT NULL,
  amount         bigint NOT NULL CHECK (amount >= 0),
  state          text   NOT NULL CHECK (state IN ('Held','Released','Consumed')),
  updated_tx     text   NOT NULL,
  PRIMARY KEY (tenant_id, domain_id, reservation_id),
  FOREIGN KEY (tenant_id, domain_id) REFERENCES risk_domains (tenant_id, domain_id)
);

-- ---------------------------------------------------------------------------
-- Dedup and intent
-- ---------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS inbox_receipts (
  tenant_id     text        NOT NULL,
  session_key   text        NOT NULL,
  session_epoch bigint      NOT NULL,
  receipt_id    bigint      NOT NULL,
  payload_hash  bytea       NOT NULL,
  apply_state   text        NOT NULL CHECK (apply_state IN ('APPLIED','NOOP','QUARANTINED')),
  tx_id         text        NOT NULL,
  received_at   timestamptz NOT NULL DEFAULT now(),
  PRIMARY KEY (tenant_id, session_key, session_epoch, receipt_id)
);

CREATE TABLE IF NOT EXISTS command_intents (
  tenant_id           text        NOT NULL,
  account_id          text        NOT NULL,
  command_id          text        NOT NULL,
  order_chain_id      text        NOT NULL,
  canonical_hash      bytea       NOT NULL,
  risk_reservation_id text,
  decision            text        NOT NULL,
  result              text        NOT NULL,
  created_tx          text        NOT NULL,
  created_at          timestamptz NOT NULL DEFAULT now(),
  PRIMARY KEY (tenant_id, account_id, command_id)
);

CREATE TABLE IF NOT EXISTS command_outbox (
  outbox_id             text        PRIMARY KEY,
  tenant_id             text        NOT NULL,
  account_id            text        NOT NULL,
  command_id            text        NOT NULL,
  session_key           text        NOT NULL,
  original_payload_hash bytea       NOT NULL,
  original_payload      bytea       NOT NULL,
  state                 text        NOT NULL CHECK (state IN
                          ('READY','CLAIMED','BOUND','UNKNOWN','CONFIRMED','HELD')),
  p01_original_seq      bigint,
  claim_owner           text,
  claim_token           bigint,
  claim_expires_at      timestamptz,
  attempts              int         NOT NULL DEFAULT 0,
  -- Committed BEFORE the dispatcher calls P01. Section 10: without it an
  -- expired claim cannot be told apart from one that may have sent.
  send_attempted        boolean     NOT NULL DEFAULT false,
  last_error            text,
  created_tx            text        NOT NULL,
  created_at            timestamptz NOT NULL DEFAULT now(),
  UNIQUE (tenant_id, account_id, command_id),
  FOREIGN KEY (tenant_id, account_id, command_id)
    REFERENCES command_intents (tenant_id, account_id, command_id)
);

-- The claim query is SELECT ... FOR UPDATE SKIP LOCKED over this index.
-- Section 9.3: SKIP LOCKED is a queue device, not a commit-order guarantee.
CREATE INDEX IF NOT EXISTS idx_outbox_ready
  ON command_outbox (tenant_id, state, created_at);

CREATE TABLE IF NOT EXISTS business_exec_keys (
  tenant_id        text        NOT NULL,
  lp_namespace     text        NOT NULL,
  semantic_exec_id text        NOT NULL,
  fingerprint      bytea       NOT NULL,
  order_chain_id   text        NOT NULL,
  applied_tx       text        NOT NULL,
  created_at       timestamptz NOT NULL DEFAULT now(),
  PRIMARY KEY (tenant_id, lp_namespace, semantic_exec_id)
);

-- ---------------------------------------------------------------------------
-- Ownership, recovery, snapshots, audit
-- ---------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS owner_fences (
  tenant_id   text        NOT NULL,
  domain_id   text        NOT NULL,
  token       bigint      NOT NULL CHECK (token > 0),
  lease_owner text        NOT NULL,
  lease_until timestamptz NOT NULL,
  PRIMARY KEY (tenant_id, domain_id)
);

CREATE TABLE IF NOT EXISTS recovery_cases (
  tenant_id      text        NOT NULL,
  case_id        text        NOT NULL,
  order_chain_id text        NOT NULL,
  reason_code    text        NOT NULL,
  evidence_hash  bytea,
  status         text        NOT NULL,
  opened_at      timestamptz NOT NULL DEFAULT now(),
  resolved_at    timestamptz,
  approvals      text        NOT NULL DEFAULT '',
  PRIMARY KEY (tenant_id, case_id)
);

CREATE INDEX IF NOT EXISTS idx_cases_open ON recovery_cases (tenant_id, status);

CREATE TABLE IF NOT EXISTS snapshots (
  tenant_id           text        NOT NULL,
  stream_id           text        NOT NULL,
  stream_version      bigint      NOT NULL,
  schema_version      int         NOT NULL,
  reducer_version     text        NOT NULL,
  serialized_state    bytea       NOT NULL,
  content_hash        bytea       NOT NULL,
  state_hash          bytea       NOT NULL,
  journal_anchor_hash bytea       NOT NULL,
  -- Set only after a read-back, a checksum check and a replay from the
  -- snapshot that reaches the journal head (section 13).
  verified            boolean     NOT NULL DEFAULT false,
  verified_at         timestamptz,
  created_at          timestamptz NOT NULL DEFAULT now(),
  PRIMARY KEY (tenant_id, stream_id, stream_version)
);

CREATE TABLE IF NOT EXISTS audit_log (
  audit_id   bigserial   PRIMARY KEY,
  tenant_id  text        NOT NULL,
  at         timestamptz NOT NULL DEFAULT now(),
  category   text        NOT NULL,
  detail     text        NOT NULL,
  data       jsonb       NOT NULL,
  tx_id      text,
  prev_hash  bytea       NOT NULL,
  entry_hash bytea       NOT NULL
);

CREATE INDEX IF NOT EXISTS idx_audit_tenant ON audit_log (tenant_id, audit_id);

COMMIT;
