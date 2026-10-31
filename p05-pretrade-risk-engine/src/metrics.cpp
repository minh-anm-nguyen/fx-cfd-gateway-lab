#include "risk/metrics.hpp"

#include <algorithm>

namespace risk {

Histogram::Histogram() {
  bounds_ = {1,     2,     5,     10,    20,     50,     100,    200,
             500,   1000,  2000,  5000,  10000,  20000,  50000,  100000,
             200000, 500000, 1000000, 5000000};
  counts_.assign(bounds_.size() + 1, 0);
}

void Histogram::observe_micros(std::int64_t micros) {
  if (micros < 0) micros = 0;
  std::lock_guard<std::mutex> lock(mu_);
  const auto it = std::lower_bound(bounds_.begin(), bounds_.end(), micros);
  const std::size_t index = static_cast<std::size_t>(it - bounds_.begin());
  ++counts_[index];
  ++total_;
  if (micros > max_) max_ = micros;
}

std::int64_t Histogram::count() const {
  std::lock_guard<std::mutex> lock(mu_);
  return total_;
}

std::int64_t Histogram::quantile_micros(double q) const {
  std::lock_guard<std::mutex> lock(mu_);
  if (total_ == 0) return 0;
  if (q < 0.0) q = 0.0;
  if (q > 1.0) q = 1.0;

  // Ceiling so p99 of 100 samples is the 99th, not the 98th.
  const auto target = static_cast<std::int64_t>(
      static_cast<double>(total_) * q + 0.999999);
  std::int64_t cumulative = 0;
  for (std::size_t i = 0; i < counts_.size(); ++i) {
    cumulative += counts_[i];
    if (cumulative >= target) {
      return i < bounds_.size() ? bounds_[i] : max_;
    }
  }
  return max_;
}

std::vector<std::pair<std::int64_t, std::int64_t>> Histogram::buckets() const {
  std::lock_guard<std::mutex> lock(mu_);
  std::vector<std::pair<std::int64_t, std::int64_t>> out;
  out.reserve(counts_.size());
  for (std::size_t i = 0; i < counts_.size(); ++i) {
    const std::int64_t bound = i < bounds_.size() ? bounds_[i] : -1;  // -1 == +Inf
    out.emplace_back(bound, counts_[i]);
  }
  return out;
}

std::int64_t Histogram::max_observed_micros() const {
  std::lock_guard<std::mutex> lock(mu_);
  return max_;
}

void Metrics::incr(const std::string& name, std::int64_t delta) {
  std::lock_guard<std::mutex> lock(mu_);
  counters_[name] += delta;
}

void Metrics::incr_labeled(const std::string& name, const std::string& label,
                           std::int64_t delta) {
  std::lock_guard<std::mutex> lock(mu_);
  counters_[name + "{" + label + "}"] += delta;
}

void Metrics::set_gauge(const std::string& name, std::int64_t value) {
  std::lock_guard<std::mutex> lock(mu_);
  gauges_[name] = value;
}

std::int64_t Metrics::counter(const std::string& name) const {
  std::lock_guard<std::mutex> lock(mu_);
  const auto it = counters_.find(name);
  return it == counters_.end() ? 0 : it->second;
}

std::int64_t Metrics::labeled(const std::string& name,
                              const std::string& label) const {
  return counter(name + "{" + label + "}");
}

std::int64_t Metrics::gauge(const std::string& name) const {
  std::lock_guard<std::mutex> lock(mu_);
  const auto it = gauges_.find(name);
  return it == gauges_.end() ? 0 : it->second;
}

std::string Metrics::render() const {
  std::string out;
  {
    std::lock_guard<std::mutex> lock(mu_);
    for (const auto& [name, value] : counters_) {
      out += name + " " + std::to_string(value) + "\n";
    }
    for (const auto& [name, value] : gauges_) {
      out += name + " " + std::to_string(value) + "\n";
    }
  }
  for (const auto& [bound, count] : admission_commit_.buckets()) {
    out += "risk_admission_commit_micros_bucket{le=\"";
    out += (bound < 0 ? "+Inf" : std::to_string(bound));
    out += "\"} " + std::to_string(count) + "\n";
  }
  out += "risk_admission_commit_micros_count " +
         std::to_string(admission_commit_.count()) + "\n";
  return out;
}

}  // namespace risk
