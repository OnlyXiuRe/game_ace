/*
 * game_log.h - game_base 内核日志模块
 *
 * 设计参考: H:\Code\GameReverse\CrossFire\Logger\Logger.h
 *
 * 三通道输出:
 *   1. DbgPrintEx  - 按级别过滤, DebugView / WinDbg 可见 (仅 debug 版)
 *   2. 环形缓冲    - 3 环通过 IOCTL 拉取, 不依赖任何调试器
 *   3. 零开销      - release 版宏全部展开为空
 *
 * 用法 (和 Logger.h 风格一致):
 *   LOG_INFO ("device %p connected", dev);
 *   LOG_WARN ("resolve failed: %08X", st);
 *   LOG_ERROR("critical: %ws", name);
 *   LOG_DEBUG("probe bm=%02X req=%02X", bm, req);
 *
 * 需要指定模块标签时:
 *   GL_INFO("usb", "device %p connected", dev);
 */
#pragma once
#include <ntddk.h>
#include "fw_user.h"        /* GL_ENTRY / GL_DUMP 定义在这里, UM/KM 共享 */

/* ==================== 日志级别 ==================== */
typedef enum _GL_LEVEL {
    GL_LVL_INFO  = 0,
    GL_LVL_WARN  = 1,
    GL_LVL_ERR   = 2,
    GL_LVL_DEBUG = 3
} GL_LEVEL;

/* ==================== 接口 ==================== */
VOID  GlInit(VOID);
VOID  GlClear(VOID);

_IRQL_requires_max_(DISPATCH_LEVEL)
VOID GlWrite(_In_ ULONG level,
             _In_opt_z_ const char *tag,
             _In_z_ _Printf_format_string_ const char *fmt,
             ...);

ULONG GlDump(GL_DUMP *out, ULONG maxEntries);

/* ==================== 宏 ==================== */
#ifdef DBG
    /* ---- 带显式 tag (框架内部用) ---- */
    #define GL_INFO(tag, fmt, ...)   GlWrite(GL_LVL_INFO,  tag, fmt, ##__VA_ARGS__)
    #define GL_WARN(tag, fmt, ...)   GlWrite(GL_LVL_WARN,  tag, fmt, ##__VA_ARGS__)
    #define GL_ERR(tag, fmt, ...)    GlWrite(GL_LVL_ERR,   tag, fmt, ##__VA_ARGS__)
    #define GL_DBG(tag, fmt, ...)    GlWrite(GL_LVL_DEBUG, tag, fmt, ##__VA_ARGS__)

    /* ---- 自动取函数名作 tag (业务代码用, 和 Logger.h 一致) ---- */
    #define LOG_INFO(fmt, ...)       GlWrite(GL_LVL_INFO,  __FUNCTION__, fmt, ##__VA_ARGS__)
    #define LOG_WARN(fmt, ...)       GlWrite(GL_LVL_WARN,  __FUNCTION__, fmt, ##__VA_ARGS__)
    #define LOG_ERROR(fmt, ...)      GlWrite(GL_LVL_ERR,   __FUNCTION__, fmt, ##__VA_ARGS__)
    #define LOG_DEBUG(fmt, ...)      GlWrite(GL_LVL_DEBUG, __FUNCTION__, fmt, ##__VA_ARGS__)
#else
    /* release: 全部展开为空, 不产生任何代码 */
    #define GL_INFO(tag, fmt, ...)   do { } while (0)
    #define GL_WARN(tag, fmt, ...)   do { } while (0)
    #define GL_ERR(tag, fmt, ...)    do { } while (0)
    #define GL_DBG(tag, fmt, ...)    do { } while (0)
    #define LOG_INFO(tag, fmt, ...)  do { } while (0)
    #define LOG_WARN(tag, fmt, ...)  do { } while (0)
    #define LOG_ERROR(tag, fmt, ...) do { } while (0)
    #define LOG_DEBUG(tag, fmt, ...) do { } while (0)
#endif
