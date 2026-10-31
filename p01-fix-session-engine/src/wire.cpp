#include "fix/wire.hpp"

namespace fixse {

std::string printable(std::string_view raw) {
    static const char* kHex = "0123456789ABCDEF";
    std::string out;
    out.reserve(raw.size());
    for (char c : raw) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (c == kSOH) {
            out.push_back('|');
        } else if (u < 0x20 || u > 0x7E) {
            // Never echo a raw control byte into a log line: a peer could
            // otherwise inject line breaks or terminal escapes (J-09).
            out.push_back('<');
            out.push_back(kHex[(u >> 4) & 0xF]);
            out.push_back(kHex[u & 0xF]);
            out.push_back('>');
        } else {
            out.push_back(c);
        }
    }
    return out;
}

int checksum_of(std::string_view bytes_before_trailer) {
    unsigned sum = 0;
    for (char c : bytes_before_trailer) sum += static_cast<unsigned char>(c);
    return static_cast<int>(sum % 256u);
}

std::string checksum_string(int sum_mod_256) {
    // Exactly three digits, zero padded, including the 0 and 255 boundaries
    // (B-25).
    std::string s(3, '0');
    const int v = ((sum_mod_256 % 256) + 256) % 256;
    s[0] = static_cast<char>('0' + (v / 100));
    s[1] = static_cast<char>('0' + ((v / 10) % 10));
    s[2] = static_cast<char>('0' + (v % 10));
    return s;
}

}  // namespace fixse
