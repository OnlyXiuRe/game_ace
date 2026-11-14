/*
 * fw_device.c - 控制设备 + 特性路由
 *
 * 所有 3 环请求都从 \\.\<SymLink> 进来:
 *   IOCTL_FW_LOG_DUMP / IOCTL_FW_LOG_CLEAR  -> 框架自身
 *   其余 IOCTL                              -> 按 IoctlBase 路由到已注册的 feature
 */
#include "module.h"

static const MODULE_DESC *g_Features[MODULE_DESC_MAX];
static volatile LONG g_FeatureCount = 0;
static PDEVICE_OBJECT g_DevObj = NULL;

DRIVER_DISPATCH FwDispatchCreateClose;
DRIVER_DISPATCH FwDispatchDeviceControl;
static UNICODE_STRING g_SymLinkName;

NTSTATUS ModuleRegister(const MODULE_DESC *feat)
{
    LONG n;
    if (feat == NULL || feat->Dispatch == NULL) return STATUS_INVALID_PARAMETER;

    n = InterlockedIncrement(&g_FeatureCount) - 1;
    if (n >= MODULE_DESC_MAX) {
        InterlockedDecrement(&g_FeatureCount);
        LOG_ERROR("feature table full");
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    g_Features[n] = feat;
    LOG_INFO("feature registered: %s (base=0x%lX)", feat->Name, feat->IoctlBase);
    return STATUS_SUCCESS;
}

const MODULE_DESC *ModuleFind(ULONG ioctlCode)
{
    LONG n = g_FeatureCount;
    for (LONG i = 0; i < n && i < MODULE_DESC_MAX; i++) {
        const MODULE_DESC *f = g_Features[i];
        if (f == NULL) continue;
        /* IOCTL 编码: base | (function << 2), 用基址比较 */
        if (ioctlCode >= f->IoctlBase && ioctlCode < f->IoctlBase + (f->IoctlCount << 2))
            return f;
    }
    return NULL;
}

NTSTATUS FwDispatchCreateClose(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    UNREFERENCED_PARAMETER(DeviceObject);
    Irp->IoStatus.Status = STATUS_SUCCESS;
    Irp->IoStatus.Information = 0;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return STATUS_SUCCESS;
}

NTSTATUS FwDispatchDeviceControl(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    UNREFERENCED_PARAMETER(DeviceObject);
    PIO_STACK_LOCATION sl = IoGetCurrentIrpStackLocation(Irp);
    ULONG code   = sl->Parameters.DeviceIoControl.IoControlCode;
    ULONG inLen  = sl->Parameters.DeviceIoControl.InputBufferLength;
    ULONG outLen = sl->Parameters.DeviceIoControl.OutputBufferLength;
    PVOID buffer = Irp->AssociatedIrp.SystemBuffer;
    ULONG info = 0;
    NTSTATUS st;

    if (code == IOCTL_FW_LOG_DUMP) {
        if (buffer == NULL || outLen < sizeof(LOG_DUMP)) {
            st = STATUS_BUFFER_TOO_SMALL;
        } else {
            LOG_DUMP *d = (LOG_DUMP *)buffer;
            ULONG maxEntries = (outLen - FIELD_OFFSET(LOG_DUMP, Entries)) / sizeof(LOG_ENTRY);
            LogDump(d, maxEntries);
            info = sizeof(LOG_DUMP);
            st = STATUS_SUCCESS;
        }
    } else if (code == IOCTL_FW_LOG_CLEAR) {
        LogClear();
        st = STATUS_SUCCESS;
    } else {
        const MODULE_DESC *f = ModuleFind(code);
        st = (f != NULL) ? f->Dispatch(buffer, inLen, outLen, code, &info)
                         : STATUS_INVALID_DEVICE_REQUEST;
    }

    Irp->IoStatus.Status = st;
    Irp->IoStatus.Information = info;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return st;
}

/*
 * RemoveStaleDevice - 尝试删除同名的残留设备对象
 *
 * 场景: 上次驱动崩溃/异常卸载时, \Device\game_base 没有被删掉,
 *       下次 IoCreateDevice 会返回 STATUS_OBJECT_NAME_COLLISION (C0000035)。
 *
 * 做法: 用 IoGetDeviceObjectPointer 打开它, 拿到底层 DEVICE_OBJECT 后删除。
 *       如果设备还被别的驱动引用, 删除会失败, 那就只能重启了。
 */
static BOOLEAN RemoveStaleDevice(_In_ PUNICODE_STRING DeviceName)
{
    PFILE_OBJECT   fileObj = NULL;
    PDEVICE_OBJECT devObj  = NULL;
    NTSTATUS       st;

    /* FILE_READ_ATTRIBUTES 是最小的访问权限 */
    st = IoGetDeviceObjectPointer(DeviceName, FILE_READ_ATTRIBUTES, &fileObj, &devObj);
    if (!NT_SUCCESS(st) || devObj == NULL) {
        LOG_WARN("RemoveStaleDevice: cannot open %wZ -> %08X", DeviceName, st);
        return FALSE;
    }

    LOG_INFO("RemoveStaleDevice: found stale device %p, deleting", devObj);

    /* 先释放 FileObject 引用 */
    if (fileObj != NULL) ObDereferenceObject(fileObj);

    /* 尝试删除设备对象。如果引用计数不为 0 会失败 */
    IoDeleteDevice(devObj);

    LOG_INFO("RemoveStaleDevice: deleted");
    return TRUE;
}
NTSTATUS ControlDeviceInit(PDRIVER_OBJECT DriverObject, PCWSTR DeviceName, PCWSTR SymLink)
{
    UNICODE_STRING devName;
    NTSTATUS st;

    RtlInitUnicodeString(&devName, DeviceName);
    st = IoCreateDevice(DriverObject, 0, &devName, FILE_DEVICE_UNKNOWN,
                        FILE_DEVICE_SECURE_OPEN, FALSE, &g_DevObj);
    if (!NT_SUCCESS(st)) {
        LOG_ERROR("IoCreateDevice failed %08X", st);
        return st;
    }

    RtlInitUnicodeString(&g_SymLinkName, SymLink);
    st = IoCreateSymbolicLink(&g_SymLinkName, &devName);
    if (!NT_SUCCESS(st)) {
        LOG_ERROR("IoCreateSymbolicLink failed %08X", st);
        IoDeleteDevice(g_DevObj);
        g_DevObj = NULL;
        return st;
    }

    DriverObject->MajorFunction[IRP_MJ_CREATE]         = FwDispatchCreateClose;
    DriverObject->MajorFunction[IRP_MJ_CLOSE]          = FwDispatchCreateClose;
    DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] = FwDispatchDeviceControl;

    LOG_INFO("control device ready: %ws", SymLink);
    return STATUS_SUCCESS;
}

VOID ControlDeviceUnload(VOID)
{
    if (g_SymLinkName.Buffer != NULL) IoDeleteSymbolicLink(&g_SymLinkName);
    if (g_DevObj != NULL) {
        IoDeleteDevice(g_DevObj);
        g_DevObj = NULL;
    }
    LOG_INFO("control device removed");
}
