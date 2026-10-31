// Policy configuration loading.
//
// The lab policy is written in the YAML subset shown in §19.3. A strict,
// hand-written parser is used instead of a YAML library for two reasons: the
// engine must build with nothing but a C++20 toolchain, and a strict parser
// rejects the surprising corners of YAML (implicit types, anchors, tabs)
// rather than resolving them silently. Anything it does not understand is an
// error, so a typo in a cap cannot be read as zero.
#pragma once

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "risk/policy.hpp"
#include "risk/result.hpp"

namespace risk {

// A node is exactly one of: scalar, mapping, or sequence of scalars.
struct YamlNode {
  enum class Kind { Scalar, Map, List } kind = Kind::Scalar;
  std::string scalar;
  std::map<std::string, YamlNode> map;
  std::vector<std::string> list;
  int line = 0;

  const YamlNode* child(const std::string& key) const;
};

// Parses the supported subset. Rejects tabs, anchors, aliases, multi-document
// streams, block scalars and nested sequences.
Result<YamlNode> parse_yaml_subset(const std::string& text);

// Builds a policy from a parsed document, then validates it. Unknown keys are
// rejected: a misspelled limit must not be silently ignored.
Result<RiskPolicy> policy_from_yaml(const YamlNode& root);

Result<RiskPolicy> load_policy_file(const std::string& path);

// Renders a policy back into the same subset, so a round-trip test can prove
// the loader and the model agree.
std::string policy_to_yaml(const RiskPolicy& policy);

}  // namespace risk
