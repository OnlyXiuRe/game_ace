/*
 * game_log.c - game_base 内核日志实现
 *
 * 命名约定:
 *   Gl  = Game Logger        (本模块前缀)
 *   GL_ = 宏/常量前缀        (GL_INFO / GL_LVL_*)
 *
 * 双通道输出:
 *   通道1 DbgPrintEx   - 按级别过滤, DebugView / WinDbg 可见 (仅 DBG 版)
 *   通道2 环形缓冲     - 1024 条固定大小记录, 3 环用 IOCTL 拉取 (所有版本)
 */
#include "game_log.h"
#include <ntstrsafe.h>

/* ==================== 内部状态 ==================== */

/* 环形缓冲本体: 固定 1024 条, 满了覆盖最旧的 */
static GL_ENTRY        g_Entries[FW_LOG_MAX];

/* 下一个要写入的下标 (0 ~ FW_LOG_MAX-1, 循环) */
static volatile LONG   g_Head = 0;

/* 已写入的总条数, 用于判断缓冲区是否已经写满一轮
 * 未满时 = 实际条数; 满了之后固定为 FW_LOG_MAX */
static volatile LONG   g_Count = 0;

/* 保护 g_Entries / g_Head / g_Count 的自旋锁
 * 驱动可能在多核上并发调用 GlWrite, 必须加锁 */
static KSPIN_LOCK      g_Lock;

/*
 * GL 级别 -> DbgPrintEx 过滤级别 的映射表
 *
 * DbgPrintEx 的过滤级别数值越小越"严重":
 *   ERROR(0) < WARNING(1) < TRACE(2) < INFO(3)
 *
 * DebugView 里可以设过滤条件, 只显示某个级别以下的输出。
 * 比如设为 WARNING 级别, 就看不到 INFO 和 TRACE 的输出。
 */
static const ULONG g_DpfltrLevel[] = {
    DPFLTR_INFO_LEVEL,      /* 下标 0 = GL_LVL_INFO  */
    DPFLTR_WARNING_LEVEL,   /* 下标 1 = GL_LVL_WARN  */
    DPFLTR_ERROR_LEVEL,     /* 下标 2 = GL_LVL_ERR   */
    DPFLTR_TRACE_LEVEL      /* 下标 3 = GL_LVL_DEBUG */
};

/* ==================== 初始化 / 清理 ==================== */

/* 清空环形缓冲并初始化自旋锁。必须在 DriverEntry 最开头调用。 */
VOID GlInit(VOID)
{
    KeInitializeSpinLock(&g_Lock);
    RtlZeroMemory(g_Entries, sizeof(g_Entries));
    g_Head = 0;
    g_Count = 0;
}

/* 运行时清空日志 (对应 IOCTL_FW_LOG_CLEAR), 驱动保持加载状态 */
VOID GlClear(VOID)
{
    KIRQL irql;
    KeAcquireSpinLock(&g_Lock, &irql);
    RtlZeroMemory(g_Entries, sizeof(g_Entries));
    g_Head = 0;
    g_Count = 0;
    KeReleaseSpinLock(&g_Lock, irql);
}

/* ==================== 辅助 ==================== */

/* 级别 -> 3 字符短名, 用于日志前缀 "[game_base][INF][usb]" */
static const char *LevelName(ULONG level)
{
    switch (level) {
    case GL_LVL_INFO:  return "INF";    /* 普通信息   */
    case GL_LVL_WARN:  return "WRN";    /* 警告       */
    case GL_LVL_ERR:   return "ERR";    /* 错误       */
    case GL_LVL_DEBUG: return "DBG";    /* 调试细节   */
    default:           return "???";
    }
}

/* ==================== 核心写入函数 ==================== */

/*
 * GlWrite - 所有日志宏最终都走到这里
 *
 * 参数:
 *   level  日志级别 (GL_LVL_*), 决定 DbgPrintEx 的过滤级别 + 记录里的标记位
 *   tag    模块标签, 如 "usb" / "hook", 通过 __FUNCTION__ 自动传入
 *   fmt    printf 风格格式串, 如 "device %p failed: %08X"
 *   ...    变参, 对应 fmt 里的占位符
 *
 * 内部做两件事:
 *   1. (仅 DBG 版) 格式化后调用 DbgPrintEx 输出到调试器
 *   2. (所有版本) 格式化后写入环形缓冲, 供 3 环读取
 *
 * 注意: 因为要对同一组变参格式化两次, 必须用 va_start/va_end 包裹两次,
 *       C 语言的 va_list 遍历一次后就失效了。
 */
VOID GlWrite(ULONG level, const char *tag, const char *fmt, ...)
{
    GL_ENTRY      e;        /* 待写入的一条记录 */
    LARGE_INTEGER ts;       /* 系统时间戳 (100ns 单位) */
    va_list       ap;       /* 变参游标 */
    KIRQL         irql;     /* 加锁前的中断级别, 解锁时恢复 */
    LONG          idx;      /* 本次写入的下标 */

    /* ---------- 通道 1: DbgPrintEx (仅 Debug 版) ---------- */
#ifdef DBG
    va_start(ap, fmt);
    {
        char dbg[FW_MSG_MAX];   /* 栈上临时缓冲, 不占内核池 */

        /* 把变参按 fmt 格式化进 dbg */
        RtlStringCchVPrintfA(dbg, FW_MSG_MAX, fmt, ap);

        /* 输出到调试器。级别决定是否被过滤掉。
         * 最终形如: [game_base][WRN][usb] device 0xFFFF... failed: C0000001 */
        if (level < ARRAYSIZE(g_DpfltrLevel)) {
            DbgPrintEx(DPFLTR_IHVDRIVER_ID,
                       g_DpfltrLevel[level],
                       "[game_base][%s][%s] %s\n",
                       LevelName(level), tag ? tag : "?", dbg);
        }
    }
    va_end(ap);
#endif

    /* ---------- 通道 2: 环形缓冲 (所有版本) ---------- */
    RtlZeroMemory(&e, sizeof(e));

    /* 时间戳: KeQuerySystemTime 返回 1601-01-01 起的 100ns 计数 */
    KeQuerySystemTime(&ts);
    e.Ts    = (ULONGLONG)ts.QuadPart;
    e.Level = level;

    /* 标签: 限长拷贝, 超长截断, 保证以 \0 结尾 */
    RtlStringCchCopyA(e.Tag, FW_MOD_MAX, tag ? tag : "?");

    /* 消息: 重新开始变参遍历 (第一次 va_start 已在上面 va_end 结束) */
    va_start(ap, fmt);
    RtlStringCchVPrintfA(e.Msg, FW_MSG_MAX, fmt, ap);
    va_end(ap);

    /* 加锁后写入。用自旋锁是因为可能在不同 IRQL 上被并发调用 */
    KeAcquireSpinLock(&g_Lock, &irql);
    idx = g_Head;
    g_Entries[idx] = e;
    g_Head = (idx + 1) % FW_LOG_MAX;        /* 环形前进一格, 越界回绕到 0 */
    if (g_Count < FW_LOG_MAX) g_Count++;    /* 写满之前一直累加, 满了就停在 1024 */
    KeReleaseSpinLock(&g_Lock, irql);
}

/* ==================== 读取环形缓冲 ==================== */

/*
 * GlDump - 把环形缓冲里的记录按时间顺序拷到 out
 *
 * 参数:
 *   out         调用方提供的输出结构 (3 环 IOCTL 的 SystemBuffer)
 *   maxEntries  最多拷贝多少条 (由 3 环给的缓冲区大小算出)
 *
 * 返回: 实际拷贝的条数
 *
 * 环形缓冲里数据的排列是"环形"的:
 *   head 指向下一个写入位置, 所以最旧的记录在 head 的位置
 *   (当缓冲写满一圈后), 拷贝时要从最旧开始, 保证调用方拿到的是时间正序。
 */
ULONG GlDump(GL_DUMP *out, ULONG maxEntries)
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
     *   缓冲还没写满时 g_Head 就是条数, 最旧的在 0
     *   写满后最旧的正好在 g_Head 处
     * 统一算法: (head - total + MAX) % MAX 避免负数 */
    start = (g_Head - total + FW_LOG_MAX) % FW_LOG_MAX;

    for (i = 0; i < total; i++) {
        out->Entries[i] = g_Entries[(start + i) % FW_LOG_MAX];
        n++;
    }
    out->Count = n;

    KeReleaseSpinLock(&g_Lock, irql);
    return n;
}
