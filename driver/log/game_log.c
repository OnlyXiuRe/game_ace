/*
 * game_log.c - game_base 内核日志实现
 *
 * 输出:
 *   DbgPrintEx  - 按级别过滤, DebugView / WinDbg 可见 (仅 debug 版)
 *   环形缓冲    - 1024 条, 3 环通过 IOCTL 拉取
 */
#include "game_log.h"
#include <ntstrsafe.h>

static GL_ENTRY        g_Entries[FW_LOG_MAX];
static volatile LONG   g_Head = 0;      /* 下一个写入位置 */
static volatile LONG   g_Count = 0;     /* 已写入总数 (可超过 FW_LOG_MAX) */
static KSPIN_LOCK      g_Lock;

/*
 * DbgPrintEx 过滤级别映射
 *   数值越小越严重: ERROR(0) < WARNING(1) < TRACE(2) < INFO(3)
 *   DebugView 里可以按 DPFLTR_* 过滤
 */
static const ULONG g_DpfltrLevel[] = {
    DPFLTR_INFO_LEVEL,      /* GL_LVL_INFO  */
    DPFLTR_WARNING_LEVEL,   /* GL_LVL_WARN  */
    DPFLTR_ERROR_LEVEL,     /* GL_LVL_ERR   */
    DPFLTR_TRACE_LEVEL      /* GL_LVL_DEBUG */
};

VOID GlInit(VOID)
{
    KeInitializeSpinLock(&g_Lock);
    RtlZeroMemory(g_Entries, sizeof(g_Entries));
    g_Head = 0;
    g_Count = 0;
}

VOID GlClear(VOID)
{
    KIRQL irql;
    KeAcquireSpinLock(&g_Lock, &irql);
    RtlZeroMemory(g_Entries, sizeof(g_Entries));
    g_Head = 0;
    g_Count = 0;
    KeReleaseSpinLock(&g_Lock, irql);
}

static const char *LevelName(ULONG level)
{
    switch (level) {
    case GL_LVL_INFO:  return "INF";
    case GL_LVL_WARN:  return "WRN";
    case GL_LVL_ERR:   return "ERR";
    case GL_LVL_DEBUG: return "DBG";
    default:           return "???";
    }
}

VOID GlWrite(ULONG level, const char *tag, const char *fmt, ...)
{
    GL_ENTRY e;
    LARGE_INTEGER ts;
    va_list ap;
    KIRQL irql;
    LONG idx;

#ifdef DBG
    /* ---- 通道1: DbgPrintEx, 按级别过滤 ---- */
    va_start(ap, fmt);
    {
        char dbg[FW_MSG_MAX];
        RtlStringCchVPrintfA(dbg, FW_MSG_MAX, fmt, ap);

        if (level < ARRAYSIZE(g_DpfltrLevel)) {
            DbgPrintEx(DPFLTR_IHVDRIVER_ID,
                       g_DpfltrLevel[level],
                       "[game_base][%s][%s] %s\n",
                       LevelName(level), tag ? tag : "?", dbg);
        }
    }
    va_end(ap);
#endif

    /* ---- 通道2: 环形缓冲 (release 版也保留, 供 3 环拉取) ---- */
    RtlZeroMemory(&e, sizeof(e));
    KeQuerySystemTime(&ts);
    e.Ts = (ULONGLONG)ts.QuadPart;
    e.Level = level;
    RtlStringCchCopyA(e.Tag, FW_MOD_MAX, tag ? tag : "?");

    va_start(ap, fmt);
    RtlStringCchVPrintfA(e.Msg, FW_MSG_MAX, fmt, ap);
    va_end(ap);

    KeAcquireSpinLock(&g_Lock, &irql);
    idx = g_Head;
    g_Entries[idx] = e;
    g_Head = (idx + 1) % FW_LOG_MAX;
    if (g_Count < FW_LOG_MAX) g_Count++;
    KeReleaseSpinLock(&g_Lock, irql);
}

ULONG GlDump(GL_DUMP *out, ULONG maxEntries)
{
    KIRQL irql;
    ULONG n = 0, total, start, i;

    if (out == NULL || maxEntries == 0) return 0;
    if (maxEntries > FW_LOG_MAX) maxEntries = FW_LOG_MAX;

    KeAcquireSpinLock(&g_Lock, &irql);
    total = (ULONG)g_Count;
    if (total > maxEntries) total = maxEntries;

    /* 从最旧的一条开始 */
    start = (g_Head - total + FW_LOG_MAX) % FW_LOG_MAX;
    for (i = 0; i < total; i++) {
        out->Entries[i] = g_Entries[(start + i) % FW_LOG_MAX];
        n++;
    }
    out->Count = n;
    KeReleaseSpinLock(&g_Lock, irql);
    return n;
}
