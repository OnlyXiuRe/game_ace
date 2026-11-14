/*
 * fw_hook.c - 驱动对象解析 (hook 框架的辅助模块)
 *
 * 本文件只负责"按名字拿到目标驱动的 DRIVER_OBJECT", 不做 hook 本身。
 * 具体的 hook 机制在 framework/hook/ 目录:
 *   hook.h           统一接口
 *   hook.c           分发 + 链表 + 生命周期
 *   hook_dispatch.c  派发表替换 (已实现)
 *   hook_inline.c    代码 patch (骨架, 待实现)
 *
 * 为什么把"解析"和"hook"分开:
 *   - 解析依赖 Windows 对象管理器的细节, 各版本行为有差异, 需要多策略回退
 *   - hook 只关心"给我一个目标指针", 不关心怎么拿到的
 *
 * 解析策略 (按顺序尝试):
 *   1. ObReferenceObjectByName(\Driver\XXX, IoDriverObjectType)
 *      - 标准做法, ACE 用这个 (sub_14008B510)
 *      - 失败常见原因: 静态链接的 IoDriverObjectType 取值不对, 或对象类型不符
 *   2. IoGetDeviceObjectPointer(\Device\XXX)
 *      - 从设备对象反推 DriverObject
 *      - 适用于设备名和驱动名不同的情况
 *   3. 都失败, 返回 STATUS_OBJECT_NAME_NOT_FOUND
 *
 * 运行时函数解析:
 *   ObReferenceObjectByName 没有导出到 ntddk.lib, 必须用
 *   MmGetSystemRoutineAddress 运行时解析。这与 ACE 的 sub_1400564D0 做法一致。
 */
#include "hook.h"
#include "module.h"
#include "game_log.h"
#include <ntstrsafe.h>

/* IoDriverObjectType 由 ntoskrnl 导出, 头文件里没有声明 */
extern POBJECT_TYPE IoDriverObjectType;

/* ==================================================================
 * 运行时解析 ObReferenceObjectByName
 * ================================================================== */
typedef NTSTATUS (NTAPI *PFN_ObReferenceObjectByName)(
    PUNICODE_STRING ObjectName,
    ULONG           Attributes,
    POBJECT_ATTRIBUTES ObjectAttributes,
    ACCESS_MASK     DesiredAccess,
    POBJECT_TYPE    ObjectType,
    KPROCESSOR_MODE AccessMode,
    PVOID           ParseContext,
    PVOID          *Object);

static PFN_ObReferenceObjectByName pObRefByName = NULL;

/* ==================================================================
 * 真实的 IoDriverObjectType
 * ==================================================================
 * 问题: 静态链接的 nt!IoDriverObjectType 符号和这台机器实际内核不匹配,
 *       用它去调 ObReferenceObjectByName 会返回 C0000024 (类型不匹配)。
 *
 * 解决: 从我们自己的 DRIVER_OBJECT 反推真实的 OBJECT_TYPE 指针。
 *
 * 原理:
 *   每个内核对象前面都有一个 OBJECT_HEADER 结构,
 *   OBJECT_HEADER->Type 就指向该对象的 _OBJECT_TYPE。
 *
 *   x64 上 OBJECT_HEADER 的布局:
 *     offset 0x00: Type         ← OBJECT_TYPE 指针
 *     offset 0x08: ...
 *   对象指针 = OBJECT_HEADER + sizeof(OBJECT_HEADER)
 *
 *   Windows 提供的宏 OBJECT_TO_OBJECT_HEADER(obj) 可以直接反推,
 *   但它需要 ntifs.h 里完整的 OBJECT_HEADER 定义。
 */
static PVOID g_RealDriverObjectType = NULL;

/*
 * DriverLookupSetDriverObject - 在 DriverEntry 里调用, 记录真实的驱动对象类型
 *
 * 参数:
 *   driverObject  DriverEntry 收到的参数 (我们的驱动对象)
 *
 * 这个函数必须在任何 DriverLookupByName 调用之前执行。
 */
VOID DriverLookupSetDriverObject(_In_ PDRIVER_OBJECT driverObject)
{
    if (driverObject == NULL) return;

    /* ================================================================
     * 为什么不能直接用 IoDriverObjectType
     * ================================================================
     * 实测发现 (WinDbg):
     *   静态链接的 nt!IoDriverObjectType 符号 = fffff805`6d373510
     *   但那个地址的 _OBJECT_HEADER.TypeIndex = 0x65
     *   ObTypeIndexTable[0x65] = 0   ← 无效类型
     *
     *   而我们的 DriverObject 的 TypeIndex = 0x22
     *   ObTypeIndexTable[0x22] = ffffbe83`0bac7560  ← 真正的 Driver 类型
     *
     * 结论: ntoskrnl.lib 里的符号地址和实际内核不匹配。
     *
     * 解决: 用微软导出的 ObGetObjectType(对象指针) 直接拿真实类型,
     *       不用静态符号, 也不用手算 OBJECT_HEADER 偏移。
     * ================================================================ */

    typedef POBJECT_TYPE (NTAPI *PFN_ObGetObjectType)(PVOID Object);

    UNICODE_STRING  fnName;
    PFN_ObGetObjectType pfn = NULL;

    RtlInitUnicodeString(&fnName, L"ObGetObjectType");
    pfn = (PFN_ObGetObjectType)MmGetSystemRoutineAddress(&fnName);

    if (pfn != NULL) {
        g_RealDriverObjectType = pfn(driverObject);

        LOG_INFO("real Driver OBJECT_TYPE = %p (static link = %p, match=%d)",
                 g_RealDriverObjectType,
                 (PVOID)IoDriverObjectType,
                 (g_RealDriverObjectType == (PVOID)IoDriverObjectType));
    } else {
        LOG_ERROR("ObGetObjectType not found, fallback to static IoDriverObjectType");
        g_RealDriverObjectType = (PVOID)IoDriverObjectType;
    }
}

/*
 * ResolveObRef - 解析 ObReferenceObjectByName 并缓存
 *
 * 返回: 函数地址, 失败返回 NULL
 */
static PFN_ObReferenceObjectByName ResolveObRef(VOID)
{
    UNICODE_STRING n;

    if (pObRefByName != NULL) return pObRefByName;

    RtlInitUnicodeString(&n, L"ObReferenceObjectByName");
    pObRefByName = (PFN_ObReferenceObjectByName)MmGetSystemRoutineAddress(&n);

    if (pObRefByName != NULL)
        LOG_INFO("resolved ObReferenceObjectByName=%p at runtime", pObRefByName);
    else
        LOG_ERROR("MmGetSystemRoutineAddress(ObReferenceObjectByName) failed");

    return pObRefByName;
}

/* ==================================================================
 * 按名字解析驱动对象
 * ================================================================== */

/*
 * DriverLookupByName - 按名字拿到 DRIVER_OBJECT
 *
 * 参数:
 *   drvName  完整路径, 例如 L"\\Driver\\USBHUB3"
 *   out      成功时填入驱动对象指针 (已增加引用计数, 调用方负责 ObDereferenceObject)
 *
 * 返回:
 *   STATUS_SUCCESS                成功
 *   STATUS_OBJECT_NAME_NOT_FOUND  两种策略都失败
 *
 * 注意: 调用方拿到的引用必须在使用完后 ObDereferenceObject 释放。
 */
NTSTATUS DriverLookupByName(const WCHAR *drvName, PDRIVER_OBJECT *out)
{
    UNICODE_STRING name;
    NTSTATUS       st;
    PVOID          obj = NULL;

    if (drvName == NULL || out == NULL) return STATUS_INVALID_PARAMETER;
    *out = NULL;

    /* ---- 策略 1: ObReferenceObjectByName(\Driver\XXX) ---- */
    {
        PFN_ObReferenceObjectByName pfn = ResolveObRef();

        RtlInitUnicodeString(&name, drvName);

        /* ===== 调试断点 2: 即将调 ObReferenceObjectByName =====
         * 停在这里可以检查:
         *   p IoDriverObjectType                      看它的值
         *   dt nt!_OBJECT_TYPE IoDriverObjectType     看 Type/Name/Size
         *   ?? IoDriverObjectType->Name                期望看到 "Driver"
         *   p (int)*(char*)IoDriverObjectType          看 Type 字段
         * 然后:
         *   p pfn(...)   单步执行这次调用, 看返回值和 obj */
        /* 优先用运行期反推的真实类型, 失败才退回静态符号 */
        POBJECT_TYPE objType = (POBJECT_TYPE)(g_RealDriverObjectType
                                              ? g_RealDriverObjectType
                                              : (PVOID)IoDriverObjectType);

        st = (pfn != NULL)
            ? pfn(&name, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE,
                  NULL, 0, objType, KernelMode, NULL, &obj)
            : STATUS_PROCEDURE_NOT_FOUND;

        /* ===== 调试断点 3: ObReferenceObjectByName 刚返回 =====
         * 停在这里可以检查:
         *   p st        看返回码 (C0000024 = 类型不匹配)
         *   p obj       看拿到的对象指针 (失败时为 0)
         *   dt nt!_DRIVER_OBJECT obj   如果非 0 看它是不是驱动对象 */
        if (NT_SUCCESS(st) && obj != NULL) {
            *out = (PDRIVER_OBJECT)obj;
            LOG_INFO("resolved %ws via ObReferenceObjectByName", drvName);
            return STATUS_SUCCESS;
        }
        LOG_WARN("ObReferenceObjectByName(%ws) -> %08X", drvName, st);
    }

    /* ---- 策略 2: IoGetDeviceObjectPointer(\Device\XXX) ----
     * 把 \Driver\X 换成 \Device\X 再试。从设备对象能反推出驱动对象。 */
    {
        WCHAR          devPath[260];
        UNICODE_STRING devName;
        PFILE_OBJECT   fo = NULL;
        PDEVICE_OBJECT dv = NULL;

        RtlStringCchCopyW(devPath, 260, drvName);

        /* 把路径里的 "Driver" 替换成 "Device" */
        for (WCHAR *p = devPath; *p; p++) {
            if (p[0] == L'D' && p[1] == L'r' && p[2] == L'i' && p[3] == L'v') {
                p[1] = L'e'; p[2] = L'v'; p[3] = L'i'; p[4] = L'c'; p[5] = L'e';
                break;
            }
        }

        RtlInitUnicodeString(&devName, devPath);
        st = IoGetDeviceObjectPointer(&devName, FILE_ALL_ACCESS, &fo, &dv);

        if (NT_SUCCESS(st) && dv != NULL && dv->DriverObject != NULL) {
            *out = dv->DriverObject;
            ObReferenceObject(*out);        /* 增加引用, 调用方负责释放 */
            if (fo) ObDereferenceObject(fo);
            LOG_INFO("resolved %ws via IoGetDeviceObjectPointer", devPath);
            return STATUS_SUCCESS;
        }

        if (fo) ObDereferenceObject(fo);
        LOG_WARN("IoGetDeviceObjectPointer(%ws) -> %08X", devPath, st);
    }

    LOG_ERROR("DriverLookupByName: all strategies failed for %ws", drvName);
    return STATUS_OBJECT_NAME_NOT_FOUND;
}
