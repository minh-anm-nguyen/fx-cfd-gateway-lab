// Content hashing for the journal hash chain (design doc section 8.1).
//
// SHA-256 over canonical bytes. The doc is careful about what this buys and
// the code should be too: a hash stored in the same database as the data it
// protects detects accidental corruption and casual tampering. It does not
// defeat an attacker with write access to the database, which is why section
// 18 asks for an external immutable anchor on top. Nothing here claims more.
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace jrnl {

using Digest = std::array<std::uint8_t, 32>;

Digest sha256(std::string_view data);

// Incremental form, for hashing a sequence without concatenating it first.
class Sha256 {
public:
    Sha256();
    void update(std::string_view data);
    void update(const Digest& d);
    Digest finish();

private:
    void compress(const std::uint8_t block[64]);

    std::uint32_t h_[8];
    std::uint8_t buf_[64];
    std::size_t buf_len_ = 0;
    std::uint64_t total_bits_ = 0;
};

std::string to_hex(const Digest& d);
std::string to_hex(std::string_view raw);

// Parses 64 hex characters. Returns false and leaves out untouched on any
// malformed input, which the integrity verifier treats as corruption rather
// than as an empty hash (G01-19).
bool from_hex(std::string_view hex, Digest* out);

// First 8 hex characters, for log lines. Section 22 asks for hash prefixes in
// logs, not full payloads.
std::string hex_prefix(const Digest& d);

inline const Digest& zero_digest() {
    static const Digest z{};
    return z;
}

// Chain step: H(prev_state_hash || canonical_event_bytes || stream_id ||
// version || reducer_version), exactly the preimage named in section 8.1.
// Each component is length-prefixed so that no two different component tuples
// can produce the same byte string.
Digest chain_step(const Digest& prev_state_hash, std::string_view canonical_event_bytes,
                  std::string_view stream_id, std::uint64_t version,
                  std::string_view reducer_version);

// Digest over an unordered set of member digests. Used for the transaction
// manifest (section 8 note 6), where the members are the events of one
// business transaction and their enumeration order must not change the result.
Digest members_digest(std::vector<Digest> members);

}  // namespace jrnl
