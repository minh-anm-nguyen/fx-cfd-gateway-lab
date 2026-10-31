// Configuration loading and validation.
//
// Reference: design doc 14.1 (simulator config) and 12.2 (safe lab design).
//
// Scenario and configuration files are UNTRUSTED input (design doc 4, trust
// boundary 2). This loader therefore reads a deliberately small, strict subset
// of YAML rather than pulling in a general parser:
//
//   * every bound is explicit - file size, line length, nesting depth, node
//     count - so a crafted file cannot exhaust memory or CPU (L-09);
//   * an unknown key is an error, not something to ignore, because a silently
//     ignored key means the operator thinks a limit is set when it is not;
//   * there is no include, no anchor, no alias and no expression evaluation, so
//     there is nothing to make cyclic and nothing to interpolate into a shell
//     (design doc 12.2);
//   * store paths are checked for traversal before anything is opened.
//
// Startup validation refuses unsafe combinations outright rather than falling
// back to a default: a blank CompID, a non-positive heartbeat, adversarial wire
// faults on a non-loopback bind (L-05, L-06), or a zero queue cap.
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "lp/domain.hpp"
#include "lp/order_actor.hpp"
#include "lp/session.hpp"
#include "lp/store.hpp"

namespace lpsim {

// ---------------------------------------------------------------------------
// Restricted document model
// ---------------------------------------------------------------------------

// A node is either a scalar, a mapping or a sequence. Nothing else exists.
struct ConfNode {
    enum class Kind { Scalar, Map, Seq } kind = Kind::Scalar;
    std::string scalar;
    std::vector<std::pair<std::string, ConfNode>> map;
    std::vector<ConfNode> seq;

    const ConfNode* find(std::string_view key) const;
    bool is_scalar() const { return kind == Kind::Scalar; }
    bool is_map() const { return kind == Kind::Map; }
    bool is_seq() const { return kind == Kind::Seq; }
};

struct ConfLimits {
    std::size_t max_file_bytes = 1 << 20;  // 1 MiB
    std::size_t max_line_bytes = 4096;
    int max_depth = 8;
    std::size_t max_nodes = 20000;
};

struct ConfError {
    bool ok = true;
    int line = 0;
    std::string message;
};

// Parse the restricted subset. Returns false with *err populated; the document
// is left empty so a partial parse cannot be mistaken for a valid one.
bool parse_conf(std::string_view text, const ConfLimits& limits, ConfNode* out,
                ConfError* err);

// Read a file with the size bound applied before the read, not after.
bool read_conf_file(const std::string& path, const ConfLimits& limits, std::string* out,
                    ConfError* err);

// ---------------------------------------------------------------------------
// Simulator configuration
// ---------------------------------------------------------------------------

enum class SafetyMode { Conformant, Adversarial };

std::string_view to_string(SafetyMode m);

struct SimulatorConfig {
    SafetyMode mode = SafetyMode::Conformant;
    std::string instance_id = "lp-lab-1";
    std::string listen_host = "127.0.0.1";
    int listen_port = 9898;

    SessionConfig session;
    ActorConfig actor;
    StoreConfig store;

    // Design doc 12.2: wire corruption is a loopback-only capability, and
    // enabling it on a public bind must fail startup rather than warn.
    bool allow_unsafe_wire_faults = false;

    std::size_t max_scheduled_events = 100000;
    int max_fault_repeats = 10;

    // Everything that was explicitly present in the file, for the run manifest.
    std::map<std::string, std::string> provenance;
};

// Build a configuration from a parsed document, applying every validation rule
// above. Returns false with *err populated.
bool build_simulator_config(const ConfNode& doc, SimulatorConfig* out, ConfError* err);

// Validate an already-built configuration. Called again at startup so a
// programmatically assembled config cannot skip the checks.
bool validate_simulator_config(const SimulatorConfig& cfg, ConfError* err);

// True when the address is a loopback literal. Used by the unsafe-mode gate.
bool is_loopback(std::string_view host);

// Reject a store path that escapes its directory or names a device.
bool safe_store_path(std::string_view path, std::string* why);

}  // namespace lpsim
