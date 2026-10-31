// SHA-256 and the digest type used for every provenance record.
//
// Design doc 2.2 R-I14 requires each normalized event to keep a raw digest, and
// 17.1 requires an append-only audit whose records are chained by hash so that
// tampering is detectable (OR-Q-18). A home-grown checksum would make those
// claims weaker than they read, so this is the real algorithm.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

namespace router {

struct Hash {
    std::array<std::uint8_t, 32> bytes{};

    friend bool operator==(const Hash& a, const Hash& b) { return a.bytes == b.bytes; }
    friend bool operator!=(const Hash& a, const Hash& b) { return a.bytes != b.bytes; }
    friend bool operator<(const Hash& a, const Hash& b) { return a.bytes < b.bytes; }

    bool is_zero() const;
    std::string hex() const;        // full 64 hex characters
    std::string short_hex() const;  // first 16, for logs and test output
};

Hash sha256(std::string_view data);

// Streaming form, used by the audit chain and the canonical command digest.
class Sha256 {
public:
    Sha256();
    void update(std::string_view data);
    void update(std::uint64_t v);  // fixed 8 byte big-endian so framing is unambiguous
    // Length prefixed, so that the pair ("ab","c") and the pair ("a","bc") cannot
    // produce the same digest (design doc 8.1: canonicalise strictly).
    void field(std::string_view data);
    Hash finish();

private:
    void block(const std::uint8_t* p);

    std::uint32_t h_[8];
    std::uint8_t buf_[64];
    std::size_t buf_len_ = 0;
    std::uint64_t total_ = 0;
};

}  // namespace router

namespace std {
template <>
struct hash<router::Hash> {
    size_t operator()(const router::Hash& h) const noexcept {
        size_t out = 0;
        for (int i = 0; i < 8; ++i) out = (out << 8) | h.bytes[static_cast<size_t>(i)];
        return out;
    }
};
}  // namespace std
