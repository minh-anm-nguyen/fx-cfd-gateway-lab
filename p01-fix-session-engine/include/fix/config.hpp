// Session configuration and startup validation.
// Mirrors design doc section 13. Every field has an explicit value; the engine
// refuses to start on a blank or unsafe setting rather than silently choosing
// an unsafe default (design doc 13, "Config validation at startup").
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace fixse {

// Policy for an INBOUND SequenceReset-GapFill that skips sequence numbers.
//
// The session layer cannot prove what was skipped: a GapFill carries no
// evidence about the message types it replaces. The design doc says so
// explicitly (playbook PB-07) and leaves the decision to the bilateral
// agreement, so it is a configuration choice rather than a hardcoded rule.
enum class GapFillPolicy {
    // Protocol-correct default. The skipped range is applied so recovery can
    // finish, and every skipped sequence is counted and audited as
    // UNRECOVERED. The engine never claims those messages were delivered.
    AcceptAndFlag,
    // For an agreement that forbids any skipping: the engine faults and waits
    // for manual reconciliation instead of advancing (PB-07, F-20).
    FailClosed,
};

enum class ResendMode {
    BoundedRange,  // ask for [begin, end] in chunks
    OpenEnded,     // ask for EndSeqNo=0 when the peer agreement prefers it
};

struct SessionConfig {
    // [session]
    std::string begin_string = "FIX.4.4";
    std::string sender_comp_id;
    std::string target_comp_id;
    std::string qualifier = "DEV";
    int heartbeat_seconds = 30;
    bool allow_reset_on_logon = false;
    bool allow_reset_on_disconnect = false;
    bool allow_unsolicited_seq_reset = false;
    int sending_time_tolerance_seconds = 120;
    std::string session_schedule = "24x7-lab";  // not a bilateral production contract

    // [network]
    std::string host = "127.0.0.1";
    uint16_t port = 9878;
    int connect_timeout_ms = 3000;
    int reconnect_initial_ms = 1000;
    int reconnect_max_ms = 60000;
    int reconnect_jitter_percent = 20;
    size_t max_frame_bytes = 65536;
    size_t max_read_buffer_bytes = 131072;
    int max_fields_per_frame = 2048;
    // A frame that starts but does not complete within this window is a
    // slowloris attempt (J-05).
    int partial_frame_deadline_ms = 30000;

    // [recovery]
    size_t max_future_messages = 256;
    size_t max_future_bytes = 1048576;
    uint64_t resend_chunk_size = 1000;
    // Gap-filling our OWN application messages is not supported in v1: the
    // resend planner always retransmits them and fails closed when one is
    // missing (scenario F-16). The flag exists so a configuration asking for
    // the opposite is rejected loudly rather than ignored.
    bool allow_gapfill_application_messages = false;
    GapFillPolicy gapfill_policy = GapFillPolicy::AcceptAndFlag;
    ResendMode resend_mode = ResendMode::BoundedRange;
    // Bounds that stop a resend storm (PB-11, F-24, J-06).
    int max_resend_cycles = 16;
    int max_recovery_duration_ms = 120000;
    int min_resend_request_interval_ms = 250;
    // Deadline for a pending TestRequest; the effective value is
    // max(heartbeat_seconds, test_request_timeout_ms) per design doc 10.
    int test_request_timeout_ms = 30000;

    // [persistence]
    std::string backend = "sqlite";
    std::string database_path = "./state/session.db";
    std::string sqlite_journal_mode = "WAL";
    std::string sqlite_synchronous = "FULL";
    int store_busy_timeout_ms = 5000;
    // Relaxing synchronous below FULL must be a deliberate, recorded decision:
    // a benchmark run without fsync is not evidence of durability (doc 17.3).
    bool allow_weak_durability = false;
    // The store directory must already exist: an engine that silently creates a
    // fresh database on a missing volume would look healthy while having lost
    // every sequence number (L-13).
    bool create_database_if_missing = false;

    // [limits]
    size_t max_event_queue = 4096;
    size_t max_inbox_pending = 10000;

    // [security]
    bool tls_enabled = false;  // local-only lab; production needs a reviewed transport
    bool redact_secret_fields = true;
    std::vector<int> secret_tags{tag_password_default(), tag_username_default()};
    std::string username;
    std::string password;  // loaded from a secret store, never committed
    size_t max_audit_value_bytes = 256;

    // [app]
    // An application message type the engine is allowed to carry. Empty means
    // every non-session MsgType is accepted (the lab default).
    std::vector<std::string> app_msg_types;

    static constexpr int tag_password_default() { return 554; }
    static constexpr int tag_username_default() { return 553; }

    // Composite session identity, also the ownership key (invariant I-01).
    std::string session_id() const;
};

struct ConfigError {
    std::string field;
    std::string reason;
};

// Validate a configuration. Returns every problem found rather than only the
// first, so an operator can fix a config in one pass.
std::vector<ConfigError> validate(const SessionConfig& cfg);

// Parse the strict TOML subset used by config/session.example.toml. Unknown
// keys are errors, not warnings: a typo in a safety-relevant setting must not
// be silently ignored.
bool load_config_file(const std::string& path, SessionConfig* out,
                      std::vector<ConfigError>* errors);

}  // namespace fixse
