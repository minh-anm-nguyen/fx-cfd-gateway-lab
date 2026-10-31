// Content hashing for test oracles and run manifests.
//
// Design doc 23 requires every case artifact to carry a final state hash, and
// doc 15.2/27.2 require a SHA-256 of the fixture and of the output state in the
// run manifest. Differential testing (doc 23 layer 4) compares the baseline and
// the optimized pipeline by hashing the committed P03/P05/P06 state, so the
// hash has to be a stable function of business content only: no addresses, no
// iteration order of unordered containers, no timestamps.
//
// SHA-256 is implemented here rather than pulled in as a dependency because it
// is 100 lines and the project otherwise needs no crypto library. It is used
// for evidence integrity, never for authentication.
#pragma once

#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <type_traits>

namespace llx {

class Sha256 {
public:
    Sha256() { reset(); }

    void reset();
    void update(const void* data, std::size_t len);
    void update(std::string_view s) { update(s.data(), s.size()); }

    // Appends the little-endian bytes of an integer or enumerator. Used so that
    // a hash over business state does not depend on how a number happens to be
    // formatted as text.
    template <typename T>
    void update_le(T v) {
        if constexpr (std::is_enum_v<T>) {
            update_le(static_cast<std::underlying_type_t<T>>(v));
        } else if constexpr (std::is_same_v<T, bool>) {
            update_le(static_cast<std::uint8_t>(v ? 1 : 0));
        } else {
            static_assert(std::is_integral_v<T>, "update_le takes integers and enums");
            using U = std::make_unsigned_t<T>;
            const auto u = static_cast<U>(v);
            unsigned char buf[sizeof(U)];
            for (std::size_t i = 0; i < sizeof(U); ++i)
                buf[i] = static_cast<unsigned char>((static_cast<std::uint64_t>(u) >> (8 * i)) & 0xFF);
            update(buf, sizeof(buf));
        }
    }

    // Finalizes and returns the lowercase hex digest. The object must be reset
    // before it is reused.
    std::string hex();

    // Length-prefixed canonical field, matching the framing the sibling
    // projects use: {"ab","c"} and {"a","bc"} must never hash alike, so every
    // field carries its own length.
    Sha256& field(std::string_view value) {
        update_le(static_cast<std::uint32_t>(value.size()));
        update(value);
        return *this;
    }

    template <typename T>
    Sha256& field_num(T v) {
        update_le(static_cast<std::uint8_t>(sizeof(T)));
        update_le(v);
        return *this;
    }

    static std::string of(std::string_view s) {
        Sha256 h;
        h.update(s);
        return h.hex();
    }

private:
    void compress(const unsigned char block[64]);

    std::array<std::uint32_t, 8> h_{};
    unsigned char buf_[64]{};
    std::size_t buf_len_ = 0;
    std::uint64_t total_bits_ = 0;
};

// Order-independent 64-bit digest of a set of per-item digests.
//
// Hashing a map by iterating it would make the result depend on container
// order, which differs between the baseline (std::map) and the optimized
// (open-addressed) pipelines even when the business content is identical. XOR of
// per-item SHA-256 prefixes is commutative, so the two agree exactly when their
// contents agree. Collisions are possible in principle; the differential tests
// also compare item counts and a sorted canonical rendering, so the digest is a
// fast screen rather than the only check.
class SetDigest {
public:
    void add(std::string_view item) {
        Sha256 h;
        h.update(item);
        const std::string hex = h.hex();
        std::uint64_t v = 0;
        for (int i = 0; i < 16; ++i) {
            const char c = hex[static_cast<std::size_t>(i)];
            const std::uint64_t nyb = (c <= '9') ? static_cast<std::uint64_t>(c - '0')
                                                 : static_cast<std::uint64_t>(c - 'a' + 10);
            v = (v << 4) | nyb;
        }
        acc_ ^= v;
        ++count_;
    }
    std::uint64_t value() const { return acc_; }
    std::size_t count() const { return count_; }

private:
    std::uint64_t acc_ = 0;
    std::size_t count_ = 0;
};

// FNV-1a, used only for in-memory bucket selection and for the cheap payload
// checksum carried on the hot path. Not an evidence hash.
constexpr std::uint64_t fnv1a(std::string_view s) noexcept {
    std::uint64_t h = 1469598103934665603ULL;
    for (char c : s) {
        h ^= static_cast<unsigned char>(c);
        h *= 1099511628211ULL;
    }
    return h;
}

constexpr std::uint64_t fnv1a_mix(std::uint64_t h, std::uint64_t v) noexcept {
    for (int i = 0; i < 8; ++i) {
        h ^= (v >> (8 * i)) & 0xFF;
        h *= 1099511628211ULL;
    }
    return h;
}

std::string to_hex(std::uint64_t v);

}  // namespace llx
