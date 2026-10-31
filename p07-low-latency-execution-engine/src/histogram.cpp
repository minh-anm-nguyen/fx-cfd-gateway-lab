#include "llx/histogram.hpp"

#include <sstream>

namespace llx {

std::string Histogram::to_json() const {
    std::ostringstream o;
    o << "{\"count\":" << count_ << ",\"invalid\":" << invalid_ << ",\"min\":" << min()
      << ",\"max\":" << max_ << ",\"sum\":" << sum_
      << ",\"p50\":" << percentile(50.0) << ",\"p95\":" << percentile(95.0)
      << ",\"p99\":" << percentile(99.0) << ",\"p999\":" << percentile(99.9)
      // Whether the sample count can support each quantile at all. A consumer
      // of this artifact must not quote p999 when this says false (doc 15.3).
      << ",\"sufficient_p99\":" << (sufficient_for(99.0) ? "true" : "false")
      << ",\"sufficient_p999\":" << (sufficient_for(99.9) ? "true" : "false")
      << ",\"bucket_relative_error\":0.015625,\"bins\":[";
    bool first = true;
    for (const Bin& b : bins()) {
        if (!first) o << ",";
        first = false;
        o << "[" << b.low << "," << b.high << "," << b.count << "]";
    }
    o << "]}";
    return o.str();
}

std::string OutcomeCounters::to_json() const {
    std::ostringstream o;
    o << "{\"offered\":" << offered << ",\"admitted\":" << admitted
      << ",\"completed\":" << completed << ",\"rejected_overload\":" << rejected_overload
      << ",\"rejected_invalid\":" << rejected_invalid << ",\"held_unknown\":" << held_unknown
      << ",\"duplicate_ignored\":" << duplicate_ignored << ",\"out_of_order\":" << out_of_order
      << ",\"stale_epoch\":" << stale_epoch << ",\"halted\":" << halted
      << ",\"timed_out\":" << timed_out << ",\"accounted\":" << accounted()
      << ",\"balanced\":" << (balanced() ? "true" : "false") << "}";
    return o.str();
}

}  // namespace llx
