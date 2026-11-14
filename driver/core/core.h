/* feat.h - 各 feature 的入口声明 */
#pragma once
#include "fw.h"

NTSTATUS CoreUsbRegister(VOID);
NTSTATUS CoreUsbInit(VOID);
VOID     CoreUsbUnload(VOID);
