#include "lp/framer.hpp"

#include "lp/wire.hpp"

namespace lpsim {

namespace {

// "8=FIX.4.4" plus SOH.
const std::string kPrefix = std::string("8=") + std::string(kBeginStringFix44) + kSOH;

// BodyLength digits beyond this cannot describe any frame this profile accepts;
// refusing them early keeps a crafted 9=99999999999999999999 from entering any
// arithmetic at all (L-08).
constexpr std::size_t kMaxBodyLengthDigits = 10;

}  // namespace

FixFramer::FixFramer(std::size_t max_frame_bytes, std::size_t max_buffer_bytes)
    : max_frame_(max_frame_bytes), max_buffer_(max_buffer_bytes) {
    if (max_buffer_ < max_frame_) max_buffer_ = max_frame_;
}

WireError FixFramer::append(std::string_view bytes) {
    if (buffered() + bytes.size() > max_buffer_) return WireError::FrameTooLarge;
    compact();
    buf_.append(bytes);
    if (buf_.size() - read_pos_ > high_water_) high_water_ = buf_.size() - read_pos_;
    return WireError::None;
}

void FixFramer::compact() {
    if (read_pos_ == 0) return;
    buf_.erase(0, read_pos_);
    read_pos_ = 0;
}

void FixFramer::reset() {
    buf_.clear();
    read_pos_ = 0;
}

bool FixFramer::next(std::string_view* out, WireError* err) {
    const std::string_view view(buf_.data() + read_pos_, buf_.size() - read_pos_);

    // --- BeginString -------------------------------------------------------
    if (view.size() < kPrefix.size()) {
        // Only report a bad BeginString once enough bytes exist to be sure the
        // mismatch is real rather than a split prefix.
        const std::size_t have = view.size();
        if (view.compare(0, have, kPrefix, 0, have) != 0) {
            // Distinguish "not a FIX frame at all" from "wrong version".
            *err = (have >= 2 && view.compare(0, 2, "8=") != 0) ? WireError::MissingBeginString
                                                                : WireError::BadBeginString;
            return false;
        }
        *err = WireError::Incomplete;
        return false;
    }
    if (view.compare(0, 2, "8=") != 0) {
        *err = WireError::MissingBeginString;
        return false;
    }
    if (view.compare(0, kPrefix.size(), kPrefix) != 0) {
        *err = WireError::BadBeginString;
        return false;
    }

    // --- BodyLength --------------------------------------------------------
    std::size_t p = kPrefix.size();
    if (view.size() < p + 2) {
        *err = WireError::Incomplete;
        return false;
    }
    if (view.compare(p, 2, "9=") != 0) {
        *err = WireError::MissingBodyLength;
        return false;
    }
    p += 2;
    const std::size_t digits_begin = p;
    while (p < view.size() && view[p] != kSOH) {
        const char c = view[p];
        if (c < '0' || c > '9') {
            *err = WireError::BadBodyLength;
            return false;
        }
        if (p - digits_begin >= kMaxBodyLengthDigits) {
            *err = WireError::BodyLengthOverflow;
            return false;
        }
        ++p;
    }
    if (p >= view.size()) {
        // No delimiter yet. A digit run longer than any legal length has
        // already been rejected by the loop above.
        *err = WireError::Incomplete;
        return false;
    }
    if (p == digits_begin) {
        *err = WireError::BadBodyLength;
        return false;
    }

    std::size_t body_len = 0;
    for (std::size_t i = digits_begin; i < p; ++i) {
        body_len = body_len * 10 + static_cast<std::size_t>(view[i] - '0');
    }
    const std::size_t body_begin = p + 1;  // past the SOH that ends tag 9

    // The trailer is exactly "10=NNN" plus SOH: seven bytes.
    constexpr std::size_t kTrailerLen = 7;
    const std::size_t total = body_begin + body_len + kTrailerLen;
    if (total > max_frame_) {
        *err = WireError::BodyLengthTooLarge;
        return false;
    }
    if (view.size() < total) {
        *err = WireError::Incomplete;
        return false;
    }

    // --- Trailer ----------------------------------------------------------
    const std::size_t trailer_begin = body_begin + body_len;
    if (view.compare(trailer_begin, 3, "10=") != 0) {
        // BodyLength did not land on the checksum field: the declared length
        // disagrees with the actual bytes (B-15).
        *err = WireError::BodyLengthMismatch;
        return false;
    }
    for (std::size_t i = trailer_begin + 3; i < trailer_begin + 6; ++i) {
        if (view[i] < '0' || view[i] > '9') {
            *err = WireError::BadCheckSumFormat;
            return false;
        }
    }
    if (view[trailer_begin + 6] != kSOH) {
        *err = WireError::BadCheckSumFormat;
        return false;
    }

    const int declared = (view[trailer_begin + 3] - '0') * 100 +
                         (view[trailer_begin + 4] - '0') * 10 +
                         (view[trailer_begin + 5] - '0');
    const int computed = checksum_of(view.substr(0, trailer_begin));
    if (declared != computed) {
        *err = WireError::CheckSumMismatch;
        return false;
    }

    *out = view.substr(0, total);
    read_pos_ += total;
    ++frames_parsed_;
    *err = WireError::None;
    return true;
}

}  // namespace lpsim
