#include "model.hpp"

#include <limits>

namespace lab {

void Model::check(int step, const char* action) {
    const bool full = opt_.full_check_every <= 1 || step < 0 ||
                      (step % opt_.full_check_every) == 0;
    const auto v = lab.violations_incremental(journal_checked_, audit_checked_,
                                              quarantine_checked_, !full);
    if (v.empty()) {
        journal_checked_ = lab.disk.journal.size();
        audit_checked_ = lab.disk.audit.size();
        quarantine_checked_ = lab.disk.quarantine.size();
        return;
    }
    throw tt::AssertionFailure("model seed=" + std::to_string(opt_.seed) + " step=" +
                               std::to_string(step) + " action=" + action +
                               " first failing invariant: " + router::render_violations(v));
}

void Model::full_check(int step, const char* action) {
    const auto v = lab.violations();
    if (v.empty()) return;
    throw tt::AssertionFailure("model seed=" + std::to_string(opt_.seed) + " step=" +
                               std::to_string(step) + " action=" + action +
                               " first failing invariant: " + router::render_violations(v));
}

void Model::run(int steps) {
    for (int step = 0; step < steps; ++step) {
        steps_run = static_cast<std::size_t>(step) + 1;
        const std::uint64_t roll = pick(100);
        const char* action = "none";

        // Retire chains nothing more can happen to. Without this the walk fills
        // its working set with held and terminal chains and stops exercising the
        // business paths, which showed up as a soak run with almost no fills.
        if ((step % 16) == 0) {
            std::vector<Live> keep;
            for (const Live& l : live_) {
                const router::p03::ChainState* c = lab.chain(l.order.chain_id);
                if (c == nullptr) continue;
                if (c->terminal() || c->hold.has_value()) continue;
                keep.push_back(l);
            }
            live_ = keep;
        }

        // Keep a working set of chains, then act on them. The weights are
        // deliberately lopsided: reports and duplicates dominate, because that
        // is where the interesting contradictions live.
        if (live_.size() < static_cast<std::size_t>(opt_.max_chains) && roll < 14) {
            action = "submit_new";
            const std::int64_t qty = static_cast<std::int64_t>(1 + pick(100)) * 1000;
            auto r = lab.submit(lab.new_order("K" + std::to_string(next_command_++),
                                              std::to_string(qty), "1.08500"));
            if (r.is_ok()) {
                Live l;
                l.order = r.value();
                l.sent_qty = qty;
                live_.push_back(l);
            }
        } else if (roll < 22) {
            action = "dispatch";
            lab.rt->dispatch_pending();
        } else if (roll < 30) {
            action = "pump";
            lab.rt->pump_inbound(32);
        } else if (!live_.empty() && roll < 40) {
            action = "lp_ack_or_trade";
            Live& l = live_[pick(live_.size())];
            const router::p03::ChainState* c = lab.chain(l.order.chain_id);
            if (c != nullptr && !c->terminal() && !c->hold.has_value()) {
                const std::int64_t remaining = c->accepted_qty - c->cum_qty;
                if (remaining > 0) {
                    const std::int64_t fill =
                        1 + static_cast<std::int64_t>(pick(static_cast<std::uint64_t>(remaining)));
                    lab.lp->execute(c->accepted_cl_ord_id.str(), fill, 108500);
                }
            }
        } else if (!live_.empty() && roll < 48) {
            action = "duplicate_report";
            // Hand an already delivered receipt back, which is what P01's
            // at-least-once contract permits.
            ++duplicate_reports;
            const std::uint64_t id = 1 + pick(lab.session_disk.next_receipt);
            lab.session->redeliver(id);
        } else if (!live_.empty() && roll < 56) {
            action = "submit_cancel";
            Live& l = live_[pick(live_.size())];
            auto r = lab.submit(
                lab.cancel_order("K" + std::to_string(next_command_++), l.order.chain_id));
            if (r.is_ok()) l.pending_cancel = r.value().cl_ord_id.str();
        } else if (!live_.empty() && roll < 62) {
            action = "submit_replace";
            Live& l = live_[pick(live_.size())];
            const router::p03::ChainState* c = lab.chain(l.order.chain_id);
            if (c != nullptr) {
                const std::int64_t total = c->cum_qty + static_cast<std::int64_t>(1 + pick(50)) * 1000;
                auto r = lab.submit(lab.replace_order("K" + std::to_string(next_command_++),
                                                      l.order.chain_id, std::to_string(total),
                                                      "1.08490"));
                if (r.is_ok()) l.pending_replace = r.value().cl_ord_id.str();
            }
        } else if (!live_.empty() && roll < 68) {
            action = "lp_resolve_request";
            Live& l = live_[pick(live_.size())];
            if (!l.pending_cancel.empty()) {
                lab.lp->accept_cancel(l.pending_cancel);
                l.pending_cancel.clear();
            } else if (!l.pending_replace.empty()) {
                lab.lp->accept_replace(l.pending_replace);
                l.pending_replace.clear();
            }
        } else if (!live_.empty() && roll < 74) {
            action = "malformed_report";
            Live& l = live_[pick(live_.size())];
            const router::p03::ChainState* c = lab.chain(l.order.chain_id);
            if (c != nullptr) {
                router::p02::ReportSpec s = lab.lp->trade(
                    c->accepted_cl_ord_id.str(), "EXM" + std::to_string(next_exec_++), 1000,
                    108500);
                switch (pick(6)) {
                    case 0: s.omit.insert(32); break;
                    case 1: s.omit.insert(31); break;
                    case 2: s.raw[150] = "Z"; break;
                    case 3: s.symbol = "GBP/USD"; break;
                    case 4: s.account = "ACC2"; break;
                    default: s.cl_ord_id = "O-ghost"; break;
                }
                lab.lp->send(s);
            }
        } else if (opt_.inject_id_collisions && !live_.empty() && roll < 80) {
            action = "id_collision";
            // Force the same execution identity onto a different chain, or the
            // same identity with different content onto the same chain.
            Live& l = live_[pick(live_.size())];
            const router::p03::ChainState* c = lab.chain(l.order.chain_id);
            if (c != nullptr) {
                router::p02::ReportSpec s =
                    lab.lp->trade(c->accepted_cl_ord_id.str(), "EX-COLLIDE", 1000, 108500);
                s.cum_qty = c->cum_qty + 1000;
                s.leaves_qty = c->accepted_qty > s.cum_qty ? c->accepted_qty - s.cum_qty : 0;
                if (pick(2) == 0) s.last_px = 109000;  // same id, different content
                lab.lp->send(s);
            }
        } else if (opt_.inject_extreme_numbers && !live_.empty() && roll < 84) {
            action = "extreme_numbers";
            Live& l = live_[pick(live_.size())];
            const router::p03::ChainState* c = lab.chain(l.order.chain_id);
            if (c != nullptr) {
                router::p02::ReportSpec s = lab.lp->trade(
                    c->accepted_cl_ord_id.str(), "EXX" + std::to_string(next_exec_++), 1000,
                    108500);
                const char* huge[] = {"9223372036854775807", "9223372036854775808",
                                      "99999999999999999999", "-1", "0"};
                const int which = static_cast<int>(pick(5));
                switch (pick(3)) {
                    case 0: s.raw[32] = huge[which]; break;
                    case 1: s.raw[14] = huge[which]; break;
                    default: s.raw[31] = huge[which]; break;
                }
                lab.lp->send(s);
            }
        } else if (opt_.inject_faults && roll < 88) {
            action = "session_flap";
            if (lab.session->ready_for_app())
                lab.session->disconnect();
            else
                lab.session->reconnect(pick(8) == 0);
        } else if (opt_.inject_faults && roll < 92) {
            // A restart is rare on purpose. A walk that reboots every twenty
            // steps spends its time in recovery and explores very little
            // business state, and each boot re-verifies the whole audit chain,
            // which is the behaviour the integrity gate wants but not something
            // a realistic run does thousands of times.
            if (pick(16) == 0) {
                action = "crash_reboot";
                ++crashes;
                lab.crash();
                lab.boot();
            } else {
                action = "session_flap";
                if (lab.session->ready_for_app())
                    lab.session->disconnect();
                else
                    lab.session->reconnect(false);
            }
        } else if (roll < 96) {
            action = "timer";
            lab.clock.advance(1 + pick(2000000));
            lab.rt->on_timer();
        } else {
            action = "advance_clock";
            lab.clock.advance(1 + pick(50000));
        }

        check(step, action);
    }

    // Final sweep: drain whatever is left and check once more, so a run cannot
    // end with work in flight that would have broken something.
    lab.drive(16);
    // The complete check, with no watermarks and no skipping, at the end of
    // every walk however long it was.
    full_check(-1, "final_drain");

    for (const auto& [id, c] : lab.disk.chains) {
        (void)id;
        applied_fills += c.fills.size();
    }
}

}  // namespace lab
