// Canonical hashing, token signing, and the durable journal format.
//
// These are the mechanisms the rest of the safety argument rests on: command
// identity (RI-02), token authenticity (RI-22), and all-or-nothing durability
// (RI-23 / RI-24). A bug here would make every higher-level assertion vacuous.
#include <cstdio>
#include <string>

#include "risk/hash.hpp"
#include "risk/journal_store.hpp"
#include "risk/memory_store.hpp"
#include "tiny_test.hpp"
#include "tmpdir.hpp"

using namespace risk;

TEST(unit_hash, sha256_matches_the_published_vectors) {
  CHECK_EQ(digest_hex(sha256("")),
           std::string("e3b0c44298fc1c149afbf4c8996fb924"
                       "27ae41e4649b934ca495991b7852b855"));
  CHECK_EQ(digest_hex(sha256("abc")),
           std::string("ba7816bf8f01cfea414140de5dae2223"
                       "b00361a396177a9cb410ff61f20015ad"));
  // 56 bytes: exercises the two-block padding path.
  CHECK_EQ(digest_hex(sha256("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq")),
           std::string("248d6a61d20638b8e5c026930c3e6039"
                       "a33ce45964ff2167f6ecedd419db06c1"));
  // 64 bytes: exactly one block plus a full padding block.
  CHECK_EQ(digest_hex(sha256(std::string(64, 'a'))),
           std::string("ffe054fe7ae0cb6dc65c3af9b61d5209"
                       "f439851db43d0ba5997337df154668eb"));
}

TEST(unit_hash, hmac_matches_rfc_4231_test_case_2) {
  CHECK_EQ(digest_hex(hmac_sha256("Jefe", "what do ya want for nothing?")),
           std::string("5bdcc146bf60754e6a042426089575c7"
                       "5a003f089d2739839dec58b964ec3843"));
}

TEST(unit_hash, hmac_handles_a_key_longer_than_the_block) {
  // A >64-byte key must be hashed down first; the two spellings below are the
  // same key, so they must sign alike.
  const std::string long_key(100, 'k');
  const Digest a = hmac_sha256(long_key, "msg");
  const Digest b = hmac_sha256(long_key, "msg");
  CHECK_EQ(digest_hex(a), digest_hex(b));
  CHECK_NE(digest_hex(hmac_sha256(std::string(100, 'j'), "msg")), digest_hex(a));
}

TEST(unit_hash, constant_time_equals_is_still_correct) {
  CHECK(constant_time_equals("abc", "abc"));
  CHECK(!constant_time_equals("abc", "abd"));
  CHECK(!constant_time_equals("abc", "ab"));
  CHECK(constant_time_equals("", ""));
}

TEST(unit_hash, field_framing_prevents_boundary_collisions) {
  // Without length prefixes, {"ab","c"} and {"a","bc"} would concatenate to
  // the same bytes and two different commands would share an identity.
  Hasher a;
  a.field("ab").field("c");
  Hasher b;
  b.field("a").field("bc");
  CHECK_NE(a.hex(), b.hex());

  // An empty field is distinguishable from an absent one.
  Hasher c;
  c.field("a").field("");
  Hasher d;
  d.field("a");
  CHECK_NE(c.hex(), d.hex());
}

TEST(unit_hash, typed_fields_are_distinguishable) {
  Hasher as_text;
  as_text.field(std::string("7"));
  Hasher as_int;
  as_int.field(static_cast<std::int64_t>(7));
  Hasher as_uint;
  as_uint.field(static_cast<std::uint64_t>(7));
  CHECK_NE(as_text.hex(), as_int.hex());
  CHECK_NE(as_int.hex(), as_uint.hex());
}

TEST(unit_hash, named_fields_detect_reordering_and_renaming) {
  Hasher a;
  a.named("qty", static_cast<std::int64_t>(100)).named("price", static_cast<std::int64_t>(5));
  Hasher b;
  b.named("price", static_cast<std::int64_t>(5)).named("qty", static_cast<std::int64_t>(100));
  CHECK_NE(a.hex(), b.hex());

  Hasher c;
  c.named("quantity", static_cast<std::int64_t>(100))
      .named("price", static_cast<std::int64_t>(5));
  CHECK_NE(c.hex(), a.hex());
}

// ---------------------------------------------------------------------------
// Journal format
// ---------------------------------------------------------------------------

TEST(unit_journal, crc32_matches_the_standard_check_value) {
  CHECK_EQ(static_cast<std::uint64_t>(crc32_of("123456789")),
           static_cast<std::uint64_t>(0xCBF43926u));
  CHECK_EQ(static_cast<std::uint64_t>(crc32_of("")), static_cast<std::uint64_t>(0u));
}

TEST(unit_journal, transaction_codec_round_trips) {
  TransactionData data;

  CommandRecord command;
  command.tenant_id = "T1";
  command.account_id = "ACC-A";
  command.command_id = "cmd-1";
  command.canonical_hash = "deadbeef";
  command.decision = "ALLOW_COMMITTED";
  command.reason = Reason::Ok;
  command.reservation_id = "RSV-1";
  command.policy_version = 3;
  data.commands.push_back(command);

  Reservation reservation;
  reservation.reservation_id = "RSV-1";
  reservation.tenant_id = "T1";
  reservation.domain_id = "D1";
  reservation.account_id = "ACC-A";
  reservation.order_chain_id = "chain-1";
  reservation.command_id = "cmd-1";
  reservation.symbol = "EURUSD";
  reservation.side = Side::Sell;
  reservation.accepted_total_units = 100000;
  reservation.cum_units = 40000;
  reservation.accepted_limit_price_ticks = 110000;
  reservation.proposed_total_units = 150000;
  reservation.proposed_limit_price_ticks = 112000;
  reservation.pending_modify_command_id = "cmd-2";
  reservation.pending_cancel = true;
  reservation.pending_cancel_command_id = "cmd-3";
  reservation.state = ReservationState::Working;
  reservation.policy_version = 3;
  reservation.policy_hash = "policyhash";
  reservation.accepted_valuation.risk_price_ticks = 111100;
  reservation.accepted_valuation.fx_upper = Rational{67, 10000};
  reservation.accepted_valuation.fx_path_kind = FxPathKind::Pivot;
  reservation.accepted_valuation.fx_path = "JPY->USD";
  Valuation proposed;
  proposed.risk_price_ticks = 113120;
  reservation.proposed_valuation = proposed;
  reservation.terminal_reason = Reason::LimitFirmGross;
  reservation.note = "a note";
  data.reservations.push_back(reservation);

  AppliedEventRecord applied;
  applied.tenant_id = "T1";
  applied.source_namespace = "lp";
  applied.event_id = "exec-1";
  applied.canonical_hash = "cafebabe";
  data.applied_events.push_back(applied);

  AuditEvent audit;
  audit.tenant_id = "T1";
  audit.domain_id = "D1";
  audit.event_type = "NewCommitted";
  audit.command_id = "cmd-1";
  audit.account_id = "ACC-A";
  audit.policy_version = 3;
  audit.payload_hash = "deadbeef";
  audit.reason = Reason::Ok;
  audit.commit_utc = 123;
  audit.commit_mono = 456;
  audit.component_version = "p05-test";
  data.audit.push_back(audit);

  OutboxIntent intent;
  intent.intent_id = "OBX-1";
  intent.tenant_id = "T1";
  intent.account_id = "ACC-A";
  intent.order_chain_id = "chain-1";
  intent.command_id = "cmd-1";
  intent.reservation_id = "RSV-1";
  intent.kind = "NEW";
  intent.symbol = "EURUSD";
  intent.side = Side::Sell;
  intent.total_units = 100000;
  intent.limit_price_ticks = 110000;
  intent.canonical_hash = "deadbeef";
  data.outbox_inserts.push_back(intent);
  data.outbox_updates.emplace_back("OBX-1", std::make_pair(true, false));

  DomainExposure exposure;
  exposure.mutable_account("ACC-A").mutable_symbol("EURUSD").position_units = -40000;
  exposure.mutable_account("ACC-A").mutable_symbol("EURUSD").pending_sell_units = 110000;
  exposure.mutable_account("ACC-A").mutable_symbol("EURUSD").working_orders = 1;
  data.exposure = exposure;

  data.policy = lab_policy();
  data.mode = DomainMode::NoNew;
  data.quarantined = true;
  data.admission_open = false;
  data.chain_index.emplace_back("chainkey", "RSV-1");
  data.account_quarantine.emplace_back("ACC-A", true);

  const std::string payload = encode_transaction(42, 7, data);
  Revision revision = 0;
  FencingToken token = 0;
  Result<TransactionData> decoded = decode_transaction(payload, revision, token);
  CHECK_MSG(decoded.ok(), decoded.detail());
  CHECK_EQ(revision, static_cast<Revision>(42));
  CHECK_EQ(token, static_cast<FencingToken>(7));

  // Re-encoding must be byte-identical: the codec is the on-disk contract.
  CHECK_EQ(encode_transaction(42, 7, decoded.value()), payload);

  const Reservation& r = decoded.value().reservations.at(0);
  CHECK_EQ(r.digest(), reservation.digest());
  CHECK_EQ(decoded.value().policy->hash(), lab_policy().hash());
  CHECK(decoded.value().mode.has_value());
  CHECK(decoded.value().quarantined.has_value() && *decoded.value().quarantined);
  CHECK(decoded.value().admission_open.has_value() &&
        !*decoded.value().admission_open);
  CHECK_EQ(decoded.value().account_quarantine.size(), static_cast<std::size_t>(1));
}

TEST(unit_journal, codec_rejects_truncated_and_trailing_bytes) {
  TransactionData data;
  data.mode = DomainMode::Normal;
  const std::string payload = encode_transaction(1, 1, data);

  Revision revision = 0;
  FencingToken token = 0;
  for (std::size_t cut = 1; cut < payload.size(); cut += 7) {
    Result<TransactionData> decoded =
        decode_transaction(payload.substr(0, cut), revision, token);
    CHECK_MSG(!decoded.ok(), "truncation at " + std::to_string(cut) + " decoded");
  }
  Result<TransactionData> extra = decode_transaction(payload + "junk", revision, token);
  CHECK(!extra.ok());
}

TEST(unit_journal, codec_rejects_an_unknown_format_version) {
  TransactionData data;
  data.mode = DomainMode::Normal;
  std::string payload = encode_transaction(1, 1, data);
  payload[0] = static_cast<char>(99);  // bump the format version
  Revision revision = 0;
  FencingToken token = 0;
  Result<TransactionData> decoded = decode_transaction(payload, revision, token);
  CHECK(!decoded.ok());
  CHECK_EQ(static_cast<int>(decoded.reason()),
           static_cast<int>(Reason::JournalCorrupt));
}

TEST(unit_journal, codec_rejects_a_structurally_invalid_projection) {
  // A negative pending quantity on disk would make the envelope formula
  // meaningless, so replay must refuse it rather than load it.
  TransactionData data;
  DomainExposure exposure;
  exposure.mutable_account("ACC-A").mutable_symbol("EURUSD").pending_buy_units = 5;
  data.exposure = exposure;
  std::string payload = encode_transaction(1, 1, data);

  // Flip the pending_buy_units field to -5 by re-encoding with a bad value.
  DomainExposure bad;
  bad.mutable_account("ACC-A").mutable_symbol("EURUSD").pending_buy_units = -5;
  bad.mutable_account("ACC-A").mutable_symbol("EURUSD").position_units = 1;
  TransactionData bad_data;
  bad_data.exposure = bad;
  payload = encode_transaction(1, 1, bad_data);

  Revision revision = 0;
  FencingToken token = 0;
  Result<TransactionData> decoded = decode_transaction(payload, revision, token);
  CHECK(!decoded.ok());
}

TEST(unit_journal, create_then_reopen_preserves_state) {
  tests::TmpDir dir("journal-reopen");
  const std::string path = dir.file("risk.journal");

  RiskDomainState domain;
  domain.tenant_id = "T1";
  domain.domain_id = "D1";
  domain.fencing_token = 1;

  std::string hash_before;
  {
    Result<std::unique_ptr<JournalStore>> store =
        JournalStore::open(path, domain, lab_policy(), nullptr);
    CHECK_MSG(store.ok(), store.detail());
    CHECK(store.value()->stats().created);

    CommitRequest request;
    request.fencing_token = 1;
    request.expected_revision = 0;
    Reservation r;
    r.reservation_id = "RSV-1";
    r.tenant_id = "T1";
    r.account_id = "ACC-A";
    r.order_chain_id = "chain-1";
    r.symbol = "EURUSD";
    r.accepted_total_units = 1000;
    r.accepted_limit_price_ticks = 110000;
    r.state = ReservationState::Committed;
    r.policy_version = 1;
    request.data.reservations.push_back(r);
    DomainExposure exposure;
    exposure.mutable_account("ACC-A").mutable_symbol("EURUSD").pending_buy_units = 1000;
    exposure.mutable_account("ACC-A").mutable_symbol("EURUSD").working_orders = 1;
    request.data.exposure = exposure;
    AuditEvent audit;
    audit.event_type = "NewCommitted";
    audit.policy_version = 1;
    audit.reservation_id = "RSV-1";
    request.data.audit.push_back(audit);

    CHECK(store.value()->commit(request) == CommitOutcome::Committed);
    CHECK_EQ(store.value()->domain_state().revision, static_cast<Revision>(1));
    hash_before = store.value()->state_hash();
  }

  Result<std::unique_ptr<JournalStore>> reopened =
      JournalStore::open(path, domain, lab_policy(), nullptr);
  CHECK_MSG(reopened.ok(), reopened.detail());
  CHECK(!reopened.value()->stats().created);
  CHECK_EQ(reopened.value()->stats().records_applied, static_cast<std::uint64_t>(2));
  CHECK_EQ(reopened.value()->stats().truncated_tail_bytes,
           static_cast<std::uint64_t>(0));
  CHECK_EQ(reopened.value()->state_hash(), hash_before);
  CHECK_EQ(reopened.value()->domain_state().revision, static_cast<Revision>(1));
  CHECK(reopened.value()->find_reservation("RSV-1").has_value());
}

TEST(unit_journal, a_torn_tail_is_discarded_not_guessed_at) {
  tests::TmpDir dir("journal-torn");
  const std::string path = dir.file("risk.journal");

  RiskDomainState domain;
  domain.tenant_id = "T1";
  domain.domain_id = "D1";
  domain.fencing_token = 1;

  std::string hash_after_one;
  {
    Result<std::unique_ptr<JournalStore>> store =
        JournalStore::open(path, domain, lab_policy(), nullptr);
    CHECK_MSG(store.ok(), store.detail());
    CommitRequest request;
    request.fencing_token = 1;
    request.expected_revision = 0;
    request.data.mode = DomainMode::NoNew;
    AuditEvent audit;
    audit.event_type = "ModeChanged";
    audit.policy_version = 1;
    request.data.audit.push_back(audit);
    CHECK(store.value()->commit(request) == CommitOutcome::Committed);
    hash_after_one = store.value()->state_hash();
  }

  // Append a half-written record, exactly as a crash mid-append would leave it.
  {
    std::FILE* f = std::fopen(path.c_str(), "ab");
    CHECK(f != nullptr);
    const char partial[] = {0x54, 0x03, 0x03, 0x05, 0x40, 0x00, 0x00};
    std::fwrite(partial, 1, sizeof(partial), f);
    std::fclose(f);
  }

  Result<std::unique_ptr<JournalStore>> reopened =
      JournalStore::open(path, domain, lab_policy(), nullptr);
  CHECK_MSG(reopened.ok(), reopened.detail());
  CHECK_EQ(reopened.value()->stats().truncated_tail_bytes,
           static_cast<std::uint64_t>(7));
  // The committed prefix is intact and the torn bytes contributed nothing.
  CHECK_EQ(reopened.value()->state_hash(), hash_after_one);

  // A further commit must overwrite the torn tail rather than append past it.
  CommitRequest request;
  request.fencing_token = 1;
  request.expected_revision = reopened.value()->domain_state().revision;
  request.data.mode = DomainMode::Normal;
  AuditEvent audit;
  audit.event_type = "ModeChanged";
  audit.policy_version = 1;
  request.data.audit.push_back(audit);
  CHECK(reopened.value()->commit(request) == CommitOutcome::Committed);

  Result<std::unique_ptr<JournalStore>> third =
      JournalStore::open(path, domain, lab_policy(), nullptr);
  CHECK_MSG(third.ok(), third.detail());
  CHECK_EQ(third.value()->stats().truncated_tail_bytes,
           static_cast<std::uint64_t>(0));
  CHECK(third.value()->domain_state().mode == DomainMode::Normal);
}

TEST(unit_journal, a_corrupt_crc_stops_replay_at_that_record) {
  tests::TmpDir dir("journal-crc");
  const std::string path = dir.file("risk.journal");

  RiskDomainState domain;
  domain.tenant_id = "T1";
  domain.domain_id = "D1";
  domain.fencing_token = 1;

  long second_record_offset = 0;
  {
    Result<std::unique_ptr<JournalStore>> store =
        JournalStore::open(path, domain, lab_policy(), nullptr);
    CHECK_MSG(store.ok(), store.detail());
    std::FILE* probe = std::fopen(path.c_str(), "rb");
    CHECK(probe != nullptr);
    std::fseek(probe, 0, SEEK_END);
    second_record_offset = std::ftell(probe);
    std::fclose(probe);

    CommitRequest request;
    request.fencing_token = 1;
    request.expected_revision = 0;
    request.data.mode = DomainMode::NoNew;
    AuditEvent audit;
    audit.event_type = "ModeChanged";
    audit.policy_version = 1;
    request.data.audit.push_back(audit);
    CHECK(store.value()->commit(request) == CommitOutcome::Committed);
  }

  // Corrupt a payload byte of the second record, leaving its header intact.
  {
    std::FILE* f = std::fopen(path.c_str(), "r+b");
    CHECK(f != nullptr);
    CHECK_EQ(std::fseek(f, second_record_offset + 12, SEEK_SET), 0);
    int byte = std::fgetc(f);
    CHECK(byte != EOF);
    CHECK_EQ(std::fseek(f, second_record_offset + 12, SEEK_SET), 0);
    std::fputc(byte ^ 0xFF, f);
    std::fclose(f);
  }

  Result<std::unique_ptr<JournalStore>> reopened =
      JournalStore::open(path, domain, lab_policy(), nullptr);
  CHECK_MSG(reopened.ok(), reopened.detail());
  // Only the bootstrap record survived, and the rest is reported as discarded.
  CHECK_EQ(reopened.value()->stats().records_applied, static_cast<std::uint64_t>(1));
  CHECK(reopened.value()->stats().truncated_tail_bytes > 0);
  CHECK(reopened.value()->domain_state().mode == DomainMode::Normal);
}

TEST(unit_journal, reopening_with_a_different_tenant_is_refused) {
  tests::TmpDir dir("journal-tenant");
  const std::string path = dir.file("risk.journal");

  RiskDomainState domain;
  domain.tenant_id = "T1";
  domain.domain_id = "D1";
  domain.fencing_token = 1;
  {
    Result<std::unique_ptr<JournalStore>> store =
        JournalStore::open(path, domain, lab_policy(), nullptr);
    CHECK(store.ok());
  }

  RiskDomainState other = domain;
  other.tenant_id = "T2";
  Result<std::unique_ptr<JournalStore>> wrong =
      JournalStore::open(path, other, lab_policy(), nullptr);
  CHECK(!wrong.ok());
  CHECK_EQ(static_cast<int>(wrong.reason()),
           static_cast<int>(Reason::SnapshotMismatch));
}

TEST(unit_store, revision_cas_rejects_a_stale_expectation) {
  RiskDomainState domain;
  domain.tenant_id = "T1";
  domain.domain_id = "D1";
  domain.fencing_token = 1;
  MemoryStore store(domain, lab_policy(), nullptr);

  CommitRequest first;
  first.fencing_token = 1;
  first.expected_revision = 0;
  first.data.mode = DomainMode::NoNew;
  CHECK(store.commit(first) == CommitOutcome::Committed);

  // RI-15: a decision computed against revision 0 cannot land now.
  CommitRequest stale;
  stale.fencing_token = 1;
  stale.expected_revision = 0;
  stale.data.mode = DomainMode::Normal;
  CHECK(store.commit(stale) == CommitOutcome::RolledBack);
  CHECK(store.domain_state().mode == DomainMode::NoNew);
}

TEST(unit_store, a_stale_fencing_token_is_refused_at_the_boundary) {
  RiskDomainState domain;
  domain.tenant_id = "T1";
  domain.domain_id = "D1";
  domain.fencing_token = 1;
  MemoryStore store(domain, lab_policy(), nullptr);

  store.install_fencing_token(5);

  CommitRequest old_writer;
  old_writer.fencing_token = 1;
  old_writer.expected_revision = store.domain_state().revision;
  old_writer.data.mode = DomainMode::CancelOnly;
  // RI-25: the previous owner is stopped by the store, not by convention.
  CHECK(store.commit(old_writer) == CommitOutcome::Fenced);
  CHECK(store.domain_state().mode == DomainMode::Normal);

  CommitRequest new_writer = old_writer;
  new_writer.fencing_token = 5;
  CHECK(store.commit(new_writer) == CommitOutcome::Committed);
}

TEST(unit_store, the_store_stamps_the_audit_state_hash_chain) {
  RiskDomainState domain;
  domain.tenant_id = "T1";
  domain.domain_id = "D1";
  domain.fencing_token = 1;
  MemoryStore store(domain, lab_policy(), nullptr);

  const std::string before = store.state_hash();

  CommitRequest request;
  request.fencing_token = 1;
  request.expected_revision = 0;
  request.data.mode = DomainMode::NoNew;
  AuditEvent audit;
  audit.event_type = "ModeChanged";
  audit.policy_version = 1;
  // Deliberately left blank by the caller: the store must fill these in, so a
  // caller cannot forge a chain that hides a mutation.
  audit.old_state_hash = "forged";
  audit.new_state_hash = "forged";
  request.data.audit.push_back(audit);
  CHECK(store.commit(request) == CommitOutcome::Committed);

  const std::vector<AuditEvent> events = store.audit();
  CHECK_EQ(events.size(), static_cast<std::size_t>(1));
  CHECK_EQ(events[0].old_state_hash, before);
  CHECK_EQ(events[0].new_state_hash, store.state_hash());
  CHECK_NE(events[0].old_state_hash, events[0].new_state_hash);
  CHECK_EQ(events[0].event_ordinal, 0);
  CHECK_EQ(events[0].commit_revision, static_cast<Revision>(1));
}
