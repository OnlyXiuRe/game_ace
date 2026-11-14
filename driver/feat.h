/* feat.h - 各 feature 的入口声明 */
#pragma once
#include "fw.h"

NTSTATUS FeatUsbRegister(VOID);
NTSTATUS FeatUsbInit(VOID);
VOID     FeatUsbUnload(VOID);
