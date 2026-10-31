#include "fix/metrics.hpp"

namespace fixse {

std::string Metrics::render() const {
    std::string out;
    out.reserve(counters_.size() * 48 + gauges_.size() * 48);
    for (const auto& kv : counters_) {
        out += kv.first;
        out += ' ';
        out += std::to_string(kv.second);
        out += '\n';
    }
    for (const auto& kv : gauges_) {
        out += kv.first;
        out += ' ';
        out += std::to_string(kv.second);
        out += '\n';
    }
    return out;
}

}  // namespace fixse
