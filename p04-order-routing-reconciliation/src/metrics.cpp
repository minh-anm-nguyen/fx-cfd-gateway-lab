#include "router/metrics.hpp"

#include <algorithm>

namespace router {

std::string Metrics::key(const std::string& metric, const std::string& label) {
    return metric + "{" + label + "}";
}

void Metrics::allow_label(const std::string& metric, const std::vector<std::string>& values) {
    allowed_[metric] = values;
}

void Metrics::inc(const std::string& metric, std::uint64_t by) { counters_[metric] += by; }

void Metrics::inc(const std::string& metric, const std::string& label, std::uint64_t by) {
    auto it = allowed_.find(metric);
    if (it != allowed_.end()) {
        if (std::find(it->second.begin(), it->second.end(), label) == it->second.end()) {
            // Unknown label value. Counting it under "other" keeps the signal
            // without letting the series set grow (design doc 17.2, OR-Q-11).
            ++rejected_labels_;
            counters_[key(metric, "other")] += by;
            return;
        }
    }
    counters_[key(metric, label)] += by;
}

void Metrics::set_gauge(const std::string& metric, std::int64_t value) { gauges_[metric] = value; }

void Metrics::observe(const std::string& metric, std::uint64_t micros) {
    auto& h = histograms_[metric];
    // Bounded sample reservoir: a soak run must not grow the process by keeping
    // every observation (OR-P-17). Oldest samples are dropped.
    constexpr std::size_t kCap = 8192;
    if (h.size() >= kCap) h.erase(h.begin());
    h.push_back(micros);
}

std::uint64_t Metrics::counter(const std::string& metric) const {
    auto it = counters_.find(metric);
    return it == counters_.end() ? 0 : it->second;
}

std::uint64_t Metrics::counter(const std::string& metric, const std::string& label) const {
    auto it = counters_.find(key(metric, label));
    return it == counters_.end() ? 0 : it->second;
}

std::int64_t Metrics::gauge(const std::string& metric) const {
    auto it = gauges_.find(metric);
    return it == gauges_.end() ? 0 : it->second;
}

std::size_t Metrics::series_count() const {
    return counters_.size() + gauges_.size() + histograms_.size();
}

Metrics::Percentiles Metrics::percentiles(const std::string& metric) const {
    Percentiles p;
    auto it = histograms_.find(metric);
    if (it == histograms_.end() || it->second.empty()) return p;
    std::vector<std::uint64_t> v = it->second;
    std::sort(v.begin(), v.end());
    const auto at = [&v](double q) {
        const std::size_t i = static_cast<std::size_t>(q * static_cast<double>(v.size() - 1));
        return v[i];
    };
    p.count = v.size();
    p.p50 = at(0.50);
    p.p95 = at(0.95);
    p.p99 = at(0.99);
    p.max = v.back();
    return p;
}

std::string Metrics::render() const {
    std::string out;
    for (const auto& [k, v] : counters_) out += k + " " + std::to_string(v) + "\n";
    for (const auto& [k, v] : gauges_) out += k + " " + std::to_string(v) + "\n";
    for (const auto& [k, v] : histograms_) {
        (void)v;
        const Percentiles p = percentiles(k);
        out += k + "{q=p50} " + std::to_string(p.p50) + "\n";
        out += k + "{q=p95} " + std::to_string(p.p95) + "\n";
        out += k + "{q=p99} " + std::to_string(p.p99) + "\n";
        out += k + "{q=count} " + std::to_string(p.count) + "\n";
    }
    return out;
}

}  // namespace router
