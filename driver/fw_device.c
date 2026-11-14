/*
 * fw_device.c - 控制设备 + 特性路由
 *
 * 所有 3 环请求都从 \\.\<SymLink> 进来:
 *   IOCTL_FW_LOG_DUMP / IOCTL_FW_LOG_CLEAR  -> 框架自身
 *   其余 IOCTL                              -> 按 IoctlBase 路由到已注册的 feature
 */
#include "fw.h"

static const FW_FEATURE *g_Features[FW_FEATURE_MAX];
static volatile LONG g_FeatureCount = 0;
static PDEVICE_OBJECT g_DevObj = NULL;

DRIVER_DISPATCH FwDispatchCreateClose;
DRIVER_DISPATCH FwDispatchDeviceControl;
static UNICODE_STRING g_SymLinkName;

NTSTATUS FwFeatureRegister(const FW_FEATURE *feat)
{
    LONG n;
    if (feat == NULL || feat->Dispatch == NULL) return STATUS_INVALID_PARAMETER;

    n = InterlockedIncrement(&g_FeatureCount) - 1;
    if (n >= FW_FEATURE_MAX) {
        InterlockedDecrement(&g_FeatureCount);
        FW_ERR("fw", "feature table full");
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    g_Features[n] = feat;
    FW_INFO("fw", "feature registered: %s (base=0x%lX)", feat->Name, feat->IoctlBase);
    return STATUS_SUCCESS;
}

const FW_FEATURE *FwFeatureFind(ULONG ioctlCode)
{
    LONG n = g_FeatureCount;
    for (LONG i = 0; i < n && i < FW_FEATURE_MAX; i++) {
        const FW_FEATURE *f = g_Features[i];
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
        if (buffer == NULL || outLen < sizeof(FW_LOG_DUMP)) {
            st = STATUS_BUFFER_TOO_SMALL;
        } else {
            FW_LOG_DUMP *d = (FW_LOG_DUMP *)buffer;
            ULONG maxEntries = (outLen - FIELD_OFFSET(FW_LOG_DUMP, Entries)) / sizeof(FW_LOG_ENTRY);
            FwLogDump(d, maxEntries);
            info = sizeof(FW_LOG_DUMP);
            st = STATUS_SUCCESS;
        }
    } else if (code == IOCTL_FW_LOG_CLEAR) {
        FwLogClear();
        st = STATUS_SUCCESS;
    } else {
        const FW_FEATURE *f = FwFeatureFind(code);
        st = (f != NULL) ? f->Dispatch(buffer, inLen, outLen, code, &info)
                         : STATUS_INVALID_DEVICE_REQUEST;
    }

    Irp->IoStatus.Status = st;
    Irp->IoStatus.Information = info;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return st;
}

NTSTATUS FwDeviceInit(PDRIVER_OBJECT DriverObject, PCWSTR DeviceName, PCWSTR SymLink)
{
    UNICODE_STRING devName;
    NTSTATUS st;

    RtlInitUnicodeString(&devName, DeviceName);
    st = IoCreateDevice(DriverObject, 0, &devName, FILE_DEVICE_UNKNOWN,
                        FILE_DEVICE_SECURE_OPEN, FALSE, &g_DevObj);
    if (!NT_SUCCESS(st)) {
        FW_ERR("fw", "IoCreateDevice failed %08X", st);
        return st;
    }

    RtlInitUnicodeString(&g_SymLinkName, SymLink);
    st = IoCreateSymbolicLink(&g_SymLinkName, &devName);
    if (!NT_SUCCESS(st)) {
        FW_ERR("fw", "IoCreateSymbolicLink failed %08X", st);
        IoDeleteDevice(g_DevObj);
        g_DevObj = NULL;
        return st;
    }

    DriverObject->MajorFunction[IRP_MJ_CREATE]         = FwDispatchCreateClose;
    DriverObject->MajorFunction[IRP_MJ_CLOSE]          = FwDispatchCreateClose;
    DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] = FwDispatchDeviceControl;

    FW_INFO("fw", "control device ready: %ws", SymLink);
    return STATUS_SUCCESS;
}

VOID FwDeviceUnload(VOID)
{
    if (g_SymLinkName.Buffer != NULL) IoDeleteSymbolicLink(&g_SymLinkName);
    if (g_DevObj != NULL) {
        IoDeleteDevice(g_DevObj);
        g_DevObj = NULL;
    }
    FW_INFO("fw", "control device removed");
}
