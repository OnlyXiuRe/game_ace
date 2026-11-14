/* fw_user.h - 框架的共享定义 (不含 ntddk, 驱动和 3 环都能用)
 *
 * 日志结构定义在这里, 驱动和客户端共用同一份布局。
 */
#pragma once

/* 与 log/game_log.h 保持一致 */
#define FW_LOG_MAX      1024
#define FW_MSG_MAX      160
#define FW_FUNC_MAX     48

/*
 * 单条日志记录
 *
 * Ts    1601-01-01 起的 100ns 计数 (KeQuerySystemTime 的原始值)
 *       3 环侧用 LogTsToString() 转成可读时间
 * Level 0=INFO 1=WARN 2=ERR 3=DEBUG
 * Func  产生这条日志的函数名 (__FUNCTION__), 用于快速定位
 * Msg   格式化后的消息正文
 */
typedef struct _LOG_ENTRY {
    ULONGLONG Ts;
    ULONG     Level;
    CHAR      Func[FW_FUNC_MAX];
    CHAR      Msg[FW_MSG_MAX];
} LOG_ENTRY;

typedef struct _LOG_DUMP {
    ULONG     Count;
    ULONG     Reserved;
    LOG_ENTRY Entries[FW_LOG_MAX];
} LOG_DUMP;

#define IOCTL_FW_LOG_DUMP   CTL_CODE(FILE_DEVICE_UNKNOWN, 0x900, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_FW_LOG_CLEAR  CTL_CODE(FILE_DEVICE_UNKNOWN, 0x901, METHOD_BUFFERED, FILE_ANY_ACCESS)
