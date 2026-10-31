#include "lp/metrics.hpp"

#include <cstdio>

#include "lp/wire.hpp"

namespace lpsim {

namespace {

void line(std::string* out, std::string_view name, std::uint64_t v) {
    out->append("lp_").append(name).append(" ").append(std::to_string(v)).push_back('\n');
}

void line_i(std::string* out, std::string_view name, std::int64_t v) {
    out->append("lp_").append(name).append(" ").append(std::to_string(v)).push_back('\n');
}

void labelled(std::string* out, std::string_view name, std::string_view label,
              std::string_view key, std::uint64_t v) {
    out->append("lp_")
        .append(name)
        .append("{")
        .append(label)
        .append("=\"")
        .append(key)
        .append("\"} ")
        .append(std::to_string(v))
        .push_back('\n');
}

}  // namespace

std::string Metrics::render() const {
    std::string out;
    out.reserve(2048);

    line(&out, "fix_sessions_active", sessions_active);
    line(&out, "logon_rejected_total", logon_rejected_total);
    line(&out, "resend_requests_total", resend_requests_total);
    line(&out, "duplicate_fix_frames_total", duplicate_fix_frames_total);
    line(&out, "sequence_divergence_total", sequence_divergence_total);

    line(&out, "orders_accepted_total", orders_accepted_total);
    line(&out, "fills_total", fills_total);
    line_i(&out, "filled_qty_units_total", filled_qty_units_total);
    line(&out, "duplicate_clordid_total", duplicate_clordid_total);

    for (const auto& kv : orders_rejected_total)
        labelled(&out, "orders_rejected_total", "reason", kv.first, kv.second);
    for (const auto& kv : cancel_reject_total)
        labelled(&out, "cancel_reject_total", "reason", kv.first, kv.second);
    for (const auto& kv : replace_reject_total)
        labelled(&out, "replace_reject_total", "reason", kv.first, kv.second);

    line(&out, "live_orders", live_orders);
    line_i(&out, "fills_ledger_qty_total", fills_ledger_qty_total);
    line(&out, "invariant_violations_total", invariant_violations_total);
    for (const auto& kv : quote_inventory_units) {
        out.append("lp_quote_inventory_units{symbol=\"")
            .append(kv.first)
            .append("\"} ")
            .append(std::to_string(kv.second))
            .push_back('\n');
    }
    line(&out, "state_recovery_failures_total", state_recovery_failures_total);

    line(&out, "db_errors_total", db_errors_total);
    line(&out, "outbox_pending", outbox_pending);
    line(&out, "send_once_conflicts_total", send_once_conflicts_total);

    line(&out, "actor_queue_depth", actor_queue_depth);
    line(&out, "scheduled_event_queue_depth", scheduled_event_queue_depth);
    line_i(&out, "scenario_elapsed_virtual_ms", scenario_elapsed_virtual_ms);
    return out;
}

std::string AuditRecord::render() const {
    std::string out;
    out.reserve(512);
    auto kv = [&out](std::string_view k, std::string_view v) {
        if (v.empty()) return;
        if (!out.empty()) out.push_back(' ');
        out.append(k).push_back('=');
        out.append(v);
    };
    auto kvn = [&out](std::string_view k, std::uint64_t v) {
        if (v == 0) return;
        if (!out.empty()) out.push_back(' ');
        out.append(k).push_back('=');
        out.append(std::to_string(v));
    };

    kv("timestamp_utc", timestamp_utc);
    if (virtual_time_us != 0) {
        if (!out.empty()) out.push_back(' ');
        out.append("virtual_time_us=").append(std::to_string(virtual_time_us));
    }
    kv("run_id", run_id);
    kvn("seed", seed);
    kv("session_id", session_id);
    kvn("epoch", epoch);
    kvn("connection_generation", connection_generation);
    kvn("inbound_seq", inbound_seq);
    kv("cl_ord_id", cl_ord_id);
    kv("orig_cl_ord_id", orig_cl_ord_id);
    kv("order_id", order_id);
    kv("exec_id", exec_id);
    kv("event_id", event_id);
    kvn("quote_version", quote_version);
    kvn("order_version", order_version);
    if (cum_qty != 0) {
        if (!out.empty()) out.push_back(' ');
        out.append("cum_qty=").append(std::to_string(cum_qty));
    }
    if (leaves_qty != 0) {
        if (!out.empty()) out.push_back(' ');
        out.append("leaves_qty=").append(std::to_string(leaves_qty));
    }
    kvn("outbox_seq", outbox_seq);
    kv("fault_id", fault_id);
    kv("decision_reason", decision_reason);

    // The audit line is itself redacted, so a decision_reason that quoted part
    // of a frame cannot leak a credential.
    return redact(out);
}

bool is_sensitive_tag(int tag_num) {
    return tag_num == tag::Password || tag_num == tag::Username;
}

std::string redact(std::string_view text) {
    // Walk the text looking for "<tag>=" where tag is sensitive, and replace the
    // value up to the next delimiter. Both SOH and the printable vertical bar
    // are treated as delimiters so a rendered frame redacts the same way as a
    // raw one.
    std::string out;
    out.reserve(text.size());

    // A tag starts at the beginning of the text or after any character that
    // cannot be part of an identifier. Restricting this to SOH, bar and space
    // was not enough: an audit field rendered as "decision_reason=554=secret"
    // puts the tag after an equals sign, and the secret survived. The net has
    // to catch a sensitive tag wherever it appears, because its whole purpose
    // is to cover the cases where raw frame text reached a log line by mistake.
    const auto is_ident = [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
               c == '_';
    };

    std::size_t i = 0;
    while (i < text.size()) {
        const bool at_boundary = (i == 0) || !is_ident(text[i - 1]);
        if (!at_boundary || text[i] < '0' || text[i] > '9') {
            out.push_back(text[i]);
            ++i;
            continue;
        }
        std::size_t j = i;
        int tag_num = 0;
        while (j < text.size() && text[j] >= '0' && text[j] <= '9') {
            tag_num = tag_num * 10 + (text[j] - '0');
            if (tag_num > 999999) break;
            ++j;
        }
        if (j >= text.size() || text[j] != '=' || !is_sensitive_tag(tag_num)) {
            out.append(text.substr(i, j - i));
            i = j;
            continue;
        }
        // Copy "<tag>=" then skip the value.
        out.append(text.substr(i, j - i + 1));
        out.append("[REDACTED]");
        std::size_t k = j + 1;
        while (k < text.size() && text[k] != kSOH && text[k] != '|' && text[k] != ' ') ++k;
        i = k;
    }
    return out;
}

}  // namespace lpsim
