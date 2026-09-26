#pragma once
#ifndef CRASH_HANDLER_H
#define CRASH_HANDLER_H

#include "crash_record.h"

namespace bkshepherd {

/** The record the hard fault handler leaves in backup SRAM. Valid only if
 *  CrashRecordIsValid() says so; cleared by the startup report after showing it. */
extern CrashRecord g_crashRecord;

/** Routes hard faults to our handler by patching the live vector table. Call once
 *  from main() after System::InitBackupSram(). No-op when the table is not in RAM. */
void InstallCrashHandler();

} // namespace bkshepherd

#endif
