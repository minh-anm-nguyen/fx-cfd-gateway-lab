// G09 / G10 / G11 — fills, cancel, replace, and the races between them.
//
// The numbers in these cases come from §10.2, §14.1 and the table in §23.3.
// Each one pins a specific way the accounting could go wrong: a quantity
// counted twice, a hold released without evidence, or a replace branch trusted
// before the LP accepted it.
#include <memory>

#include "risk/scenario.hpp"
#include "tiny_test.hpp"

using namespace risk;

namespace {

constexpr std::int64_t kPrice = 110000;

std::unique_ptr<LabEnv> make_env() {
  LabOptions options;
  Result<std::unique_ptr<LabEnv>> env = LabEnv::create(options);
  CHECK_MSG(env.ok(), std::string(reason_name(env.reason())) + ": " + env.detail());
  return std::move(env.value());
}

// Places a 100,000-unit BUY, gets it acknowledged, and fills 40,000 of it.
// This is fixture F2, built inline so each test can keep its own ids.
std::unique_ptr<LabEnv> env_with_partial_fill() {
  std::unique_ptr<LabEnv> env = make_env();
  CHECK(env->new_order("ACC-A", "chain-1", "EURUSD", Side::Buy, 100000, kPrice,
                       "new-1")
            .allowed());
  CHECK(env->dispatch("new-1").allowed());
  CHECK(env->lp_accept("ACC-A", "chain-1", "ack-1").allowed());
  CHECK(env->lp_fill("ACC-A", "chain-1", "exec-1", 40000).allowed());
  return env;
}

std::int64_t buy_held(const LabEnv& env) {
  return env.effective_remaining("ACC-A", "EURUSD", Side::Buy);
}

VerifiedOrderEvent event_for(const LabEnv& env, const OrderChainId& chain,
                             OrderEventKind kind, const EventId& id) {
  VerifiedOrderEvent e;
  e.tenant_id = env.options().tenant_id;
  e.account_id = "ACC-A";
  e.order_chain_id = chain;
  e.source_namespace = "lp";
  e.event_id = id;
  e.kind = kind;
  if (std::optional<Reservation> r = env.reservation_for("ACC-A", chain);
      r.has_value()) {
    e.symbol = r->symbol;
    e.side = r->side;
    e.verified_cum_units = r->cum_units;
  }
  return e;
}

void check_invariants(LabEnv& env, const char* where) {
  const Status cross = env.coordinator().cross_check_projection();
  CHECK_MSG(cross.ok(), std::string(where) + ": " + cross.detail());
  const AuditChainReport chain = verify_audit_chain(env.store().audit());
  CHECK_MSG(chain.ok, std::string(where) + ": " + chain.detail);
}

}  // namespace

TEST(scenario_lifecycle, a_partial_fill_transfers_without_changing_the_envelope) {
  // RI-07 and the §10.2 worked example: after the fill, P = 40,000 and
  // B = 60,000, so P + B is unchanged and no budget appeared out of nowhere.
  std::unique_ptr<LabEnv> env = make_env();
  CHECK(env->new_order("ACC-A", "chain-1", "EURUSD", Side::Buy, 100000, kPrice,
                       "new-1")
            .allowed());
  CHECK(env->dispatch("new-1").allowed());
  CHECK(env->lp_accept("ACC-A", "chain-1", "ack-1").allowed());

  Result<DomainMetrics> before = env->coordinator().current_metrics();
  CHECK_MSG(before.ok(), before.detail());
  const SymbolMetrics before_metrics =
      before.value().find("ACC-A")->symbols.at("EURUSD");

  CHECK(env->lp_fill("ACC-A", "chain-1", "exec-1", 40000).allowed());

  CHECK_EQ(env->position("ACC-A", "EURUSD"), 40000);
  CHECK_EQ(buy_held(*env), 60000);

  Result<DomainMetrics> after = env->coordinator().current_metrics();
  CHECK_MSG(after.ok(), after.detail());
  const SymbolMetrics after_metrics =
      after.value().find("ACC-A")->symbols.at("EURUSD");

  CHECK_EQ(after_metrics.net_units, before_metrics.net_units);
  CHECK_EQ(after_metrics.gross_units, before_metrics.gross_units);
  CHECK_EQ(after_metrics.net_notional_minor, before_metrics.net_notional_minor);
  check_invariants(*env, "partial fill");
}

TEST(scenario_lifecycle, repeated_fills_never_overcount) {
  std::unique_ptr<LabEnv> env = make_env();
  CHECK(env->new_order("ACC-A", "chain-1", "EURUSD", Side::Buy, 100000, kPrice,
                       "new-1")
            .allowed());
  CHECK(env->dispatch("new-1").allowed());
  CHECK(env->lp_accept("ACC-A", "chain-1", "ack-1").allowed());

  std::int64_t expected_position = 0;
  for (int i = 0; i < 10; ++i) {
    CHECK(env->lp_fill("ACC-A", "chain-1", "exec-" + std::to_string(i), 10000)
              .allowed());
    expected_position += 10000;
    CHECK_EQ(env->position("ACC-A", "EURUSD"), expected_position);
    CHECK_EQ(buy_held(*env), 100000 - expected_position);
    // P + B is invariant across the whole sequence.
    CHECK_EQ(env->position("ACC-A", "EURUSD") + buy_held(*env), 100000);
  }
  check_invariants(*env, "repeated fills");
}

TEST(scenario_lifecycle, a_sell_fill_moves_the_position_negative) {
  std::unique_ptr<LabEnv> env = make_env();
  CHECK(env->new_order("ACC-A", "chain-1", "EURUSD", Side::Sell, 50000, kPrice,
                       "new-1")
            .allowed());
  CHECK(env->dispatch("new-1").allowed());
  CHECK(env->lp_accept("ACC-A", "chain-1", "ack-1").allowed());
  CHECK_EQ(env->effective_remaining("ACC-A", "EURUSD", Side::Sell), 50000);

  CHECK(env->lp_fill("ACC-A", "chain-1", "exec-1", 20000).allowed());
  CHECK_EQ(env->position("ACC-A", "EURUSD"), -20000);
  CHECK_EQ(env->effective_remaining("ACC-A", "EURUSD", Side::Sell), 30000);
  // P - S is the invariant on this side.
  CHECK_EQ(env->position("ACC-A", "EURUSD") -
               env->effective_remaining("ACC-A", "EURUSD", Side::Sell),
           -50000);
  check_invariants(*env, "sell fill");
}

TEST(scenario_lifecycle, a_duplicate_exec_id_is_a_no_op) {
  // RI-03 / PB-13: the same execution replayed after a session resend must not
  // move the ledger a second time.
  std::unique_ptr<LabEnv> env = env_with_partial_fill();
  const std::string hash_before = env->store().state_hash();
  const Revision revision_before = env->store().domain_state().revision;

  // Rebuild the exact bytes of the original event: a redelivery carries the
  // cumulative it reported the first time, not a recomputed one.
  VerifiedOrderEvent replay =
      event_for(*env, "chain-1", OrderEventKind::AppliedFill, "exec-1");
  replay.fill_units = 40000;
  replay.verified_cum_units = 40000;

  for (int i = 0; i < 5; ++i) {
    const RiskResult again = env->lp_event(replay);
    CHECK_MSG(again.allowed(), std::string(reason_name(again.reason)));
    CHECK_EQ(static_cast<int>(again.reason), static_cast<int>(Reason::DuplicateEvent));
  }

  CHECK_EQ(env->position("ACC-A", "EURUSD"), 40000);
  CHECK_EQ(buy_held(*env), 60000);
  CHECK_EQ(env->store().state_hash(), hash_before);
  CHECK_EQ(env->store().domain_state().revision, revision_before);
  CHECK(env->coordinator().metrics().labeled("risk_event_dedup_total",
                                             "result=duplicate_noop") >= 5);
  check_invariants(*env, "duplicate exec id");
}

TEST(scenario_lifecycle, the_same_exec_id_with_different_content_quarantines) {
  // RI-19 / PB-14: contradictory evidence under one key is not a fill, it is a
  // quarantine. The exposure is retained, not reduced.
  std::unique_ptr<LabEnv> env = env_with_partial_fill();
  const std::int64_t held_before = buy_held(*env);

  const RiskResult conflicting = env->lp_fill("ACC-A", "chain-1", "exec-1", 10000);
  CHECK(conflicting.decision == RiskDecision::Quarantined);
  CHECK_EQ(static_cast<int>(conflicting.reason),
           static_cast<int>(Reason::EventHashMismatch));

  CHECK(env->store().account_quarantined("ACC-A"));
  std::optional<Reservation> r = env->reservation_for("ACC-A", "chain-1");
  CHECK(r.has_value());
  CHECK(r->state == ReservationState::Quarantine);
  // Quarantine keeps the budget: we stopped trusting the evidence, not the
  // exposure.
  CHECK(r->holds_budget());
  CHECK_EQ(buy_held(*env), held_before);
  CHECK_EQ(env->position("ACC-A", "EURUSD"), 40000);

  // A quarantined account admits nothing new.
  const RiskResult blocked =
      env->new_order("ACC-A", "chain-2", "EURUSD", Side::Buy, 1000, kPrice, "new-2");
  CHECK_EQ(static_cast<int>(blocked.reason), static_cast<int>(Reason::Quarantined));
  // Another account is unaffected: quarantine is per account.
  CHECK(env->new_order("ACC-B", "chain-b", "EURUSD", Side::Buy, 1000, kPrice, "new-b")
            .allowed());
  check_invariants(*env, "exec id conflict");
}

TEST(scenario_lifecycle, a_fill_beyond_the_reserved_ceiling_quarantines) {
  // §13: no silent clamping. 100,000 accepted with 40,000 filled leaves
  // 60,000; a 70,000 fill is evidence that something is wrong.
  std::unique_ptr<LabEnv> env = env_with_partial_fill();

  const RiskResult over = env->lp_fill("ACC-A", "chain-1", "exec-2", 70000);
  CHECK(over.decision == RiskDecision::Quarantined);
  CHECK_EQ(static_cast<int>(over.reason),
           static_cast<int>(Reason::FillExceedsAccepted));
  CHECK_EQ(env->position("ACC-A", "EURUSD"), 40000);  // unchanged
  CHECK(env->store().account_quarantined("ACC-A"));
  check_invariants(*env, "fill over ceiling");
}

TEST(scenario_lifecycle, a_fill_with_the_wrong_side_or_symbol_quarantines) {
  {
    std::unique_ptr<LabEnv> env = env_with_partial_fill();
    VerifiedOrderEvent e =
        event_for(*env, "chain-1", OrderEventKind::AppliedFill, "exec-wrong-side");
    e.side = Side::Sell;
    e.fill_units = 1000;
    e.verified_cum_units = 0;
    const RiskResult r = env->lp_event(e);
    CHECK(r.decision == RiskDecision::Quarantined);
    CHECK_EQ(static_cast<int>(r.reason), static_cast<int>(Reason::FillSideMismatch));
  }
  {
    std::unique_ptr<LabEnv> env = env_with_partial_fill();
    VerifiedOrderEvent e =
        event_for(*env, "chain-1", OrderEventKind::AppliedFill, "exec-wrong-symbol");
    e.symbol = "GBPUSD";
    e.fill_units = 1000;
    e.verified_cum_units = 0;
    const RiskResult r = env->lp_event(e);
    CHECK(r.decision == RiskDecision::Quarantined);
    CHECK_EQ(static_cast<int>(r.reason), static_cast<int>(Reason::FillSymbolMismatch));
  }
}

TEST(scenario_lifecycle, a_cumulative_disagreement_on_a_fill_quarantines) {
  std::unique_ptr<LabEnv> env = env_with_partial_fill();
  VerifiedOrderEvent e =
      event_for(*env, "chain-1", OrderEventKind::AppliedFill, "exec-2");
  e.fill_units = 10000;
  e.verified_cum_units = 99999;  // should be 50,000
  const RiskResult r = env->lp_event(e);
  CHECK(r.decision == RiskDecision::Quarantined);
  CHECK_EQ(static_cast<int>(r.reason), static_cast<int>(Reason::CumulativeMismatch));
  CHECK_EQ(env->position("ACC-A", "EURUSD"), 40000);
}

TEST(scenario_lifecycle, an_event_for_an_unknown_chain_quarantines) {
  // PB-20: an orphan execution is not discarded and not clamped; it stops the
  // account until someone reconciles it.
  std::unique_ptr<LabEnv> env = make_env();
  VerifiedOrderEvent e;
  e.tenant_id = "T1";
  e.account_id = "ACC-A";
  e.order_chain_id = "never-reserved";
  e.source_namespace = "lp";
  e.event_id = "orphan-1";
  e.kind = OrderEventKind::AppliedFill;
  e.symbol = "EURUSD";
  e.side = Side::Buy;
  e.fill_units = 10000;

  const RiskResult r = env->lp_event(e);
  CHECK(r.decision == RiskDecision::Quarantined);
  CHECK_EQ(static_cast<int>(r.reason), static_cast<int>(Reason::UnknownChain));
  CHECK(env->store().account_quarantined("ACC-A"));
  CHECK_EQ(env->position("ACC-A", "EURUSD"), 0);

  // Redelivering the identical orphan is a dedup no-op rather than a second
  // quarantine event.
  const Revision revision = env->store().domain_state().revision;
  const RiskResult again = env->lp_event(e);
  CHECK_EQ(static_cast<int>(again.reason), static_cast<int>(Reason::DuplicateEvent));
  CHECK_EQ(env->store().domain_state().revision, revision);
}

TEST(scenario_lifecycle, a_pending_cancel_does_not_release_anything) {
  // RI-04 and the first row of §23.3: 100k accepted, 40k filled, cancel sent
  // leaves 60k held.
  std::unique_ptr<LabEnv> env = env_with_partial_fill();
  CHECK_EQ(buy_held(*env), 60000);

  CHECK(env->cancel_order("ACC-A", "chain-1", "cancel-1").allowed());
  CHECK_EQ(buy_held(*env), 60000);
  std::optional<Reservation> r = env->reservation_for("ACC-A", "chain-1");
  CHECK(r.has_value());
  CHECK(r->pending_cancel);
  CHECK(r->state == ReservationState::Working);
  CHECK_EQ(r->risk_effective_remaining(), 60000);

  // A fill can still arrive before the cancel is confirmed.
  CHECK(env->lp_fill("ACC-A", "chain-1", "exec-2", 10000).allowed());
  CHECK_EQ(env->position("ACC-A", "EURUSD"), 50000);
  CHECK_EQ(buy_held(*env), 50000);

  // Time passing changes nothing: a timeout is not evidence (RI-26).
  env->clock().advance_millis(60'000);
  CHECK(env->publish_default_market().ok());
  CHECK_EQ(buy_held(*env), 50000);
  check_invariants(*env, "pending cancel");
}

TEST(scenario_lifecycle, a_cancel_reject_leaves_the_order_working) {
  std::unique_ptr<LabEnv> env = env_with_partial_fill();
  CHECK(env->cancel_order("ACC-A", "chain-1", "cancel-1").allowed());

  VerifiedOrderEvent reject =
      event_for(*env, "chain-1", OrderEventKind::CancelRejected, "cxlrej-1");
  CHECK(env->lp_event(reject).allowed());

  std::optional<Reservation> r = env->reservation_for("ACC-A", "chain-1");
  CHECK(r.has_value());
  CHECK(!r->pending_cancel);
  CHECK(r->state == ReservationState::Working);
  CHECK_EQ(buy_held(*env), 60000);
  check_invariants(*env, "cancel reject");
}

TEST(scenario_lifecycle, a_confirmed_cancel_releases_only_the_unfilled_part) {
  // §23.3 row 1, right-hand column: cancel accepted takes the remainder to
  // zero while the filled position stays.
  std::unique_ptr<LabEnv> env = env_with_partial_fill();
  CHECK(env->cancel_order("ACC-A", "chain-1", "cancel-1").allowed());

  VerifiedOrderEvent accepted =
      event_for(*env, "chain-1", OrderEventKind::CancelAccepted, "cxl-1");
  accepted.verified_cum_units = 40000;
  CHECK(env->lp_event(accepted).allowed());

  std::optional<Reservation> r = env->reservation_for("ACC-A", "chain-1");
  CHECK(r.has_value());
  CHECK(r->state == ReservationState::Final);
  CHECK(!r->holds_budget());
  CHECK_EQ(buy_held(*env), 0);
  CHECK_EQ(env->position("ACC-A", "EURUSD"), 40000);
  check_invariants(*env, "cancel accepted");
}

TEST(scenario_lifecycle, a_final_cancel_with_the_wrong_cumulative_quarantines) {
  // PB-19: if the terminal report disagrees about how much executed, we do not
  // know the true position, so nothing is released.
  std::unique_ptr<LabEnv> env = env_with_partial_fill();
  VerifiedOrderEvent accepted =
      event_for(*env, "chain-1", OrderEventKind::CancelAccepted, "cxl-1");
  accepted.verified_cum_units = 45000;  // ledger says 40,000

  const RiskResult r = env->lp_event(accepted);
  CHECK(r.decision == RiskDecision::Quarantined);
  CHECK_EQ(static_cast<int>(r.reason), static_cast<int>(Reason::CumulativeMismatch));
  CHECK_EQ(buy_held(*env), 60000);
  CHECK_EQ(env->position("ACC-A", "EURUSD"), 40000);
  check_invariants(*env, "cancel cumulative mismatch");
}

TEST(scenario_lifecycle, a_new_reject_after_fills_quarantines) {
  // RI-17: a reject that arrives after executions contradicts the ledger, so
  // the confirmed position is not wiped.
  std::unique_ptr<LabEnv> env = env_with_partial_fill();
  VerifiedOrderEvent reject =
      event_for(*env, "chain-1", OrderEventKind::NewRejected, "rej-1");

  const RiskResult r = env->lp_event(reject);
  CHECK(r.decision == RiskDecision::Quarantined);
  CHECK_EQ(static_cast<int>(r.reason), static_cast<int>(Reason::CumulativeMismatch));
  CHECK_EQ(env->position("ACC-A", "EURUSD"), 40000);
  CHECK_EQ(buy_held(*env), 60000);
}

TEST(scenario_lifecycle, a_clean_new_reject_releases_the_hold) {
  std::unique_ptr<LabEnv> env = make_env();
  CHECK(env->new_order("ACC-A", "chain-1", "EURUSD", Side::Buy, 50000, kPrice,
                       "new-1")
            .allowed());
  CHECK(env->dispatch("new-1").allowed());
  CHECK_EQ(buy_held(*env), 50000);

  VerifiedOrderEvent reject =
      event_for(*env, "chain-1", OrderEventKind::NewRejected, "rej-1");
  CHECK(env->lp_event(reject).allowed());

  std::optional<Reservation> r = env->reservation_for("ACC-A", "chain-1");
  CHECK(r.has_value());
  CHECK(r->state == ReservationState::Final);
  CHECK_EQ(buy_held(*env), 0);
  CHECK_EQ(env->position("ACC-A", "EURUSD"), 0);
  check_invariants(*env, "clean new reject");
}

TEST(scenario_lifecycle, a_status_snapshot_never_creates_a_fill) {
  // RI-20 / P-12: a status report that claims more executed quantity than the
  // ledger knows about moves the chain to UNKNOWN for reconciliation.
  std::unique_ptr<LabEnv> env = env_with_partial_fill();

  VerifiedOrderEvent agreeing =
      event_for(*env, "chain-1", OrderEventKind::StatusSnapshot, "status-1");
  agreeing.verified_cum_units = 40000;
  const RiskResult ok = env->lp_event(agreeing);
  CHECK(ok.allowed());
  CHECK_EQ(env->position("ACC-A", "EURUSD"), 40000);

  VerifiedOrderEvent disagreeing =
      event_for(*env, "chain-1", OrderEventKind::StatusSnapshot, "status-2");
  disagreeing.verified_cum_units = 90000;
  const RiskResult mismatch = env->lp_event(disagreeing);
  CHECK(mismatch.decision == RiskDecision::HoldRetryable);
  CHECK_EQ(static_cast<int>(mismatch.reason),
           static_cast<int>(Reason::ReconciliationRequired));

  // No synthetic 50,000 fill appeared.
  CHECK_EQ(env->position("ACC-A", "EURUSD"), 40000);
  std::optional<Reservation> r = env->reservation_for("ACC-A", "chain-1");
  CHECK(r.has_value());
  CHECK_EQ(r->cum_units, 40000);
  CHECK(r->state == ReservationState::Unknown);
  CHECK_EQ(buy_held(*env), 60000);
  check_invariants(*env, "status snapshot");
}

TEST(scenario_lifecycle, trade_correct_and_bust_are_refused) {
  // RI-21: there is no automatic path that rewrites the fill ledger.
  for (const OrderEventKind kind :
       {OrderEventKind::TradeCorrect, OrderEventKind::TradeBust}) {
    std::unique_ptr<LabEnv> env = env_with_partial_fill();
    VerifiedOrderEvent e = event_for(*env, "chain-1", kind, "correct-1");
    const RiskResult r = env->lp_event(e);
    CHECK(r.decision == RiskDecision::Quarantined);
    CHECK_EQ(static_cast<int>(r.reason),
             static_cast<int>(Reason::TradeCorrectUnsupported));
    CHECK_EQ(env->position("ACC-A", "EURUSD"), 40000);
    CHECK_EQ(buy_held(*env), 60000);
  }
}

TEST(scenario_lifecycle, an_unknown_outcome_keeps_the_worst_case_hold) {
  // RI-26 / F6: nothing about an unresolved outcome shrinks over time.
  std::unique_ptr<LabEnv> env = env_with_partial_fill();
  VerifiedOrderEvent unknown =
      event_for(*env, "chain-1", OrderEventKind::UnknownOutcome, "unk-1");
  const RiskResult r = env->lp_event(unknown);
  CHECK(r.decision == RiskDecision::HoldRetryable);

  std::optional<Reservation> reservation = env->reservation_for("ACC-A", "chain-1");
  CHECK(reservation.has_value());
  CHECK(reservation->state == ReservationState::Unknown);
  CHECK_EQ(buy_held(*env), 60000);

  env->clock().advance_millis(3600 * 1000);  // an hour, and a day rollover
  CHECK(env->publish_default_market().ok());
  CHECK_EQ(buy_held(*env), 60000);
  CHECK_EQ(env->position("ACC-A", "EURUSD"), 40000);
  check_invariants(*env, "unknown outcome");
}

// ---------------------------------------------------------------------------
// Replace
// ---------------------------------------------------------------------------

TEST(scenario_lifecycle, replace_up_reserves_the_maximum_branch) {
  // §23.3 row 2: accepted 100k, cum 40k, proposed 150k -> max(60k, 110k).
  std::unique_ptr<LabEnv> env = env_with_partial_fill();
  CHECK(env->replace_order("ACC-A", "chain-1", 150000, kPrice, "repl-1").allowed());

  std::optional<Reservation> r = env->reservation_for("ACC-A", "chain-1");
  CHECK(r.has_value());
  CHECK_EQ(r->accepted_total_units, 100000);
  CHECK(r->proposed_total_units.has_value());
  CHECK_EQ(*r->proposed_total_units, 150000);
  CHECK_EQ(r->accepted_remaining(), 60000);
  CHECK_EQ(r->proposed_remaining(), 110000);
  CHECK_EQ(r->risk_effective_remaining(), 110000);
  CHECK_EQ(buy_held(*env), 110000);

  // Two intents now exist, and both trace back to the same reservation.
  CHECK_EQ(env->store().outbox().size(), static_cast<std::size_t>(2));
  CHECK_EQ(env->store().outbox()[1].kind, std::string("REPLACE"));
  check_invariants(*env, "replace up");
}

TEST(scenario_lifecycle, replace_down_keeps_the_old_branch) {
  // §23.3 row 3: proposed 50k against accepted 100k with 40k filled is
  // max(60k, 10k) = 60k. The smaller proposal releases nothing (RI-05).
  std::unique_ptr<LabEnv> env = env_with_partial_fill();
  CHECK(env->replace_order("ACC-A", "chain-1", 50000, kPrice, "repl-1").allowed());

  std::optional<Reservation> r = env->reservation_for("ACC-A", "chain-1");
  CHECK(r.has_value());
  CHECK_EQ(r->proposed_remaining(), 10000);
  CHECK_EQ(r->risk_effective_remaining(), 60000);
  CHECK_EQ(buy_held(*env), 60000);
  check_invariants(*env, "replace down");
}

TEST(scenario_lifecycle, a_fill_during_a_pending_replace_uses_the_latest_cumulative) {
  // The §24.5 scenario and the note under §23.3: accepted 100k, cum 40k,
  // proposed 150k, then a 20k fill. cum becomes 60k, r_old = 40k,
  // r_new = 90k, so the effective hold is 90k -- not a second reservation.
  std::unique_ptr<LabEnv> env = env_with_partial_fill();
  CHECK(env->replace_order("ACC-A", "chain-1", 150000, kPrice, "repl-1").allowed());
  CHECK_EQ(buy_held(*env), 110000);

  CHECK(env->lp_fill("ACC-A", "chain-1", "exec-2", 20000).allowed());

  std::optional<Reservation> r = env->reservation_for("ACC-A", "chain-1");
  CHECK(r.has_value());
  CHECK_EQ(r->cum_units, 60000);
  CHECK_EQ(r->accepted_remaining(), 40000);
  CHECK_EQ(r->proposed_remaining(), 90000);
  CHECK_EQ(r->risk_effective_remaining(), 90000);
  CHECK_EQ(buy_held(*env), 90000);
  CHECK_EQ(env->position("ACC-A", "EURUSD"), 60000);
  check_invariants(*env, "fill during pending replace");
}

TEST(scenario_lifecycle, a_fill_past_the_accepted_total_is_allowed_during_replace_up) {
  // §14.1: while a replace-up is in flight the LP may legitimately fill beyond
  // the old accepted total, because the larger branch is already reserved.
  std::unique_ptr<LabEnv> env = env_with_partial_fill();
  CHECK(env->replace_order("ACC-A", "chain-1", 150000, kPrice, "repl-1").allowed());

  // 40,000 already filled; 70,000 more takes the cumulative to 110,000, past
  // the accepted 100,000 but within the proposed 150,000.
  CHECK(env->lp_fill("ACC-A", "chain-1", "exec-2", 70000).allowed());
  std::optional<Reservation> r = env->reservation_for("ACC-A", "chain-1");
  CHECK(r.has_value());
  CHECK_EQ(r->cum_units, 110000);
  CHECK_EQ(r->accepted_remaining(), 0);
  CHECK_EQ(r->proposed_remaining(), 40000);
  CHECK_EQ(buy_held(*env), 40000);
  CHECK_EQ(env->position("ACC-A", "EURUSD"), 110000);

  // But not past the larger branch.
  const RiskResult over = env->lp_fill("ACC-A", "chain-1", "exec-3", 50000);
  CHECK(over.decision == RiskDecision::Quarantined);
  CHECK_EQ(static_cast<int>(over.reason),
           static_cast<int>(Reason::FillExceedsAccepted));
}

TEST(scenario_lifecycle, replace_accepted_promotes_and_releases_the_difference) {
  std::unique_ptr<LabEnv> env = env_with_partial_fill();
  CHECK(env->replace_order("ACC-A", "chain-1", 50000, kPrice, "repl-1").allowed());
  CHECK_EQ(buy_held(*env), 60000);

  VerifiedOrderEvent accepted =
      event_for(*env, "chain-1", OrderEventKind::ReplaceAccepted, "repl-ack-1");
  accepted.accepted_total_units = 50000;
  CHECK(env->lp_event(accepted).allowed());

  std::optional<Reservation> r = env->reservation_for("ACC-A", "chain-1");
  CHECK(r.has_value());
  CHECK_EQ(r->accepted_total_units, 50000);
  CHECK(!r->proposed_total_units.has_value());
  CHECK_EQ(r->cum_units, 40000);  // fills survive the promotion
  CHECK_EQ(r->risk_effective_remaining(), 10000);
  CHECK_EQ(buy_held(*env), 10000);
  CHECK_EQ(env->position("ACC-A", "EURUSD"), 40000);
  check_invariants(*env, "replace accepted");
}

TEST(scenario_lifecycle, replace_rejected_drops_the_proposal_and_keeps_fills) {
  std::unique_ptr<LabEnv> env = env_with_partial_fill();
  CHECK(env->replace_order("ACC-A", "chain-1", 150000, kPrice, "repl-1").allowed());
  CHECK_EQ(buy_held(*env), 110000);

  VerifiedOrderEvent rejected =
      event_for(*env, "chain-1", OrderEventKind::ReplaceRejected, "repl-rej-1");
  CHECK(env->lp_event(rejected).allowed());

  std::optional<Reservation> r = env->reservation_for("ACC-A", "chain-1");
  CHECK(r.has_value());
  CHECK_EQ(r->accepted_total_units, 100000);
  CHECK(!r->proposed_total_units.has_value());
  CHECK_EQ(r->cum_units, 40000);
  CHECK_EQ(buy_held(*env), 60000);
  check_invariants(*env, "replace rejected");
}

TEST(scenario_lifecycle, a_replace_granted_beyond_the_reservation_quarantines) {
  // §14.2: the LP accepting a *larger* total than we proposed means the
  // reservation and the LP disagree. That is a quarantine, not extra credit.
  std::unique_ptr<LabEnv> env = env_with_partial_fill();
  CHECK(env->replace_order("ACC-A", "chain-1", 50000, kPrice, "repl-1").allowed());

  VerifiedOrderEvent accepted =
      event_for(*env, "chain-1", OrderEventKind::ReplaceAccepted, "repl-ack-1");
  accepted.accepted_total_units = 190000;  // not what we proposed
  const RiskResult r = env->lp_event(accepted);
  CHECK(r.decision == RiskDecision::Quarantined);
  CHECK_EQ(static_cast<int>(r.reason), static_cast<int>(Reason::CumulativeMismatch));
  CHECK_EQ(buy_held(*env), 60000);
}

TEST(scenario_lifecycle, only_one_modification_may_be_in_flight_per_chain) {
  // P-11 / §14.2.
  std::unique_ptr<LabEnv> env = env_with_partial_fill();
  CHECK(env->replace_order("ACC-A", "chain-1", 150000, kPrice, "repl-1").allowed());

  const RiskResult second =
      env->replace_order("ACC-A", "chain-1", 120000, kPrice, "repl-2");
  CHECK(second.decision == RiskDecision::DenyDefinitive);
  CHECK_EQ(static_cast<int>(second.reason),
           static_cast<int>(Reason::PendingModifyBusy));
  CHECK_EQ(buy_held(*env), 110000);

  // And a cancel plus a replace must not coexist either.
  std::unique_ptr<LabEnv> other = env_with_partial_fill();
  CHECK(other->cancel_order("ACC-A", "chain-1", "cancel-1").allowed());
  const RiskResult blocked =
      other->replace_order("ACC-A", "chain-1", 150000, kPrice, "repl-1");
  CHECK_EQ(static_cast<int>(blocked.reason),
           static_cast<int>(Reason::PendingModifyBusy));
}

TEST(scenario_lifecycle, a_replace_below_the_cumulative_is_refused) {
  // §23.3 row 4 / PB-17: 100k accepted with 80k filled, proposed 50k. The
  // engine must not compute 50k - 80k and read the negative as a release.
  std::unique_ptr<LabEnv> env = make_env();
  CHECK(env->new_order("ACC-A", "chain-1", "EURUSD", Side::Buy, 100000, kPrice,
                       "new-1")
            .allowed());
  CHECK(env->dispatch("new-1").allowed());
  CHECK(env->lp_accept("ACC-A", "chain-1", "ack-1").allowed());
  CHECK(env->lp_fill("ACC-A", "chain-1", "exec-1", 80000).allowed());
  CHECK_EQ(buy_held(*env), 20000);

  const RiskResult r = env->replace_order("ACC-A", "chain-1", 50000, kPrice, "repl-1");
  CHECK(r.decision == RiskDecision::DenyDefinitive);
  CHECK_EQ(static_cast<int>(r.reason),
           static_cast<int>(Reason::ReplaceBelowCumulative));
  CHECK_EQ(buy_held(*env), 20000);
  CHECK_EQ(env->position("ACC-A", "EURUSD"), 80000);
  check_invariants(*env, "replace below cumulative");
}

TEST(scenario_lifecycle, a_price_only_replace_upward_reserves_the_worse_price) {
  // §9.2: the pending branch is valued at the max of the two branches, so a
  // replace to a worse price costs budget before it is accepted.
  std::unique_ptr<LabEnv> env = make_env();
  CHECK(env->new_order("ACC-A", "chain-1", "EURUSD", Side::Buy, 100000, 109000,
                       "new-1")
            .allowed());

  Result<DomainMetrics> before = env->coordinator().current_metrics();
  CHECK_MSG(before.ok(), before.detail());
  const std::int64_t notional_before =
      before.value().find("ACC-A")->symbols.at("EURUSD").gross_notional_minor;

  // 1.14000 is within the 5% collar of a 1.09995 midpoint and well above the
  // original limit.
  CHECK(env->replace_order("ACC-A", "chain-1", 100000, 114000, "repl-1").allowed());

  Result<DomainMetrics> after = env->coordinator().current_metrics();
  CHECK_MSG(after.ok(), after.detail());
  const std::int64_t notional_after =
      after.value().find("ACC-A")->symbols.at("EURUSD").gross_notional_minor;

  // The quantity is unchanged, so only the worse price can have moved this.
  CHECK_EQ(buy_held(*env), 100000);
  CHECK_MSG(notional_after > notional_before,
            std::to_string(notional_after) + " <= " + std::to_string(notional_before));
  check_invariants(*env, "price-only replace up");
}

TEST(scenario_lifecycle, a_price_only_replace_downward_releases_nothing_yet) {
  // §14.2: a lower price may reduce the notional, but not before the LP
  // accepts it.
  std::unique_ptr<LabEnv> env = make_env();
  CHECK(env->new_order("ACC-A", "chain-1", "EURUSD", Side::Buy, 100000, 114000,
                       "new-1")
            .allowed());
  Result<DomainMetrics> before = env->coordinator().current_metrics();
  CHECK_MSG(before.ok(), before.detail());
  const std::int64_t notional_before =
      before.value().find("ACC-A")->symbols.at("EURUSD").gross_notional_minor;

  CHECK(env->replace_order("ACC-A", "chain-1", 100000, 106000, "repl-1").allowed());
  Result<DomainMetrics> after = env->coordinator().current_metrics();
  CHECK_MSG(after.ok(), after.detail());
  CHECK_EQ(after.value().find("ACC-A")->symbols.at("EURUSD").gross_notional_minor,
           notional_before);

  // Once accepted, the lower price does take effect.
  VerifiedOrderEvent accepted =
      event_for(*env, "chain-1", OrderEventKind::ReplaceAccepted, "repl-ack-1");
  accepted.accepted_total_units = 100000;
  CHECK(env->lp_event(accepted).allowed());
  Result<DomainMetrics> promoted = env->coordinator().current_metrics();
  CHECK_MSG(promoted.ok(), promoted.detail());
  CHECK(promoted.value().find("ACC-A")->symbols.at("EURUSD").gross_notional_minor <
        notional_before);
  check_invariants(*env, "price-only replace down");
}

TEST(scenario_lifecycle, a_replace_on_an_unknown_chain_is_held_not_decided) {
  // PB-18 / RI-26: an unresolved outcome is not a base to modify from.
  std::unique_ptr<LabEnv> env = env_with_partial_fill();
  VerifiedOrderEvent unknown =
      event_for(*env, "chain-1", OrderEventKind::UnknownOutcome, "unk-1");
  CHECK(env->lp_event(unknown).decision == RiskDecision::HoldRetryable);

  const RiskResult r = env->replace_order("ACC-A", "chain-1", 50000, kPrice, "repl-1");
  CHECK(r.decision == RiskDecision::HoldRetryable);
  CHECK_EQ(static_cast<int>(r.reason),
           static_cast<int>(Reason::ReconciliationRequired));
  CHECK_EQ(buy_held(*env), 60000);
  // No durable decision was recorded for a hold.
  CHECK(!env->store().find_command("T1", "ACC-A", "repl-1").has_value());
}

TEST(scenario_lifecycle, a_pending_replace_report_without_a_reservation_quarantines) {
  // RI-01 detector: a G in flight for a branch we never reserved means
  // something sent a message without a hold.
  std::unique_ptr<LabEnv> env = env_with_partial_fill();
  VerifiedOrderEvent pending =
      event_for(*env, "chain-1", OrderEventKind::PendingReplace, "pend-1");
  const RiskResult r = env->lp_event(pending);
  CHECK(r.decision == RiskDecision::Quarantined);
  CHECK_EQ(static_cast<int>(r.reason),
           static_cast<int>(Reason::ReplaceNoAcceptedVersion));
}

TEST(scenario_lifecycle, a_fully_filled_order_goes_terminal_only_with_a_filled_report) {
  std::unique_ptr<LabEnv> env = make_env();
  CHECK(env->new_order("ACC-A", "chain-1", "EURUSD", Side::Buy, 50000, kPrice,
                       "new-1")
            .allowed());
  CHECK(env->dispatch("new-1").allowed());
  CHECK(env->lp_accept("ACC-A", "chain-1", "ack-1").allowed());
  CHECK(env->lp_fill("ACC-A", "chain-1", "exec-1", 50000).allowed());

  // Fully executed, but no terminal report yet: still WORKING, and the hold is
  // zero only because the remainder is zero.
  std::optional<Reservation> r = env->reservation_for("ACC-A", "chain-1");
  CHECK(r.has_value());
  CHECK(r->state == ReservationState::Working);
  CHECK_EQ(r->risk_effective_remaining(), 0);
  CHECK_EQ(env->position("ACC-A", "EURUSD"), 50000);

  VerifiedOrderEvent filled =
      event_for(*env, "chain-1", OrderEventKind::OrderFilled, "fill-final");
  filled.verified_cum_units = 50000;
  CHECK(env->lp_event(filled).allowed());

  r = env->reservation_for("ACC-A", "chain-1");
  CHECK(r.has_value());
  CHECK(r->state == ReservationState::Final);
  CHECK_EQ(env->position("ACC-A", "EURUSD"), 50000);
  check_invariants(*env, "filled");
}

TEST(scenario_lifecycle, a_filled_report_that_does_not_match_quarantines) {
  std::unique_ptr<LabEnv> env = env_with_partial_fill();
  VerifiedOrderEvent filled =
      event_for(*env, "chain-1", OrderEventKind::OrderFilled, "fill-final");
  filled.verified_cum_units = 40000;  // agrees with the ledger...
  // ...but the order is only 40% done, so "Filled" is not credible.
  const RiskResult r = env->lp_event(filled);
  CHECK(r.decision == RiskDecision::Quarantined);
  CHECK_EQ(static_cast<int>(r.reason), static_cast<int>(Reason::CumulativeMismatch));
  CHECK_EQ(buy_held(*env), 60000);
}

TEST(scenario_lifecycle, a_late_report_on_a_terminal_chain_is_held_not_applied) {
  std::unique_ptr<LabEnv> env = env_with_partial_fill();
  VerifiedOrderEvent accepted =
      event_for(*env, "chain-1", OrderEventKind::CancelAccepted, "cxl-1");
  accepted.verified_cum_units = 40000;
  CHECK(env->lp_event(accepted).allowed());
  CHECK_EQ(buy_held(*env), 0);

  // A fill arriving after the terminal report is contradictory.
  const RiskResult late_fill = env->lp_fill("ACC-A", "chain-1", "exec-late", 1000);
  CHECK(late_fill.decision == RiskDecision::Quarantined);
  CHECK_EQ(static_cast<int>(late_fill.reason),
           static_cast<int>(Reason::ReservationTerminal));
  CHECK_EQ(env->position("ACC-A", "EURUSD"), 40000);

  // A duplicate terminal report is merely logged.
  std::unique_ptr<LabEnv> other = env_with_partial_fill();
  VerifiedOrderEvent first =
      event_for(*other, "chain-1", OrderEventKind::CancelAccepted, "cxl-1");
  first.verified_cum_units = 40000;
  CHECK(other->lp_event(first).allowed());
  VerifiedOrderEvent second =
      event_for(*other, "chain-1", OrderEventKind::Expired, "exp-1");
  second.verified_cum_units = 40000;
  const RiskResult late = other->lp_event(second);
  CHECK(late.decision == RiskDecision::HoldRetryable);
  CHECK_EQ(static_cast<int>(late.reason),
           static_cast<int>(Reason::ReconciliationRequired));
}

TEST(scenario_lifecycle, events_still_apply_while_new_risk_is_blocked) {
  // §17: failing closed must not block recovery. Fills, cancels and rejects
  // keep flowing in NO_NEW.
  std::unique_ptr<LabEnv> env = env_with_partial_fill();
  CHECK(env->coordinator()
            .set_mode(DomainMode::NoNew, env->operator_principal(), "test")
            .allowed());

  CHECK(env->new_order("ACC-A", "chain-2", "EURUSD", Side::Buy, 1000, kPrice, "new-2")
            .decision == RiskDecision::DenyDefinitive);

  CHECK(env->lp_fill("ACC-A", "chain-1", "exec-2", 10000).allowed());
  CHECK_EQ(env->position("ACC-A", "EURUSD"), 50000);
  CHECK(env->cancel_order("ACC-A", "chain-1", "cancel-1").allowed());

  VerifiedOrderEvent accepted =
      event_for(*env, "chain-1", OrderEventKind::CancelAccepted, "cxl-1");
  accepted.verified_cum_units = 50000;
  CHECK(env->lp_event(accepted).allowed());
  CHECK_EQ(buy_held(*env), 0);
  check_invariants(*env, "events under NO_NEW");
}

TEST(scenario_lifecycle, a_cancel_is_refused_in_recon_only_mode) {
  std::unique_ptr<LabEnv> env = env_with_partial_fill();
  CHECK(env->coordinator()
            .set_mode(DomainMode::ReconOnly, env->operator_principal(), "test")
            .allowed());

  const RiskResult r = env->cancel_order("ACC-A", "chain-1", "cancel-1");
  CHECK(r.decision == RiskDecision::DenyDefinitive);
  CHECK_EQ(static_cast<int>(r.reason),
           static_cast<int>(Reason::KillSwitchReconOnly));
  // But a verified fill still applies.
  CHECK(env->lp_fill("ACC-A", "chain-1", "exec-2", 10000).allowed());
}

TEST(scenario_lifecycle, a_cancel_in_cancel_only_mode_still_holds_the_budget) {
  // §7.3: CANCEL_ONLY lets the cancel through but does not release anything.
  std::unique_ptr<LabEnv> env = env_with_partial_fill();
  CHECK(env->coordinator()
            .set_mode(DomainMode::CancelOnly, env->operator_principal(), "test")
            .allowed());

  CHECK(env->cancel_order("ACC-A", "chain-1", "cancel-1").allowed());
  CHECK_EQ(buy_held(*env), 60000);
  check_invariants(*env, "cancel-only");
}

TEST(scenario_lifecycle, a_cancel_on_a_terminal_or_unknown_chain) {
  {
    std::unique_ptr<LabEnv> env = env_with_partial_fill();
    VerifiedOrderEvent accepted =
        event_for(*env, "chain-1", OrderEventKind::CancelAccepted, "cxl-1");
    accepted.verified_cum_units = 40000;
    CHECK(env->lp_event(accepted).allowed());

    const RiskResult r = env->cancel_order("ACC-A", "chain-1", "cancel-2");
    CHECK(r.decision == RiskDecision::DenyDefinitive);
    CHECK_EQ(static_cast<int>(r.reason),
             static_cast<int>(Reason::ReservationTerminal));
  }
  {
    std::unique_ptr<LabEnv> env = make_env();
    const RiskResult r = env->cancel_order("ACC-A", "never-existed", "cancel-1");
    CHECK(r.decision == RiskDecision::DenyDefinitive);
    CHECK_EQ(static_cast<int>(r.reason), static_cast<int>(Reason::UnknownChain));
  }
}

TEST(scenario_lifecycle, the_whole_24_5_scenario_reproduces_its_numbers) {
  // The worked example at the end of §24.5, minus the crash step (which the
  // crash suite covers): accepted 100k, fill 40k, replace to 150k, fill 20k.
  std::unique_ptr<LabEnv> env = make_env();
  CHECK(env->new_order("ACC-A", "chain-1", "EURUSD", Side::Buy, 100000, kPrice,
                       "new-1")
            .allowed());
  CHECK(env->dispatch("new-1").allowed());
  CHECK(env->lp_accept("ACC-A", "chain-1", "ack-1").allowed());
  CHECK(env->lp_fill("ACC-A", "chain-1", "e-1", 40000).allowed());
  CHECK(env->replace_order("ACC-A", "chain-1", 150000, kPrice, "repl-1").allowed());
  CHECK_EQ(buy_held(*env), 110000);

  CHECK(env->lp_fill("ACC-A", "chain-1", "e-2", 20000).allowed());

  std::optional<Reservation> r = env->reservation_for("ACC-A", "chain-1");
  CHECK(r.has_value());
  CHECK_EQ(r->cum_units, 60000);
  CHECK_EQ(buy_held(*env), 90000);

  VerifiedOrderEvent accepted =
      event_for(*env, "chain-1", OrderEventKind::ReplaceAccepted, "repl-ack-1");
  accepted.accepted_total_units = 150000;
  CHECK(env->lp_event(accepted).allowed());

  r = env->reservation_for("ACC-A", "chain-1");
  CHECK(r.has_value());
  CHECK_EQ(r->accepted_total_units, 150000);
  CHECK_EQ(r->cum_units, 60000);
  CHECK_EQ(buy_held(*env), 90000);
  CHECK_EQ(env->position("ACC-A", "EURUSD"), 60000);
  // No duplicate executions and exactly two original messages.
  CHECK_EQ(env->store().outbox().size(), static_cast<std::size_t>(2));
  check_invariants(*env, "§24.5 scenario");
}
