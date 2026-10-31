#include "jrnl/config.hpp"

#include <fstream>
#include <sstream>

namespace jrnl {
namespace {

std::string trim(const std::string& s) {
    std::size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t')) ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r')) --b;
    return s.substr(a, b - a);
}

std::string strip_comment(const std::string& s) {
    const std::size_t h = s.find('#');
    return h == std::string::npos ? s : s.substr(0, h);
}

std::string unquote(const std::string& s) {
    if (s.size() >= 2 && ((s.front() == '"' && s.back() == '"') ||
                          (s.front() == '\'' && s.back() == '\''))) {
        return s.substr(1, s.size() - 2);
    }
    return s;
}

bool to_bool(const std::string& v, bool* out) {
    if (v == "true" || v == "yes" || v == "on" || v == "enabled") { *out = true; return true; }
    if (v == "false" || v == "no" || v == "off" || v == "disabled") { *out = false; return true; }
    return false;
}

bool to_int(const std::string& v, int* out) {
    try {
        std::size_t used = 0;
        const int n = std::stoi(v, &used);
        if (used != v.size()) return false;
        *out = n;
        return true;
    } catch (...) {
        return false;
    }
}

}  // namespace

bool parse_config(const std::string& text, RecoveryConfig* out, std::string* why) {
    RecoveryConfig c;
    std::istringstream in(text);
    std::string line;
    std::string section;
    int lineno = 0;

    auto bad = [&](const std::string& m) {
        if (why != nullptr) *why = "line " + std::to_string(lineno) + ": " + m;
        return false;
    };

    while (std::getline(in, line)) {
        ++lineno;
        const std::string raw = strip_comment(line);
        const std::string t = trim(raw);
        if (t.empty()) continue;

        const bool indented = !raw.empty() && (raw[0] == ' ' || raw[0] == '\t');
        const std::size_t colon = t.find(':');
        if (colon == std::string::npos) return bad("expected key: value");
        const std::string key = trim(t.substr(0, colon));
        const std::string value = unquote(trim(t.substr(colon + 1)));

        if (!indented) {
            if (!value.empty()) return bad("top-level key " + key + " must open a section");
            section = key;
            if (section != "storage" && section != "recovery" && section != "outbox" &&
                section != "backup") {
                return bad("unknown section " + section);
            }
            continue;
        }
        if (section.empty()) return bad("indented key outside any section");
        if (value.empty()) return bad("key " + key + " has no value");

        bool ok = true;
        if (section == "storage") {
            if (key == "pg_version_major") ok = to_int(value, &c.storage.pg_version_major);
            else if (key == "require_fsync") ok = to_bool(value, &c.storage.require_fsync);
            else if (key == "require_full_page_writes")
                ok = to_bool(value, &c.storage.require_full_page_writes);
            else if (key == "require_synchronous_commit")
                c.storage.require_synchronous_commit = value;
            else if (key == "max_transaction_retries")
                ok = to_int(value, &c.storage.max_transaction_retries);
            else if (key == "statement_timeout_ms")
                ok = to_int(value, &c.storage.statement_timeout_ms);
            else if (key == "lock_timeout_ms") ok = to_int(value, &c.storage.lock_timeout_ms);
            else return bad("unknown storage key " + key);
        } else if (section == "recovery") {
            if (key == "dispatch_default") {
                // Spelled as a word in the doc's example, so both the word
                // and a boolean are accepted, but nothing else.
                if (value == "disabled") c.recovery.dispatch_default_enabled = false;
                else if (value == "enabled") c.recovery.dispatch_default_enabled = true;
                else ok = to_bool(value, &c.recovery.dispatch_default_enabled);
            } else if (key == "verify_journal_hashes")
                ok = to_bool(value, &c.recovery.verify_journal_hashes);
            else if (key == "verify_projections")
                ok = to_bool(value, &c.recovery.verify_projections);
            else if (key == "verify_outbox_receipts")
                ok = to_bool(value, &c.recovery.verify_outbox_receipts);
            else if (key == "max_replay_batch_events")
                ok = to_int(value, &c.recovery.max_replay_batch_events);
            else if (key == "on_unknown_lp_outcome") c.recovery.on_unknown_lp_outcome = value;
            else if (key == "on_data_mismatch") c.recovery.on_data_mismatch = value;
            else return bad("unknown recovery key " + key);
        } else if (section == "outbox") {
            if (key == "claim_limit") ok = to_int(value, &c.outbox.claim_limit);
            else if (key == "safe_retry_requires_p01_stable_send")
                ok = to_bool(value, &c.outbox.safe_retry_requires_p01_stable_send);
            else if (key == "default_ambiguous_action")
                c.outbox.default_ambiguous_action = value;
            else return bad("unknown outbox key " + key);
        } else {
            if (key == "require_restore_drill") ok = to_bool(value, &c.backup.require_restore_drill);
            else if (key == "block_unsafe_wal_pruning")
                ok = to_bool(value, &c.backup.block_unsafe_wal_pruning);
            else return bad("unknown backup key " + key);
        }
        if (!ok) return bad("cannot parse value for " + key);
    }

    if (out != nullptr) *out = c;
    return true;
}

bool load_config(const std::string& path, RecoveryConfig* out, std::string* why) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        if (why != nullptr) *why = "cannot open " + path;
        return false;
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    return parse_config(ss.str(), out, why);
}

std::vector<ConfigProblem> validate_config(const RecoveryConfig& c) {
    std::vector<ConfigProblem> out;

    // Section 17.1 lists these three as the floor. Below it, the engine can
    // still run, but ACCEPTED_DURABLE stops meaning what section 1.2 says
    // it means, so the configuration is refused rather than annotated.
    if (!c.storage.require_fsync) {
        out.push_back({"storage.require_fsync",
                       "fsync off makes a committed transaction survivable only by luck"});
    }
    if (!c.storage.require_full_page_writes) {
        out.push_back({"storage.require_full_page_writes",
                       "torn pages after a crash become undetectable"});
    }
    if (c.storage.require_synchronous_commit != "on") {
        out.push_back({"storage.require_synchronous_commit",
                       "only 'on' supports the ACCEPTED_DURABLE definition in section 1.2; "
                       "remote_write and off both acknowledge before the flush"});
    }

    if (c.recovery.dispatch_default_enabled) {
        // JI-24. Dispatch enabled at startup means effects can leave before
        // the gates have run, which is the failure the whole supervisor
        // exists to prevent.
        out.push_back({"recovery.dispatch_default",
                       "dispatch must start disabled and be enabled only by the recovery gates"});
    }
    if (!c.recovery.verify_journal_hashes) {
        out.push_back({"recovery.verify_journal_hashes",
                       "replay without hash validation cannot detect a tampered event"});
    }
    if (!c.recovery.verify_projections) {
        out.push_back({"recovery.verify_projections",
                       "JI-12 requires comparing the replayed state to the projection"});
    }
    if (c.recovery.on_unknown_lp_outcome != "hold_and_reconcile") {
        out.push_back({"recovery.on_unknown_lp_outcome",
                       "an unknown counterparty outcome has exactly one safe handling"});
    }
    if (c.recovery.on_data_mismatch != "quarantine") {
        out.push_back({"recovery.on_data_mismatch",
                       "a data mismatch must quarantine; there is no safe repair"});
    }

    if (!c.outbox.safe_retry_requires_p01_stable_send) {
        out.push_back({"outbox.safe_retry_requires_p01_stable_send",
                       "retrying an ambiguous send without a durable stable-send mapping can "
                       "issue the same order twice"});
    }
    if (c.outbox.default_ambiguous_action != "unknown" &&
        c.outbox.default_ambiguous_action != "held") {
        out.push_back({"outbox.default_ambiguous_action",
                       "an ambiguous send may only become UNKNOWN or HELD"});
    }
    if (c.outbox.claim_limit <= 0) {
        out.push_back({"outbox.claim_limit", "an unbounded claim is not backpressure"});
    }

    if (!c.backup.require_restore_drill) {
        out.push_back({"backup.require_restore_drill",
                       "section 17.2: a backup that has never been restored is not a backup"});
    }
    if (!c.backup.block_unsafe_wal_pruning) {
        out.push_back({"backup.block_unsafe_wal_pruning",
                       "pruning write-ahead data still needed for recovery destroys the "
                       "restore path"});
    }

    if (c.storage.max_transaction_retries <= 0) {
        out.push_back({"storage.max_transaction_retries",
                       "retries must be bounded so a hot row cannot spin for ever"});
    }
    if (c.recovery.max_replay_batch_events <= 0) {
        out.push_back({"recovery.max_replay_batch_events",
                       "an unbounded replay batch is an unbounded memory claim"});
    }
    return out;
}

}  // namespace jrnl
