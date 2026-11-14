/*
 * hook_dispatch.c - HOOK_DISPATCH 实现 (驱动对象派发表替换)
 *
 * 原理:
 *   每个 DRIVER_OBJECT 有一个 MajorFunction[] 数组, 索引是 IRP 主功能号
 *   (IRP_MJ_CREATE=0, IRP_MJ_PNP=27, ...), 值是处理该 IRP 的函数指针。
 *
 *   把数组里某一项换掉, 该主功能的所有 IRP 就会先到我们的 handler:
 *
 *     I/O 管理器
 *        │  派发 IRP
 *        ▼
 *     DriverObject->MajorFunction[27]  ─── 原来是原函数, 现在是我们的
 *        │
 *        ▼
 *     MyHandler(devObj, irp)
 *        │  做点事 (记录/改参数/直接完成)
 *        ▼
 *     entry->Original(devObj, irp)      ← 调回原函数 (trampoline)
 *
 * 优点:
 *   - 不需要改代码, 不碰内存保护
 *   - 不影响其他驱动
 *   - 不需要指令长度解码
 *
 * 缺点:
 *   - 只能拦这个驱动对象的 IRP 入口
 *   - 拦不到驱动内部函数之间的调用
 *
 * 竞态:
 *   原指针保存和替换必须原子。用 InterlockedExchangePointer 一步完成,
 *   否则中间时刻可能有 IRP 派发到 NULL。
 */
#include "hook.h"
#include "game_log.h"

/* 所有已安装 hook 的链表头 + 保护它的锁。
 * 定义在 hook.c, 这里只声明 */
extern LIST_ENTRY  g_HookList;
extern KSPIN_LOCK  g_HookLock;

/*
 * HookDispatchInstall - 安装派发表 hook
 *
 * 参数:
 *   entry  调用方提供的 HOOK_ENTRY
 *   drvObj 目标驱动对象的 DRIVER_OBJECT 指针
 *   index  MajorFunction 索引 (0 ~ IRP_MJ_MAXIMUM_FUNCTION-1)
 *   handler 我们的处理函数
 *
 * 返回 STATUS_SUCCESS 时 entry->Original 是原派发函数。
 */
NTSTATUS HookDispatchInstall(HOOK_ENTRY *entry, PVOID drvObj,
                             ULONG index, PVOID handler)
{
    PDRIVER_OBJECT drv = (PDRIVER_OBJECT)drvObj;
    PDRIVER_DISPATCH old;

    /* ---- 参数检查 ---- */
    if (entry == NULL || drv == NULL || handler == NULL)
        return STATUS_INVALID_PARAMETER;

    /* MajorFunction 数组有 IRP_MJ_MAXIMUM_FUNCTION 个元素 (28),
     * 合法索引 0 ~ 27。IRP_MJ_PNP = 27 是最后一个。
     * 注意: 不同 WDK 版本里 IRP_MJ_MAXIMUM_FUNCTION 的值可能是 28 或 27,
     *       所以这里用 > 而不是 >=, 并打印实际值便于排查。 */
    LOG_INFO("HookDispatchInstall: index=%lu IRP_MJ_MAXIMUM_FUNCTION=%d",
             index, (int)IRP_MJ_MAXIMUM_FUNCTION);

    if (index > IRP_MJ_MAXIMUM_FUNCTION) {
        LOG_ERROR("HookDispatchInstall: index %lu out of range", index);
        return STATUS_INVALID_PARAMETER;
    }

    /* 目标派发函数必须存在, 否则说明传错了驱动对象或索引 */
    if (drv->MajorFunction[index] == NULL) {
        LOG_WARN("HookDispatchInstall: MajorFunction[%lu] is NULL", index);
        return STATUS_INVALID_PARAMETER;
    }

    /* ---- 原子替换 ----
     * InterlockedExchangePointer 返回替换前的值, 正好就是原函数。
     * 一步完成"保存 + 替换", 中间不会出现空窗。 */
    old = (PDRIVER_DISPATCH)InterlockedExchangePointer(
              (PVOID volatile *)&drv->MajorFunction[index],
              handler);

    if (old == NULL) {
        LOG_ERROR("HookDispatchInstall: atomic exchange failed (index=%lu)", index);
        return STATUS_UNSUCCESSFUL;
    }

    /* ---- 填 entry ---- */
    entry->Type          = HOOK_DISPATCH;
    entry->Target        = drvObj;
    entry->Handler       = handler;
    entry->Original      = (PVOID)old;
    entry->DispatchIndex = index;
    entry->Active        = TRUE;

    LOG_INFO("HookDispatchInstall ok: drv=%p index=%lu orig=%p handler=%p",
             drvObj, index, old, handler);
    return STATUS_SUCCESS;
}

/*
 * HookDispatchRemove - 卸载派发表 hook
 *
 * 把 entry->Original 写回 MajorFunction[index]。
 *
 * 注意: 这里不用 InterlockedExchangePointer, 因为卸载时不会有新的
 *       hook 同时安装到同一位置 (业务逻辑保证)。但为了安全还是用它,
 *       保证写回是原子的。
 */
VOID HookDispatchRemove(HOOK_ENTRY *entry)
{
    PDRIVER_OBJECT drv;
    PVOID prev;

    if (entry == NULL || !entry->Active || entry->Type != HOOK_DISPATCH)
        return;

    drv = (PDRIVER_OBJECT)entry->Target;
    if (drv == NULL) {
        entry->Active = FALSE;
        return;
    }

    /* 只有当前挂的还是我们的 handler 才恢复, 否则可能被别人二次 hook 了,
     * 直接把我们的 Original 写回去会把别人的 hook 冲掉 */
    if ((PVOID)drv->MajorFunction[entry->DispatchIndex] != entry->Handler) {
        LOG_WARN("HookDispatchRemove: slot %lu changed by someone else, skip",
                 entry->DispatchIndex);
        entry->Active = FALSE;
        return;
    }

    prev = InterlockedExchangePointer(
               (PVOID volatile *)&drv->MajorFunction[entry->DispatchIndex],
               entry->Original);

    LOG_INFO("HookDispatchRemove ok: drv=%p index=%lu restored=%p (was %p)",
             drv, entry->DispatchIndex, entry->Original, prev);

    entry->Active   = FALSE;
    entry->Original = NULL;
}
