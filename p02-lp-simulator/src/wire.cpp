#include "lp/wire.hpp"

namespace lpsim {

std::string printable(std::string_view raw) {
    std::string out;
    out.reserve(raw.size());
    for (char c : raw) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (c == kSOH) {
            out.push_back('|');
        } else if (u < 0x20 || u > 0x7E) {
            // Never echo raw control bytes into a log line (L-07, B-19).
            out.append("<?>");
        } else {
            out.push_back(c);
        }
    }
    return out;
}

int checksum_of(std::string_view bytes_before_trailer) {
    unsigned int sum = 0;
    for (char c : bytes_before_trailer) sum += static_cast<unsigned char>(c);
    return static_cast<int>(sum % 256u);
}

std::string checksum_string(int sum_mod_256) {
    char buf[4];
    buf[0] = static_cast<char>('0' + (sum_mod_256 / 100) % 10);
    buf[1] = static_cast<char>('0' + (sum_mod_256 / 10) % 10);
    buf[2] = static_cast<char>('0' + sum_mod_256 % 10);
    buf[3] = '\0';
    return std::string(buf, 3);
}

}  // namespace lpsim
