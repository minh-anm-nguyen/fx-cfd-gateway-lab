// Group Q -- observability, runbooks, SLOs and adversarial performance
// (design doc 18.Q).
//
// Two kinds of claim are kept strictly apart here. Correctness claims are
// asserted. Performance numbers are measured and reported, never asserted
// against a target this project has not earned: the design doc is explicit
// that the CV's 95 microsecond receive-to-route figure belongs to a different
// system and must not be reused (section 2.3, open question 11).
#include <cstdio>

#include "../support/lab.hpp"

using namespace lab;

SCENARIO("Q-01", "P0", "every commit records before and after hashes with its source reference") {
    Lab L;
    const OrderId id = L.working("O1", "100000");
    CHECK(L.submit(trade("X1", "O1").seq(42).lastq("40000").lastpx("1.08490").cum("40000")).ok());

    CHECK(!L.engine().audit().empty());
    const AuditRecord& last = L.engine().audit().back();
    CHECK_EQ(last.chain_id, id.value);
    CHECK_EQ(last.fix_exec_id, std::string("X1"));
    CHECK_EQ(last.fix_cl_ord_id, std::string("O1"));
    CHECK_EQ(last.source_seq, uint64_t{42});
    CHECK_EQ(last.source_session_id, std::string("S1"));
    CHECK_EQ(last.decision, std::string("APPLIED"));
    CHECK_EQ(last.new_revision, last.old_revision + 1);
    CHECK_NE(last.before_hash, last.after_hash);
    CHECK_EQ(last.after_hash, L.state(id).state_hash());
    CHECK(last.source_inbox_id != 0);
    CHECK_EQ(last.policy_version, L.policy().policy_version);

    // The causal chain can be walked: each record's before-hash is the
    // previous record's after-hash for the same chain.
    uint64_t previous_after = 0;
    bool first = true;
    for (const AuditRecord& rec : L.engine().audit()) {
        if (rec.chain_id != id.value) continue;
        if (rec.decision != "APPLIED") continue;
        if (!first) CHECK_EQ(rec.before_hash, previous_after);
        previous_after = rec.after_hash;
        first = false;
    }
}

SCENARIO("Q-02", "P0", "a detected duplicate increments a counter and produces no effect") {
    Lab L;
    const OrderId id = L.working("O1", "100000");
    const auto t = trade("X1", "O1").lastq("40000").lastpx("1.08490").cum("40000");
    CHECK(L.submit(t).ok());
    const size_t effects_before = L.engine().sink().dispatched().size();
    const uint64_t dupes_before = L.engine().metrics().duplicates;
    const uint64_t revision_before = L.state(id).revision;

    expect_decision(L.submit(t), Decision::DuplicateNoop);

    CHECK_EQ(L.engine().metrics().duplicates, dupes_before + 1);
    CHECK_EQ(L.engine().sink().dispatched().size(), effects_before);
    CHECK_EQ(L.state(id).revision, revision_before);
    // The reason is in the audit trail, so the no-op is explainable.
    CHECK_EQ(L.engine().audit().back().decision, std::string("DUPLICATE_NOOP"));
    CHECK_EQ(L.engine().audit().back().reason, std::string("DUPLICATE_SAME_PAYLOAD"));
}

SCENARIO("Q-03", "P0", "an overfill alert names the chain, the ExecID and both quantities") {
    Lab L;
    const OrderId id = L.part_filled("O1", "100000", "40000");
    const EngineResult r = L.submit(trade("X2", "O1").lastq("70000").lastpx("1.08490"));
    expect_quarantine(r, Reason::Overfill);

    // The decision carries the numbers an operator needs.
    CHECK(r.detail.find("110000") != std::string::npos);  // what it would have become
    CHECK(r.detail.find("100000") != std::string::npos);  // the accepted quantity

    const QuarantineIncident& incident = L.state(id).incidents.back();
    CHECK_EQ(incident.reason_code, std::string("OVERFILL"));
    CHECK_EQ(incident.exec_id, std::string("X2"));
    CHECK_EQ(incident.cl_ord_id, std::string("O1"));

    // And an alert effect was raised rather than only a log line.
    bool alerted = false;
    for (const OutboxRow& row : L.engine().sink().dispatched()) {
        if (row.kind == EffectIntent::Kind::Alert && row.detail.find("110000") != std::string::npos) {
            alerted = true;
        }
    }
    CHECK(alerted);
}

SCENARIO("Q-04", "P0", "a lost acknowledgement increments unknown_outcome and queries once") {
    Lab L;
    const EngineResult n = L.submit(cmd_new("O1", "100000"));
    CHECK(L.submit(sys(SystemKind::Dispatched, "O1")).ok());
    CHECK_EQ(L.engine().metrics().unknown_outcome, uint64_t{0});

    CHECK(L.submit(sys(SystemKind::AckTimeout, "O1")).ok());
    CHECK_EQ(L.engine().metrics().unknown_outcome, uint64_t{1});
    CHECK_EQ(L.engine().sink().count_key("query-status|1|O1"), size_t{1});

    // A repeated timeout escalates rather than issuing a second identical
    // query, and never records success.
    for (int i = 0; i < 8; ++i) {
        CHECK(L.submit(sys(SystemKind::AckTimeout, "O1")).ok());
    }
    CHECK(L.state(n.id).reconcile_attempts > L.policy().limits.max_reconcile_attempts);
    // The key was offered once per timeout and sent exactly once: the port
    // suppressed the repeats rather than issuing a second query.
    size_t queries_sent = 0;
    for (const OutboxRow& row : L.engine().sink().dispatched()) {
        if (row.idempotency_key == "query-status|1|O1") ++queries_sent;
    }
    CHECK_EQ(queries_sent, size_t{1});
    CHECK(L.engine().sink().count_key("query-status|1|O1") > size_t{1});
    CHECK(L.engine().sink().count_key("query-status|1|O1|escalate") >= size_t{1});
    CHECK_EQ(L.lc(n.id), Lifecycle::ReconciliationRequired);
}

SCENARIO("Q-05", "P0", "a commit timeout alerts and publishes no confirmed revision") {
    Lab L;
    const OrderId id = L.working("O1", "100000");
    const uint64_t revision = L.state(id).revision;

    L.store().failpoint().arm(CrashWindow::W12_AmbiguousFlush);
    const EngineResult r = L.submit(trade("X1", "O1").lastq("40000").lastpx("1.08490").cum("40000"));
    L.store().failpoint().disarm();

    CHECK(!r.committed);
    CHECK_EQ(r.commit_status, CommitStatus::Ambiguous);
    CHECK_EQ(L.state(id).revision, revision);
    CHECK_EQ(L.engine().metrics().ambiguous_commits, uint64_t{1});
    CHECK(!L.engine().trading_allowed());

    // A storage failure is counted separately from an ambiguous one.
    L.store().set_fail_next_commit(true, "commit timed out");
    CHECK(!L.submit(trade("X2", "O1").lastq("40000").lastpx("1.08490").cum("40000")).committed);
    CHECK_EQ(L.engine().metrics().commit_failures, uint64_t{1});
    CHECK_EQ(L.state(id).revision, revision);
}

SCENARIO("Q-06", "P0", "a replay hash divergence blocks trading and names the first event") {
    Lab L;
    const OrderId id = L.working("O1", "100000");
    CHECK(L.submit(trade("X1", "O1").lastq("40000").lastpx("1.08490").cum("40000")).ok());
    CHECK(L.submit(trade("X2", "O1").lastq("20000").lastpx("1.08491").cum("60000")).ok());
    CHECK(L.engine().trading_allowed());

    CHECK(L.store().corrupt_journal_hash(3));
    const RecoveryReport rep = L.engine().recover();
    CHECK_EQ(rep.status, RecoveryStatus::HashDivergence);
    CHECK(!rep.trading_allowed);
    CHECK(!L.engine().trading_allowed());
    CHECK(rep.first_divergent_event != 0);
    CHECK(rep.detail.find("journal recorded") != std::string::npos);
    (void)id;
}

SCENARIO("Q-07", "P0", "queue saturation is visible and throttles rather than drops") {
    Policy p = default_lab_policy();
    p.limits.max_queue_events = 16;
    Lab L(p);
    const OrderId id = L.working("O1", "100000");

    size_t admitted = 0;
    for (int i = 0; i < 64; ++i) {
        PeerReport r = trade("X" + std::to_string(i), "O1").lastq("1000").lastpx("1.08490").r;
        r.source_seq = static_cast<uint64_t>(i + 1);
        if (L.engine().ingest(OrderEvent{r})) ++admitted;
    }
    CHECK(L.engine().metrics().backpressure_rejections > 0);
    CHECK(L.engine().metrics().queue_high_water > 0);
    CHECK(L.engine().metrics().queue_high_water <= p.limits.max_queue_events);
    CHECK_EQ(admitted, p.limits.max_queue_events);

    // Everything admitted is applied; nothing admitted was discarded.
    CHECK_EQ(L.engine().pump_all(), admitted);
    CHECK_EQ(L.state(id).ledger.size(), admitted);
}

SCENARIO("Q-08", "P1", "the latency metric is labelled as event to commit, not receive to route") {
    Lab L;
    const OrderId id = L.working("O1", "100000000");
    for (int i = 0; i < 800; ++i) {
        CHECK(L.submit(trade("X" + std::to_string(i), "O1")
                           .lastq("1000")
                           .lastpx("1.08490")
                           .cum(std::to_string((i + 1) * 1000)))
                  .ok());
    }
    const LatencySummary s = L.engine().event_to_commit_latency();
    CHECK(s.samples > 0);
    CHECK(s.p50_ns > 0);
    CHECK(s.p99_ns >= s.p95_ns);
    CHECK(s.p95_ns >= s.p50_ns);
    CHECK(s.max_ns >= s.p99_ns);
    CHECK(std::string(s.what_is_measured()).find("excludes TCP receive") != std::string::npos);
    // Reported, not asserted against a target: this is the honest form.
    std::printf("          [measure] event->commit over %zu samples: p50=%lluns p95=%lluns "
                "p99=%lluns max=%lluns (%s)\n",
                s.samples, static_cast<unsigned long long>(s.p50_ns),
                static_cast<unsigned long long>(s.p95_ns),
                static_cast<unsigned long long>(s.p99_ns),
                static_cast<unsigned long long>(s.max_ns), s.what_is_measured());
    CHECK_EQ(L.state(id).ledger.size(), size_t{800});
}

SCENARIO("Q-09", "P1", "the same seed and trace reproduce the same business result") {
    const auto run = [] {
        Lab L;
        const OrderId id = L.working("O1", "1000000");
        for (int i = 0; i < 500; ++i) {
            CHECK(L.submit(trade("X" + std::to_string(i), "O1")
                               .lastq("1000")
                               .lastpx("1.08490")
                               .cum(std::to_string((i + 1) * 1000)))
                      .ok());
        }
        return L.state(id).state_hash();
    };
    // The business result is reproducible. The timings are not part of it and
    // are not compared, because they are a property of this host.
    CHECK_EQ(run(), run());
    CHECK_EQ(run(), run());
}

SCENARIO("Q-10", "P1", "a latency breach is reported separately from correctness") {
    Lab L;
    const OrderId id = L.working("O1", "1000000");
    for (int i = 0; i < 500; ++i) {
        CHECK(L.submit(trade("X" + std::to_string(i), "O1")
                           .lastq("1000")
                           .lastpx("1.08490")
                           .cum(std::to_string((i + 1) * 1000)))
                  .ok());
    }
    const LatencySummary s = L.engine().event_to_commit_latency();

    // A deliberately tiny budget, so the breach path is exercised on any host.
    constexpr uint64_t kAbsurdBudgetNs = 1;
    const bool breached = s.p99_ns > kAbsurdBudgetNs;
    if (breached) {
        std::printf("          [perf] p99 %lluns exceeds the illustrative %lluns budget; this is "
                    "a performance observation, not a correctness failure\n",
                    static_cast<unsigned long long>(s.p99_ns),
                    static_cast<unsigned long long>(kAbsurdBudgetNs));
    }
    // Correctness is unaffected by the breach, which is the whole point.
    const OrderAggregate& a = L.state(id);
    CHECK_EQ(a.ledger.size(), size_t{500});
    CHECK_EQ(a.cum_qty_units, int64_t{500000});
    CHECK(check_invariants(a, L.policy()).empty());
}

SCENARIO("Q-11", "P1", "the bounded structures do not grow under a stable workload") {
    Lab L;
    const OrderId id = L.working("O1", "100000000");
    // The ledger must grow; everything else must not. Resident memory itself
    // is measured by the external soak script, not from inside the process.
    size_t last_incidents = 0;
    for (int round = 0; round < 800; ++round) {
        CHECK(L.submit(trade("X" + std::to_string(round), "O1")
                           .lastq("1000")
                           .lastpx("1.08490")
                           .cum(std::to_string((round + 1) * 1000)))
                  .ok());
        CHECK_EQ(L.engine().total_held(), size_t{0});
        CHECK_EQ(L.state(id).incidents.size(), last_incidents);
        CHECK_EQ(L.store().unconsumed_inbox(), size_t{0});
        CHECK(L.state(id).accepted_versions.size() == 1);
    }
    CHECK_EQ(L.state(id).ledger.size(), size_t{800});
    CHECK(L.engine().event_to_commit_latency().samples <= size_t{65536});
}

SCENARIO("Q-12", "P1", "per-chain progress is observable when one chain is hot") {
    Lab L;
    const OrderId hot = L.working("HOT", "100000000");
    const OrderId cold = L.working("COLD", "100000");

    for (int i = 0; i < 1000; ++i) {
        CHECK(L.submit(trade("H" + std::to_string(i), "HOT")
                           .lastq("1000")
                           .lastpx("1.08490")
                           .cum(std::to_string((i + 1) * 1000)))
                  .ok());
    }
    // The cold chain is not starved: a single event still gets through, and
    // each chain's own progress is visible through its revision and index.
    CHECK(L.submit(trade("C1", "COLD").lastq("40000").lastpx("1.08490").cum("40000")).ok());

    // The hot chain did far more work; the cold one still made progress, and
    // each chain own position is visible rather than aggregated away. The
    // cold event was submitted last, so it holds the later index.
    CHECK(L.state(hot).revision > L.state(cold).revision);
    CHECK(L.state(cold).revision > uint64_t{0});
    CHECK(L.state(cold).last_applied_event_index > L.state(hot).last_applied_event_index);
    CHECK_EQ(L.state(cold).cum_qty_units, int64_t{40000});
    CHECK_EQ(L.state(hot).ledger.size(), size_t{1000});
}

SCENARIO("Q-13", "P0", "releasing a quarantine without evidence is refused") {
    Lab L;
    const OrderId id = L.part_filled("O1", "100000", "40000");
    expect_quarantine(L.submit(trade("X2", "O1").lastq("70000").lastpx("1.08490")),
                      Reason::Overfill);
    CHECK(L.state(id).quarantined);
    const uint64_t before = L.state(id).state_hash();

    SystemEvent bare = sys(SystemKind::ManualOverride, "O1");
    bare.resolved_status = "WORKING";
    expect_reject(L.submit(bare), Reason::NotAuthorized);
    CHECK(L.state(id).quarantined);
    CHECK_EQ(L.state(id).state_hash(), before);

    SystemEvent reconcile = sys(SystemKind::ReconcileResolved, "O1");
    reconcile.resolved_status = "WORKING";
    expect_reject(L.submit(reconcile), Reason::NotAuthorized);
    CHECK(L.state(id).quarantined);

    // With evidence and dual control the release is admitted and audited.
    SystemEvent authorized = bare;
    authorized.evidence_provided = true;
    authorized.dual_control_signed = true;
    CHECK(L.submit(authorized).ok());
    CHECK(!L.state(id).quarantined);
    CHECK_EQ(L.state(id).cum_qty_units, int64_t{40000});  // the ledger was never touched
    CHECK_EQ(L.engine().audit().back().decision, std::string("APPLIED"));
}

SCENARIO("Q-14", "P1", "audit retention shorter than dedup retention fails validation") {
    Policy p = default_lab_policy();
    p.dedup_retention_units = 100;
    p.audit_retention_units = 10;
    const ConfigError e = p.validate();
    CHECK(!e.ok());
    CHECK_EQ(e.code, ConfigErrorCode::AuditRetentionBelowDedup);

    Lab L;
    CHECK(!L.engine().reload_policy(p).ok());
    CHECK(L.policy().audit_retention_units >= L.policy().dedup_retention_units);
}

SCENARIO("Q-15", "P1", "metrics are best effort; the audit trail is not") {
    Lab L;
    const OrderId id = L.working("O1", "100000");
    const size_t audit_before = L.engine().audit().size();
    CHECK(L.submit(trade("X1", "O1").lastq("40000").lastpx("1.08490").cum("40000")).ok());

    // The commit does not depend on any metrics sink: the counters are plain
    // in-process fields and the audit record is produced in the same step as
    // the commit.
    CHECK_EQ(L.engine().audit().size(), audit_before + 1);
    CHECK_EQ(L.state(id).cum_qty_units, int64_t{40000});
    // The journal, which is the durable record, has the event too.
    bool journaled = false;
    for (const JournalRecord& rec : L.store().journal()) {
        const auto* r = std::get_if<PeerReport>(&rec.event);
        if (r != nullptr && r->exec_id == "X1") journaled = true;
    }
    CHECK(journaled);
}

SCENARIO("Q-16", "P0", "no credential or free-text payload reaches the audit trail") {
    Lab L;
    const OrderId id = L.working("O1", "100000");
    const std::string secret = "PASSWORD=hunter2;RawData=SECRETKEY";
    CHECK(L.submit(trade("X1", "O1")
                       .lastq("40000")
                       .lastpx("1.08490")
                       .cum("40000")
                       .text(secret))
              .ok());

    for (const AuditRecord& rec : L.engine().audit()) {
        const std::string line = rec.text();
        CHECK_MSG(line.find("hunter2") == std::string::npos, line);
        CHECK_MSG(line.find("SECRETKEY") == std::string::npos, line);
        CHECK_MSG(line.find("PASSWORD") == std::string::npos, line);
    }
    // Nor does it reach the canonical state or the dedup fingerprint.
    CHECK(L.state(id).canonical_text().find("hunter2") == std::string::npos);
    const auto without = trade("X1", "O1").lastq("40000").lastpx("1.08490").cum("40000");
    const auto with = trade("X1", "O1").lastq("40000").lastpx("1.08490").cum("40000").text(secret);
    CHECK_EQ(business_fingerprint(without.r), business_fingerprint(with.r));
}

SCENARIO("Q-17", "P1", "a thousand recovery cycles leak no bounded resource") {
    Lab L;
    const OrderId id = L.part_filled("O1", "100000", "40000");
    const uint64_t expected = L.state(id).state_hash();
    const size_t journal = L.store().journal().size();
    const size_t keys = L.store().applied_key_count();
    const size_t outbox = L.store().outbox().size();

    for (int cycle = 0; cycle < 1000; ++cycle) {
        const RecoveryReport rep = L.engine().recover();
        CHECK_MSG(rep.status == RecoveryStatus::Clean, rep.detail);
        CHECK_EQ(L.state(id).state_hash(), expected);
    }
    // Nothing accumulated across a thousand reconnect-and-recover cycles.
    CHECK_EQ(L.store().journal().size(), journal);
    CHECK_EQ(L.store().applied_key_count(), keys);
    CHECK_EQ(L.store().outbox().size(), outbox);
    CHECK_EQ(L.engine().total_held(), size_t{0});
    CHECK_EQ(L.engine().chain_count(), size_t{1});
}

SCENARIO("Q-18", "P0", "an invariant failure halts the chain and keeps the evidence") {
    // A state that cannot have arisen from any legal history: the cached
    // CumQty disagrees with the ledger. The reducer must refuse to build on it
    // rather than commit a state it cannot justify.
    Policy p = default_lab_policy();
    OrderAggregate corrupted;
    corrupted.id = OrderId{1};
    corrupted.firm = "FIRM";
    corrupted.business_day = "2026-10-30";
    corrupted.account = "ACC1";
    corrupted.symbol = "EURUSD";
    corrupted.side = Side::Buy;
    corrupted.initial_cl_ord_id = "O1";
    corrupted.peer_id = "LP1";
    corrupted.base = BaseState::Working;
    corrupted.cum_qty_units = 50000;  // no ledger entry backs this
    AcceptedVersion v;
    v.version = 0;
    v.cl_ord_id = "O1";
    v.qty_units = 100000;
    v.price_ticks = 108500;
    v.scale_id = 1;
    v.policy_version = 1;
    corrupted.accepted_versions.push_back(v);

    // The checker names the broken invariant.
    const std::vector<InvariantViolation> found = check_invariants(corrupted, p);
    CHECK(!found.empty());
    bool saw_conservation = false;
    for (const InvariantViolation& x : found) {
        if (x.id == 11) saw_conservation = true;
    }
    CHECK(saw_conservation);

    // And the reducer converts the violation into a quarantine instead of a
    // commit, with the evidence attached.
    PeerReport fill = trade("X1", "O1").lastq("10000").lastpx("1.08490").r;
    fill.source_seq = 1;
    const Reduction r = reduce(corrupted, OrderEvent{fill}, p, 1);
    CHECK_EQ(r.decision, Decision::Quarantined);
    CHECK_EQ(r.reason, Reason::InvariantViolated);
    CHECK(!r.violations.empty());
    CHECK(r.detail.find("I11") != std::string::npos);
    CHECK(r.next.quarantined);
    // No fill was applied on top of the broken state.
    CHECK_EQ(r.next.cum_qty_units, int64_t{50000});
    CHECK_EQ(r.next.ledger.size(), size_t{0});
}

SCENARIO("Q-19", "P0", "a blocked case is reported as blocked, never counted as a pass") {
    // The release gate in design doc 21.6 rejects a skipped P0 marked passed,
    // so the harness has to be able to say BLOCKED at all.
    size_t blocked_p0 = 0;
    size_t total = 0;
    for (const tt::Case& c : tt::registry()) {
        ++total;
        if (c.fn == nullptr) {
            // A blocked case must carry its reason, or it is indistinguishable
            // from a silent skip.
            CHECK_MSG(!c.blocked_reason.empty(), c.id + " is blocked without a reason");
            if (c.priority == "P0") ++blocked_p0;
        }
    }
    CHECK(total > 300);  // the catalog is registered, not partially linked
    // This suite does have blocked P0 cases, and they are visible as such.
    CHECK(blocked_p0 > 0);
    // A blocked case has no function to run, so it cannot accidentally pass.
    for (const tt::Case& c : tt::registry()) {
        if (!c.blocked_reason.empty()) CHECK(c.fn == nullptr);
    }
}

SCENARIO("Q-20", "P0", "a second writer is refused: v1 does not support failover writers") {
    Lab L;
    const OrderId id = L.working("O1", "100000");
    const uint64_t revision = L.state(id).revision;

    PeerReport a = trade("XA", "O1").lastq("10000").lastpx("1.08490").cum("10000").r;
    a.source_seq = 1;
    PeerReport b = trade("XB", "O1").lastq("20000").lastpx("1.08490").cum("20000").r;
    b.source_seq = 2;

    // Both writers believe they own the chain at the same revision. The CAS
    // lets exactly one through; there is no second-writer mode to fall back to.
    CHECK_EQ(L.engine().commit_as_second_writer(id, revision, OrderEvent{a}).status,
             CommitStatus::Committed);
    const CommitResult loser = L.engine().commit_as_second_writer(id, revision, OrderEvent{b});
    CHECK_EQ(loser.status, CommitStatus::RevisionConflict);
    CHECK(loser.detail.find("expected revision") != std::string::npos);

    size_t applied = 0;
    for (const JournalRecord& rec : L.store().journal()) {
        const auto* r = std::get_if<PeerReport>(&rec.event);
        if (r != nullptr && (r->exec_id == "XA" || r->exec_id == "XB")) ++applied;
    }
    CHECK_EQ(applied, size_t{1});
}
