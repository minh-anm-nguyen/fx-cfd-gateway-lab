// Independent FIX 4.4 frame builder, validator and mutator for tests.
//
// This file deliberately does NOT use src/codec.cpp. The design doc warns that
// a peer you wrote yourself cannot prove your own protocol compliance (17.3
// layer 6); the next best thing available in-tree is a second, independently
// written implementation of BodyLength and CheckSum so that a mistake has to be
// made twice, in two different styles, to go unnoticed.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace peer {

constexpr char SOH = 1;

// Sum every byte and take the remainder only once at the end: a different
// arrangement from the engine implementation on purpose.
inline int independent_checksum(std::string_view bytes) {
    unsigned long long total = 0;
    for (size_t i = 0; i < bytes.size(); ++i)
        total += static_cast<unsigned char>(bytes[i]);
    return static_cast<int>(total % 256ull);
}

inline std::string three_digits(int v) {
    std::string s = std::to_string(((v % 256) + 256) % 256);
    while (s.size() < 3) s.insert(s.begin(), '0');
    return s;
}

struct Field {
    int tag;
    std::string value;
};

// Build a frame from an explicit field list. The caller controls the exact
// field order, which is what lets the B group tests produce deliberately
// malformed headers.
inline std::string build_from_fields(const std::vector<Field>& body_fields) {
    std::string body;
    for (const Field& f : body_fields) {
        body += std::to_string(f.tag);
        body += '=';
        body += f.value;
        body += SOH;
    }
    std::string head = std::string("8=FIX.4.4") + SOH + "9=" + std::to_string(body.size()) + SOH;
    std::string frame = head + body;
    frame += "10=" + three_digits(independent_checksum(frame));
    frame += SOH;
    return frame;
}

struct Msg {
    std::string msg_type = "0";
    uint64_t seq = 1;
    std::string sender = "SIMLP01";
    std::string target = "TRADER01";
    std::string sending_time = "20261030-05:00:00.000";
    bool poss_dup = false;
    std::string orig_sending_time;
    std::vector<Field> extra;

    std::string build() const {
        std::vector<Field> f;
        f.push_back({35, msg_type});
        f.push_back({49, sender});
        f.push_back({56, target});
        f.push_back({34, std::to_string(seq)});
        f.push_back({52, sending_time});
        if (poss_dup) {
            f.push_back({43, "Y"});
            f.push_back({122, orig_sending_time.empty() ? sending_time : orig_sending_time});
        }
        for (const Field& e : extra) f.push_back(e);
        return build_from_fields(f);
    }
};

// Independent structural verification: confirms that the declared BodyLength
// and CheckSum of a frame agree with its own bytes. Returns an explanation when
// they do not, so a failing assertion names the actual defect.
//
// The trailer is located from the END of the frame, never by searching for the
// bytes "10=": that substring occurs inside ordinary tag numbers such as 20110
// and inside field values, which is the same trap the framer has to avoid
// (B-11). An earlier version of this checker made exactly that mistake and the
// B-24 property test caught it.
inline bool verify_frame(std::string_view raw, std::string* why) {
    const auto fail = [&](const char* m) {
        if (why) *why = m;
        return false;
    };
    if (raw.size() < 20) return fail("frame shorter than any legal FIX message");
    if (raw.rfind("8=FIX.4.", 0) != 0) return fail("does not start with 8=FIX.4.x");
    const size_t soh1 = raw.find(SOH);
    if (soh1 == std::string_view::npos) return fail("no SOH after BeginString");
    if (raw.compare(soh1 + 1, 2, "9=") != 0) return fail("second field is not BodyLength");
    const size_t soh2 = raw.find(SOH, soh1 + 1);
    if (soh2 == std::string_view::npos) return fail("no SOH after BodyLength");

    const std::string_view len_text = raw.substr(soh1 + 3, soh2 - (soh1 + 3));
    if (len_text.empty()) return fail("BodyLength is empty");
    size_t declared_len = 0;
    for (char c : len_text) {
        if (c < '0' || c > '9') return fail("BodyLength is not numeric");
        declared_len = declared_len * 10 + static_cast<size_t>(c - '0');
    }

    // Trailer is always the final seven bytes: "10=" + three digits + SOH.
    if (raw[raw.size() - 1] != SOH) return fail("frame does not end with SOH");
    const size_t trailer = raw.size() - 7;
    if (raw.compare(trailer, 3, "10=") != 0) return fail("final field is not CheckSum");
    if (trailer == 0 || raw[trailer - 1] != SOH)
        return fail("CheckSum is not preceded by a field delimiter");

    const size_t body_begin = soh2 + 1;
    if (trailer < body_begin) return fail("trailer overlaps the header");
    const size_t actual_len = trailer - body_begin;
    if (actual_len != declared_len) {
        if (why)
            *why = "BodyLength says " + std::to_string(declared_len) + " but body is " +
                   std::to_string(actual_len) + " bytes";
        return false;
    }

    const std::string_view declared_sum = raw.substr(trailer + 3, 3);
    for (char c : declared_sum)
        if (c < '0' || c > '9') return fail("CheckSum is not three digits");
    const int expect = independent_checksum(raw.substr(0, trailer));
    if (three_digits(expect) != declared_sum) {
        if (why)
            *why = "CheckSum says " + std::string(declared_sum) + " but bytes sum to " +
                   three_digits(expect);
        return false;
    }
    return true;
}

// Length and checksum consistency only, with no opinion about the protocol
// version. The framer deliberately does not validate BeginString, because the
// version is a decoder concern (scenario B-16), so this is the right check to
// hold the FRAMER to.
inline bool verify_lengths(std::string_view raw, std::string* why) {
    const auto fail = [&](const char* m) {
        if (why) *why = m;
        return false;
    };
    if (raw.size() < 12) return fail("shorter than any legal frame");
    if (raw.compare(0, 2, "8=") != 0) return fail("does not start with tag 8");
    const size_t soh1 = raw.find(SOH);
    if (soh1 == std::string_view::npos) return fail("no SOH after BeginString");
    if (raw.compare(soh1 + 1, 2, "9=") != 0) return fail("second field is not BodyLength");
    const size_t soh2 = raw.find(SOH, soh1 + 1);
    if (soh2 == std::string_view::npos) return fail("no SOH after BodyLength");

    size_t declared = 0;
    const std::string_view len_text = raw.substr(soh1 + 3, soh2 - (soh1 + 3));
    if (len_text.empty()) return fail("BodyLength is empty");
    for (char c : len_text) {
        if (c < '0' || c > '9') return fail("BodyLength is not numeric");
        declared = declared * 10 + static_cast<size_t>(c - '0');
    }
    if (raw[raw.size() - 1] != SOH) return fail("frame does not end with SOH");
    const size_t trailer = raw.size() - 7;
    if (raw.compare(trailer, 3, "10=") != 0) return fail("final field is not CheckSum");
    if (trailer == 0 || raw[trailer - 1] != SOH)
        return fail("CheckSum is not preceded by a delimiter");
    const size_t body_begin = soh2 + 1;
    if (trailer < body_begin) return fail("trailer overlaps the header");
    if (trailer - body_begin != declared) return fail("BodyLength disagrees with the body");
    const std::string_view sum = raw.substr(trailer + 3, 3);
    for (char c : sum)
        if (c < '0' || c > '9') return fail("CheckSum is not three digits");
    if (three_digits(independent_checksum(raw.substr(0, trailer))) != sum)
        return fail("CheckSum disagrees with the bytes");
    return true;
}

// First value of a tag, searching on SOH boundaries so that a tag number that
// appears inside another value is not matched.
inline std::optional<std::string> get(std::string_view raw, int tag) {
    const std::string needle = std::string(1, SOH) + std::to_string(tag) + "=";
    const std::string head = std::to_string(tag) + "=";
    size_t pos;
    if (raw.rfind(head, 0) == 0) {
        pos = head.size();
    } else {
        const size_t f = raw.find(needle);
        if (f == std::string_view::npos) return std::nullopt;
        pos = f + needle.size();
    }
    const size_t end = raw.find(SOH, pos);
    if (end == std::string_view::npos) return std::nullopt;
    return std::string(raw.substr(pos, end - pos));
}

inline int count_tag(std::string_view raw, int tag) {
    const std::string needle = std::string(1, SOH) + std::to_string(tag) + "=";
    const std::string head = std::to_string(tag) + "=";
    int n = (raw.rfind(head, 0) == 0) ? 1 : 0;
    size_t pos = 0;
    while ((pos = raw.find(needle, pos)) != std::string_view::npos) {
        ++n;
        pos += needle.size();
    }
    return n;
}

// Replace a field value WITHOUT fixing BodyLength or CheckSum: used to inject
// the inconsistencies the B group expects the framer to catch.
inline std::string corrupt_field(std::string_view raw, int tag, std::string_view new_value) {
    const std::string needle = std::string(1, SOH) + std::to_string(tag) + "=";
    size_t pos = raw.find(needle);
    size_t vbegin;
    if (raw.rfind(std::to_string(tag) + "=", 0) == 0) {
        vbegin = std::to_string(tag).size() + 1;
    } else {
        if (pos == std::string_view::npos) return std::string(raw);
        vbegin = pos + needle.size();
    }
    const size_t vend = raw.find(SOH, vbegin);
    std::string out(raw);
    out.replace(vbegin, vend - vbegin, new_value);
    return out;
}


// Split a byte stream into frames, computed from BodyLength the same way a real
// peer would. Used to inspect what the engine actually wrote.
inline std::vector<std::string> split_frames(std::string_view stream) {
    std::vector<std::string> out;
    size_t pos = 0;
    while (pos + 12 <= stream.size()) {
        if (stream.compare(pos, 9, "8=FIX.4.4") != 0) break;
        const size_t soh1 = stream.find(SOH, pos);
        if (soh1 == std::string_view::npos) break;
        if (stream.compare(soh1 + 1, 2, "9=") != 0) break;
        const size_t soh2 = stream.find(SOH, soh1 + 1);
        if (soh2 == std::string_view::npos) break;
        size_t len = 0;
        bool numeric = soh2 > soh1 + 3;
        for (size_t i = soh1 + 3; i < soh2; ++i) {
            if (stream[i] < '0' || stream[i] > '9') {
                numeric = false;
                break;
            }
            len = len * 10 + static_cast<size_t>(stream[i] - '0');
        }
        if (!numeric) break;
        const size_t total = (soh2 + 1 - pos) + len + 7;
        if (pos + total > stream.size()) break;
        out.emplace_back(stream.substr(pos, total));
        pos += total;
    }
    return out;
}

}  // namespace peer
