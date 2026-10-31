#include "jrnl/hash.hpp"

#include <algorithm>
#include <cstring>

namespace jrnl {
namespace {

constexpr std::uint32_t kK[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u,
    0x923f82a4u, 0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
    0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u,
    0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u,
    0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
    0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu,
    0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au,
    0x5b9cca4fu, 0x682e6ff3u, 0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u,
};

inline std::uint32_t rotr(std::uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

// Big-endian 64-bit, for the length-prefix helper and the padding block.
void put_be64(std::uint8_t* p, std::uint64_t v) {
    for (int i = 7; i >= 0; --i) {
        p[7 - i] = static_cast<std::uint8_t>((v >> (i * 8)) & 0xFF);
    }
}

}  // namespace

Sha256::Sha256() {
    h_[0] = 0x6a09e667u; h_[1] = 0xbb67ae85u; h_[2] = 0x3c6ef372u; h_[3] = 0xa54ff53au;
    h_[4] = 0x510e527fu; h_[5] = 0x9b05688cu; h_[6] = 0x1f83d9abu; h_[7] = 0x5be0cd19u;
}

void Sha256::compress(const std::uint8_t block[64]) {
    std::uint32_t w[64];
    for (int i = 0; i < 16; ++i) {
        w[i] = (static_cast<std::uint32_t>(block[i * 4]) << 24) |
               (static_cast<std::uint32_t>(block[i * 4 + 1]) << 16) |
               (static_cast<std::uint32_t>(block[i * 4 + 2]) << 8) |
               static_cast<std::uint32_t>(block[i * 4 + 3]);
    }
    for (int i = 16; i < 64; ++i) {
        const std::uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        const std::uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    std::uint32_t a = h_[0], b = h_[1], c = h_[2], d = h_[3];
    std::uint32_t e = h_[4], f = h_[5], g = h_[6], hh = h_[7];
    for (int i = 0; i < 64; ++i) {
        const std::uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
        const std::uint32_t ch = (e & f) ^ ((~e) & g);
        const std::uint32_t t1 = hh + S1 + ch + kK[i] + w[i];
        const std::uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
        const std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        const std::uint32_t t2 = S0 + maj;
        hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    h_[0] += a; h_[1] += b; h_[2] += c; h_[3] += d;
    h_[4] += e; h_[5] += f; h_[6] += g; h_[7] += hh;
}

void Sha256::update(std::string_view data) {
    total_bits_ += static_cast<std::uint64_t>(data.size()) * 8;
    std::size_t off = 0;
    if (buf_len_ > 0) {
        const std::size_t take = std::min(sizeof(buf_) - buf_len_, data.size());
        std::memcpy(buf_ + buf_len_, data.data(), take);
        buf_len_ += take;
        off += take;
        if (buf_len_ == sizeof(buf_)) {
            compress(buf_);
            buf_len_ = 0;
        }
    }
    while (data.size() - off >= 64) {
        compress(reinterpret_cast<const std::uint8_t*>(data.data()) + off);
        off += 64;
    }
    const std::size_t rest = data.size() - off;
    if (rest > 0) {
        std::memcpy(buf_, data.data() + off, rest);
        buf_len_ = rest;
    }
}

void Sha256::update(const Digest& d) {
    update(std::string_view(reinterpret_cast<const char*>(d.data()), d.size()));
}

Digest Sha256::finish() {
    const std::uint64_t bits = total_bits_;
    std::uint8_t pad = 0x80;
    update(std::string_view(reinterpret_cast<const char*>(&pad), 1));
    const std::uint8_t zero = 0;
    while (buf_len_ != 56) {
        update(std::string_view(reinterpret_cast<const char*>(&zero), 1));
    }
    std::uint8_t len[8];
    put_be64(len, bits);
    // Feed the length directly: update() would add it to total_bits_ again.
    std::memcpy(buf_ + 56, len, 8);
    compress(buf_);
    buf_len_ = 0;

    Digest out{};
    for (int i = 0; i < 8; ++i) {
        out[i * 4] = static_cast<std::uint8_t>((h_[i] >> 24) & 0xFF);
        out[i * 4 + 1] = static_cast<std::uint8_t>((h_[i] >> 16) & 0xFF);
        out[i * 4 + 2] = static_cast<std::uint8_t>((h_[i] >> 8) & 0xFF);
        out[i * 4 + 3] = static_cast<std::uint8_t>(h_[i] & 0xFF);
    }
    return out;
}

Digest sha256(std::string_view data) {
    Sha256 s;
    s.update(data);
    return s.finish();
}

std::string to_hex(const Digest& d) {
    static const char* kHex = "0123456789abcdef";
    std::string out(64, '0');
    for (std::size_t i = 0; i < d.size(); ++i) {
        out[i * 2] = kHex[(d[i] >> 4) & 0xF];
        out[i * 2 + 1] = kHex[d[i] & 0xF];
    }
    return out;
}

std::string to_hex(std::string_view raw) {
    static const char* kHex = "0123456789abcdef";
    std::string out;
    out.reserve(raw.size() * 2);
    for (char c : raw) {
        const unsigned char u = static_cast<unsigned char>(c);
        out.push_back(kHex[(u >> 4) & 0xF]);
        out.push_back(kHex[u & 0xF]);
    }
    return out;
}

bool from_hex(std::string_view hex, Digest* out) {
    if (out == nullptr || hex.size() != 64) return false;
    Digest tmp{};
    for (std::size_t i = 0; i < 32; ++i) {
        int hi = -1, lo = -1;
        const char a = hex[i * 2], b = hex[i * 2 + 1];
        if (a >= '0' && a <= '9') hi = a - '0';
        else if (a >= 'a' && a <= 'f') hi = a - 'a' + 10;
        if (b >= '0' && b <= '9') lo = b - '0';
        else if (b >= 'a' && b <= 'f') lo = b - 'a' + 10;
        if (hi < 0 || lo < 0) return false;
        tmp[i] = static_cast<std::uint8_t>((hi << 4) | lo);
    }
    *out = tmp;
    return true;
}

std::string hex_prefix(const Digest& d) { return to_hex(d).substr(0, 8); }

Digest chain_step(const Digest& prev_state_hash, std::string_view canonical_event_bytes,
                  std::string_view stream_id, std::uint64_t version,
                  std::string_view reducer_version) {
    // Every variable-length component is length-prefixed. Without this,
    // ("ab","c") and ("a","bc") would hash identically and the chain would
    // admit a substitution the verifier could not see.
    Sha256 s;
    std::uint8_t len[8];
    auto feed = [&](std::string_view v) {
        put_be64(len, static_cast<std::uint64_t>(v.size()));
        s.update(std::string_view(reinterpret_cast<const char*>(len), 8));
        s.update(v);
    };
    s.update(prev_state_hash);
    feed(canonical_event_bytes);
    feed(stream_id);
    put_be64(len, version);
    s.update(std::string_view(reinterpret_cast<const char*>(len), 8));
    feed(reducer_version);
    return s.finish();
}

Digest members_digest(std::vector<Digest> members) {
    // Sorted so the manifest does not depend on the order in which the caller
    // happened to enumerate the events of a transaction.
    std::sort(members.begin(), members.end());
    Sha256 s;
    std::uint8_t len[8];
    put_be64(len, static_cast<std::uint64_t>(members.size()));
    s.update(std::string_view(reinterpret_cast<const char*>(len), 8));
    for (const Digest& d : members) s.update(d);
    return s.finish();
}

}  // namespace jrnl
