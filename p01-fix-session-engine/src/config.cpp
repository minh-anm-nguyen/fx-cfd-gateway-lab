#include "fix/config.hpp"

#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_map>

#include "fix/wire.hpp"

namespace fixse {
namespace {

void add(std::vector<ConfigError>* errs, std::string field, std::string reason) {
    errs->push_back(ConfigError{std::move(field), std::move(reason)});
}

std::string trim(std::string_view s) {
    size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return std::string(s.substr(b, e - b));
}

bool parse_bool_literal(const std::string& v, bool* out) {
    if (v == "true") {
        *out = true;
        return true;
    }
    if (v == "false") {
        *out = false;
        return true;
    }
    return false;
}

bool parse_i64_literal(const std::string& v, long long* out) {
    if (v.empty()) return false;
    size_t i = 0;
    bool neg = false;
    if (v[0] == '-') {
        neg = true;
        i = 1;
    }
    if (i >= v.size()) return false;
    long long acc = 0;
    for (; i < v.size(); ++i) {
        if (v[i] < '0' || v[i] > '9') return false;
        if (acc > 900000000000000000LL) return false;
        acc = acc * 10 + (v[i] - '0');
    }
    *out = neg ? -acc : acc;
    return true;
}

bool parse_string_literal(const std::string& v, std::string* out) {
    if (v.size() < 2) return false;
    if (v.front() != '"' || v.back() != '"') return false;
    *out = v.substr(1, v.size() - 2);
    return true;
}

}  // namespace

std::string SessionConfig::session_id() const {
    std::string s;
    s.reserve(begin_string.size() + sender_comp_id.size() + target_comp_id.size() +
              qualifier.size() + 3);
    s.append(begin_string).push_back(':');
    s.append(sender_comp_id).push_back(':');
    s.append(target_comp_id).push_back(':');
    s.append(qualifier);
    return s;
}

std::vector<ConfigError> validate(const SessionConfig& c) {
    std::vector<ConfigError> e;

    if (c.begin_string != std::string(kBeginStringFix44))
        add(&e, "session.begin_string", "only FIX.4.4 is supported by this engine");
    if (c.sender_comp_id.empty()) add(&e, "session.sender_comp_id", "must not be blank");
    if (c.target_comp_id.empty()) add(&e, "session.target_comp_id", "must not be blank");
    if (c.sender_comp_id == c.target_comp_id)
        add(&e, "session.target_comp_id", "sender and target CompID must differ");
    // A HeartBtInt of zero disables heartbeats entirely and a huge value makes a
    // dead peer undetectable; both are rejected rather than negotiated (C-15).
    if (c.heartbeat_seconds < 1 || c.heartbeat_seconds > 3600)
        add(&e, "session.heartbeat_seconds", "must be within 1..3600");
    if (c.sending_time_tolerance_seconds < 1)
        add(&e, "session.sending_time_tolerance_seconds", "must be positive");

    if (c.port == 0) add(&e, "network.port", "must be a usable TCP port");
    if (c.host.empty()) add(&e, "network.host", "must not be blank");
    if (c.connect_timeout_ms < 1) add(&e, "network.connect_timeout_ms", "must be positive");
    if (c.reconnect_initial_ms < 1) add(&e, "network.reconnect_initial_ms", "must be positive");
    if (c.reconnect_max_ms < c.reconnect_initial_ms)
        add(&e, "network.reconnect_max_ms", "must be >= reconnect_initial_ms");
    if (c.reconnect_jitter_percent < 0 || c.reconnect_jitter_percent > 100)
        add(&e, "network.reconnect_jitter_percent", "must be within 0..100");
    // A frame cap below the smallest legal FIX message would reject every
    // message while looking like a working configuration (I-16).
    if (c.max_frame_bytes < 512 || c.max_frame_bytes > (16u << 20))
        add(&e, "network.max_frame_bytes", "must be within 512..16777216");
    if (c.max_read_buffer_bytes < c.max_frame_bytes)
        add(&e, "network.max_read_buffer_bytes", "must be >= max_frame_bytes");
    if (c.max_fields_per_frame < 8) add(&e, "network.max_fields_per_frame", "must be >= 8");
    if (c.partial_frame_deadline_ms < 1)
        add(&e, "network.partial_frame_deadline_ms", "must be positive");

    if (c.max_future_messages == 0) add(&e, "recovery.max_future_messages", "must be positive");
    if (c.max_future_bytes == 0) add(&e, "recovery.max_future_bytes", "must be positive");
    if (c.resend_chunk_size == 0) add(&e, "recovery.resend_chunk_size", "must be positive");
    if (c.max_resend_cycles < 1) add(&e, "recovery.max_resend_cycles", "must be positive");
    if (c.max_recovery_duration_ms < 1)
        add(&e, "recovery.max_recovery_duration_ms", "must be positive");
    if (c.min_resend_request_interval_ms < 0)
        add(&e, "recovery.min_resend_request_interval_ms", "must not be negative");
    if (c.test_request_timeout_ms < 1)
        add(&e, "recovery.test_request_timeout_ms", "must be positive");
    if (c.allow_gapfill_application_messages)
        add(&e, "recovery.allow_gapfill_application_messages",
            "gap filling our own application messages is not supported in v1; the resend "
            "planner retransmits them and fails closed when one is missing");

    if (c.backend != "sqlite") add(&e, "persistence.backend", "only sqlite is implemented in v1");
    if (c.database_path.empty()) add(&e, "persistence.database_path", "must not be blank");
    if (c.sqlite_journal_mode != "WAL")
        add(&e, "persistence.sqlite_journal_mode", "must be WAL for crash consistency");
    const bool strong = c.sqlite_synchronous == "FULL" || c.sqlite_synchronous == "EXTRA";
    if (!strong && !c.allow_weak_durability)
        add(&e, "persistence.sqlite_synchronous",
            "must be FULL or EXTRA unless allow_weak_durability is set explicitly");
    if (c.store_busy_timeout_ms < 0)
        add(&e, "persistence.store_busy_timeout_ms", "must not be negative");

    if (c.max_event_queue == 0) add(&e, "limits.max_event_queue", "must be positive");
    if (c.max_inbox_pending == 0) add(&e, "limits.max_inbox_pending", "must be positive");

    // TLS is specified in the design doc but not implemented in v1. Asking for
    // it must stop startup, never silently continue in plaintext (A-19, L-05).
    if (c.tls_enabled)
        add(&e, "security.tls_enabled",
            "TLS transport is not implemented in v1; refusing to fall back to plaintext");
    if (!c.username.empty() && c.password.empty())
        add(&e, "security.password", "a username without a secret cannot authenticate");
    if (c.max_audit_value_bytes < 16)
        add(&e, "security.max_audit_value_bytes", "must be >= 16");

    for (const std::string& t : c.app_msg_types) {
        if (t.empty()) add(&e, "app.app_msg_types", "must not contain a blank MsgType");
        if (is_session_message(t))
            add(&e, "app.app_msg_types", "must not contain a session MsgType: " + t);
    }

    return e;
}

bool load_config_file(const std::string& path, SessionConfig* out,
                      std::vector<ConfigError>* errors) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        add(errors, "file", "cannot open config file: " + path);
        return false;
    }

    SessionConfig c;
    std::string line, section;
    int line_no = 0;
    bool ok = true;

    while (std::getline(in, line)) {
        ++line_no;
        if (!line.empty() && line.back() == 13) line.pop_back();
        // Strip comments. No escaping is supported inside strings, so a value
        // containing a comment marker is rejected, not silently truncated.
        const size_t hash = line.find(35);
        std::string body = trim(hash == std::string::npos ? line : line.substr(0, hash));
        if (body.empty()) continue;

        if (body.front() == 91) {
            if (body.back() != 93) {
                add(errors, "line " + std::to_string(line_no), "malformed section header");
                ok = false;
                continue;
            }
            section = body.substr(1, body.size() - 2);
            continue;
        }

        const size_t eq = body.find(61);
        if (eq == std::string::npos) {
            add(errors, "line " + std::to_string(line_no), "expected key = value");
            ok = false;
            continue;
        }
        const std::string key = trim(body.substr(0, eq));
        const std::string val = trim(body.substr(eq + 1));
        const std::string full = section.empty() ? key : section + "." + key;

        std::string sv;
        long long iv = 0;
        bool bv = false;
        long long tmp = 0;

        const auto want_str = [&](std::string* dst) {
            if (parse_string_literal(val, &sv)) {
                *dst = sv;
                return true;
            }
            add(errors, full, "expected a quoted string");
            ok = false;
            return false;
        };
        const auto want_int = [&](long long lo, long long hi, long long* dst) {
            if (!parse_i64_literal(val, &iv)) {
                add(errors, full, "expected an integer");
                ok = false;
                return false;
            }
            if (iv < lo || iv > hi) {
                add(errors, full, "out of range");
                ok = false;
                return false;
            }
            *dst = iv;
            return true;
        };
        const auto want_bool = [&](bool* dst) {
            if (!parse_bool_literal(val, &bv)) {
                add(errors, full, "expected true or false");
                ok = false;
                return false;
            }
            *dst = bv;
            return true;
        };
        const auto want_enum = [&](const char* a, const char* b, int* dst) {
            if (!parse_string_literal(val, &sv)) {
                add(errors, full, "expected a quoted string");
                ok = false;
                return;
            }
            if (sv == a) *dst = 0;
            else if (sv == b) *dst = 1;
            else {
                add(errors, full, std::string("expected ") + a + " or " + b);
                ok = false;
            }
        };

        int ev = 0;
        if (full == "session.begin_string") want_str(&c.begin_string);
        else if (full == "session.sender_comp_id") want_str(&c.sender_comp_id);
        else if (full == "session.target_comp_id") want_str(&c.target_comp_id);
        else if (full == "session.qualifier") want_str(&c.qualifier);
        else if (full == "session.heartbeat_seconds") {
            if (want_int(0, 100000, &tmp)) c.heartbeat_seconds = static_cast<int>(tmp);
        } else if (full == "session.allow_reset_on_logon") want_bool(&c.allow_reset_on_logon);
        else if (full == "session.allow_reset_on_disconnect")
            want_bool(&c.allow_reset_on_disconnect);
        else if (full == "session.allow_unsolicited_seq_reset")
            want_bool(&c.allow_unsolicited_seq_reset);
        else if (full == "session.sending_time_tolerance_seconds") {
            if (want_int(0, 100000, &tmp)) c.sending_time_tolerance_seconds = static_cast<int>(tmp);
        } else if (full == "session.session_schedule") want_str(&c.session_schedule);
        else if (full == "network.host") want_str(&c.host);
        else if (full == "network.port") {
            if (want_int(0, 65535, &tmp)) c.port = static_cast<uint16_t>(tmp);
        } else if (full == "network.connect_timeout_ms") {
            if (want_int(0, 3600000, &tmp)) c.connect_timeout_ms = static_cast<int>(tmp);
        } else if (full == "network.reconnect_initial_ms") {
            if (want_int(0, 3600000, &tmp)) c.reconnect_initial_ms = static_cast<int>(tmp);
        } else if (full == "network.reconnect_max_ms") {
            if (want_int(0, 3600000, &tmp)) c.reconnect_max_ms = static_cast<int>(tmp);
        } else if (full == "network.reconnect_jitter_percent") {
            if (want_int(-1, 1000, &tmp)) c.reconnect_jitter_percent = static_cast<int>(tmp);
        } else if (full == "network.max_frame_bytes") {
            if (want_int(0, 1LL << 30, &tmp)) c.max_frame_bytes = static_cast<size_t>(tmp);
        } else if (full == "network.max_read_buffer_bytes") {
            if (want_int(0, 1LL << 30, &tmp)) c.max_read_buffer_bytes = static_cast<size_t>(tmp);
        } else if (full == "network.max_fields_per_frame") {
            if (want_int(0, 1000000, &tmp)) c.max_fields_per_frame = static_cast<int>(tmp);
        } else if (full == "network.partial_frame_deadline_ms") {
            if (want_int(0, 3600000, &tmp)) c.partial_frame_deadline_ms = static_cast<int>(tmp);
        } else if (full == "recovery.max_future_messages") {
            if (want_int(0, 1000000, &tmp)) c.max_future_messages = static_cast<size_t>(tmp);
        } else if (full == "recovery.max_future_bytes") {
            if (want_int(0, 1LL << 30, &tmp)) c.max_future_bytes = static_cast<size_t>(tmp);
        } else if (full == "recovery.resend_chunk_size") {
            if (want_int(0, 1000000, &tmp)) c.resend_chunk_size = static_cast<uint64_t>(tmp);
        } else if (full == "recovery.allow_gapfill_application_messages") {
            want_bool(&c.allow_gapfill_application_messages);
        } else if (full == "recovery.gapfill_policy") {
            want_enum("accept_and_flag", "fail_closed", &ev);
            c.gapfill_policy = ev == 0 ? GapFillPolicy::AcceptAndFlag : GapFillPolicy::FailClosed;
        } else if (full == "recovery.resend_mode") {
            want_enum("bounded_range", "open_ended", &ev);
            c.resend_mode = ev == 0 ? ResendMode::BoundedRange : ResendMode::OpenEnded;
        } else if (full == "recovery.max_resend_cycles") {
            if (want_int(0, 1000000, &tmp)) c.max_resend_cycles = static_cast<int>(tmp);
        } else if (full == "recovery.max_recovery_duration_ms") {
            if (want_int(0, 86400000, &tmp)) c.max_recovery_duration_ms = static_cast<int>(tmp);
        } else if (full == "recovery.min_resend_request_interval_ms") {
            if (want_int(-1, 3600000, &tmp)) c.min_resend_request_interval_ms = static_cast<int>(tmp);
        } else if (full == "recovery.test_request_timeout_ms") {
            if (want_int(0, 3600000, &tmp)) c.test_request_timeout_ms = static_cast<int>(tmp);
        } else if (full == "persistence.backend") want_str(&c.backend);
        else if (full == "persistence.database_path") want_str(&c.database_path);
        else if (full == "persistence.sqlite_journal_mode") want_str(&c.sqlite_journal_mode);
        else if (full == "persistence.sqlite_synchronous") want_str(&c.sqlite_synchronous);
        else if (full == "persistence.store_busy_timeout_ms") {
            if (want_int(-1, 600000, &tmp)) c.store_busy_timeout_ms = static_cast<int>(tmp);
        } else if (full == "persistence.allow_weak_durability") want_bool(&c.allow_weak_durability);
        else if (full == "persistence.create_database_if_missing")
            want_bool(&c.create_database_if_missing);
        else if (full == "limits.max_event_queue") {
            if (want_int(0, 10000000, &tmp)) c.max_event_queue = static_cast<size_t>(tmp);
        } else if (full == "limits.max_inbox_pending") {
            if (want_int(0, 10000000, &tmp)) c.max_inbox_pending = static_cast<size_t>(tmp);
        } else if (full == "security.tls_enabled") want_bool(&c.tls_enabled);
        else if (full == "security.redact_secret_fields") want_bool(&c.redact_secret_fields);
        else if (full == "security.max_audit_value_bytes") {
            if (want_int(0, 1 << 20, &tmp)) c.max_audit_value_bytes = static_cast<size_t>(tmp);
        } else if (full == "security.username") want_str(&c.username);
        else if (full == "security.password_env") {
            // The secret never lives in the config file; only the name of the
            // environment variable that holds it does (design doc 11.2).
            if (want_str(&sv)) {
                const char* v = std::getenv(sv.c_str());
                if (v == nullptr) {
                    add(errors, full, "environment variable is not set: " + sv);
                    ok = false;
                } else {
                    c.password = v;
                }
            }
        } else {
            // An unknown key is an error, not a warning: a typo in a
            // safety-relevant setting must never be silently ignored.
            add(errors, full, "unknown configuration key");
            ok = false;
        }
    }

    auto sem = validate(c);
    for (auto& v : sem) errors->push_back(v);
    if (!sem.empty()) ok = false;

    if (ok) *out = c;
    return ok;
}

}  // namespace fixse
