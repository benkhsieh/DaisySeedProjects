// Minimal host tests for Util/tape_saturator.h. No framework: each check prints and counts failures.
#include "../Util/tape_saturator.h"

#include <cstdio>

using namespace bkshepherd;

static int failures = 0;

#define CHECK(c)                                                                                                                      \
    do {                                                                                                                              \
        if (!(c)) {                                                                                                                   \
            std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c);                                                                   \
            ++failures;                                                                                                               \
        }                                                                                                                             \
    } while (0)

static void test_zero_maps_to_zero() {
    CHECK(TapeSaturate(0.0f, 1.0f) == 0.0f);
    CHECK(TapeSaturate(0.0f, 4.0f) == 0.0f);
}

static void test_bounded() {
    for (float x = -10.0f; x <= 10.0f; x += 0.25f) {
        // The ceiling of TanhApprox(drive * x) / drive is 1 / drive.
        float y = TapeSaturate(x, 2.0f);
        CHECK(y >= -(0.5f + 1e-4f) && y <= 0.5f + 1e-4f);
    }
}

static void test_odd_symmetry() {
    for (float x = 0.0f; x <= 3.0f; x += 0.1f) {
        CHECK(TapeSaturate(x, 3.0f) == -TapeSaturate(-x, 3.0f));
    }
}

static void test_monotonic() {
    float prev = TapeSaturate(-5.0f, 2.0f);
    for (float x = -4.9f; x <= 5.0f; x += 0.1f) {
        float y = TapeSaturate(x, 2.0f);
        CHECK(y >= prev);
        prev = y;
    }
}

static void test_small_signal_gain_near_unity_at_drive_1() {
    float y = TapeSaturate(0.05f, 1.0f);
    CHECK(y > 0.045f && y < 0.055f);
}

// With the / drive normalization the small-signal gain is unity at any drive:
// TanhApprox(0.15) / 3 is about 0.0499.
static void test_small_signal_gain_near_unity_at_drive_3() {
    float y = TapeSaturate(0.05f, 3.0f);
    CHECK(y > 0.045f && y < 0.055f);
}

// Bounds derived from the formula TanhApprox(drive * x) / drive: at drive 1 a full-scale
// input maps to TanhApprox(1) = 28/36 = 0.778; at drive 3 the curve has reached its
// ceiling, TanhApprox(3) / 3 = 1/3.
static void test_full_scale() {
    CHECK(TapeSaturate(1.0f, 1.0f) > 0.75f);
    const float y3 = TapeSaturate(1.0f, 3.0f);
    CHECK(y3 > 0.33f && y3 < 0.334f);
}

int main() {
    test_zero_maps_to_zero();
    test_bounded();
    test_odd_symmetry();
    test_monotonic();
    test_small_signal_gain_near_unity_at_drive_1();
    test_small_signal_gain_near_unity_at_drive_3();
    test_full_scale();
    if (!failures) {
        std::printf("test_tape_saturator: all passed\n");
    }
    return failures ? 1 : 0;
}
