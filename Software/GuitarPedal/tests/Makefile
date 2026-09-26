# Host-side unit tests for header-only utilities. Run with: make -C tests
# These headers must not include any Daisy or STM32 headers.
CXX ?= c++
# The firmware builds with -Ofast. -O3 -ffast-math is the same float model (it implies
# -ffinite-math-only) spelled so Apple clang does not warn that -Ofast is deprecated.
CXXFLAGS ?= -std=c++20 -Wall -Wextra -Werror -O3 -ffast-math

TESTS := test_audio_guard test_crash_record
BUILD := build

.PHONY: all clean $(TESTS)

all: $(TESTS)

$(TESTS): %: $(BUILD)/%
	./$<

# Map each test to the header it covers so make rebuilds when the header changes.
$(BUILD)/test_audio_guard: test_audio_guard.cpp ../Util/audio_guard.h | $(BUILD)
	$(CXX) $(CXXFLAGS) $< -o $@

$(BUILD)/test_crash_record: test_crash_record.cpp ../Util/crash_record.h | $(BUILD)
	$(CXX) $(CXXFLAGS) $< -o $@

$(BUILD):
	mkdir -p $(BUILD)

clean:
	rm -rf $(BUILD)
