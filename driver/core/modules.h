/* feat.h - 各 feature 的入口声明 */
#pragma once
#include "module.h"

NTSTATUS UsbAuditRegister(VOID);
NTSTATUS UsbAuditInit(VOID);
VOID     UsbAuditUnload(VOID);
