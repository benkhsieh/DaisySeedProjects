// Hard fault handler that leaves a CrashRecord in backup SRAM and then resets the MCU
// (or, with a debugger attached, halts in place so the fault can be inspected).
//
// libDaisy defines its own HardFault_Handler (a debugger-only stub), and its startup
// file holds a weak copy in the same object as the vector table, so ours cannot replace
// it at link time. Instead InstallCrashHandler() patches the live vector table, which is
// writable because BOOT_SRAM apps run from AXI SRAM.

#include "crash_handler.h"

#include "daisy_seed.h"

extern int activeEffectID; // defined in guitar_pedal.cpp

namespace bkshepherd {
// Backup SRAM is not zeroed at reset, so the record survives a watchdog or fault reboot.
// The section is ".backup_sram.crash", not ".backup_sram": libDaisy's boot_info must stay
// at the start of backup SRAM because the Daisy bootloader writes it there, and the linker
// script places plain ".backup_sram" input before ".backup_sram*" input.
CrashRecord g_crashRecord __attribute__((section(".backup_sram.crash")));
} // namespace bkshepherd

extern "C" {

// Called from the naked handler below with a pointer to the exception stack frame.
// Frame layout: r0, r1, r2, r3, r12, lr, pc, xpsr.
void CrashHardFaultHandlerC(uint32_t *stackFrame) {
    const uint32_t stackedLr = stackFrame[5];
    const uint32_t stackedPc = stackFrame[6];
    bkshepherd::CrashRecordFill(bkshepherd::g_crashRecord, stackedPc, stackedLr, SCB->CFSR, activeEffectID, daisy::System::GetNow());

    // libDaisy's MPU setup (region 2) makes backup SRAM non-cacheable, so the record is
    // already in memory. The clean is belt-and-braces in case that setup ever changes.
    SCB_CleanDCache_by_Addr(reinterpret_cast<uint32_t *>(&bkshepherd::g_crashRecord), sizeof(bkshepherd::CrashRecord));
    __DSB();

    // With a debugger attached, stop here so the fault can be inspected.
    if (CoreDebug->DHCSR & CoreDebug_DHCSR_C_DEBUGEN_Msk) {
        while (true) {
        }
    }

    // Otherwise reset now. Waiting for the watchdog is not enough: it may not be running
    // yet (a fault before WatchdogStart would hang forever), and a fault in the audio
    // interrupt would leave the SAI DMA replaying the last buffer as a buzz until it
    // fired. The next boot reports the record.
    NVIC_SystemReset();
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

    // Only patch a table that lives in writable RAM: AXI SRAM (BOOT_SRAM apps) or DTCM.
    // Internal flash and memory-mapped QSPI tables are left to libDaisy's handler.
    const bool inAxiSram = vectorTableAddress >= 0x24000000u && vectorTableAddress < 0x24080000u;
    const bool inDtcm = vectorTableAddress >= 0x20000000u && vectorTableAddress < 0x20020000u;
    if (!inAxiSram && !inDtcm) {
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
