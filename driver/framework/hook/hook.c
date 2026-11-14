/*
 * hook.c - hook 框架核心 (分发 + 链表管理 + 生命周期)
 *
 * 职责:
 *   - 维护"已安装 hook"的全局链表
 *   - 把 HookInstall 分发到具体类型的实现
 *   - 卸载时按类型分发
 *   - 提供 HookFindOriginal 给 inline hook 串链用
 *   - 驱动卸载时兜底清理所有残留 hook
 *
 * 具体实现:
 *   hook_dispatch.c  HOOK_DISPATCH (已实现)
 *   hook_inline.c    HOOK_INLINE   (骨架, 待实现)
 */
#include "hook.h"
#include "game_log.h"

/* ==================================================================
 * 全局状态
 * ================================================================== */

/* 所有已安装 hook 的链表。用 LIST_ENTRY 串起来, 支持任意多个 hook 并存。
 * 之所以要维护这个链表:
 *   1. HookFindOriginal 需要按 Target 查找
 *   2. HookUninit 兜底清理时要知道还剩哪些没卸
 *   3. 调试时可以枚举所有 hook 状态 */
LIST_ENTRY  g_HookList;

/* 保护 g_HookList。HookInstall/Remove 一般在 PASSIVE_LEVEL,
 * 但为了安全还是加锁 (可能被 feature 的 PnP 回调间接调用)。 */
KSPIN_LOCK  g_HookLock;

/* 框架是否已初始化 */
static BOOLEAN g_HookReady = FALSE;

/* ==================================================================
 * 具体类型的实现 (定义在各自文件里)
 * ================================================================== */

/* hook_dispatch.c */
extern NTSTATUS HookDispatchInstall(HOOK_ENTRY *entry, PVOID drvObj,
                                    ULONG index, PVOID handler);
extern VOID     HookDispatchRemove(HOOK_ENTRY *entry);

/* hook_inline.c */
extern NTSTATUS HookInlineInstall(HOOK_ENTRY *entry, PVOID target, PVOID handler);
extern VOID     HookInlineRemove(HOOK_ENTRY *entry);

/* ==================================================================
 * 初始化 / 清理
 * ================================================================== */

/*
 * HookInit - 初始化 hook 框架
 *
 * 在 DriverEntry 里、安装任何 hook 之前调用。
 * 只做链表头和锁的初始化。
 */
NTSTATUS HookInit(VOID)
{
    InitializeListHead(&g_HookList);
    KeInitializeSpinLock(&g_HookLock);
    g_HookReady = TRUE;
    LOG_INFO("hook framework initialized");
    return STATUS_SUCCESS;
}

/*
 * HookUninit - 卸载所有残留 hook
 *
 * 驱动卸载前调用。正常情况下业务模块应该已经自己 HookRemove 过了,
 * 这里只做兜底: 万一有漏的, 强制恢复, 避免驱动卸载后派发表还指着
 * 已释放的代码导致蓝屏。
 */
VOID HookUninit(VOID)
{
    PLIST_ENTRY  link;
    HOOK_ENTRY  *entry;

    if (!g_HookReady) return;

    LOG_INFO("hook framework uninit, draining remaining hooks...");

    /* 从链表头开始逐个卸载。卸载过程中会把自己从链表摘掉,
     * 所以每次重新取头。 */
    for (;;) {
        KIRQL irql;
        BOOLEAN empty;

        KeAcquireSpinLock(&g_HookLock, &irql);
        empty = IsListEmpty(&g_HookList);
        if (!empty) {
            link = RemoveHeadList(&g_HookList);
            /* CONTAINING_RECORD 从 LIST_ENTRY 反推 HOOK_ENTRY 首地址 */
            entry = CONTAINING_RECORD(link, HOOK_ENTRY, List);
        } else {
            entry = NULL;
        }
        KeReleaseSpinLock(&g_HookLock, irql);

        if (entry == NULL) break;

        /* 链表节点已经被摘掉, 这里只做恢复动作, 不再操作链表 */
        if (entry->Type == HOOK_DISPATCH) {
            HookDispatchRemove(entry);
        } else if (entry->Type == HOOK_INLINE) {
            HookInlineRemove(entry);
        }
        LOG_WARN("drained leftover hook: type=%d target=%p",
                 entry->Type, entry->Target);
    }

    g_HookReady = FALSE;
    LOG_INFO("hook framework uninit done");
}

/* ==================================================================
 * 安装 / 卸载
 * ================================================================== */

/*
 * HookInstall - 安装 hook (统一入口)
 *
 * 参数见 hook.h 的说明:
 *   aux 在 HOOK_DISPATCH 时是 MajorFunction 索引, 其他类型传 0。
 */
NTSTATUS HookInstall(HOOK_ENTRY *entry, HOOK_TYPE type,
                     PVOID target, PVOID handler, ULONG_PTR aux)
{
    NTSTATUS st;

    if (!g_HookReady) {
        LOG_ERROR("HookInstall: framework not initialized");
        return STATUS_UNSUCCESSFUL;
    }
    if (entry == NULL || target == NULL || handler == NULL)
        return STATUS_INVALID_PARAMETER;

    /* 同一个 entry 重复安装会产生悬空 hook, 先拒绝 */
    if (entry->Active) {
        LOG_WARN("HookInstall: entry %p already active", entry);
        return STATUS_ALREADY_COMMITTED;
    }

    RtlZeroMemory(entry, sizeof(*entry));
    InitializeListHead(&entry->List);

    /* ---- 按类型分发 ---- */
    switch (type) {
    case HOOK_DISPATCH:
        st = HookDispatchInstall(entry, target, (ULONG)aux, handler);
        break;

    case HOOK_INLINE:
        st = HookInlineInstall(entry, target, handler);
        break;

    case HOOK_IAT:
    default:
        LOG_ERROR("HookInstall: type %d not supported", type);
        return STATUS_NOT_SUPPORTED;
    }

    if (!NT_SUCCESS(st)) {
        LOG_ERROR("HookInstall failed: type=%d target=%p st=%08X",
                  type, target, st);
        return st;
    }

    /* ---- 挂到全局链表, 方便查找和兜底清理 ---- */
    {
        KIRQL irql;
        KeAcquireSpinLock(&g_HookLock, &irql);
        InsertTailList(&g_HookList, &entry->List);
        KeReleaseSpinLock(&g_HookLock, irql);
    }

    LOG_INFO("HookInstall ok: type=%d target=%p handler=%p orig=%p",
             type, target, handler, entry->Original);
    return STATUS_SUCCESS;
}

/*
 * HookRemove - 卸载 hook
 *
 * 恢复原状, 从全局链表摘除, 清空 entry。
 */
VOID HookRemove(HOOK_ENTRY *entry)
{
    KIRQL irql;

    if (entry == NULL || !entry->Active) return;

    LOG_INFO("HookRemove: type=%d target=%p", entry->Type, entry->Target);

    /* ---- 按类型恢复 ---- */
    switch (entry->Type) {
    case HOOK_DISPATCH:
        HookDispatchRemove(entry);
        break;
    case HOOK_INLINE:
        HookInlineRemove(entry);
        break;
    default:
        LOG_WARN("HookRemove: unknown type %d", entry->Type);
        break;
    }

    /* ---- 从全局链表摘除 ----
     * 注意: 如果是从 HookUninit 里调过来, 节点已经被摘掉了,
     * 这时 IsListLinked 判断会失败, 跳过即可。 */
    KeAcquireSpinLock(&g_HookLock, &irql);
    if (entry->List.Flink != NULL &&
        entry->List.Flink != &entry->List &&
        entry->List.Flink != (PLIST_ENTRY)&entry->List) {
        /* 只有当 Flink 指向的不是自己时才认为是链接状态 */
        RemoveEntryList(&entry->List);
    }
    KeReleaseSpinLock(&g_HookLock, irql);

    entry->Active = FALSE;
}

/*
 * HookFindOriginal - 查某个目标上最底层的原函数
 *
 * 场景: 目标已经被别人 (比如 ACE 或另一个 feature) hook 过,
 *       我们想在它之上再挂一层, 需要拿到最底层的原函数。
 *
 * 实现: 遍历链表找 Target 匹配且 Active 的第一项, 返回它的 Original。
 *
 * 注意: 目前只做简单匹配, 没有处理"同一 target 多层 hook"的完整链式还原。
 *       真正完善的做法是沿着每个 hook 的 Original 继续往前找。
 */
PVOID HookFindOriginal(PVOID target)
{
    PLIST_ENTRY  link;
    HOOK_ENTRY  *entry;
    PVOID        result = NULL;
    KIRQL        irql;

    if (target == NULL) return NULL;

    KeAcquireSpinLock(&g_HookLock, &irql);
    for (link = g_HookList.Flink; link != &g_HookList; link = link->Flink) {
        entry = CONTAINING_RECORD(link, HOOK_ENTRY, List);
        if (entry->Active && entry->Target == target) {
            result = entry->Original;
            break;
        }
    }
    KeReleaseSpinLock(&g_HookLock, irql);

    return result;
}
