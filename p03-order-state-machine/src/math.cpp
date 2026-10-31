#include "order_core/math.hpp"

namespace order_core {

std::string to_string(Int128 v) {
    if (v == 0) return "0";
    const bool negative = v < 0;
    std::string digits;
    // Accumulate on the negative side so the most negative value does not
    // overflow when negated.
    Int128 n = negative ? v : -v;
    while (n != 0) {
        digits.push_back(static_cast<char>('0' - static_cast<int>(n % 10)));
        n /= 10;
    }
    if (negative) digits.push_back('-');
    return std::string(digits.rbegin(), digits.rend());
}

}  // namespace order_core
