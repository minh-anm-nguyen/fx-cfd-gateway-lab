// Stream framer: turns a TCP byte stream into complete FIX 4.4 frames.
//
// Reference: design doc 16.3 (group B) and 16.12 (group K). The rules the test
// matrix exercises directly:
//
//   * one recv() is not one message, and one message may span many recv()
//     calls (K-06 coalescing, K-07 one-byte splitting);
//   * a frame boundary comes from BodyLength(9) alone, so the bytes "10=" that
//     happen to appear inside a field value must not end a frame (B-15);
//   * BodyLength is untrusted input and is range-checked before any allocation
//     (L-08: 9=1000000000 must not reserve a gigabyte);
//   * the buffer is hard-capped, so an incomplete frame cannot grow without
//     bound (K-14 slow reader, L-08).
//
// On a malformed frame the framer does not attempt byte-level resynchronisation.
// Skipping bytes to find the next plausible "8=FIX" would desynchronise
// sequencing, so the session layer decides whether to reject or disconnect.
#pragma once

#include <cstddef>
#include <string>
#include <string_view>

#include "lp/result.hpp"

namespace lpsim {

class FixFramer {
public:
    FixFramer(std::size_t max_frame_bytes, std::size_t max_buffer_bytes);

    // Append received bytes. Returns WireError::FrameTooLarge when the buffer
    // cap would be exceeded, in which case nothing is appended and the caller
    // must close the connection (invariant I-17).
    WireError append(std::string_view bytes);

    // Extract the next complete frame.
    //   true  -> *out is a frame whose BodyLength and CheckSum are both
    //            verified, and *err is None.
    //   false -> *err is Incomplete (need more bytes) or a malformed-frame
    //            error, and the framer has consumed nothing.
    bool next(std::string_view* out, WireError* err);

    // Bytes currently held and whether a partial frame is in progress, used by
    // the session layer to enforce a partial-frame deadline.
    std::size_t buffered() const { return buf_.size() - read_pos_; }
    bool frame_in_progress() const { return buffered() > 0; }

    // Drop everything buffered, after a fatal framing error or a disconnect.
    void reset();

    std::size_t frames_parsed() const { return frames_parsed_; }
    std::size_t high_water_bytes() const { return high_water_; }

private:
    void compact();

    std::string buf_;
    std::size_t read_pos_ = 0;
    std::size_t max_frame_;
    std::size_t max_buffer_;
    std::size_t frames_parsed_ = 0;
    std::size_t high_water_ = 0;
};

}  // namespace lpsim
