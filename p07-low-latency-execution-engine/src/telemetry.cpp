#include "llx/telemetry.hpp"

#include <sstream>

namespace llx {

std::string MeasurementValidity::to_json() const {
    std::ostringstream o;
    o << "{\"coverage_complete\":" << (coverage_complete ? "true" : "false")
      << ",\"clock_on_target_platform\":" << (clock_on_target_platform ? "true" : "false")
      << ",\"outcomes_balanced\":" << (outcomes_balanced ? "true" : "false")
      << ",\"sufficient_p99\":" << (sufficient_p99 ? "true" : "false")
      << ",\"sufficient_p999\":" << (sufficient_p999 ? "true" : "false")
      << ",\"failpoints_compiled_in\":" << (failpoints_compiled_in ? "true" : "false")
      << ",\"lost_samples\":" << lost_samples
      << ",\"invalid_samples\":" << invalid_samples << ",\"clock_source\":\"" << clock_source
      << "\",\"performance_claim_admissible\":"
      << (performance_claim_admissible() ? "true" : "false") << ",\"note\":\"" << note
      << "\"}";
    return o.str();
}

}  // namespace llx
