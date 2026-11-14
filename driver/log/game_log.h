/*
 * game_log.h - game_base 内核日志模块
 *
 * 命名:
 *   Gl / GL_  = Game Logger, 本模块前缀
 *
 * 设计参考: H:\Code\GameReverse\CrossFire\Logger\Logger.h
 *   - Debug 版: 宏展开为实际调用 (DbgPrintEx + 环形缓冲)
 *   - Release 版: 宏展开为 do{}while(0), 零开销
 *
 * 两个输出通道:
 *   1. DbgPrintEx  - 按级别过滤, DebugView / WinDbg 可见 (仅 DBG 版)
 *   2. 环形缓冲    - 1024 条, 3 环用 IOCTL 拉取 (所有版本)
 *
 * 用法 (与 Logger.h 一致):
 *   LOG_INFO ("device %p connected", dev);
 *   LOG_WARN ("resolve failed: %08X", st);
 *   LOG_ERROR("critical: %ws", name);
 *   LOG_DEBUG("probe bm=%02X", bm);
 *
 *   需要固定模块标签时 (框架内部代码用):
 *   GL_INFO("usb", "device %p connected", dev);
 */
#pragma once
#include <ntddk.h>
#include "fw_user.h"        /* GL_ENTRY / GL_DUMP 定义在这里, UM/KM 共享 */

/* ==================== 日志级别 ====================
 * 数值越大越详细:
 *   0 INFO  - 正常流程关键节点
 *   1 WARN  - 可以恢复的异常
 *   2 ERR   - 功能失败
 *   3 DEBUG - 高频细节, 默认会在 DbgPrintEx 层被过滤掉
 */
typedef enum _GL_LEVEL {
    GL_LVL_INFO  = 0,
    GL_LVL_WARN  = 1,
    GL_LVL_ERR   = 2,
    GL_LVL_DEBUG = 3
} GL_LEVEL;

/* ==================== 接口 ==================== */

/* 初始化环形缓冲 + 自旋锁。必须最先调用。 */
VOID  GlInit(VOID);

/* 清空日志 (运行时, 对应 IOCTL_FW_LOG_CLEAR)。 */
VOID  GlClear(VOID);

/*
 * 核心写入函数, 所有日志宏最终都走到这里。
 *
 * level  - GL_LVL_* 级别, 同时决定 DbgPrintEx 的过滤级别
 * tag    - 模块标签, 3 环里显示为 [tag]; 传 NULL 时显示 "?"
 * fmt    - printf 风格格式串
 * ...    - 变参
 *
 * 注意: _Printf_format_string_ 是 SAL 注解, 让编译器检查
 *       fmt 和后续参数的类型是否匹配 (传 %d 却给 char* 会报错)。
 */
_IRQL_requires_max_(DISPATCH_LEVEL)
VOID GlWrite(_In_ ULONG level,
             _In_opt_z_ const char *tag,
             _In_z_ _Printf_format_string_ const char *fmt,
             ...);

/*
 * 把环形缓冲里的记录按时间正序拷到 out。
 * out          - 3 环提供的输出缓冲 (GL_DUMP)
 * maxEntries   - 最多拷多少条
 * 返回实际拷贝条数。
 */
ULONG GlDump(_Out_ GL_DUMP *out, _In_ ULONG maxEntries);

/* ==================== 宏 ====================
 *
 * 两套宏并存:
 *   GL_*  - 显式传 tag, 框架内部代码用 (例如 "usb" / "hook")
 *   LOG_* - 自动用 __FUNCTION__ 当 tag, 业务代码用, 和 Logger.h 风格一致
 *
 * 每个宏在 Debug 版和 Release 版的展开:
 *   Debug:   GlWrite(...)              -> 真正记录
 *   Release: do { } while (0)          -> 编译器直接丢弃所有参数
 */
#ifdef DBG
    /* ---- 带显式 tag ---- */
    #define GL_INFO(tag, fmt, ...)   GlWrite(GL_LVL_INFO,  tag, fmt, ##__VA_ARGS__)
    #define GL_WARN(tag, fmt, ...)   GlWrite(GL_LVL_WARN,  tag, fmt, ##__VA_ARGS__)
    #define GL_ERR(tag, fmt, ...)    GlWrite(GL_LVL_ERR,   tag, fmt, ##__VA_ARGS__)
    #define GL_DBG(tag, fmt, ...)    GlWrite(GL_LVL_DEBUG, tag, fmt, ##__VA_ARGS__)

    /* ---- 自动取函数名当 tag ---- */
    #define LOG_INFO(fmt, ...)       GlWrite(GL_LVL_INFO,  __FUNCTION__, fmt, ##__VA_ARGS__)
    #define LOG_WARN(fmt, ...)       GlWrite(GL_LVL_WARN,  __FUNCTION__, fmt, ##__VA_ARGS__)
    #define LOG_ERROR(fmt, ...)      GlWrite(GL_LVL_ERR,   __FUNCTION__, fmt, ##__VA_ARGS__)
    #define LOG_DEBUG(fmt, ...)      GlWrite(GL_LVL_DEBUG, __FUNCTION__, fmt, ##__VA_ARGS__)
#else
    /* Release: 全部展开为空, 连 fmt 字符串都不会进二进制 */
    #define GL_INFO(tag, fmt, ...)   do { } while (0)
    #define GL_WARN(tag, fmt, ...)   do { } while (0)
    #define GL_ERR(tag, fmt, ...)    do { } while (0)
    #define GL_DBG(tag, fmt, ...)    do { } while (0)
    #define LOG_INFO(fmt, ...)       do { } while (0)
    #define LOG_WARN(fmt, ...)       do { } while (0)
    #define LOG_ERROR(fmt, ...)      do { } while (0)
    #define LOG_DEBUG(fmt, ...)      do { } while (0)
#endif
