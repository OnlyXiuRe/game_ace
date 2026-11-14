/*
 * game_log.c - game_base 内核日志实现
 *
 * 输出前缀固定为 [game_base], 调用方不需要传 tag。
 * 这与 Logger.h 里硬编码 [CrossFireLogger] 的做法一致。
 *
 * 双通道:
 *   通道1 DbgPrintEx   - 按级别过滤, DebugView / WinDbg 可见 (仅 DBG 版)
 *   通道2 环形缓冲     - 1024 条固定记录, 3 环用 IOCTL 拉取 (所有版本)
 */
#include "game_log.h"
#include <ntstrsafe.h>

/* ==================================================================
 * 固定日志前缀
 * ==================================================================
 * 只在这里定义一次。输出形如:
 *   [game_base][1234567.890][WRN] resolve failed: C0000024
 *    ^^^^^^^^^^  ^^^^^^^^^^^  ^^^  ^^^^^^^^^^^^^^^^^^^^^^^^^
 *    前缀(固定)  时间戳(秒.毫秒) 级别  消息正文
 */
#define GAME_LOG_TAG    "game_base"

/* ==================== 内部状态 ==================== */

/* 环形缓冲本体: 固定 1024 条, 满了覆盖最旧的 */
static LOG_ENTRY       g_Entries[FW_LOG_MAX];

/* 下一个要写入的下标 (0 ~ FW_LOG_MAX-1, 循环) */
static volatile LONG   g_Head = 0;

/* 已写入的总条数。未满时 = 实际条数; 满了之后固定为 FW_LOG_MAX */
static volatile LONG   g_Count = 0;

/* 保护 g_Entries / g_Head / g_Count。
 * 驱动可能在多核上并发调用 LogWrite, 必须加锁 */
static KSPIN_LOCK      g_Lock;

/*
 * LOG 级别 -> DbgPrintEx 过滤级别 的映射表
 *
 * DbgPrintEx 的过滤级别数值越小越严重:
 *   ERROR(0) < WARNING(1) < TRACE(2) < INFO(3)
 *
 * DebugView 里可以设过滤条件, 比如设为 WARNING 就看不到 INFO/TRACE。
 */
static const ULONG g_DpfltrLevel[] = {
    DPFLTR_INFO_LEVEL,      /* 下标 0 = LOG_LVL_INFO  */
    DPFLTR_WARNING_LEVEL,   /* 下标 1 = LOG_LVL_WARN  */
    DPFLTR_ERROR_LEVEL,     /* 下标 2 = LOG_LVL_ERR   */
    DPFLTR_TRACE_LEVEL      /* 下标 3 = LOG_LVL_DEBUG */
};

/* ==================== 初始化 / 清理 ==================== */

/* 清空环形缓冲并初始化自旋锁。必须在 DriverEntry 最开头调用。 */
VOID LogInit(VOID)
{
    KeInitializeSpinLock(&g_Lock);
    RtlZeroMemory(g_Entries, sizeof(g_Entries));
    g_Head = 0;
    g_Count = 0;
}

/* 运行时清空日志 (对应 IOCTL_FW_LOG_CLEAR), 驱动保持加载状态 */
VOID LogClear(VOID)
{
    KIRQL irql;
    KeAcquireSpinLock(&g_Lock, &irql);
    RtlZeroMemory(g_Entries, sizeof(g_Entries));
    g_Head = 0;
    g_Count = 0;
    KeReleaseSpinLock(&g_Lock, irql);
}

/* ==================== 辅助 ==================== */

/* 级别 -> 3 字符短名, 用于日志前缀里的 [INF]/[WRN]/[ERR]/[DBG] */
static const char *LevelName(ULONG level)
{
    switch (level) {
    case LOG_LVL_INFO:  return "INF";   /* 普通信息 */
    case LOG_LVL_WARN:  return "WRN";   /* 警告     */
    case LOG_LVL_ERR:   return "ERR";   /* 错误     */
    case LOG_LVL_DEBUG: return "DBG";   /* 调试细节 */
    default:            return "???";
    }
}

/* ==================== 核心写入函数 ==================== */

/*
 * LogWrite - 所有日志宏最终都走到这里
 *
 * 参数:
 *   level  日志级别 (LOG_LVL_*), 决定 DbgPrintEx 过滤级别 + 记录里的标记位
 *   fmt    printf 风格格式串, 如 "device %p failed: %08X"
 *   ...    变参, 对应 fmt 里的占位符
 *
 * 两件事:
 *   1. (仅 DBG 版) 格式化后调用 DbgPrintEx 输出到调试器
 *   2. (所有版本) 格式化后写入环形缓冲, 供 3 环读取
 *
 * 注意: 因为要对同一组变参格式化两次, 必须用 va_start/va_end 包裹两次,
 *       C 语言的 va_list 遍历一次后就失效了。
 */
VOID LogWrite(ULONG level, const char *fmt, ...)
{
    LOG_ENTRY     e;        /* 待写入的一条记录 */
    LARGE_INTEGER ts;       /* 系统时间戳 (100ns 单位) */
    va_list       ap;       /* 变参游标 */
    KIRQL         irql;     /* 加锁前的中断级别, 解锁时恢复 */
    LONG          idx;      /* 本次写入的下标 */

    (VOID)level;            /* Release 版下面两个通道都不用 level 时消除警告 */

    /* 先取时间戳, 两个通道都要用 */
    KeQuerySystemTime(&ts);

    /* ---------- 通道 1: DbgPrintEx (仅 Debug 版) ---------- */
#ifdef DBG
    va_start(ap, fmt);
    {
        char dbg[FW_MSG_MAX];   /* 栈上临时缓冲, 不占内核池 */

        /* 把变参按 fmt 格式化进 dbg */
        RtlStringCchVPrintfA(dbg, FW_MSG_MAX, fmt, ap);

        /* 输出到调试器。级别决定是否被过滤掉。
         * 最终形如: [game_base][1234567.890][WRN] resolve failed: C0000024 */
        if (level < ARRAYSIZE(g_DpfltrLevel)) {
            DbgPrintEx(DPFLTR_IHVDRIVER_ID,
                       g_DpfltrLevel[level],
                       "[%s][%lu.%03lu][%s] %s\n",
                       GAME_LOG_TAG,
                       (ULONG)(ts.QuadPart / 10000000),
                       (ULONG)((ts.QuadPart / 10000) % 1000),
                       LevelName(level), dbg);
        }
    }
    va_end(ap);
#endif

    /* ---------- 通道 2: 环形缓冲 (所有版本) ---------- */
    RtlZeroMemory(&e, sizeof(e));

    e.Ts    = (ULONGLONG)ts.QuadPart;
    e.Level = level;

    /* 固定前缀: 调用方不传, 这里写死 */
    RtlStringCchCopyA(e.Tag, FW_MOD_MAX, GAME_LOG_TAG);

    /* 消息: 重新开始变参遍历 (第一次 va_start 已在上面 va_end 结束) */
    va_start(ap, fmt);
    RtlStringCchVPrintfA(e.Msg, FW_MSG_MAX, fmt, ap);
    va_end(ap);

    /* 加锁后写入。用自旋锁是因为可能在不同 IRQL 上被并发调用 */
    KeAcquireSpinLock(&g_Lock, &irql);
    idx = g_Head;
    g_Entries[idx] = e;
    g_Head = (idx + 1) % FW_LOG_MAX;        /* 环形前进一格, 越界回绕到 0 */
    if (g_Count < FW_LOG_MAX) g_Count++;    /* 写满之前一直累加, 满了停在 1024 */
    KeReleaseSpinLock(&g_Lock, irql);
}

/* ==================== 读取环形缓冲 ==================== */

/*
 * LogDump - 把环形缓冲里的记录按时间顺序拷到 out
 *
 * 参数:
 *   out         调用方提供的输出结构 (3 环 IOCTL 的 SystemBuffer)
 *   maxEntries  最多拷贝多少条 (由 3 环给的缓冲区大小算出)
 *
 * 返回: 实际拷贝的条数
 *
 * 环形缓冲里数据的排列是"环形"的:
 *   最旧的记录正好在 g_Head 处 (写满一圈后), 拷贝时从那里开始,
 *   保证调用方拿到的是时间正序。
 */
ULONG LogDump(LOG_DUMP *out, ULONG maxEntries)
{
    KIRQL irql;
    ULONG n = 0, total, start, i;

    if (out == NULL || maxEntries == 0) return 0;
    if (maxEntries > FW_LOG_MAX) maxEntries = FW_LOG_MAX;

    KeAcquireSpinLock(&g_Lock, &irql);

    /* total = min(已写入条数, 调用方能接收的条数) */
    total = (ULONG)g_Count;
    if (total > maxEntries) total = maxEntries;

    /* 最旧的一条的下标:
     *   还没写满时最旧的在 0
     *   写满后最旧的正好在 g_Head 处
     * 统一算法: (head - total + MAX) % MAX, 加 MAX 是为了避免负数 */
    start = (g_Head - total + FW_LOG_MAX) % FW_LOG_MAX;

    for (i = 0; i < total; i++) {
        out->Entries[i] = g_Entries[(start + i) % FW_LOG_MAX];
        n++;
    }
    out->Count = n;

    KeReleaseSpinLock(&g_Lock, irql);
    return n;
}
