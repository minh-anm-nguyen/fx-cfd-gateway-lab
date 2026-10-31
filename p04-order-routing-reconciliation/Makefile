# Build for the router and its test binary.
#
# A Makefile rather than hand-rolled shell: header dependencies are tracked with
# -MMD and included below, so editing a header rebuilds every translation unit
# that includes it. Getting that wrong once already produced a binary with two
# incompatible definitions of one struct linked together, which crashed instead
# of failing to build. CMakeLists.txt builds the same targets.

CXX      ?= g++
JOBS     ?= 8
BUILD    ?= build
# -g1 rather than -g: the test translation units are large, and full debug info
# made this toolchain's assembler run out of memory. Line tables are enough to
# read a stack trace.
CXXFLAGS := -std=c++20 -Wall -Wextra -Werror -Iinclude -Itests -O1 -g1 -MMD -MP
LDFLAGS  :=
# Static by default so the test binary runs without the toolchain on PATH,
# which keeps the evidence artifacts reproducible on a clean machine.
LDLIBS   := -static

ifeq ($(ASAN),1)
  CXXFLAGS += -fsanitize=address,undefined -fno-omit-frame-pointer
  LDFLAGS  += -fsanitize=address,undefined
  LDLIBS   :=
endif

LIB_SRC  := $(wildcard src/*.cpp)
TEST_SRC := tests/main.cpp $(wildcard tests/support/*.cpp) \
            $(wildcard tests/unit/*.cpp) $(wildcard tests/matrix/*.cpp) \
            $(wildcard tests/playbooks/*.cpp) $(wildcard tests/property/*.cpp)
APP_SRC  := $(wildcard apps/*/main.cpp)

LIB_OBJ  := $(patsubst %.cpp,$(BUILD)/obj/%.o,$(LIB_SRC))
TEST_OBJ := $(patsubst %.cpp,$(BUILD)/obj/%.o,$(TEST_SRC))

TEST_BIN := $(BUILD)/router_test.exe
CLI_BIN  := $(BUILD)/order_router_cli.exe
OPS_BIN  := $(BUILD)/recon_operator_cli.exe

.PHONY: all test clean apps
all: $(TEST_BIN) apps
apps: $(CLI_BIN) $(OPS_BIN)

$(BUILD)/obj/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(TEST_BIN): $(LIB_OBJ) $(TEST_OBJ)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) $^ $(LDLIBS) -o $@

$(CLI_BIN): $(BUILD)/obj/apps/router_cli/main.o $(LIB_OBJ)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) $^ $(LDLIBS) -o $@

$(OPS_BIN): $(BUILD)/obj/apps/recon_operator_cli/main.o $(LIB_OBJ)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) $^ $(LDLIBS) -o $@

# Runs every suite and writes the evidence artifact release gate 23.2 asks for.
test: $(TEST_BIN)
	$(TEST_BIN) --artifact=test-artifacts/all.json

clean:
	rm -rf $(BUILD)

-include $(shell find $(BUILD)/obj -name '*.d' 2>/dev/null)
