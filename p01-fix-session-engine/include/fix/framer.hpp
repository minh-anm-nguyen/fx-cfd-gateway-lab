// Stream framer: turns a TCP byte stream into complete FIX frames.
// Reference: design doc 5.2 and the B group of the what-if matrix (15.3).
//
// Rules that the test matrix exercises directly:
//   * One recv() is not one message, and one message may span many recv()
//     calls (B-01, B-02, B-03).
//   * A frame boundary is computed from BodyLength(9) only; an occurrence of
//     the bytes "10=" inside a field value must not terminate a frame (B-11).
//   * BodyLength is untrusted input: it is range-checked before any allocation
//     (B-06, B-07, B-08, J-04).
//   * The buffer is hard-capped; an incomplete frame cannot grow without bound
//     (J-05 slowloris).
#pragma once

#include <cstddef>
#include <string>
#include <string_view>

#include "fix/clock.hpp"
#include "fix/result.hpp"

namespace fixse {

class FixFramer {
public:
    FixFramer(size_t max_frame_bytes, size_t max_buffer_bytes);

    // Append received bytes. Returns WireError::FrameTooLarge when the buffer
    // cap would be exceeded, in which case nothing is appended and the caller
    // must close the connection (bounded resource, invariant I-09).
    WireError append(std::string_view bytes);

    // Extract the next complete frame.
    //   returns true  -> *out holds a frame whose BodyLength and CheckSum are
    //                    both verified; *err is WireError::None.
    //   returns false -> *err is Incomplete (need more bytes) or a malformed
    //                    frame error. On a malformed frame the framer does not
    //                    attempt byte-level resynchronisation: the session
    //                    layer decides whether to reject or disconnect, because
    //                    silently skipping bytes would desynchronise sequencing
    //                    (B-04, B-26).
    bool next(std::string_view* out, WireError* err);

    // Bytes currently held, and whether a partial frame is in progress. Used by
    // the session layer to enforce partial_frame_deadline_ms (J-05).
    size_t buffered() const { return buf_.size() - read_pos_; }
    bool frame_in_progress() const { return buffered() > 0; }

    // Drop everything buffered, used after a fatal framing error or disconnect.
    void reset();

    // Statistics for metrics and tests.
    size_t frames_parsed() const { return frames_parsed_; }
    size_t high_water_bytes() const { return high_water_; }

private:
    void compact();

    std::string buf_;
    size_t read_pos_ = 0;
    size_t max_frame_;
    size_t max_buffer_;
    size_t frames_parsed_ = 0;
    size_t high_water_ = 0;
};

}  // namespace fixse
