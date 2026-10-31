// Structured audit log with secret redaction.
//
// Design doc 14.2 lists the fields every audit record carries, and 11.2 forbids
// writing Logon credentials to logs, traces or crash reports. Redaction happens
// here, at the single place that renders values, so a new call site cannot
// bypass it (scenarios C-16, J-03, J-09).
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace fixse {

struct AuditField {
    std::string key;
    std::string value;
};

class AuditLog {
public:
    // Tags whose values are replaced by a fixed marker. Defaults to Username
    // and Password; a deployment adds its own custom secret tags.
    void set_secret_tags(std::vector<int> tags) { secret_tags_ = std::move(tags); }
    void set_max_value_bytes(size_t n) { max_value_bytes_ = n; }
    void set_redaction_enabled(bool on) { redact_ = on; }
    // Values that must never appear in output even when they arrive through an
    // unexpected field, for example a password read from the environment.
    void add_secret_value(std::string v) {
        if (!v.empty()) secret_values_.push_back(std::move(v));
    }

    void event(std::string_view type, std::vector<AuditField> fields);

    // Render a FIX frame for an audit record: SOH becomes a vertical bar,
    // control bytes are escaped, secret tag values are replaced and the result
    // is truncated to the configured cap.
    std::string sanitize_frame(std::string_view raw) const;
    // Redact and bound a single free-text value, for example tag 58.
    std::string sanitize_value(std::string_view v) const;

    const std::vector<std::string>& lines() const { return lines_; }
    void clear() { lines_.clear(); }
    size_t size() const { return lines_.size(); }
    // True when any emitted line contains the given text. Used by tests that
    // assert a secret never reached the log.
    bool contains(std::string_view needle) const;

private:
    std::vector<std::string> lines_;
    std::vector<int> secret_tags_{553, 554};
    std::vector<std::string> secret_values_;
    size_t max_value_bytes_ = 256;
    bool redact_ = true;
};

}  // namespace fixse
