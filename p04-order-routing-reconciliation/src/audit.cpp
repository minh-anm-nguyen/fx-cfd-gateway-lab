#include "router/audit.hpp"

#include <algorithm>

namespace router {

std::string_view to_string(AuditKind k) {
    switch (k) {
        case AuditKind::CommandAdmitted: return "CommandAdmitted";
        case AuditKind::CommandRejected: return "CommandRejected";
        case AuditKind::CommandDuplicate: return "CommandDuplicate";
        case AuditKind::RouteDecided: return "RouteDecided";
        case AuditKind::OriginalSendAssigned: return "OriginalSendAssigned";
        case AuditKind::SendAmbiguous: return "SendAmbiguous";
        case AuditKind::InboundApplied: return "InboundApplied";
        case AuditKind::InboundDeduplicated: return "InboundDeduplicated";
        case AuditKind::InboundQuarantined: return "InboundQuarantined";
        case AuditKind::HoldOpened: return "HoldOpened";
        case AuditKind::HoldResolved: return "HoldResolved";
        case AuditKind::ReconOpened: return "ReconOpened";
        case AuditKind::ReconProbeSent: return "ReconProbeSent";
        case AuditKind::ReconProbeResult: return "ReconProbeResult";
        case AuditKind::ReconResolved: return "ReconResolved";
        case AuditKind::OperatorAction: return "OperatorAction";
        case AuditKind::OperatorDenied: return "OperatorDenied";
        case AuditKind::ConfigChanged: return "ConfigChanged";
        case AuditKind::IntegrityFailure: return "IntegrityFailure";
        case AuditKind::StateHalted: return "StateHalted";
        case AuditKind::StateResumed: return "StateResumed";
    }
    return "Unknown";
}

Hash AuditLog::compute(const AuditRecord& rec) {
    Sha256 s;
    s.update(rec.seq);
    s.field(to_string(rec.kind));
    s.update(static_cast<std::uint64_t>(rec.wall));
    s.update(rec.mono);
    s.field(rec.actor);
    s.field(rec.subject);
    s.field(rec.reason);
    s.field(rec.detail);
    s.update(std::string_view(reinterpret_cast<const char*>(rec.evidence.bytes.data()), 32));
    s.update(std::string_view(reinterpret_cast<const char*>(rec.prev_hash.bytes.data()), 32));
    return s.finish();
}

std::optional<AuditRecord> AuditLog::append(AuditRecord rec) {
    if (!available_) return std::nullopt;
    rec.seq = records_.size() + 1;
    rec.prev_hash = records_.empty() ? Hash{} : records_.back().self_hash;
    rec.detail = sanitize_text(rec.detail, 400);
    rec.self_hash = compute(rec);
    records_.push_back(rec);
    return records_.back();
}

bool AuditLog::verify() const { return verify_from(0); }

bool AuditLog::verify_from(std::size_t from) const {
    if (from > records_.size()) return false;
    Hash prev{};
    std::uint64_t expected_seq = 1;
    if (from > 0) {
        prev = records_[from - 1].self_hash;
        expected_seq = records_[from - 1].seq + 1;
    }
    for (std::size_t i = from; i < records_.size(); ++i) {
        const AuditRecord& r = records_[i];
        if (r.seq != expected_seq) return false;
        if (r.prev_hash != prev) return false;
        if (r.self_hash != compute(r)) return false;
        prev = r.self_hash;
        ++expected_seq;
    }
    return true;
}

std::size_t AuditLog::count(AuditKind k) const {
    return static_cast<std::size_t>(
        std::count_if(records_.begin(), records_.end(),
                      [k](const AuditRecord& r) { return r.kind == k; }));
}

std::vector<AuditRecord> AuditLog::with_subject(const std::string& subject) const {
    std::vector<AuditRecord> out;
    for (const AuditRecord& r : records_)
        if (r.subject == subject) out.push_back(r);
    return out;
}

bool AuditLog::tamper_for_test(std::size_t index, const std::string& new_detail) {
    if (index >= records_.size()) return false;
    records_[index].detail = new_detail;  // self_hash intentionally left stale
    return true;
}

// ---------------------------------------------------------------------------

std::string sanitize_text(std::string_view raw, std::size_t max_len) {
    std::string out;
    out.reserve(std::min(raw.size(), max_len));
    for (char c : raw) {
        if (out.size() >= max_len) {
            out += "...";
            break;
        }
        const unsigned char u = static_cast<unsigned char>(c);
        if (c == '\n' || c == '\r') {
            // A newline is what makes log injection work, so it never survives.
            out += "\\n";
        } else if (c == '\t') {
            out += "\\t";
        } else if (c == 1) {
            out += "|";  // SOH, rendered rather than emitted
        } else if (u < 0x20 || u > 0x7e) {
            out += "?";
        } else if (c == '"' || c == '\\') {
            out.push_back('\\');
            out.push_back(c);
        } else {
            out.push_back(c);
        }
    }
    return out;
}

namespace {

// FIX tags whose values are credentials or password-like material.
bool is_secret_tag(int tag) {
    return tag == 554 || tag == 925 || tag == 90 || tag == 91;
}

}  // namespace

std::string redact_fix_frame(std::string_view frame) {
    std::string out;
    out.reserve(frame.size());
    std::size_t i = 0;
    while (i < frame.size()) {
        const std::size_t soh = frame.find('\x01', i);
        const std::size_t end = soh == std::string_view::npos ? frame.size() : soh;
        std::string_view field = frame.substr(i, end - i);
        const std::size_t eq = field.find('=');
        if (eq != std::string_view::npos) {
            int tag = 0;
            bool numeric = eq > 0;
            for (std::size_t k = 0; k < eq; ++k) {
                const char c = field[k];
                if (c < '0' || c > '9') {
                    numeric = false;
                    break;
                }
                tag = tag * 10 + (c - '0');
            }
            if (numeric && is_secret_tag(tag)) {
                out.append(field.substr(0, eq + 1));
                out.append("***");
            } else {
                out.append(field);
            }
        } else {
            out.append(field);
        }
        if (soh == std::string_view::npos) break;
        out.push_back('\x01');
        i = soh + 1;
    }
    return out;
}

std::string wire_text(std::string_view frame) {
    std::string out;
    out.reserve(frame.size());
    for (char c : frame) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (c == 1)
            out.push_back('|');
        else if (u < 0x20 || u > 0x7e)
            out += "<?>";
        else
            out.push_back(c);
    }
    return out;
}

bool may_submit_orders(const Principal& p) {
    return p.role == Role::Trader || p.role == Role::Operator || p.role == Role::Admin;
}
bool may_force_resolve(const Principal& p) {
    return p.role == Role::Operator || p.role == Role::Admin;
}
bool may_change_config(const Principal& p) { return p.role == Role::Admin; }
bool may_view_raw_frames(const Principal& p) {
    return p.role == Role::Operator || p.role == Role::Admin;
}

}  // namespace router
