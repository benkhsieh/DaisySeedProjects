#pragma once
#ifndef FOOTSWITCH_GESTURES_H
#define FOOTSWITCH_GESTURES_H

#include <algorithm>
#include <cstdint>

// Footswitch gesture recognizer. Header-only, no Daisy includes, so tests/ can build it on
// a host machine. Fed once per audio block with the debounced switch states; returns a
// bitmask of the gestures that completed in that block.
//
// Individual presses are "committed" after bothWindowMs (or on release if sooner). If the
// other switch goes down while the first is still inside its window, both presses are
// consumed by a both-gesture and no individual events fire for them.

namespace bkshepherd {

class FootswitchGestures {
  public:
    enum Event : uint32_t {
        kNone = 0,
        kBypassTap = 1u << 0,
        kBypassHold = 1u << 1,
        kAltPress = 1u << 2,
        kAltRelease = 1u << 3,
        kAltDoubleTap = 1u << 4,
        kAltHold = 1u << 5,
        kBothTap = 1u << 6,
        kBothHold = 1u << 7,
    };

    struct Config {
        float bothWindowMs = 80.0f;
        float bypassHoldMs = 2000.0f;
        float altHoldMs = 1000.0f;
        float doubleTapWindowMs = 2000.0f;
        float bothHoldMs = 2000.0f;
    };

    void Init(const Config &config) {
        m_cfg = config;
        m_bypass = SwitchState{};
        m_alt = SwitchState{};
        m_both = false;
        m_bothHeldMs = 0.0f;
        m_bothHoldEmitted = false;
        m_bothTapArmed = false;
        m_sinceLastAltRiseMs = kFarPast;
        m_altRiseInterval = kFarPast;
        m_lastDoubleTapIntervalMs = 0.0f;
    }

    float LastDoubleTapIntervalMs() const { return m_lastDoubleTapIntervalMs; }

    uint32_t Update(bool bypassDown, bool altDown, float dtMs) {
        uint32_t ev = kNone;

        // Advance time. A block in which a switch rises counts toward that press (see Press()),
        // so a press that starts in block 1 has heldMs == 80 at the end of block 80.
        if (m_bypass.down)
            m_bypass.heldMs += dtMs;
        if (m_alt.down)
            m_alt.heldMs += dtMs;
        if (m_both)
            m_bothHeldMs += dtMs;
        m_sinceLastAltRiseMs = std::min(m_sinceLastAltRiseMs + dtMs, kFarPast);

        // Edges.
        const bool bypassRise = bypassDown && !m_bypass.down;
        const bool bypassFall = !bypassDown && m_bypass.down;
        const bool altRise = altDown && !m_alt.down;
        const bool altFall = !altDown && m_alt.down;

        if (bypassRise)
            m_bypass.Press(dtMs);
        if (altRise) {
            m_alt.Press(dtMs);
            m_altRiseInterval = m_sinceLastAltRiseMs; // time since the previous Alt rise
            m_sinceLastAltRiseMs = 0.0f;
        }

        // Both-gesture: the second switch rises while the first is down and still inside
        // its window and not yet committed as an individual press.
        if (!m_both && m_bypass.down && m_alt.down && !m_bypass.emitted && !m_alt.emitted && !m_bypass.consumed && !m_alt.consumed &&
            m_bypass.heldMs <= m_cfg.bothWindowMs && m_alt.heldMs <= m_cfg.bothWindowMs) {
            m_both = true;
            m_bothHeldMs = dtMs; // the second rise block counts, as for a single press
            m_bothHoldEmitted = false;
            m_bothTapArmed = true;
            m_bypass.consumed = true;
            m_alt.consumed = true;
        }

        // Commit individual presses.
        if (m_bypass.down && !m_bypass.emitted && !m_bypass.consumed && (m_bypass.heldMs >= m_cfg.bothWindowMs || bypassFall)) {
            m_bypass.emitted = true;
            ev |= kBypassTap;
        }
        if (m_alt.down && !m_alt.emitted && !m_alt.consumed && (m_alt.heldMs >= m_cfg.bothWindowMs || altFall)) {
            m_alt.emitted = true;
            ev |= kAltPress;
            if (m_altRiseInterval <= m_cfg.doubleTapWindowMs) {
                ev |= kAltDoubleTap;
                m_lastDoubleTapIntervalMs = m_altRiseInterval;
            }
        }

        // Holds. These need the switch down in this block, so a release block never fires one.
        if (bypassDown && m_bypass.emitted && !m_bypass.consumed && !m_bypass.holdEmitted && !altDown &&
            m_bypass.heldMs >= m_cfg.bypassHoldMs) {
            m_bypass.holdEmitted = true;
            ev |= kBypassHold;
        }
        if (altDown && m_alt.emitted && !m_alt.consumed && m_alt.heldMs >= m_cfg.altHoldMs) {
            ev |= kAltHold;
        }

        // Release of a committed Alt press.
        if (altFall && m_alt.emitted && !m_alt.consumed) {
            ev |= kAltRelease;
        }

        // Both-gesture completion.
        if (m_both) {
            // Both-hold needs both switches still down: after a both-tap release, holding the
            // other switch must not save.
            if (!m_bothHoldEmitted && bypassDown && altDown && m_bothHeldMs >= m_cfg.bothHoldMs) {
                m_bothHoldEmitted = true;
                m_bothTapArmed = false;
                ev |= kBothHold;
            }
            if ((bypassFall || altFall) && m_bothTapArmed) {
                m_bothTapArmed = false;
                ev |= kBothTap;
            }
            if (!bypassDown && !altDown) {
                m_both = false;
            }
        }

        if (bypassFall)
            m_bypass.Release();
        if (altFall)
            m_alt.Release();
        return ev;
    }

  private:
    static constexpr float kFarPast = 1.0e9f;

    struct SwitchState {
        bool down = false;
        float heldMs = 0.0f;
        bool emitted = false;     // individual press event already sent
        bool consumed = false;    // part of a both-gesture; never sends individual events
        bool holdEmitted = false; // hold event already sent for this press
        void Press(float dtMs) {
            down = true;
            heldMs = dtMs;
            emitted = false;
            consumed = false;
            holdEmitted = false;
        }
        void Release() {
            down = false;
            heldMs = 0.0f;
        }
    };

    Config m_cfg;
    SwitchState m_bypass;
    SwitchState m_alt;
    bool m_both = false;
    float m_bothHeldMs = 0.0f;
    bool m_bothHoldEmitted = false;
    bool m_bothTapArmed = false;
    float m_sinceLastAltRiseMs = kFarPast;
    float m_altRiseInterval = kFarPast;
    float m_lastDoubleTapIntervalMs = 0.0f;
};

} // namespace bkshepherd

#endif
