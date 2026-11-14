/*
 * hook_inline.c - HOOK_INLINE 骨架 (待实现)
 *
 * ==================================================================
 * 原理
 * ==================================================================
 * 把目标函数开头 N 字节改成一个 jmp, 跳到我们的 handler:
 *
 *   目标函数 (hook 前):
 *     0x140001000  48 89 5C 24 08   mov [rsp+8], rbx    ← 被覆盖
 *     0x140001005  57               push rdi            ← 被覆盖
 *     0x140001006  48 83 EC 20      sub rsp, 0x20       ← 可能被覆盖
 *     0x14000100A  ...              函数正文
 *
 *   目标函数 (hook 后):
 *     0x140001000  E9 xx xx xx xx   jmp MyHandler       ← 5 字节
 *     0x140001005  ?? ??            -- 原指令的剩余部分 (不能用了)
 *
 *   同时把被覆盖的原指令拷到一块可执行内存 (trampoline), 后面接一个
 *   jmp 回 0x140001000+N, 这样调 trampoline 就等于调原函数:
 *
 *   trampoline:
 *     48 89 5C 24 08   mov [rsp+8], rbx      ← 原指令副本
 *     57               push rdi
 *     48 83 EC 20      sub rsp, 0x20
 *     E9 xx xx xx xx   jmp 0x14000100A         ← 跳回函数正文
 *
 * ==================================================================
 * 实现步骤 (按顺序)
 * ==================================================================
 *   1. 计算需要覆盖的字节数 N
 *      - x64 相对 jmp 是 5 字节 (E9 + rel32, ±2GB)
 *      - 如果目标离 handler 超过 2GB, 需要 14 字节的绝对跳转:
 *          FF 25 00 00 00 00       jmp [rip+0]
 *          <8 字节绝对地址>
 *      - N 必须 >= 指令长度, 且不能把一条指令劈成两半
 *      -> 需要指令长度解码器 (Zydis / hde64)
 *
 *   2. 分配 trampoline 内存
 *      - 必须可执行
 *      - ExAllocatePoolWithTag(NonPagedPoolExecute, ...)  (Win8+)
 *      - 或 MmAllocateContiguousMemory + 改 PTE 的 NX 位
 *
 *   3. 拷贝原指令到 trampoline, 末尾补 jmp 回 target+N
 *
 *   4. 备份 target 前 N 字节到 entry->Backup (用于恢复)
 *
 *   5. 关写保护, 把 target 前 N 字节改成 jmp handler, 开写保护
 *      - x64 的 .text 段是只读的 (PTE 的 WP 位), 直接写会蓝屏
 *      - 方法A: 改 CR0 的 WP 位 (经典做法, 但 PatchGuard 可能检测)
 *      - 方法B: 找到对应的 PTE, 直接改 PTE 的可写位
 *
 *   6. 刷 TLB (__invlpg) 让新指令生效
 *
 *   7. 填 entry (Original = trampoline 地址)
 *
 * ==================================================================
 * 风险
 * ==================================================================
 *   - PatchGuard (KPP) 会周期性校验内核代码段, 发现被改会 BSOD 0x109
 *     只有"启用调试程序"启动项 (bcdedit /debug on) 时 KPP 不初始化
 *   - 被覆盖的指令如果包含 rip 相对寻址, 拷到 trampoline 后必须重定位
 *   - 多核并发执行时, 改指令的瞬间别的核可能正在执行那几条指令
 *     -> 需要跨核同步 (KeIpiGenericCall)
 */
#include "hook.h"
#include "game_log.h"

/* 从 hook.c 引入的链表和锁 */
extern LIST_ENTRY  g_HookList;
extern KSPIN_LOCK  g_HookLock;

/* ==================================================================
 * 下面的工具函数都需要你来实现
 * ================================================================== */

/*
 * GetInstructionLength - 计算从 addr 开始的一条指令的字节数
 *
 * 这是 inline hook 最基础的能力, 没有它无法安全地覆盖指令。
 *
 * 可选方案:
 *   A. 集成 Zydis (smallzhong/KernelHook 用的)
 *      优点: 完整 x86/x64 解码, 支持所有指令
 *      缺点: 117KB 源码 + 400KB 头文件, 驱动体积暴涨
 *
 *   B. 用 hde64 (HookLib 用的, 单文件 ~1.5K 行)
 *      优点: 极小, 够用
 *      缺点: 只支持 32/64 位常用指令
 *
 *   C. 自己写最小实现
 *      只处理 prologue 常见指令 (push/mov/sub/lea), 遇到不认识的失败
 *      优点: 可控
 *      缺点: 覆盖面窄, 遇到罕见指令会失败
 *
 * 返回: 指令长度 (1~15), 失败返回 0
 */
static ULONG GetInstructionLength(_In_ const UCHAR *addr)
{
    UNREFERENCED_PARAMETER(addr);
    /* TODO: 实现指令长度解码 */
    return 0;
}

/*
 * CalcPatchSize - 计算需要覆盖多少字节才能放下跳转
 *
 * 参数:
 *   addr      目标函数地址
 *   handler   我们的 handler 地址
 *   outSize   输出: 需要覆盖的字节数 (每条完整指令累加)
 *   outTrampLen 输出: trampoline 需要的长度 (原指令副本 + 跳回指令)
 *
 * 逻辑:
 *   1. 判断 addr 和 handler 距离是否在 ±2GB 内
 *      - 是: 用 5 字节 E9 相对跳转
 *      - 否: 用 14 字节 FF 25 绝对跳转
 *   2. 从 addr 开始逐条解码指令, 累加长度直到 >= 跳转指令大小
 *   3. 注意: 累加过程中不能把一条指令劈开
 *
 * 返回: STATUS_SUCCESS 或错误
 */
static NTSTATUS CalcPatchSize(_In_ PVOID addr, _In_ PVOID handler,
                              _Out_ ULONG *outSize, _Out_ ULONG *outTrampLen)
{
    UNREFERENCED_PARAMETER(addr);
    UNREFERENCED_PARAMETER(handler);
    *outSize = 0;
    *outTrampLen = 0;
    /* TODO: 实现 */
    return STATUS_NOT_IMPLEMENTED;
}

/*
 * AllocExecMemory - 分配一块可执行内存放 trampoline
 *
 * 方案 A (推荐, Win8+):
 *     ExAllocatePoolWithTag(NonPagedPoolExecute, size, 'xecH')
 *
 * 方案 B (兼容旧系统):
 *     MmAllocateContiguousMemory(size, maxPhysAddr)
 *     + 找到 PTE, 清 NX 位
 *     + __invlpg 刷 TLB
 *
 * 返回: 可执行内存地址, 失败返回 NULL
 */
static PVOID AllocExecMemory(_In_ ULONG size)
{
    UNREFERENCED_PARAMETER(size);
    /* TODO: 实现 */
    return NULL;
}

/*
 * FreeExecMemory - 释放 AllocExecMemory 分配的内存
 */
static VOID FreeExecMemory(_In_ PVOID mem)
{
    UNREFERENCED_PARAMETER(mem);
    /* TODO: 实现 */
}

/*
 * WriteKernelMemory - 绕过写保护写内核内存
 *
 * .text 段在 x64 上是只读的, 直接 memcpy 会触发 0xBE (ATTEMPTED_WRITE_TO_READONLY_MEMORY)。
 *
 * 方案 A: 改 CR0 的 WP 位 (第 16 位)
 *     ULONG64 cr0 = __readcr0();
 *     __writecr0(cr0 & ~(1ULL << 16));
 *     ... 写内存 ...
 *     __writecr0(cr0);
 *   注意: 多核上必须每个核都做, 或者用 KeIpiGenericCall 广播
 *
 * 方案 B: 找到 PTE, 直接改 PTE 的 RW 位
 *     更隐蔽, 但需要页表遍历
 *
 * 返回: TRUE 成功
 */
static BOOLEAN WriteKernelMemory(_In_ PVOID dest, _In_ const VOID *src, _In_ ULONG size)
{
    UNREFERENCED_PARAMETER(dest);
    UNREFERENCED_PARAMETER(src);
    UNREFERENCED_PARAMETER(size);
    /* TODO: 实现 */
    return FALSE;
}

/* ==================================================================
 * 对外接口 (等你实现上面的工具函数后这里就能跑通)
 * ================================================================== */

/*
 * HookInlineInstall - 安装 inline hook
 *
 * 当前实现: 返回 STATUS_NOT_SUPPORTED, 等你补完上面的工具函数。
 */
NTSTATUS HookInlineInstall(HOOK_ENTRY *entry, PVOID target, PVOID handler)
{
    UNREFERENCED_PARAMETER(entry);
    UNREFERENCED_PARAMETER(target);
    UNREFERENCED_PARAMETER(handler);

    LOG_WARN("HookInlineInstall: not implemented yet");
    return STATUS_NOT_SUPPORTED;

    /* 实现顺序:
     *
     * 1. ULONG patchSize, trampLen;
     *    st = CalcPatchSize(target, handler, &patchSize, &trampLen);
     *    if (!NT_SUCCESS(st)) return st;
     *
     * 2. PVOID tramp = AllocExecMemory(trampLen);
     *    if (!tramp) return STATUS_INSUFFICIENT_RESOURCES;
     *
     * 3. RtlCopyMemory(tramp, target, patchSize);
     *    // 末尾补 jmp 回 target+patchSize
     *    // (需要用 GetInstructionLength 重定位 rip 相对指令)
     *
     * 4. RtlCopyMemory(entry->Backup, target, patchSize);
     *    entry->BackupLen = patchSize;
     *
     * 5. 生成 jmp 到 handler 的字节序列, 用 WriteKernelMemory 写进去
     *
     * 6. KeIpiGenericCall 广播 __invlpg (或者直接 __invlpg)
     *
     * 7. entry->Type     = HOOK_INLINE;
     *    entry->Target   = target;
     *    entry->Handler  = handler;
     *    entry->Original = tramp;      // 业务代码用这个调原函数
     *    entry->Active   = TRUE;
     */
}

/*
 * HookInlineRemove - 卸载 inline hook
 *
 * 把 entry->Backup 里的原字节写回 target, 释放 trampoline。
 */
VOID HookInlineRemove(HOOK_ENTRY *entry)
{
    UNREFERENCED_PARAMETER(entry);
    /* TODO:
     * 1. if (!entry->Active || entry->Type != HOOK_INLINE) return;
     * 2. WriteKernelMemory(entry->Target, entry->Backup, entry->BackupLen);
     * 3. __invlpg(entry->Target);
     * 4. FreeExecMemory(entry->Original);
     * 5. entry->Active = FALSE;
     */
}
