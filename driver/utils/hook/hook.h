/*
 * hook.h - game_base 通用内核 hook 框架 (接口定义)
 *
 * ==================================================================
 * 设计目标
 * ==================================================================
 * 把"怎么 hook"和"hook 什么"分开:
 *   - 本模块只负责底层 hook 机制 (派发表 / inline / IAT)
 *   - 业务模块 (features/*) 只说"我要 hook 这个目标, 用我给的 handler"
 *
 * 参考:
 *   - smallzhong/KernelHook  (hook_record 链表 + hook_by_addr/reset_hook)
 *   - HoShiMin/HookLib       (trampoline 选择逻辑 + Zydis 指令长度)
 *   - ACE-CORE3              ([hook] install ok slots=%u hook 链)
 *
 * ==================================================================
 * 当前支持的 hook 类型
 * ==================================================================
 *   HOOK_DISPATCH  替换 DriverObject->MajorFunction[] (已实现)
 *                  - 最简单, 不需要改代码, 不触发 PatchGuard
 *                  - 只能拦该驱动对象的 IRP 入口
 *                  - ACE 的 DoIrpHook / 我们的 feat_usb 用它
 *
 *   HOOK_INLINE    改目标函数开头几字节为 jmp (待实现)
 *                  - 能拦任意内核函数, 包括驱动内部函数
 *                  - 需要: 指令长度解码 + trampoline + 内存写保护绕过
 *                  - 风险: 触发 PatchGuard (非调试启动时)
 *
 *   HOOK_IAT       改导入地址表 (待实现)
 *                  - 只影响调用方模块自己的导入
 *                  - 对内核模块用处有限, 优先级最低
 *
 * ==================================================================
 * 生命周期
 * ==================================================================
 *   HookInstall(&entry, HOOK_DISPATCH, targetDrvObj, myHandler)
 *       -> entry.Original 填好, 原来的派发函数能从 Original 调回去
 *
 *   ... 运行期拦截 ...
 *
 *   HookRemove(&entry)
 *       -> 恢复原状, 释放内部资源
 *
 *   驱动卸载前必须把安装过的 hook 全部 HookRemove, 否则:
 *   - 派发表还指着已卸载驱动的代码 -> 蓝屏
 *   - inline hook 还跳向已释放内存 -> 蓝屏
 */
#pragma once
#include <ntddk.h>

/* ==================================================================
 * hook 类型
 * ================================================================== */
typedef enum _HOOK_TYPE {
    HOOK_DISPATCH = 0,      /* 驱动对象派发表替换 (已实现)          */
    HOOK_INLINE   = 1,      /* 代码前几字节 inline hook (待实现)    */
    HOOK_IAT      = 2,      /* 导入地址表替换 (待实现)              */
} HOOK_TYPE;

/* ==================================================================
 * 单个 hook 的描述
 * ==================================================================
 * 由调用方分配 (通常放在 feature 模块的静态区), 框架不负责释放。
 * 同一时刻只能描述一个 hook —— 要 hook 多个目标就定义多个 HOOK_ENTRY。
 */
typedef struct _HOOK_ENTRY {
    /* ---- 链表节点 ----
     * 框架内部把已安装的 hook 串起来, 用于:
     *   1. 卸载时统一检查
     *   2. inline hook 查找"这个地址已经被谁 hook 过" (串 hook 链)
     * 调用方不要直接操作这个字段。 */
    LIST_ENTRY          List;

    /* ---- hook 类型 ---- */
    HOOK_TYPE           Type;

    /* ---- 目标 ----
     * HOOK_DISPATCH: 目标驱动的 DRIVER_OBJECT 指针
     * HOOK_INLINE:   目标函数的地址
     * HOOK_IAT:      目标模块基址 (后续再定) */
    PVOID               Target;

    /* ---- 我们的处理函数 ----
     * HOOK_DISPATCH: PDRIVER_DISPATCH (同 MajorFunction[])
     * HOOK_INLINE:   与目标函数同签名的函数指针 */
    PVOID               Handler;

    /* ---- 原始入口 (HookInstall 成功后填好) ----
     * HOOK_DISPATCH: 原来的 MajorFunction[index]
     * HOOK_INLINE:   trampoline 地址 (调用它就等于调原函数)
     *
     * 业务代码在自己的 handler 里用这个指针调回原函数, 例如:
     *     return ((PDRIVER_DISPATCH)e->Original)(devObj, irp);
     */
    PVOID               Original;

    /* ---- 派发表专用: 要替换的索引 ----
     * 取值范围 0 ~ IRP_MJ_MAXIMUM_FUNCTION-1,
     * 常用: IRP_MJ_PNP(27) / IRP_MJ_CREATE(0) / IRP_MJ_DEVICE_CONTROL(14) */
    ULONG               DispatchIndex;

    /* ---- inline hook 专用: 被覆盖的原字节 ----
     * HOOK_INLINE 会改目标函数开头, 为了能恢复必须备份。
     * 长度取决于要插入的 jmp 指令大小 (通常 5~14 字节) */
    UCHAR               Backup[16];
    ULONG               BackupLen;

    /* ---- 状态 ---- */
    BOOLEAN             Active;         /* TRUE = 已安装 */

} HOOK_ENTRY;

/* ==================================================================
 * 安装 hook
 * ==================================================================
 * 参数:
 *   entry     调用方提供的 HOOK_ENTRY, 函数会把 Target/Handler 之外
 *             的字段填好 (Original / Active / Backup 等)
 *   type      hook 类型
 *   target    目标 (见 HOOK_ENTRY.Target 说明)
 *   handler   我们的处理函数 (见 HOOK_ENTRY.Handler 说明)
 *   aux       额外参数:
 *                HOOK_DISPATCH: MajorFunction 索引 (转成 ULONG 传)
 *                HOOK_INLINE:   保留, 传 0
 *
 * 返回:
 *   STATUS_SUCCESS          安装成功, entry.Active = TRUE
 *   STATUS_INVALID_PARAMETER 参数非法
 *   STATUS_NOT_FOUND         HOOK_INLINE: 目标地址无法定位
 *   STATUS_NOT_SUPPORTED     该 hook 类型还没实现
 *   其他 NTSTATUS
 *
 * 注意:
 *   - 必须在 PASSIVE_LEVEL 调用 (inline hook 需要改 PTE)
 *   - 同一个 target 可以被多次安装 (hook 链), 卸载时按 entry 独立性恢复
 */
NTSTATUS HookInstall(_Inout_ HOOK_ENTRY *entry,
                     _In_ HOOK_TYPE type,
                     _In_ PVOID target,
                     _In_ PVOID handler,
                     _In_ ULONG_PTR aux);

/* ==================================================================
 * 卸载 hook
 * ==================================================================
 * 恢复原状并清空 entry。
 * 若 entry 从未安装 (Active == FALSE), 静默返回。
 *
 * 必须在 PASSIVE_LEVEL 调用。
 * 驱动卸载前必须对每个安装过的 entry 调一次。
 */
VOID HookRemove(_Inout_ HOOK_ENTRY *entry);

/* ==================================================================
 * 查已安装的 hook (供 inline hook 串链用)
 * ==================================================================
 * 返回挂在 target 上的第一个 hook 的 Original (即最底层的原函数),
 * 没有找到返回 NULL。
 */
PVOID HookFindOriginal(_In_ PVOID target);

/* ==================================================================
 * 框架内部: 初始化 / 清理
 * ==================================================================
 * DriverEntry 里调用 HookInit, DriverUnload 里调用 HookUninit。
 * HookUninit 会自动卸载所有还活着的 hook (防御性, 正常流程应该已经卸完)。
 */
NTSTATUS HookInit(VOID);
VOID     HookUninit(VOID);
