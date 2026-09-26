#pragma once
#ifndef WATCHDOG_H
#define WATCHDOG_H

// Thin wrapper over the STM32H7 independent watchdog (IWDG1).
// Once started the watchdog cannot be disabled; only a reset clears it.

#include "stm32h7xx_hal.h"

namespace bkshepherd {

inline IWDG_HandleTypeDef &WatchdogHandle() {
    static IWDG_HandleTypeDef handle;
    return handle;
}

/** Start the watchdog with the given timeout. LSI is 32 kHz; with prescaler 256 one tick
 *  is 8 ms, and the reload register is 12 bits, so the maximum timeout is about 32 s. */
inline void WatchdogStart(float timeoutSeconds) {
    // 32 kHz LSI / 256 = 125 ticks per second. Round to nearest so 2.0 s is exactly 250.
    constexpr float kTicksPerSecond = 125.0f;
    uint32_t reload = static_cast<uint32_t>(timeoutSeconds * kTicksPerSecond + 0.5f);
    if (reload < 1) {
        reload = 1;
    }
    if (reload > 0x0FFF) {
        reload = 0x0FFF;
    }

    IWDG_HandleTypeDef &h = WatchdogHandle();
    h.Instance = IWDG1;
    h.Init.Prescaler = IWDG_PRESCALER_256;
    h.Init.Reload = reload;
    h.Init.Window = IWDG_WINDOW_DISABLE;
    HAL_IWDG_Init(&h);
}

inline void WatchdogKick() { HAL_IWDG_Refresh(&WatchdogHandle()); }

} // namespace bkshepherd

#endif
