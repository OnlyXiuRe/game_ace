/* fw_user.h - 框架的共享定义 (不含 ntddk, 驱动和 3 环都能用) */
#pragma once

#define FW_LOG_MAX      1024
#define FW_MSG_MAX      160
#define FW_MOD_MAX      16

typedef struct _FW_LOG_ENTRY {
    ULONGLONG Ts;
    ULONG     Level;
    CHAR      Module[FW_MOD_MAX];
    CHAR      Msg[FW_MSG_MAX];
} FW_LOG_ENTRY;

typedef struct _FW_LOG_DUMP {
    ULONG        Count;
    ULONG        Reserved;
    FW_LOG_ENTRY Entries[FW_LOG_MAX];
} FW_LOG_DUMP;

#define IOCTL_FW_LOG_DUMP   CTL_CODE(FILE_DEVICE_UNKNOWN, 0x900, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_FW_LOG_CLEAR  CTL_CODE(FILE_DEVICE_UNKNOWN, 0x901, METHOD_BUFFERED, FILE_ANY_ACCESS)
