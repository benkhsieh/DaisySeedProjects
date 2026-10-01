// Scripted tests for Util/footswitch_gestures.h. Time advances in 1 ms blocks.
#include "../Util/footswitch_gestures.h"
#include <cstdio>
#include <vector>
using namespace bkshepherd;
using G = FootswitchGestures;
static int failures = 0;
#define CHECK(c)                                                                                                                      \
    do {                                                                                                                              \
        if (!(c)) {                                                                                                                   \
            std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c);                                                                   \
            ++failures;                                                                                                               \
        }                                                                                                                             \
    } while (0)

struct Rec {
    float t;
    uint32_t ev;
};
struct Script {
    G g;
    float t = 0;
    std::vector<Rec> log;
    Script() { g.Init(G::Config{}); }
    // Hold the given states for ms, recording every non-empty event mask with its time.
    void run(bool byp, bool alt, float ms) {
        for (float e = 0; e < ms; e += 1.0f) {
            t += 1.0f;
            uint32_t ev = g.Update(byp, alt, 1.0f);
            if (ev)
                log.push_back({t, ev});
        }
    }
    int count(uint32_t mask) const {
        int n = 0;
        for (auto &r : log)
            if (r.ev & mask)
                ++n;
        return n;
    }
    float first(uint32_t mask) const {
        for (auto &r : log)
            if (r.ev & mask)
                return r.t;
        return -1;
    }
};

static void test_quick_bypass_tap_fires_on_release() {
    Script s;
    s.run(true, false, 30);
    s.run(false, false, 50);
    CHECK(s.count(G::kBypassTap) == 1);
    CHECK(s.first(G::kBypassTap) == 31);
    CHECK(s.count(G::kBypassHold) == 0);
    CHECK(s.count(G::kBothTap) == 0);
}
static void test_bypass_tap_fires_at_window_when_held_longer() {
    Script s;
    s.run(true, false, 200);
    s.run(false, false, 50);
    CHECK(s.count(G::kBypassTap) == 1);
    CHECK(s.first(G::kBypassTap) == 80);
}
static void test_bypass_hold_fires_once_at_2s() {
    Script s;
    s.run(true, false, 2500);
    s.run(false, false, 50);
    CHECK(s.count(G::kBypassTap) == 1);
    CHECK(s.count(G::kBypassHold) == 1);
    CHECK(s.first(G::kBypassHold) == 2000);
}
static void test_alt_tap_press_and_release() {
    Script s;
    s.run(false, true, 40);
    s.run(false, false, 50);
    CHECK(s.count(G::kAltPress) == 1);
    CHECK(s.count(G::kAltRelease) == 1);
    CHECK(s.count(G::kAltDoubleTap) == 0);
    CHECK(s.first(G::kAltRelease) >= s.first(G::kAltPress));
}
static void test_alt_double_tap_reports_interval_between_rises() {
    Script s;
    s.run(false, true, 40);
    s.run(false, false, 460);
    s.run(false, true, 40);
    s.run(false, false, 50);
    CHECK(s.count(G::kAltPress) == 2);
    CHECK(s.count(G::kAltDoubleTap) == 1);
    CHECK(s.g.LastDoubleTapIntervalMs() == 500.0f);
}
static void test_alt_taps_far_apart_are_not_a_double_tap() {
    Script s;
    s.run(false, true, 40);
    s.run(false, false, 2500);
    s.run(false, true, 40);
    s.run(false, false, 50);
    CHECK(s.count(G::kAltDoubleTap) == 0);
}
static void test_alt_hold_fires_every_block_after_1s() {
    Script s;
    s.run(false, true, 1500);
    s.run(false, false, 50);
    CHECK(s.count(G::kAltPress) == 1);
    CHECK(s.first(G::kAltHold) == 1000);
    CHECK(s.count(G::kAltHold) == 501);
    CHECK(s.count(G::kAltRelease) == 1);
}
static void test_both_tap_suppresses_individual_events_and_fires_on_release() {
    Script s;
    s.run(true, false, 30);
    s.run(true, true, 270);
    s.run(false, true, 20);
    s.run(false, false, 50);
    CHECK(s.count(G::kBypassTap) == 0);
    CHECK(s.count(G::kAltPress) == 0);
    CHECK(s.count(G::kAltRelease) == 0);
    CHECK(s.count(G::kBothTap) == 1);
    CHECK(s.first(G::kBothTap) == 301);
    CHECK(s.count(G::kBothHold) == 0);
}
static void test_both_hold_fires_once_and_no_both_tap_after() {
    Script s;
    s.run(false, true, 20);
    s.run(true, true, 2480);
    s.run(true, false, 100);
    s.run(false, false, 50);
    CHECK(s.count(G::kBothHold) == 1);
    CHECK(s.first(G::kBothHold) == 2020);
    CHECK(s.count(G::kBothTap) == 0);
    CHECK(s.count(G::kBypassTap) == 0);
    CHECK(s.count(G::kAltPress) == 0);
}
static void test_second_switch_outside_window_is_two_separate_presses() {
    Script s;
    s.run(true, false, 200);
    s.run(true, true, 200);
    s.run(false, false, 50);
    CHECK(s.count(G::kBypassTap) == 1);
    CHECK(s.count(G::kAltPress) == 1);
    CHECK(s.count(G::kBothTap) == 0);
    CHECK(s.count(G::kBypassHold) == 0);
}
static void test_bypass_hold_blocked_while_alt_down() {
    Script s;
    s.run(true, false, 200);
    s.run(true, true, 2500);
    s.run(false, false, 50);
    CHECK(s.count(G::kBypassHold) == 0);
}
static void test_new_press_after_both_gesture_works_normally() {
    Script s;
    s.run(true, true, 100);
    s.run(false, false, 100);
    s.run(true, false, 30);
    s.run(false, false, 50);
    CHECK(s.count(G::kBothTap) == 1);
    CHECK(s.count(G::kBypassTap) == 1);
}
// After a both-tap, keeping one switch down must not turn into a both-hold (save needs both held).
static void test_both_hold_needs_both_switches_down() {
    Script s;
    s.run(true, true, 100);
    s.run(false, true, 3000);
    s.run(false, false, 50);
    CHECK(s.count(G::kBothTap) == 1);
    CHECK(s.count(G::kBothHold) == 0);
    CHECK(s.count(G::kAltPress) == 0);
    CHECK(s.count(G::kAltHold) == 0);
}
int main() {
    test_quick_bypass_tap_fires_on_release();
    test_bypass_tap_fires_at_window_when_held_longer();
    test_bypass_hold_fires_once_at_2s();
    test_alt_tap_press_and_release();
    test_alt_double_tap_reports_interval_between_rises();
    test_alt_taps_far_apart_are_not_a_double_tap();
    test_alt_hold_fires_every_block_after_1s();
    test_both_tap_suppresses_individual_events_and_fires_on_release();
    test_both_hold_fires_once_and_no_both_tap_after();
    test_second_switch_outside_window_is_two_separate_presses();
    test_bypass_hold_blocked_while_alt_down();
    test_new_press_after_both_gesture_works_normally();
    test_both_hold_needs_both_switches_down();
    if (!failures)
        std::printf("test_footswitch_gestures: all passed\n");
    return failures ? 1 : 0;
}
