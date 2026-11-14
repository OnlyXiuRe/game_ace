/*
 * game_log.h - game_base 内核日志模块
 *
 * 设计参考: H:\Code\GameReverse\CrossFire\Logger\Logger.h
 *   - debug 版: 宏展开为实际调用 (环形缓冲 + DbgPrint)
 *   - release 版: 宏展开为空, 零开销
 *
 * 用法:
 *   GL_INFO("usb", "device %p connected", dev);
 *   GL_WARN("hook", "resolve failed: %08X", st);
 *   GL_ERR ("usb", "critical: %ws", name);
 *   GL_DBG ("usb", "probe bm=%02X req=%02X", bm, req);
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
VOID  GlWrite(ULONG level, const char *tag, const char *fmt, ...);
ULONG GlDump(GL_DUMP *out, ULONG maxEntries);

/* ==================== 宏 (debug / release 双版本) ==================== */
#ifdef DBG
    /* debug: 带日志 */
    #define GL_INFO(tag, fmt, ...)   GlWrite(GL_LVL_INFO,  tag, fmt, ##__VA_ARGS__)
    #define GL_WARN(tag, fmt, ...)   GlWrite(GL_LVL_WARN,  tag, fmt, ##__VA_ARGS__)
    #define GL_ERR(tag, fmt, ...)    GlWrite(GL_LVL_ERR,   tag, fmt, ##__VA_ARGS__)
    #define GL_DBG(tag, fmt, ...)    GlWrite(GL_LVL_DEBUG, tag, fmt, ##__VA_ARGS__)
#else
    /* release: 全部展开为空, 不产生任何代码 */
    #define GL_INFO(tag, fmt, ...)   do { } while (0)
    #define GL_WARN(tag, fmt, ...)   do { } while (0)
    #define GL_ERR(tag, fmt, ...)    do { } while (0)
    #define GL_DBG(tag, fmt, ...)    do { } while (0)
#endif
