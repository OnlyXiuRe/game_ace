/* fw_user.h - 框架的共享定义 (不含 ntddk, 驱动和 3 环都能用)
 *
 * 日志结构直接复用 log/game_log.h 的 GL_ENTRY / GL_DUMP,
 * 这里只做别名 + 定义 IOCTL, 避免两处结构体不一致。
 */
#pragma once

/* 与 log/game_log.h 保持一致 */
#define FW_LOG_MAX      1024
#define FW_MSG_MAX      160
#define FW_MOD_MAX      16

typedef struct _GL_ENTRY {
    ULONGLONG Ts;
    ULONG     Level;
    CHAR      Tag[FW_MOD_MAX];
    CHAR      Msg[FW_MSG_MAX];
} GL_ENTRY;

typedef struct _GL_DUMP {
    ULONG    Count;
    ULONG    Reserved;
    GL_ENTRY Entries[FW_LOG_MAX];
} GL_DUMP;

/* 兼容别名 */
typedef GL_ENTRY FW_LOG_ENTRY;
typedef GL_DUMP  FW_LOG_DUMP;

#define IOCTL_FW_LOG_DUMP   CTL_CODE(FILE_DEVICE_UNKNOWN, 0x900, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_FW_LOG_CLEAR  CTL_CODE(FILE_DEVICE_UNKNOWN, 0x901, METHOD_BUFFERED, FILE_ANY_ACCESS)
