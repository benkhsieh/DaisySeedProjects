// Minimal host tests for Util/audio_guard.h. No framework: each check prints and counts failures.
#include "../Util/audio_guard.h"

#include <cstdint>
#include <cstdio>
#include <cstring>

using namespace bkshepherd::audio_guard;

// The tests build with the firmware's fast-math rules, where a NaN or infinity literal is
// undefined behavior and std::isnan/std::isfinite may fold to constants. Build the special
// values from their bit patterns through a volatile so the optimizer cannot see them, the
// same way a bad sample reaches the guard at run time.
static float FromBits(uint32_t bits) {
    volatile uint32_t v = bits;
    uint32_t u = v;
    float f;
    std::memcpy(&f, &u, sizeof f);
    return f;
}
static const float kPosInf = FromBits(0x7F800000u);
static const float kNegInf = FromBits(0xFF800000u);
static const float kQuietNaN = FromBits(0x7FC00000u);
static const float kNegNaN = FromBits(0xFFC00001u);

static int failures = 0;
#define CHECK(cond)                                                                                                                   \
    do {                                                                                                                              \
        if (!(cond)) {                                                                                                                \
            std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #cond);                                                                \
            ++failures;                                                                                                               \
        }                                                                                                                             \
    } while (0)

static void test_clamp_input_passes_normal_values() {
    CHECK(ClampInput(0.0f) == 0.0f);
    CHECK(ClampInput(0.5f) == 0.5f);
    CHECK(ClampInput(-0.5f) == -0.5f);
    CHECK(ClampInput(1.0f) == 1.0f);
    CHECK(ClampInput(-1.0f) == -1.0f);
}

static void test_clamp_input_limits_out_of_range() {
    CHECK(ClampInput(3.7f) == 1.0f);
    CHECK(ClampInput(-42.0f) == -1.0f);
    CHECK(ClampInput(kPosInf) == 1.0f);
    CHECK(ClampInput(kNegInf) == -1.0f);
}

static void test_clamp_input_nan_becomes_zero() {
    CHECK(ClampInput(kQuietNaN) == 0.0f);
    CHECK(ClampInput(kNegNaN) == 0.0f);
}

static void test_sanitize_pair_leaves_finite_untouched() {
    float l = 0.25f, r = -0.75f;
    CHECK(SanitizePair(l, r) == false);
    CHECK(l == 0.25f);
    CHECK(r == -0.75f);
}

static void test_sanitize_pair_replaces_nan() {
    float l = kQuietNaN, r = 0.1f;
    CHECK(SanitizePair(l, r) == true);
    CHECK(l == 0.0f);
    CHECK(r == 0.1f);
}

static void test_sanitize_pair_replaces_inf_on_either_side() {
    float l = 0.1f, r = kNegInf;
    CHECK(SanitizePair(l, r) == true);
    CHECK(l == 0.1f);
    CHECK(r == 0.0f);

    float l2 = kPosInf, r2 = kQuietNaN;
    CHECK(SanitizePair(l2, r2) == true);
    CHECK(l2 == 0.0f);
    CHECK(r2 == 0.0f);

    float l3 = kNegNaN, r3 = kNegInf;
    CHECK(SanitizePair(l3, r3) == true);
    CHECK(l3 == 0.0f);
    CHECK(r3 == 0.0f);
}

int main() {
    test_clamp_input_passes_normal_values();
    test_clamp_input_limits_out_of_range();
    test_clamp_input_nan_becomes_zero();
    test_sanitize_pair_leaves_finite_untouched();
    test_sanitize_pair_replaces_nan();
    test_sanitize_pair_replaces_inf_on_either_side();
    if (failures == 0) {
        std::printf("test_audio_guard: all passed\n");
    }
    return failures == 0 ? 0 : 1;
}
