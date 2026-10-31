// order_model_check -- bounded exhaustive exploration of the reducer's state
// space, as a standalone binary so CI can run it outside the test suite.
#include <cstdio>
#include <string>

#include "model/model_check.hpp"

int main(int argc, char** argv) {
    model::Bounds b;
    for (int i = 1; i + 1 < argc; ++i) {
        const std::string flag = argv[i];
        const std::string value = argv[i + 1];
        if (flag == "--max-orders") {
            // One chain per exploration in this model; the flag is accepted so
            // the documented command line works, and the value is reported
            // rather than silently ignored.
            std::printf("note: --max-orders %s; this model explores one chain at a time\n",
                        value.c_str());
            ++i;
        } else if (flag == "--max-depth") {
            b.max_depth = static_cast<size_t>(std::stoul(value));
            ++i;
        } else if (flag == "--order-qty") {
            b.order_qty = std::stoll(value);
            ++i;
        } else if (flag == "--max-fill") {
            b.max_fill = std::stoll(value);
            ++i;
        } else if (flag == "--max-states") {
            b.max_states = static_cast<size_t>(std::stoul(value));
            ++i;
        }
    }

    std::printf("exploring: order_qty=%lld max_fill=%lld replace_qty=%lld depth=%zu\n",
                static_cast<long long>(b.order_qty), static_cast<long long>(b.max_fill),
                static_cast<long long>(b.replace_qty), b.max_depth);
    const model::Report r = model::explore(b);
    std::printf("states=%zu transitions=%zu deepest=%zu\n", r.states_visited, r.transitions,
                r.deepest);
    std::printf("committed=%zu quarantined=%zu duplicate=%zu rejected=%zu held=%zu\n",
                r.committed, r.quarantined, r.duplicates, r.rejected, r.held);
    if (r.hit_state_limit) {
        std::printf("WARNING: hit the state ceiling, so the bound was not fully explored\n");
    }
    if (!r.ok()) {
        std::printf("FAIL: %s\n", order_core::describe(r.violations).c_str());
        std::printf("trace: %s\n", r.failing_trace.c_str());
        return 1;
    }
    std::printf("PASS: no invariant violation inside the bound\n");
    return r.hit_state_limit ? 2 : 0;
}
