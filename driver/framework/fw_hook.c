/*
 * fw_hook.c - 通用 hook 模块
 *
 * 提供:
 *   FwHookResolveDriver  按名字解析驱动对象 (多策略回退, 解决 ACE 同样的痛点)
 *   FwHookInstall        替换 DriverObject->MajorFunction[index], 保存原指针
 *   FwHookRemove         恢复原指针
 *
 * 与 ACE sub_140086760 等价:
 *   return InterlockedExchange64((__int64*)&DrvObj->MajorFunction[index], handler);
 */
#include "fw.h"
#include <ntstrsafe.h>

/* IoDriverObjectType 由 ntddk.h 提供, 不再手写 extern */

/* 运行时解析: 参考 Kernel-Bridge Importer (MmGetSystemRoutineAddress),
 * 避免静态导入拿到不可靠的地址 (之前 C0000024 就是这个问题) */
typedef NTSTATUS (NTAPI *PFN_ObReferenceObjectByName)(
    PUNICODE_STRING, ULONG, POBJECT_ATTRIBUTES, ACCESS_MASK,
    POBJECT_TYPE, KPROCESSOR_MODE, PVOID, PVOID*);

static PFN_ObReferenceObjectByName pObRefByName = NULL;

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

NTSTATUS FwHookResolveDriver(const WCHAR *drvName, PDRIVER_OBJECT *out)
{
    UNICODE_STRING name;
    NTSTATUS st;
    PVOID obj = NULL;

    if (drvName == NULL || out == NULL) return STATUS_INVALID_PARAMETER;
    *out = NULL;

    /* 策略1: \Driver\<name> via ObReferenceObjectByName */
    PFN_ObReferenceObjectByName pfn = ResolveObRef();
    RtlInitUnicodeString(&name, drvName);

    /* 关键: ObjectType 传 NULL 跳过类型校验 (IoDriverObjectType 静态链接取值可能不对),
     * 拿到对象后手动校验它是不是驱动对象 */
    st = (pfn != NULL)
        ? pfn(&name, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE,
              NULL, 0, NULL, KernelMode, NULL, &obj)
        : STATUS_PROCEDURE_NOT_FOUND;

    if (NT_SUCCESS(st) && obj != NULL) {
        PDRIVER_OBJECT drv = (PDRIVER_OBJECT)obj;
        if (drv->DriverStart != NULL && drv->DriverSize > 0 &&
            drv->MajorFunction[IRP_MJ_CREATE] != NULL) {
            *out = drv;
            LOG_INFO("resolved %ws (unchecked type, drv=%p, start=%p, size=%lu)",
                    drvName, drv, drv->DriverStart, drv->DriverSize);
            return STATUS_SUCCESS;
        }
        LOG_WARN("object %ws is not a driver (start=%p size=%lu)",
                drvName, drv->DriverStart, drv->DriverSize);
        ObDereferenceObject(drv);
        obj = NULL;
    }
    if (NT_SUCCESS(st) && obj != NULL) {
        *out = (PDRIVER_OBJECT)obj;
        LOG_INFO("resolved %ws via ObReferenceObjectByName", drvName);
        return STATUS_SUCCESS;
    }
    LOG_WARN("ObReferenceObjectByName(%ws) -> %08X", drvName, st);

    /* 策略2: \Device\<name> 打开设备再取 DriverObject */
    {
        WCHAR devPath[260];
        UNICODE_STRING devName;
        PFILE_OBJECT fo = NULL;
        PDEVICE_OBJECT dv = NULL;

        RtlStringCchCopyW(devPath, 260, drvName);
        /* 把 \Driver\X 换成 \Device\X */
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
            ObReferenceObject(*out);
            if (fo) ObDereferenceObject(fo);
            LOG_INFO("resolved %ws via IoGetDeviceObjectPointer", devPath);
            return STATUS_SUCCESS;
        }
        if (fo) ObDereferenceObject(fo);
        LOG_WARN("IoGetDeviceObjectPointer(%ws) -> %08X", devPath, st);
    }

    return STATUS_OBJECT_NAME_NOT_FOUND;
}

NTSTATUS FwHookInstall(FW_HOOK *hook, PDRIVER_OBJECT drv,
                       ULONG index, PDRIVER_DISPATCH handler)
{
    if (hook == NULL || drv == NULL || handler == NULL || index >= IRP_MJ_MAXIMUM_FUNCTION)
        return STATUS_INVALID_PARAMETER;

    hook->Drv = drv;
    hook->Index = index;
    hook->Handler = handler;
    hook->Original = (PDRIVER_DISPATCH)InterlockedExchangePointer(
                        (PVOID volatile *)&drv->MajorFunction[index], (PVOID)handler);
    hook->Active = (hook->Original != NULL);

    if (!hook->Active) {
        LOG_ERROR("install failed (index=%lu)", index);
        return STATUS_UNSUCCESSFUL;
    }
    LOG_INFO("installed MajorFunction[%lu] orig=%p", index, hook->Original);
    return STATUS_SUCCESS;
}

VOID FwHookRemove(FW_HOOK *hook)
{
    if (hook == NULL || !hook->Active || hook->Drv == NULL) return;

    InterlockedExchangePointer((PVOID volatile *)&hook->Drv->MajorFunction[hook->Index],
                               (PVOID)hook->Original);
    hook->Active = FALSE;
    LOG_INFO("removed MajorFunction[%lu]", hook->Index);

    if (hook->Drv != NULL) {
        ObDereferenceObject(hook->Drv);
        hook->Drv = NULL;
    }
}
