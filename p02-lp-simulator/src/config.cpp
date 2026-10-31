#include "lp/config.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>

namespace lpsim {

const ConfNode* ConfNode::find(std::string_view key) const {
    for (const auto& kv : map) {
        if (kv.first == key) return &kv.second;
    }
    return nullptr;
}

std::string_view to_string(SafetyMode m) {
    switch (m) {
        case SafetyMode::Conformant: return "conformant";
        case SafetyMode::Adversarial: return "adversarial";
    }
    return "?";
}

// ---------------------------------------------------------------------------
// Parser
// ---------------------------------------------------------------------------

namespace {

struct Line {
    int number = 0;
    int indent = 0;
    std::string text;  // trimmed of indentation and trailing whitespace
};

std::string trim(std::string_view s) {
    std::size_t b = 0;
    while (b < s.size() && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r')) ++b;
    std::size_t e = s.size();
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r')) --e;
    return std::string(s.substr(b, e - b));
}

// Strip a trailing comment that is not inside quotes.
std::string strip_comment(std::string_view s) {
    bool in_single = false;
    bool in_double = false;
    for (std::size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        if (c == '\'' && !in_double) in_single = !in_single;
        else if (c == '"' && !in_single) in_double = !in_double;
        else if (c == '#' && !in_single && !in_double) return std::string(s.substr(0, i));
    }
    return std::string(s);
}

std::string unquote(std::string_view s) {
    if (s.size() >= 2 && ((s.front() == '\'' && s.back() == '\'') ||
                          (s.front() == '"' && s.back() == '"')))
        return std::string(s.substr(1, s.size() - 2));
    return std::string(s);
}

// Split "a: 1, b: 2" at top level, honouring quotes and nested braces.
std::vector<std::string> split_top(std::string_view s, char sep) {
    std::vector<std::string> out;
    int depth = 0;
    bool in_single = false;
    bool in_double = false;
    std::size_t start = 0;
    for (std::size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        if (c == '\'' && !in_double) in_single = !in_single;
        else if (c == '"' && !in_single) in_double = !in_single;
        else if (!in_single && !in_double) {
            if (c == '{' || c == '[') ++depth;
            else if (c == '}' || c == ']') --depth;
            else if (c == sep && depth == 0) {
                out.push_back(std::string(s.substr(start, i - start)));
                start = i + 1;
            }
        }
    }
    out.push_back(std::string(s.substr(start)));
    return out;
}

struct Parser {
    const std::vector<Line>* lines = nullptr;
    std::size_t pos = 0;
    const ConfLimits* limits = nullptr;
    std::size_t nodes = 0;
    ConfError* err = nullptr;

    bool fail(int line, std::string msg) {
        err->ok = false;
        err->line = line;
        err->message = std::move(msg);
        return false;
    }

    bool count_node(int line) {
        if (++nodes > limits->max_nodes) return fail(line, "node budget exceeded");
        return true;
    }

    // Inline flow value: a scalar, {a: 1, b: 2} or [a, b, c].
    bool parse_flow(const std::string& raw, int line, int depth, ConfNode* out) {
        if (depth > limits->max_depth) return fail(line, "nesting too deep");
        const std::string v = trim(raw);
        if (!count_node(line)) return false;

        if (v.size() >= 2 && v.front() == '{' && v.back() == '}') {
            out->kind = ConfNode::Kind::Map;
            const std::string inner = trim(v.substr(1, v.size() - 2));
            if (inner.empty()) return true;
            for (const std::string& part : split_top(inner, ',')) {
                const std::string p = trim(part);
                if (p.empty()) continue;
                const std::size_t colon = p.find(':');
                if (colon == std::string::npos) return fail(line, "expected key: value");
                const std::string key = unquote(trim(p.substr(0, colon)));
                if (key.empty()) return fail(line, "empty key");
                if (out->find(key) != nullptr)
                    return fail(line, "duplicate key " + key);
                ConfNode child;
                if (!parse_flow(p.substr(colon + 1), line, depth + 1, &child)) return false;
                out->map.emplace_back(key, std::move(child));
            }
            return true;
        }
        if (v.size() >= 2 && v.front() == '[' && v.back() == ']') {
            out->kind = ConfNode::Kind::Seq;
            const std::string inner = trim(v.substr(1, v.size() - 2));
            if (inner.empty()) return true;
            for (const std::string& part : split_top(inner, ',')) {
                const std::string p = trim(part);
                if (p.empty()) continue;
                ConfNode child;
                if (!parse_flow(p, line, depth + 1, &child)) return false;
                out->seq.push_back(std::move(child));
            }
            return true;
        }
        out->kind = ConfNode::Kind::Scalar;
        out->scalar = unquote(v);
        return true;
    }

    // Block at a given indent: either a mapping or a sequence of "- " items.
    bool parse_block(int indent, int depth, ConfNode* out) {
        if (depth > limits->max_depth) {
            const int ln = pos < lines->size() ? (*lines)[pos].number : 0;
            return fail(ln, "nesting too deep");
        }
        bool decided = false;
        while (pos < lines->size()) {
            const Line& ln = (*lines)[pos];
            if (ln.indent < indent) break;
            if (ln.indent > indent) return fail(ln.number, "unexpected indentation");

            if (ln.text.rfind("- ", 0) == 0 || ln.text == "-") {
                if (decided && out->kind != ConfNode::Kind::Seq)
                    return fail(ln.number, "sequence item inside a mapping");
                out->kind = ConfNode::Kind::Seq;
                decided = true;
                if (!count_node(ln.number)) return false;
                const std::string item = trim(ln.text.size() > 1 ? ln.text.substr(2) : "");
                ++pos;
                ConfNode child;
                if (item.empty()) {
                    if (!parse_block(indent + 2, depth + 1, &child)) return false;
                } else {
                    if (!parse_flow(item, ln.number, depth + 1, &child)) return false;
                }
                out->seq.push_back(std::move(child));
                continue;
            }

            if (decided && out->kind != ConfNode::Kind::Map)
                return fail(ln.number, "mapping key inside a sequence");
            out->kind = ConfNode::Kind::Map;
            decided = true;

            const std::size_t colon = ln.text.find(':');
            if (colon == std::string::npos) return fail(ln.number, "expected key: value");
            const std::string key = unquote(trim(ln.text.substr(0, colon)));
            if (key.empty()) return fail(ln.number, "empty key");
            if (out->find(key) != nullptr) return fail(ln.number, "duplicate key " + key);
            const std::string rest = trim(ln.text.substr(colon + 1));
            const int line_no = ln.number;
            ++pos;

            ConfNode child;
            if (!count_node(line_no)) return false;
            if (rest.empty()) {
                // A nested block, or an explicitly empty value.
                if (pos < lines->size() && (*lines)[pos].indent > indent) {
                    if (!parse_block((*lines)[pos].indent, depth + 1, &child)) return false;
                } else {
                    child.kind = ConfNode::Kind::Scalar;
                }
            } else {
                if (!parse_flow(rest, line_no, depth + 1, &child)) return false;
            }
            out->map.emplace_back(key, std::move(child));
        }
        if (!decided) out->kind = ConfNode::Kind::Map;
        return true;
    }
};

}  // namespace

bool parse_conf(std::string_view text, const ConfLimits& limits, ConfNode* out,
                ConfError* err) {
    *out = ConfNode{};
    err->ok = true;
    err->line = 0;
    err->message.clear();

    if (text.size() > limits.max_file_bytes) {
        err->ok = false;
        err->message = "document exceeds the size bound";
        return false;
    }

    std::vector<Line> lines;
    int number = 0;
    std::size_t start = 0;
    while (start <= text.size()) {
        const std::size_t nl = text.find('\n', start);
        const std::string_view raw =
            text.substr(start, nl == std::string_view::npos ? std::string_view::npos
                                                            : nl - start);
        ++number;
        if (raw.size() > limits.max_line_bytes) {
            err->ok = false;
            err->line = number;
            err->message = "line exceeds the length bound";
            return false;
        }
        const std::string stripped = strip_comment(raw);
        const std::string body = trim(stripped);
        if (!body.empty()) {
            int indent = 0;
            while (indent < static_cast<int>(stripped.size()) && stripped[indent] == ' ')
                ++indent;
            if (stripped.find('\t') != std::string::npos) {
                err->ok = false;
                err->line = number;
                err->message = "tabs are not accepted as indentation";
                return false;
            }
            lines.push_back(Line{number, indent, body});
        }
        if (nl == std::string_view::npos) break;
        start = nl + 1;
    }

    if (lines.empty()) {
        out->kind = ConfNode::Kind::Map;
        return true;
    }

    Parser p;
    p.lines = &lines;
    p.limits = &limits;
    p.err = err;
    if (!p.parse_block(lines.front().indent, 0, out)) {
        *out = ConfNode{};
        return false;
    }
    if (p.pos != lines.size()) {
        err->ok = false;
        err->line = lines[p.pos].number;
        err->message = "unexpected content after the document";
        *out = ConfNode{};
        return false;
    }
    return true;
}

bool read_conf_file(const std::string& path, const ConfLimits& limits, std::string* out,
                    ConfError* err) {
    err->ok = true;
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    if (ec) {
        err->ok = false;
        err->message = "cannot stat " + path;
        return false;
    }
    // Bound checked BEFORE reading, so an oversized file is never loaded.
    if (size > limits.max_file_bytes) {
        err->ok = false;
        err->message = "file exceeds the size bound";
        return false;
    }
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        err->ok = false;
        err->message = "cannot open " + path;
        return false;
    }
    out->assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return true;
}

// ---------------------------------------------------------------------------
// Validation helpers
// ---------------------------------------------------------------------------

bool is_loopback(std::string_view host) {
    if (host == "localhost" || host == "::1") return true;
    // 127.0.0.0/8
    if (host.rfind("127.", 0) == 0) return true;
    return false;
}

bool safe_store_path(std::string_view path, std::string* why) {
    if (path.empty()) {
        *why = "store path is empty";
        return false;
    }
    if (path.find("..") != std::string_view::npos) {
        *why = "store path contains a parent-directory reference";
        return false;
    }
    // Windows device names and POSIX device nodes are both refused: a database
    // pointed at a device is not something to discover at first commit.
    static const char* kDevices[] = {"/dev/", "CON", "NUL", "PRN", "AUX", "COM1", "LPT1"};
    for (const char* d : kDevices) {
        if (path.find(d) != std::string_view::npos) {
            *why = "store path names a device";
            return false;
        }
    }
    return true;
}

namespace {

bool scalar_of(const ConfNode& parent, std::string_view key, std::string* out) {
    const ConfNode* n = parent.find(key);
    if (n == nullptr || !n->is_scalar() || n->scalar.empty()) return false;
    *out = n->scalar;
    return true;
}

bool int_of(const ConfNode& parent, std::string_view key, long long* out) {
    std::string s;
    if (!scalar_of(parent, key, &s)) return false;
    char* end = nullptr;
    const long long v = std::strtoll(s.c_str(), &end, 10);
    if (end == nullptr || *end != '\0') return false;
    *out = v;
    return true;
}

bool bool_of(const ConfNode& parent, std::string_view key, bool* out) {
    std::string s;
    if (!scalar_of(parent, key, &s)) return false;
    if (s == "true" || s == "yes" || s == "1") {
        *out = true;
        return true;
    }
    if (s == "false" || s == "no" || s == "0") {
        *out = false;
        return true;
    }
    return false;
}

// Reject any key the loader does not understand, so a typo in a limit is loud.
bool only_known_keys(const ConfNode& n, const std::vector<std::string>& known,
                     const std::string& where, ConfError* err) {
    for (const auto& kv : n.map) {
        if (std::find(known.begin(), known.end(), kv.first) == known.end()) {
            err->ok = false;
            err->message = "unknown key '" + kv.first + "' in " + where;
            return false;
        }
    }
    return true;
}

}  // namespace

bool build_simulator_config(const ConfNode& doc, SimulatorConfig* out, ConfError* err) {
    err->ok = true;
    *out = SimulatorConfig{};

    const ConfNode* sim = doc.find("simulator");
    if (sim == nullptr || !sim->is_map()) {
        err->ok = false;
        err->message = "missing 'simulator' section";
        return false;
    }

    static const std::vector<std::string> kSimKeys = {
        "mode", "instance_id", "listen", "fix_version", "sender_comp_id",
        "target_comp_id", "heartbeat_seconds", "fill_trigger_policy",
        "session_reset_policy", "business_day_timezone", "max_outstanding_orders",
        "allow_unsafe_wire_faults", "store", "symbols", "fault_limits",
        "emit_pending_cancel", "emit_pending_replace", "require_credentials",
        "username", "password", "max_pending_outbox", "max_inbox"};
    if (!only_known_keys(*sim, kSimKeys, "simulator", err)) return false;

    std::string s;
    if (scalar_of(*sim, "mode", &s)) {
        if (s == "conformant") out->mode = SafetyMode::Conformant;
        else if (s == "adversarial") out->mode = SafetyMode::Adversarial;
        else {
            err->ok = false;
            err->message = "mode must be conformant or adversarial";
            return false;
        }
    }
    if (scalar_of(*sim, "instance_id", &s)) out->instance_id = s;

    if (scalar_of(*sim, "listen", &s)) {
        const std::size_t colon = s.rfind(':');
        if (colon == std::string::npos) {
            err->ok = false;
            err->message = "listen must be host:port";
            return false;
        }
        out->listen_host = s.substr(0, colon);
        out->listen_port = std::atoi(s.c_str() + colon + 1);
    }

    if (scalar_of(*sim, "fix_version", &s) && s != std::string(kBeginStringFix44)) {
        err->ok = false;
        err->message = "only FIX.4.4 is implemented";
        return false;
    }

    out->session.session_id = out->instance_id;
    out->actor.session_id = out->instance_id;
    out->store.session_id = out->instance_id;

    if (scalar_of(*sim, "sender_comp_id", &s)) {
        out->session.sender_comp_id = s;
        out->store.sender_comp_id = s;
    }
    if (scalar_of(*sim, "target_comp_id", &s)) {
        out->session.target_comp_id = s;
        out->store.target_comp_id = s;
        out->actor.firm = s;
    }

    long long n = 0;
    if (int_of(*sim, "heartbeat_seconds", &n)) out->session.heartbeat_seconds = static_cast<int>(n);
    if (int_of(*sim, "max_outstanding_orders", &n)) {
        out->actor.max_live_orders = static_cast<std::size_t>(n);
        out->store.max_live_orders = static_cast<std::size_t>(n);
    }
    if (int_of(*sim, "max_pending_outbox", &n)) {
        out->actor.max_pending_outbox = static_cast<std::size_t>(n);
        out->store.max_pending_outbox = static_cast<std::size_t>(n);
    }
    if (int_of(*sim, "max_inbox", &n)) out->store.max_inbox = static_cast<std::size_t>(n);

    if (scalar_of(*sim, "fill_trigger_policy", &s)) {
        if (s == "scripted") out->actor.fill_trigger = FillTrigger::Scripted;
        else if (s == "auto_on_new_or_quote")
            out->actor.fill_trigger = FillTrigger::AutoOnNewOrQuote;
        else {
            err->ok = false;
            err->message = "fill_trigger_policy must be scripted or auto_on_new_or_quote";
            return false;
        }
    }
    if (scalar_of(*sim, "session_reset_policy", &s)) {
        if (s == "coordinated_only") out->session.allow_reset_on_logon = false;
        else if (s == "peer_may_reset") out->session.allow_reset_on_logon = true;
        else {
            err->ok = false;
            err->message = "session_reset_policy must be coordinated_only or peer_may_reset";
            return false;
        }
    }
    bool b = false;
    if (bool_of(*sim, "allow_unsafe_wire_faults", &b)) out->allow_unsafe_wire_faults = b;
    if (bool_of(*sim, "emit_pending_cancel", &b)) out->actor.emit_pending_cancel = b;
    if (bool_of(*sim, "emit_pending_replace", &b)) out->actor.emit_pending_replace = b;
    if (bool_of(*sim, "require_credentials", &b)) out->session.require_credentials = b;
    if (scalar_of(*sim, "username", &s)) out->session.username = s;
    if (scalar_of(*sim, "password", &s)) out->session.password = s;

    // --- store -------------------------------------------------------------
    if (const ConfNode* st = sim->find("store")) {
        static const std::vector<std::string> kStoreKeys = {"driver", "path", "wal",
                                                            "synchronous"};
        if (!only_known_keys(*st, kStoreKeys, "simulator.store", err)) return false;
        if (scalar_of(*st, "driver", &s) && s != "sqlite") {
            err->ok = false;
            err->message = "only the sqlite store driver is implemented";
            return false;
        }
        if (scalar_of(*st, "path", &s)) out->store.path = s;
        if (bool_of(*st, "wal", &b)) out->store.wal = b;
        if (scalar_of(*st, "synchronous", &s)) out->store.synchronous_full = (s == "FULL");
    }

    // --- symbols -----------------------------------------------------------
    if (const ConfNode* syms = sim->find("symbols")) {
        if (!syms->is_map()) {
            err->ok = false;
            err->message = "symbols must be a mapping of symbol to profile";
            return false;
        }
        for (const auto& kv : syms->map) {
            static const std::vector<std::string> kSymKeys = {
                "tick_size", "lot_step_base_units", "allowed_order_types", "allowed_tif",
                "quote_stale_after_ms", "enabled", "price_scale"};
            if (!only_known_keys(kv.second, kSymKeys, "symbols." + kv.first, err))
                return false;

            SymbolProfile p;
            p.symbol = kv.first;
            long long v = 0;
            if (int_of(kv.second, "price_scale", &v)) p.price.scale = static_cast<int>(v);
            if (scalar_of(kv.second, "tick_size", &s)) {
                // tick_size is written as a decimal, for example '0.00001'. The
                // scale is its number of fractional digits and the step is the
                // integer it represents at that scale.
                const std::size_t dot = s.find('.');
                const int scale =
                    dot == std::string::npos ? 0 : static_cast<int>(s.size() - dot - 1);
                p.price.scale = scale;
                PriceFormat tmp{scale, 1};
                Ticks step = 0;
                if (parse_price_ticks(s, tmp, &step) != NumParse::Ok || step <= 0) {
                    err->ok = false;
                    err->message = "tick_size for " + kv.first + " is not a positive decimal";
                    return false;
                }
                p.price.tick_step = step;
            }
            if (int_of(kv.second, "lot_step_base_units", &v)) p.lot_step = v;
            if (int_of(kv.second, "quote_stale_after_ms", &v)) p.quote_stale_after_ms = v;
            if (bool_of(kv.second, "enabled", &b)) p.enabled = b;

            if (const ConfNode* types = kv.second.find("allowed_order_types")) {
                p.allowed_ord_types.clear();
                for (const ConfNode& t : types->seq) {
                    if (t.scalar == "LIMIT") p.allowed_ord_types.push_back(OrdType::Limit);
                    else {
                        // IOC, FOK and MARKET are phase-2 items. Accepting them
                        // in configuration without implementing them would be a
                        // false claim of support (M-19).
                        err->ok = false;
                        err->message = "order type " + t.scalar + " is not implemented";
                        return false;
                    }
                }
            }
            if (const ConfNode* tifs = kv.second.find("allowed_tif")) {
                p.allowed_tifs.clear();
                for (const ConfNode& t : tifs->seq) {
                    if (t.scalar == "DAY") p.allowed_tifs.push_back(Tif::Day);
                    else {
                        err->ok = false;
                        err->message = "time in force " + t.scalar + " is not implemented";
                        return false;
                    }
                }
            }
            out->actor.symbols.push_back(std::move(p));
        }
    }

    // --- fault limits ------------------------------------------------------
    if (const ConfNode* fl = sim->find("fault_limits")) {
        static const std::vector<std::string> kFaultKeys = {"max_scheduled_events",
                                                            "max_repeats"};
        if (!only_known_keys(*fl, kFaultKeys, "simulator.fault_limits", err)) return false;
        long long v = 0;
        if (int_of(*fl, "max_scheduled_events", &v))
            out->max_scheduled_events = static_cast<std::size_t>(v);
        if (int_of(*fl, "max_repeats", &v)) out->max_fault_repeats = static_cast<int>(v);
    }

    return validate_simulator_config(*out, err);
}

bool validate_simulator_config(const SimulatorConfig& cfg, ConfError* err) {
    err->ok = true;
    auto bad = [&](std::string msg) {
        err->ok = false;
        err->message = std::move(msg);
        return false;
    };

    if (cfg.session.sender_comp_id.empty()) return bad("sender_comp_id is blank");
    if (cfg.session.target_comp_id.empty()) return bad("target_comp_id is blank");
    if (cfg.session.sender_comp_id == cfg.session.target_comp_id)
        return bad("sender_comp_id and target_comp_id are identical");
    if (cfg.session.heartbeat_seconds <= 0) return bad("heartbeat_seconds must be positive");
    if (cfg.listen_port <= 0 || cfg.listen_port > 65535) return bad("listen port is invalid");
    if (cfg.actor.symbols.empty()) return bad("no symbols are configured");

    for (const SymbolProfile& p : cfg.actor.symbols) {
        if (p.price.scale < 0 || p.price.scale > 9)
            return bad("price scale for " + p.symbol + " is out of range");
        if (p.price.tick_step <= 0) return bad("tick step for " + p.symbol + " is not positive");
        if (p.lot_step <= 0) return bad("lot step for " + p.symbol + " is not positive");
        if (p.quote_stale_after_ms <= 0)
            return bad("quote_stale_after_ms for " + p.symbol + " is not positive");
        if (p.allowed_ord_types.empty())
            return bad("no order types are allowed for " + p.symbol);
        if (p.allowed_tifs.empty()) return bad("no time in force is allowed for " + p.symbol);
    }

    if (cfg.actor.max_live_orders == 0) return bad("max_outstanding_orders must be positive");
    if (cfg.actor.max_pending_outbox == 0) return bad("max_pending_outbox must be positive");
    if (cfg.store.max_inbox == 0) return bad("max_inbox must be positive");
    if (cfg.max_scheduled_events == 0) return bad("max_scheduled_events must be positive");
    if (cfg.max_fault_repeats < 1) return bad("max_repeats must be at least 1");

    std::string why;
    if (!safe_store_path(cfg.store.path, &why)) return bad(why);

    if (cfg.session.require_credentials && cfg.session.password.empty())
        return bad("require_credentials is set but no password is configured");

    // L-05 and L-06: the unsafe capabilities are loopback-only, and asking for
    // them on a public bind fails startup rather than producing a warning.
    if (cfg.allow_unsafe_wire_faults && !is_loopback(cfg.listen_host))
        return bad("allow_unsafe_wire_faults requires a loopback bind");
    if (cfg.mode == SafetyMode::Adversarial && !is_loopback(cfg.listen_host))
        return bad("adversarial mode requires a loopback bind");

    return true;
}

}  // namespace lpsim
