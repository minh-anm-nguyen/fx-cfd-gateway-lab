// Deterministic canonical hashing.
//
// Two different hashes serve two different invariants:
//   * the *canonical payload hash* pins command identity, so the same
//     command_id with a different payload is a CONFLICT (RI-02);
//   * the *state hash* lets a test or a recovery pass assert that the engine's
//     state after a restart equals the replay of the committed journal (RI-16,
//     P-13).
// Both must be stable across runs, builds and platforms, so nothing is hashed
// by memcpy'ing a struct: every field is fed through length-prefixed framing.
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

namespace risk {

using Digest = std::array<std::uint8_t, 32>;

Digest sha256(std::string_view data);
std::string digest_hex(const Digest& d);

// HMAC-SHA256, used to sign reservation tokens so P04 cannot forge one from a
// modified payload (RI-22). The key is the risk domain's secret.
Digest hmac_sha256(std::string_view key, std::string_view message);

// Constant-time comparison for signatures, so a verification failure does not
// leak how much of the signature matched.
bool constant_time_equals(std::string_view a, std::string_view b);

// Incremental, length-prefixed canonical encoder. Field framing is explicit
// ("len:bytes") so that {"ab","c"} and {"a","bc"} never hash alike.
class Hasher {
 public:
  Hasher& field(std::string_view value);
  // Without this overload a string literal binds to `field(bool)` -- pointer
  // to bool is a standard conversion and beats the user-defined conversion to
  // string_view -- so every literal section marker would hash as "true".
  Hasher& field(const char* value);
  Hasher& field(std::int64_t value);
  Hasher& field(std::uint64_t value);
  // Named `flag` rather than another `field` overload so a bool can never be
  // picked by accident for something that is not one.
  Hasher& flag(bool value);

  // Tags a named field so reordering or renaming changes the digest.
  Hasher& named(std::string_view name, std::string_view value);
  Hasher& named(std::string_view name, std::int64_t value);
  Hasher& named(std::string_view name, std::uint64_t value);

  Digest digest() const;
  std::string hex() const;

  const std::string& canonical_bytes() const noexcept { return buf_; }

 private:
  std::string buf_;
};

}  // namespace risk
