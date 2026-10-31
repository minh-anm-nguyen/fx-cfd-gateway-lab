# Incremental build for day-to-day work. CMakeLists.txt is the build the
# release gates use (sanitizer matrix, ctest groups); this exists because a
# full relink of the scenario suite is slow and the suite is edited constantly.
#
#   make -j8          build the suite
#   make test         build and run every scenario
#   make test F=J-    run one catalog group
#   make tools        build the replay and model-check binaries
#   make asan ubsan   sanitizer builds (never both with tsan in one binary)

CXX      ?= g++
BUILD    ?= build
CXXFLAGS := -std=c++20 -g -O1 -Wall -Wextra -Iinclude -Itests
LDFLAGS  ?=

LIB_SRC   := $(wildcard src/*.cpp)
SUITE_SRC := tests/test_main.cpp tests/support/tiny_test.cpp \
             $(wildcard tests/unit/*.cpp) $(wildcard tests/property/*.cpp) \
             $(wildcard tests/model/*.cpp) $(wildcard tests/integration/*.cpp) \
             $(wildcard tests/failure/*.cpp)

TOOL_SRC  := tools/order_trace_replay/main.cpp tools/order_model_check/main.cpp \
             apps/order_lab_cli/main.cpp

LIB_OBJ   := $(patsubst %.cpp,$(BUILD)/%.o,$(LIB_SRC))
SUITE_OBJ := $(patsubst %.cpp,$(BUILD)/%.o,$(SUITE_SRC))
TOOL_OBJ  := $(patsubst %.cpp,$(BUILD)/%.o,$(TOOL_SRC))
# Every object, tools included. Leaving the tools out of this list means a
# header change does not rebuild them, and an object compiled against an older
# struct layout corrupts memory rather than failing to link.
DEPS      := $(LIB_OBJ:.o=.d) $(SUITE_OBJ:.o=.d) $(TOOL_OBJ:.o=.d)

SUITE   := $(BUILD)/order_core_tests
REPLAY  := $(BUILD)/order_trace_replay
MODEL   := $(BUILD)/order_model_check
CLI     := $(BUILD)/order_lab_cli

.PHONY: all test tools clean asan tsan ubsan report
all: $(SUITE)

$(BUILD)/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -MMD -MP -c $< -o $@

$(SUITE): $(LIB_OBJ) $(SUITE_OBJ)
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDFLAGS)

$(REPLAY): $(LIB_OBJ) $(BUILD)/tools/order_trace_replay/main.o
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDFLAGS)

$(MODEL): $(LIB_OBJ) $(BUILD)/tools/order_model_check/main.o
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDFLAGS)

$(CLI): $(LIB_OBJ) $(BUILD)/apps/order_lab_cli/main.o
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDFLAGS)

tools: $(REPLAY) $(MODEL) $(CLI)

# F selects a catalog prefix, for example `make test F=C-`.
test: $(SUITE)
	./$(SUITE) $(if $(F),--filter=$(F),) --artifact=test-artifacts/order_core.json

report: test
	python scripts/report.py test-artifacts/order_core.json docs/What_If_Catalog.md

asan:
	$(MAKE) BUILD=build-asan CXXFLAGS="-std=c++20 -g -O1 -Wall -Wextra -Iinclude -Itests -fsanitize=address,undefined -fno-omit-frame-pointer" LDFLAGS="-fsanitize=address,undefined" test

ubsan:
	$(MAKE) BUILD=build-ubsan CXXFLAGS="-std=c++20 -g -O1 -Wall -Wextra -Iinclude -Itests -fsanitize=undefined -fno-sanitize-recover=all" LDFLAGS="-fsanitize=undefined" test

# Hardened libstdc++: bounds and iterator checks inside the standard
# containers, with no sanitizer runtime needed. Not a replacement for ASan, but
# it is available on toolchains that ship no libasan, and it catches the
# container misuse that an ODR or layout mismatch produces.
hardened:
	$(MAKE) BUILD=build-hardened CXXFLAGS="-std=c++20 -g -O1 -Wall -Wextra -Iinclude -Itests -D_GLIBCXX_ASSERTIONS" test

# Kept separate on purpose: ASan and TSan in one binary is unsupported.
tsan:
	$(MAKE) BUILD=build-tsan CXXFLAGS="-std=c++20 -g -O1 -Wall -Wextra -Iinclude -Itests -fsanitize=thread -fno-omit-frame-pointer" LDFLAGS="-fsanitize=thread" test

clean:
	rm -rf build build-asan build-ubsan build-tsan

-include $(DEPS)
