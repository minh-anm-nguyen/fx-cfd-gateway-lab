#include "fix/audit.hpp"

#include <algorithm>

#include "fix/wire.hpp"

namespace fixse {
namespace {
constexpr std::string_view kRedacted = "[REDACTED]";
}

void AuditLog::event(std::string_view type, std::vector<AuditField> fields) {
    std::string line;
    line.reserve(64 + fields.size() * 24);
    line += "event=";
    line += type;
    for (const AuditField& f : fields) {
        line += ' ';
        line += f.key;
        line += '=';
        line += sanitize_value(f.value);
    }
    lines_.push_back(std::move(line));
}

std::string AuditLog::sanitize_value(std::string_view v) const {
    std::string out;
    out.reserve(std::min(v.size(), max_value_bytes_) + 8);
    for (char c : v) {
        const unsigned char u = static_cast<unsigned char>(c);
        // No raw control byte ever reaches a log line: a peer could otherwise
        // inject newlines and forge audit records (J-09).
        if (u < 0x20 || u > 0x7E) out += '.';
        else out += c;
        if (out.size() >= max_value_bytes_) {
            out += "...";
            break;
        }
    }
    if (redact_) {
        for (const std::string& secret : secret_values_) {
            if (secret.empty()) continue;
            size_t pos = 0;
            while ((pos = out.find(secret, pos)) != std::string::npos) {
                out.replace(pos, secret.size(), kRedacted);
                pos += kRedacted.size();
            }
        }
    }
    return out;
}

std::string AuditLog::sanitize_frame(std::string_view raw) const {
    std::string out;
    out.reserve(std::min(raw.size(), max_value_bytes_) + 16);

    size_t pos = 0;
    while (pos < raw.size()) {
        const size_t eq = raw.find('=', pos);
        if (eq == std::string_view::npos) break;
        size_t soh = raw.find(kSOH, eq + 1);
        const bool last = soh == std::string_view::npos;
        if (last) soh = raw.size();

        const std::string_view tag_text = raw.substr(pos, eq - pos);
        const std::string_view value = raw.substr(eq + 1, soh - eq - 1);

        int tag_num = 0;
        bool numeric = !tag_text.empty() && tag_text.size() <= 9;
        for (char c : tag_text) {
            if (c < '0' || c > '9') {
                numeric = false;
                break;
            }
            tag_num = tag_num * 10 + (c - '0');
        }

        out += tag_text;
        out += '=';
        const bool secret =
            redact_ && numeric &&
            std::find(secret_tags_.begin(), secret_tags_.end(), tag_num) != secret_tags_.end();
        if (secret) {
            out += kRedacted;
        } else {
            for (char c : value) {
                const unsigned char u = static_cast<unsigned char>(c);
                out += (u < 0x20 || u > 0x7E) ? '.' : c;
            }
        }
        out += '|';
        pos = soh + 1;
        if (out.size() >= max_value_bytes_) {
            out += "...";
            break;
        }
    }

    if (redact_) {
        for (const std::string& s : secret_values_) {
            if (s.empty()) continue;
            size_t p = 0;
            while ((p = out.find(s, p)) != std::string::npos) {
                out.replace(p, s.size(), kRedacted);
                p += kRedacted.size();
            }
        }
    }
    return out;
}

bool AuditLog::contains(std::string_view needle) const {
    for (const std::string& l : lines_)
        if (l.find(needle) != std::string::npos) return true;
    return false;
}

}  // namespace fixse
