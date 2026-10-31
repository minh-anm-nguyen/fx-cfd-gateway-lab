#include "risk/hash.hpp"

#include <cstring>

namespace risk {
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
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u};

constexpr std::uint32_t rotr(std::uint32_t x, int n) {
  return (x >> n) | (x << (32 - n));
}

void compress(std::uint32_t h[8], const std::uint8_t block[64]) {
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

  std::uint32_t a = h[0], b = h[1], c = h[2], d = h[3];
  std::uint32_t e = h[4], f = h[5], g = h[6], hh = h[7];

  for (int i = 0; i < 64; ++i) {
    const std::uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
    const std::uint32_t ch = (e & f) ^ (~e & g);
    const std::uint32_t t1 = hh + S1 + ch + kK[i] + w[i];
    const std::uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
    const std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
    const std::uint32_t t2 = S0 + maj;
    hh = g;
    g = f;
    f = e;
    e = d + t1;
    d = c;
    c = b;
    b = a;
    a = t1 + t2;
  }

  h[0] += a;
  h[1] += b;
  h[2] += c;
  h[3] += d;
  h[4] += e;
  h[5] += f;
  h[6] += g;
  h[7] += hh;
}

}  // namespace

Digest sha256(std::string_view data) {
  std::uint32_t h[8] = {0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
                        0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};

  const auto* p = reinterpret_cast<const std::uint8_t*>(data.data());
  std::size_t remaining = data.size();
  while (remaining >= 64) {
    compress(h, p);
    p += 64;
    remaining -= 64;
  }

  std::uint8_t tail[128] = {};
  std::memcpy(tail, p, remaining);
  tail[remaining] = 0x80;
  const std::size_t tail_len = (remaining < 56) ? 64 : 128;
  const std::uint64_t bits = static_cast<std::uint64_t>(data.size()) * 8ULL;
  for (std::size_t i = 0; i < 8; ++i) {
    tail[tail_len - 1 - i] = static_cast<std::uint8_t>((bits >> (8 * i)) & 0xFF);
  }
  compress(h, tail);
  if (tail_len == 128) compress(h, tail + 64);

  Digest out{};
  for (std::size_t i = 0; i < 8; ++i) {
    out[i * 4] = static_cast<std::uint8_t>((h[i] >> 24) & 0xFF);
    out[i * 4 + 1] = static_cast<std::uint8_t>((h[i] >> 16) & 0xFF);
    out[i * 4 + 2] = static_cast<std::uint8_t>((h[i] >> 8) & 0xFF);
    out[i * 4 + 3] = static_cast<std::uint8_t>(h[i] & 0xFF);
  }
  return out;
}

Digest hmac_sha256(std::string_view key, std::string_view message) {
  constexpr std::size_t kBlock = 64;
  std::string k(key);
  if (k.size() > kBlock) {
    const Digest d = sha256(k);
    k.assign(reinterpret_cast<const char*>(d.data()), d.size());
  }
  k.resize(kBlock, '\0');

  std::string inner(kBlock, '\0');
  std::string outer(kBlock, '\0');
  for (std::size_t i = 0; i < kBlock; ++i) {
    inner[i] = static_cast<char>(static_cast<unsigned char>(k[i]) ^ 0x36u);
    outer[i] = static_cast<char>(static_cast<unsigned char>(k[i]) ^ 0x5Cu);
  }

  inner.append(message);
  const Digest inner_digest = sha256(inner);
  outer.append(reinterpret_cast<const char*>(inner_digest.data()),
               inner_digest.size());
  return sha256(outer);
}

bool constant_time_equals(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  unsigned char diff = 0;
  for (std::size_t i = 0; i < a.size(); ++i) {
    diff = static_cast<unsigned char>(
        diff | (static_cast<unsigned char>(a[i]) ^ static_cast<unsigned char>(b[i])));
  }
  return diff == 0;
}

std::string digest_hex(const Digest& d) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string out;
  out.reserve(64);
  for (const std::uint8_t b : d) {
    out.push_back(kHex[b >> 4]);
    out.push_back(kHex[b & 0x0F]);
  }
  return out;
}

Hasher& Hasher::field(std::string_view value) {
  buf_ += std::to_string(value.size());
  buf_ += ':';
  buf_.append(value.data(), value.size());
  buf_ += '|';
  return *this;
}

Hasher& Hasher::field(std::int64_t value) {
  const std::string s = std::to_string(value);
  buf_ += "i";
  buf_ += std::to_string(s.size());
  buf_ += ':';
  buf_ += s;
  buf_ += '|';
  return *this;
}

Hasher& Hasher::field(std::uint64_t value) {
  const std::string s = std::to_string(value);
  buf_ += "u";
  buf_ += std::to_string(s.size());
  buf_ += ':';
  buf_ += s;
  buf_ += '|';
  return *this;
}

Hasher& Hasher::field(const char* value) {
  return field(value == nullptr ? std::string_view{} : std::string_view(value));
}

Hasher& Hasher::flag(bool value) {
  buf_ += value ? "b1|" : "b0|";
  return *this;
}

Hasher& Hasher::named(std::string_view name, std::string_view value) {
  field(name);
  return field(value);
}

Hasher& Hasher::named(std::string_view name, std::int64_t value) {
  field(name);
  return field(value);
}

Hasher& Hasher::named(std::string_view name, std::uint64_t value) {
  field(name);
  return field(value);
}

Digest Hasher::digest() const { return sha256(buf_); }

std::string Hasher::hex() const { return digest_hex(digest()); }

}  // namespace risk
