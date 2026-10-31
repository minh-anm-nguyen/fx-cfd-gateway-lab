#include "fix/framer.hpp"

#include <algorithm>
#include <cstring>

#include "fix/wire.hpp"

namespace fixse {
namespace {

bool is_digit(char c) { return c >= '0' && c <= '9'; }

// Parse a decimal run with an explicit digit cap so that a 300-digit value from
// an untrusted peer can never overflow or spin the CPU (B-07, B-21, J-11).
bool parse_bounded_u64(std::string_view s, size_t max_digits, uint64_t* out) {
    if (s.empty() || s.size() > max_digits) return false;
    uint64_t v = 0;
    for (char c : s) {
        if (!is_digit(c)) return false;
        v = v * 10 + static_cast<uint64_t>(c - '0');
    }
    *out = v;
    return true;
}

}  // namespace

FixFramer::FixFramer(size_t max_frame_bytes, size_t max_buffer_bytes)
    : max_frame_(max_frame_bytes), max_buffer_(std::max(max_buffer_bytes, max_frame_bytes)) {}

WireError FixFramer::append(std::string_view bytes) {
    if (buffered() + bytes.size() > max_buffer_) {
        // Refuse rather than grow: an unbounded receive buffer is how a peer
        // turns a protocol error into an out-of-memory kill (invariant I-09).
        return WireError::FrameTooLarge;
    }
    compact();
    buf_.append(bytes);
    high_water_ = std::max(high_water_, buffered());
    return WireError::None;
}

void FixFramer::reset() {
    buf_.clear();
    read_pos_ = 0;
}

void FixFramer::compact() {
    if (read_pos_ == 0) return;
    if (read_pos_ == buf_.size()) {
        buf_.clear();
        read_pos_ = 0;
        return;
    }
    // Only pay for the move once the dead prefix is worth reclaiming.
    if (read_pos_ >= 4096 || read_pos_ * 2 >= buf_.size()) {
        buf_.erase(0, read_pos_);
        read_pos_ = 0;
    }
}

bool FixFramer::next(std::string_view* out, WireError* err) {
    const char* p = buf_.data() + read_pos_;
    const size_t n = buffered();

    const auto incomplete = [&]() -> bool {
        // A buffer at or beyond the frame cap that still holds no complete
        // frame cannot ever produce one.
        *err = (n >= max_frame_) ? WireError::FrameTooLarge : WireError::Incomplete;
        return false;
    };

    if (n < 2) return incomplete();

    // Field 1 must be BeginString. The framer does not scan forward looking for
    // a plausible start: skipping unexplained bytes would hide a desynchronised
    // stream (B-12, B-26).
    if (p[0] != '8' || p[1] != '=') {
        *err = WireError::MissingBeginString;
        return false;
    }

    const void* soh1 = std::memchr(p + 2, kSOH, n - 2);
    if (soh1 == nullptr) return incomplete();
    const size_t pos9 = static_cast<size_t>(static_cast<const char*>(soh1) - p) + 1;

    // Field 2 must be BodyLength.
    if (n < pos9 + 2) return incomplete();
    if (p[pos9] != '9' || p[pos9 + 1] != '=') {
        *err = WireError::MissingBodyLength;
        return false;
    }

    const void* soh2 = std::memchr(p + pos9 + 2, kSOH, n - (pos9 + 2));
    if (soh2 == nullptr) return incomplete();
    const size_t len_begin = pos9 + 2;
    const size_t len_end = static_cast<size_t>(static_cast<const char*>(soh2) - p);
    const std::string_view len_text(p + len_begin, len_end - len_begin);

    if (len_text.empty()) {
        *err = WireError::BadBodyLength;
        return false;
    }
    if (len_text.size() > 10) {
        *err = WireError::BodyLengthOverflow;
        return false;
    }
    uint64_t body_len = 0;
    if (!parse_bounded_u64(len_text, 10, &body_len)) {
        // Covers an empty value, a sign character and any non-digit (B-06).
        *err = WireError::BadBodyLength;
        return false;
    }

    const size_t body_start = len_end + 1;
    // "10=" + three digits + SOH is always seven bytes.
    constexpr size_t kTrailerBytes = 7;
    if (body_len > max_frame_ || body_start + body_len + kTrailerBytes > max_frame_) {
        // Rejected before a single byte is reserved for the declared length
        // (B-08, J-04).
        *err = WireError::BodyLengthTooLarge;
        return false;
    }

    const size_t body_end = body_start + static_cast<size_t>(body_len);
    const size_t total = body_end + kTrailerBytes;

    // Validate the trailer progressively so a malformed checksum is rejected as
    // soon as the bytes that prove it malformed have arrived, instead of
    // waiting for a frame length that will never materialise (B-10).
    if (n > body_end && p[body_end] != '1') {
        *err = WireError::BodyLengthMismatch;
        return false;
    }
    if (n > body_end + 1 && p[body_end + 1] != '0') {
        *err = WireError::BodyLengthMismatch;
        return false;
    }
    if (n > body_end + 2 && p[body_end + 2] != '=') {
        *err = WireError::BodyLengthMismatch;
        return false;
    }
    for (size_t i = 3; i <= 5; ++i) {
        if (n > body_end + i && !is_digit(p[body_end + i])) {
            *err = WireError::BadCheckSumFormat;
            return false;
        }
    }
    if (n > body_end + 6 && p[body_end + 6] != kSOH) {
        *err = WireError::BadCheckSumFormat;
        return false;
    }
    if (n < total) return incomplete();

    const std::string_view declared(p + body_end + 3, 3);
    uint64_t declared_sum = 0;
    if (!parse_bounded_u64(declared, 3, &declared_sum)) {
        *err = WireError::BadCheckSumFormat;
        return false;
    }
    const int computed = checksum_of(std::string_view(p, body_end));
    if (static_cast<int>(declared_sum) != computed) {
        *err = WireError::CheckSumMismatch;
        return false;
    }

    *out = std::string_view(p, total);
    *err = WireError::None;
    read_pos_ += total;
    ++frames_parsed_;
    return true;
}

}  // namespace fixse
