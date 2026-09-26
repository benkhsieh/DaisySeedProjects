// Hard fault handler that leaves a CrashRecord in backup SRAM and then waits for the
// independent watchdog to reboot the pedal.
//
// libDaisy defines its own HardFault_Handler (a debugger-only stub), and its startup
// file holds a weak copy in the same object as the vector table, so ours cannot replace
// it at link time. Instead InstallCrashHandler() patches the live vector table, which is
// writable because BOOT_SRAM apps run from AXI SRAM.

#include "crash_record.h"

#include "daisy_seed.h"

extern int activeEffectID; // defined in guitar_pedal.cpp

namespace bkshepherd {
// Backup SRAM is not zeroed at reset, so the record survives a watchdog or fault reboot.
CrashRecord g_crashRecord __attribute__((section(".backup_sram")));
} // namespace bkshepherd

extern "C" {

// Called from the naked handler below with a pointer to the exception stack frame.
// Frame layout: r0, r1, r2, r3, r12, lr, pc, xpsr.
void CrashHardFaultHandlerC(uint32_t *stackFrame) {
    const uint32_t stackedLr = stackFrame[5];
    const uint32_t stackedPc = stackFrame[6];
    bkshepherd::CrashRecordFill(bkshepherd::g_crashRecord, stackedPc, stackedLr, SCB->CFSR, activeEffectID, daisy::System::GetNow());

    // Do not try to recover. The watchdog started in main() resets the MCU within its
    // timeout, and the next boot reports the record.
    while (true) {
    }
}

// Naked so the stack pointer we inspect is the one the fault pushed onto.
__attribute__((naked)) void CrashHardFaultHandler(void) {
    __asm volatile("tst lr, #4              \n"
                   "ite eq                  \n"
                   "mrseq r0, msp           \n"
                   "mrsne r0, psp           \n"
                   "b CrashHardFaultHandlerC \n");
}
}

namespace bkshepherd {

void InstallCrashHandler() {
    const uint32_t vectorTableAddress = SCB->VTOR;

    // A table in internal flash (non-bootloader builds) cannot be patched; leave libDaisy's.
    if (vectorTableAddress >= 0x08000000u && vectorTableAddress < 0x08200000u) {
        return;
    }

    // Cortex-M vector table: [0] initial SP, [1] Reset, [2] NMI, [3] HardFault.
    constexpr uint32_t kHardFaultSlot = 3;
    volatile uint32_t *vectors = reinterpret_cast<volatile uint32_t *>(vectorTableAddress);
    vectors[kHardFaultSlot] = reinterpret_cast<uint32_t>(&CrashHardFaultHandler);

    // The table lives in cacheable RAM; push the write out so the core fetches the new entry.
    SCB_CleanDCache_by_Addr(reinterpret_cast<uint32_t *>(vectorTableAddress), 32);
    __DSB();
    __ISB();
}

} // namespace bkshepherd
