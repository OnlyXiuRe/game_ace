/*
 * game_log.h - game_base 内核日志模块
 *
 * 设计参考: H:\Code\GameReverse\CrossFire\Logger\Logger.h
 *
 * 关键设计:
 *   - 调用方不传 tag, 前缀在 game_log.c 里固定为 [game_base]
 *   - Debug 版: 宏展开为实际调用 (DbgPrintEx + 环形缓冲)
 *   - Release 版: 宏展开为 do{}while(0), 零开销
 *
 * 用法:
 *   LOG_INFO ("device %p connected", dev);
 *   LOG_WARN ("resolve failed: %08X", st);
 *   LOG_ERROR("critical: %ws", name);
 *   LOG_DEBUG("probe bm=%02X", bm);
 */
#pragma once
#include <ntddk.h>
#include "fw_user.h"        /* LOG_ENTRY / LOG_DUMP 定义在这里, UM/KM 共享 */

/* ==================== 日志级别 ====================
 * 数值越大越详细:
 *   0 INFO  - 正常流程关键节点
 *   1 WARN  - 可以恢复的异常
 *   2 ERR   - 功能失败
 *   3 DEBUG - 高频细节, DbgPrintEx 层默认过滤
 */
typedef enum _LOG_LEVEL {
    LOG_LVL_INFO  = 0,
    LOG_LVL_WARN  = 1,
    LOG_LVL_ERR   = 2,
    LOG_LVL_DEBUG = 3
} LOG_LEVEL;

/* ==================== 接口 ==================== */

/* 初始化环形缓冲 + 自旋锁。必须在 DriverEntry 最开头调用。 */
VOID  LogInit(VOID);

/* 清空日志 (运行时, 对应 IOCTL_FW_LOG_CLEAR)。 */
VOID  LogClear(VOID);

/*
 * 核心写入函数。前缀 [game_base] 在实现里固定, 调用方不用关心。
 *
 * level  - LOG_LVL_* 级别, 同时决定 DbgPrintEx 的过滤级别
 * fmt    - printf 风格格式串
 * ...    - 变参
 *
 * _Printf_format_string_ 是 SAL 注解, 让编译器检查参数类型是否匹配。
 */
_IRQL_requires_max_(DISPATCH_LEVEL)
VOID LogWrite(_In_ ULONG level,
              _In_z_ _Printf_format_string_ const char *fmt,
              ...);

/*
 * 把环形缓冲里的记录按时间正序拷到 out。
 * 返回实际拷贝条数。
 */
ULONG LogDump(_Out_ LOG_DUMP *out, _In_ ULONG maxEntries);

/* ==================== 宏 ====================
 *
 * 只有这 4 个宏, 调用方只传格式串和参数。
 *
 *   Debug 版:   LOG_INFO(...)   ->  LogWrite(LOG_LVL_INFO, ...)   真正记录
 *   Release 版: LOG_INFO(...)   ->  do { } while (0)              编译器丢弃全部参数
 */
#ifdef DBG
    #define LOG_INFO(fmt, ...)   LogWrite(LOG_LVL_INFO,  fmt, ##__VA_ARGS__)
    #define LOG_WARN(fmt, ...)   LogWrite(LOG_LVL_WARN,  fmt, ##__VA_ARGS__)
    #define LOG_ERROR(fmt, ...)  LogWrite(LOG_LVL_ERR,   fmt, ##__VA_ARGS__)
    #define LOG_DEBUG(fmt, ...)  LogWrite(LOG_LVL_DEBUG, fmt, ##__VA_ARGS__)
#else
    #define LOG_INFO(fmt, ...)   do { } while (0)
    #define LOG_WARN(fmt, ...)   do { } while (0)
    #define LOG_ERROR(fmt, ...)  do { } while (0)
    #define LOG_DEBUG(fmt, ...)  do { } while (0)
#endif
