// Single test binary for every suite. Select with --filter=<id or group>,
// --priority=P0, --list, --artifact=<path>, --seed=<n>.
#include "support/tiny_test.hpp"

int main(int argc, char** argv) { return tt::run_all(argc, argv, "router"); }
