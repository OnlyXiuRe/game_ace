/*
 * fw_log.c - 环形日志 (内核侧收集, 用户态通过 IOCTL 拉取)
 *
 * 目的: 不依赖 DbgView/WinDbg, 用户在 3 环就能读驱动日志。
 */
#include "fw.h"
#include <ntstrsafe.h>

static FW_LOG_ENTRY g_Log[FW_LOG_MAX];
static volatile LONG g_LogHead = 0;    /* 下一个写入位置 */
static volatile LONG g_LogCount = 0;   /* 已写入总数 (可超过 FW_LOG_MAX) */
static KSPIN_LOCK g_LogLock;

VOID FwLogInit(VOID)
{
    KeInitializeSpinLock(&g_LogLock);
    RtlZeroMemory(g_Log, sizeof(g_Log));
    g_LogHead = 0;
    g_LogCount = 0;
}

VOID FwLog(ULONG level, const CHAR *mod, const CHAR *fmt, ...)
{
    FW_LOG_ENTRY e;
    LARGE_INTEGER ts;
    va_list ap;
    KIRQL irql;
    LONG idx;

    RtlZeroMemory(&e, sizeof(e));
    KeQuerySystemTime(&ts);
    e.Ts = (ULONGLONG)ts.QuadPart;
    e.Level = level;
    RtlStringCchCopyA(e.Module, FW_MOD_MAX, mod ? mod : "?");

    va_start(ap, fmt);
    RtlStringCchVPrintfA(e.Msg, FW_MSG_MAX, fmt, ap);
    va_end(ap);

    KeAcquireSpinLock(&g_LogLock, &irql);
    idx = g_LogHead;
    g_Log[idx] = e;
    g_LogHead = (idx + 1) % FW_LOG_MAX;
    if (g_LogCount < FW_LOG_MAX) g_LogCount++;
    KeReleaseSpinLock(&g_LogLock, irql);
}

ULONG FwLogDump(FW_LOG_DUMP *out, ULONG max)
{
    KIRQL irql;
    ULONG n = 0, total, start, i;

    if (out == NULL || max == 0) return 0;
    if (max > FW_LOG_MAX) max = FW_LOG_MAX;

    KeAcquireSpinLock(&g_LogLock, &irql);
    total = (ULONG)g_LogCount;
    if (total > FW_LOG_MAX) total = FW_LOG_MAX;
    if (total > max) total = max;

    /* 从最旧的一条开始 */
    start = (g_LogHead - total + FW_LOG_MAX) % FW_LOG_MAX;
    for (i = 0; i < total; i++) {
        out->Entries[i] = g_Log[(start + i) % FW_LOG_MAX];
        n++;
    }
    out->Count = n;
    KeReleaseSpinLock(&g_LogLock, irql);
    return n;
}

VOID FwLogClear(VOID)
{
    KIRQL irql;
    KeAcquireSpinLock(&g_LogLock, &irql);
    RtlZeroMemory(g_Log, sizeof(g_Log));
    g_LogHead = 0;
    g_LogCount = 0;
    KeReleaseSpinLock(&g_LogLock, irql);
}
