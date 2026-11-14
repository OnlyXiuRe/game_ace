/*
 * feat_usb.c - USB 特性模块 (第一个 feature)
 *
 * 职责:
 *   1. 解析 \Driver\USBHUB3, 安装 IRP_MJ_PNP hook
 *   2. PnP 事件里记录设备 (VID/PID/REV)
 *   3. 提供 ENUM / CTRL(原始 URB) / BLOCK / DEBUG 四个 IOCTL
 *   4. 解析失败时枚举 \Driver 目录, 打印含 HUB 的对象名 (排查用)
 *
 * 后续加新 feature 时, 照这个文件写一个 feat_xxx.c + 一个 FW_FEATURE 即可。
 */
#include "fw.h"
#include "game_base.h"
#include <usb.h>
#include <usbdlib.h>
#include <usbioctl.h>
#include <ntstrsafe.h>
#include <initguid.h>

#define USB_MOD "usb"

/* WDK 未声明, 手动补 */
typedef struct _OBJECT_DIRECTORY_INFORMATION {
    UNICODE_STRING Name;
    UNICODE_STRING TypeName;
} OBJECT_DIRECTORY_INFORMATION, *POBJECT_DIRECTORY_INFORMATION;

NTSYSAPI NTSTATUS NTAPI ZwOpenDirectoryObject(PHANDLE DirectoryHandle,
                                              ACCESS_MASK DesiredAccess,
                                              POBJECT_ATTRIBUTES ObjectAttributes);
NTSYSAPI NTSTATUS NTAPI ZwQueryDirectoryObject(HANDLE DirectoryHandle,
                                               PVOID Buffer,
                                               ULONG BufferLength,
                                               BOOLEAN ReturnSingleEntry,
                                               BOOLEAN RestartScan,
                                               PULONG Context,
                                               PULONG ReturnLength);

DEFINE_GUID(GUID_DEVINTERFACE_USB_DEVICE,
    0xA5DCBF10, 0x6530, 0x11D2, 0x90, 0x1F, 0x00, 0xC0, 0x4F, 0xB9, 0x51, 0xED);

/* ================= 状态 ================= */
typedef struct _TRACKED_DEV {
    PDEVICE_OBJECT DeviceObject;
    USHORT  Vid, Pid, BcdDevice;
    USHORT  BusNumber;
    ULONG   DeviceAddress;
    BOOLEAN Active;
    BOOLEAN Blocked;
    WCHAR   HardwareId[128];
} TRACKED_DEV;

static KSPIN_LOCK     g_Lock;
static TRACKED_DEV    g_Devs[USB_MAX_DEVICES];
static HOOK_ENTRY      g_PnpHook;
static volatile LONG  g_Unloading = 0;
static volatile LONG  g_InHook = 0;
static ULONG          g_HookStatus = 0xFFFFFFFF;
static ULONG          g_HubFound = 0;
static ULONG          g_Tracked = 0;
static ULONG          g_ScanCount = 0;
static ULONG          g_ScanStatus = 0xFFFFFFFF;

/* 前向声明 */
static NTSTATUS CoreUsbDispatch(PVOID buffer, ULONG inLen, ULONG outLen, ULONG code, PULONG info);
static NTSTATUS MyHubPnp(PDEVICE_OBJECT DeviceObject, PIRP Irp);
static VOID     EnumExistingDevices(VOID);
static VOID     DumpDriverDirectory(VOID);
static NTSTATUS DoEnum(PVOID buffer, ULONG outLen, PULONG info);
static NTSTATUS DoCtrl(PVOID buffer, ULONG inLen, PULONG info);
static NTSTATUS DoBlock(PVOID buffer, ULONG inLen);
static NTSTATUS DoDebug(PVOID buffer, ULONG outLen, PULONG info);

/* ================= 工具 ================= */
static BOOLEAN ParseHex4(const WCHAR *p, PUSHORT out)
{
    USHORT v = 0;
    for (int i = 0; i < 4; i++) {
        WCHAR c = p[i];
        v <<= 4;
        if (c >= L'0' && c <= L'9') v |= (USHORT)(c - L'0');
        else if (c >= L'a' && c <= L'f') v |= (USHORT)(c - L'a' + 10);
        else if (c >= L'A' && c <= L'F') v |= (USHORT)(c - L'A' + 10);
        else return FALSE;
    }
    *out = v;
    return TRUE;
}

static BOOLEAN ParseHid(const WCHAR *hid, PUSHORT vid, PUSHORT pid, PUSHORT rev)
{
    *vid = *pid = *rev = 0;
    for (const WCHAR *p = hid; *p; p++) {
        if (p[0]==L'V'&&p[1]==L'I'&&p[2]==L'D'&&p[3]==L'_') ParseHex4(p+4, vid);
        else if (p[0]==L'P'&&p[1]==L'I'&&p[2]==L'D'&&p[3]==L'_') ParseHex4(p+4, pid);
        else if (p[0]==L'R'&&p[1]==L'E'&&p[2]==L'V'&&p[3]==L'_') ParseHex4(p+4, rev);
    }
    return (*vid != 0 || *pid != 0);
}

static BOOLEAN QueryDeviceId(PDEVICE_OBJECT devObj, PUSHORT vid, PUSHORT pid,
                             PUSHORT rev, PWCHAR hidOut, ULONG hidChars)
{
    WCHAR buf[512];
    ULONG len = 0;
    RtlZeroMemory(buf, sizeof(buf));
    NTSTATUS st = IoGetDeviceProperty(devObj, DevicePropertyHardwareID,
                                      sizeof(buf) - sizeof(WCHAR), buf, &len);
    if (!NT_SUCCESS(st) || len < 10) {
        LOG_WARN("IoGetDeviceProperty dev=%p -> %08X len=%lu", devObj, st, len);
        return FALSE;
    }
    buf[(sizeof(buf)/sizeof(WCHAR)) - 1] = 0;
    if (!ParseHid(buf, vid, pid, rev)) {
        LOG_WARN("hardware id parse failed");
        return FALSE;
    }
    if (hidOut && hidChars) {
        ULONG i = 0;
        for (; buf[i] && i + 1 < hidChars; i++) hidOut[i] = buf[i];
        hidOut[i] = 0;
    }
    return TRUE;
}

static TRACKED_DEV *FindDev(PDEVICE_OBJECT devObj)
{
    for (int i = 0; i < USB_MAX_DEVICES; i++)
        if (g_Devs[i].Active && g_Devs[i].DeviceObject == devObj) return &g_Devs[i];
    return NULL;
}

static VOID TrackDevice(PDEVICE_OBJECT devObj)
{
    USHORT vid = 0, pid = 0, rev = 0;
    WCHAR hid[128];
    KIRQL irql;
    RtlZeroMemory(hid, sizeof(hid));
    if (!QueryDeviceId(devObj, &vid, &pid, &rev, hid, 128)) return;

    KeAcquireSpinLock(&g_Lock, &irql);
    if (FindDev(devObj) == NULL) {
        for (int i = 0; i < USB_MAX_DEVICES; i++) {
            if (!g_Devs[i].Active) {
                RtlZeroMemory(&g_Devs[i], sizeof(TRACKED_DEV));
                g_Devs[i].DeviceObject = devObj;
                g_Devs[i].Vid = vid; g_Devs[i].Pid = pid; g_Devs[i].BcdDevice = rev;
                g_Devs[i].Active = TRUE;
                RtlStringCchCopyW(g_Devs[i].HardwareId, 128, hid);
                g_Tracked++;
                break;
            }
        }
    }
    KeReleaseSpinLock(&g_Lock, irql);
    LOG_INFO("START dev=%p VID=%04X PID=%04X REV=%04X", devObj, vid, pid, rev);
}

static VOID UntrackDevice(PDEVICE_OBJECT devObj)
{
    KIRQL irql;
    KeAcquireSpinLock(&g_Lock, &irql);
    TRACKED_DEV *d = FindDev(devObj);
    if (d) { d->Active = FALSE; d->DeviceObject = NULL; }
    KeReleaseSpinLock(&g_Lock, irql);
    LOG_INFO("REMOVE dev=%p", devObj);
}

/* ================= PnP Hook ================= */
static NTSTATUS MyHubPnp(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    PIO_STACK_LOCATION sl = IoGetCurrentIrpStackLocation(Irp);
    UCHAR minor = sl->MinorFunction;

    if (g_Unloading == 0 && g_PnpHook.Original != NULL) {
        InterlockedIncrement(&g_InHook);
        if (minor == IRP_MN_START_DEVICE)       TrackDevice(DeviceObject);
        else if (minor == IRP_MN_REMOVE_DEVICE) UntrackDevice(DeviceObject);
        InterlockedDecrement(&g_InHook);
    }
    return ((PDRIVER_DISPATCH)g_PnpHook.Original)(DeviceObject, Irp);
}

/* ================= \Driver 目录枚举 (排查) ================= */
static VOID DumpDriverDirectory(VOID)
{
    UNICODE_STRING dirName;
    OBJECT_ATTRIBUTES oa;
    HANDLE hDir = NULL;
    NTSTATUS st;

    RtlInitUnicodeString(&dirName, L"\\Driver");
    InitializeObjectAttributes(&oa, &dirName, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);

    st = ZwOpenDirectoryObject(&hDir, DIRECTORY_QUERY, &oa);
    if (!NT_SUCCESS(st)) {
        LOG_ERROR("ZwOpenDirectoryObject(\\Driver) -> %08X", st);
        return;
    }

    ULONG bufSize = 8192;
    PVOID buf = ExAllocatePoolWithTag(NonPagedPool, bufSize, 'gniD');
    if (buf == NULL) { ZwClose(hDir); return; }

    ULONG ctx = 0, total = 0, hit = 0;
    for (;;) {
        ULONG retLen = 0;
        st = ZwQueryDirectoryObject(hDir, buf, bufSize, TRUE, FALSE, &ctx, &retLen);
        if (!NT_SUCCESS(st)) break;

        POBJECT_DIRECTORY_INFORMATION info = (POBJECT_DIRECTORY_INFORMATION)buf;
        for (; info->Name.Length != 0; info++) {
            total++;
            USHORT nchars = info->Name.Length / sizeof(WCHAR);
            BOOLEAN match = FALSE;
            for (USHORT i = 0; i + 2 < nchars; i++) {
                WCHAR c = info->Name.Buffer[i];
                if (c == L'H' || c == L'h') {
                    WCHAR c1 = info->Name.Buffer[i+1], c2 = info->Name.Buffer[i+2];
                    if ((c1 == L'U' || c1 == L'u') && (c2 == L'B' || c2 == L'b')) {
                        match = TRUE; break;
                    }
                }
            }
            if (!match) continue;
            hit++;
            ANSI_STRING ansi;
            CHAR tmp[128];
            ansi.Buffer = tmp;
            ansi.MaximumLength = sizeof(tmp) - 1;
            ansi.Length = 0;
            if (NT_SUCCESS(RtlUnicodeStringToAnsiString(&ansi, &info->Name, FALSE))) {
                tmp[ansi.Length] = 0;
                LOG_INFO("\\Driver\\%s", tmp);
            }
        }
    }

    LOG_INFO("\\Driver total=%lu matched(HUB)=%lu", total, hit);
    ExFreePool(buf);
    ZwClose(hDir);
}

/* ================= URB / Block ================= */
static NTSTATUS SubmitUrb(PDEVICE_OBJECT target, URB *urb)
{
    KEVENT event;
    IO_STATUS_BLOCK iosb;
    PDEVICE_OBJECT top;
    NTSTATUS st;

    top = IoGetAttachedDeviceReference(target);
    if (top == NULL) return STATUS_UNSUCCESSFUL;

    KeInitializeEvent(&event, NotificationEvent, FALSE);
    RtlZeroMemory(&iosb, sizeof(iosb));
    PIRP irp = IoBuildDeviceIoControlRequest(IOCTL_INTERNAL_USB_SUBMIT_URB, top,
                                             urb, sizeof(URB), NULL, 0, TRUE, &event, &iosb);
    if (irp == NULL) { ObDereferenceObject(top); return STATUS_INSUFFICIENT_RESOURCES; }

    st = IofCallDriver(top, irp);
    if (st == STATUS_PENDING) {
        KeWaitForSingleObject(&event, Executive, KernelMode, FALSE, NULL);
        st = iosb.Status;
    }
    ObDereferenceObject(top);
    return st;
}

static NTSTATUS BlockDevice(PDEVICE_OBJECT target, BOOLEAN block)
{
    KEVENT event;
    IO_STATUS_BLOCK iosb;
    PDEVICE_OBJECT top;
    NTSTATUS st;

    top = IoGetAttachedDeviceReference(target);
    if (top == NULL) return STATUS_UNSUCCESSFUL;

    KeInitializeEvent(&event, NotificationEvent, FALSE);
    RtlZeroMemory(&iosb, sizeof(iosb));
    PIRP irp = IoBuildSynchronousFsdRequest(IRP_MJ_PNP, top, NULL, 0, NULL, &event, &iosb);
    if (irp == NULL) { ObDereferenceObject(top); return STATUS_INSUFFICIENT_RESOURCES; }

    PIO_STACK_LOCATION sl = IoGetNextIrpStackLocation(irp);
    sl->MinorFunction = block ? IRP_MN_STOP_DEVICE : IRP_MN_START_DEVICE;
    irp->IoStatus.Status = STATUS_BUFFER_TOO_SMALL;

    st = IofCallDriver(top, irp);
    if (st == STATUS_PENDING) {
        KeWaitForSingleObject(&event, Executive, KernelMode, FALSE, NULL);
        st = iosb.Status;
    }
    ObDereferenceObject(top);
    return st;
}

/* ================= 枚举已存在设备 ================= */
static VOID EnumExistingDevices(VOID)
{
    PWSTR list = NULL;
    NTSTATUS st = IoGetDeviceInterfaces(&GUID_DEVINTERFACE_USB_DEVICE, NULL, 0, &list);
    g_ScanStatus = (ULONG)st;
    if (!NT_SUCCESS(st) || list == NULL) {
        LOG_WARN("IoGetDeviceInterfaces -> %08X", st);
        return;
    }
    int n = 0;
    for (PWSTR p = list; *p; p += wcslen(p) + 1) {
        UNICODE_STRING path;
        PFILE_OBJECT fo = NULL;
        PDEVICE_OBJECT dev = NULL;
        RtlInitUnicodeString(&path, p);
        if (NT_SUCCESS(IoGetDeviceObjectPointer(&path, FILE_READ_ATTRIBUTES, &fo, &dev))) {
            TrackDevice(dev);
            ObDereferenceObject(fo);
            n++;
        }
    }
    ExFreePool(list);
    g_ScanCount = (ULONG)n;
    LOG_INFO("scanned %d existing USB interfaces", n);
}

/* ================= IOCTL ================= */
static NTSTATUS DoEnum(PVOID buffer, ULONG outLen, PULONG info)
{
    if (buffer == NULL || outLen < sizeof(USB_DEV_LIST)) return STATUS_BUFFER_TOO_SMALL;
    USB_DEV_LIST *out = (USB_DEV_LIST *)buffer;
    KIRQL irql;
    RtlZeroMemory(out, sizeof(USB_DEV_LIST));

    KeAcquireSpinLock(&g_Lock, &irql);
    ULONG n = 0;
    for (int i = 0; i < USB_MAX_DEVICES; i++) {
        if (!g_Devs[i].Active) continue;
        USB_DEV_INFO *d = &out->Devs[n];
        d->Index = (ULONG)i;
        d->Active = TRUE;
        d->Vid = g_Devs[i].Vid; d->Pid = g_Devs[i].Pid; d->BcdDevice = g_Devs[i].BcdDevice;
        d->BusNumber = g_Devs[i].BusNumber; d->DeviceAddress = g_Devs[i].DeviceAddress;
        RtlStringCchCopyW(d->HardwareId, 128, g_Devs[i].HardwareId);
        n++;
    }
    out->Count = n;
    KeReleaseSpinLock(&g_Lock, irql);
    *info = sizeof(USB_DEV_LIST);
    return STATUS_SUCCESS;
}

static NTSTATUS DoCtrl(PVOID buffer, ULONG inLen, PULONG info)
{
    if (buffer == NULL || inLen < sizeof(USB_CTRL_REQ)) return STATUS_BUFFER_TOO_SMALL;
    USB_CTRL_REQ *req = (USB_CTRL_REQ *)buffer;
    if (req->DevIndex >= USB_MAX_DEVICES || req->wLength > USB_MAX_DATA) return STATUS_INVALID_PARAMETER;

    PDEVICE_OBJECT target = NULL;
    KIRQL irql;
    KeAcquireSpinLock(&g_Lock, &irql);
    if (g_Devs[req->DevIndex].Active) target = g_Devs[req->DevIndex].DeviceObject;
    KeReleaseSpinLock(&g_Lock, irql);
    if (target == NULL) return STATUS_DEVICE_NOT_CONNECTED;

    PUCHAR data = (PUCHAR)buffer + sizeof(USB_CTRL_REQ);
    URB urb;
    RtlZeroMemory(&urb, sizeof(urb));
    urb.UrbHeader.Length = sizeof(URB);
    urb.UrbHeader.Function = URB_FUNCTION_CONTROL_TRANSFER;
    urb.UrbControlTransfer.TransferFlags = (req->bmRequestType & 0x80)
        ? USBD_TRANSFER_DIRECTION_IN : USBD_TRANSFER_DIRECTION_OUT;
    urb.UrbControlTransfer.TransferBufferLength = req->wLength;
    urb.UrbControlTransfer.TransferBuffer = (req->wLength > 0) ? data : NULL;
    urb.UrbControlTransfer.SetupPacket[0] = req->bmRequestType;
    urb.UrbControlTransfer.SetupPacket[1] = req->bRequest;
    urb.UrbControlTransfer.SetupPacket[2] = (UCHAR)req->wValue;
    urb.UrbControlTransfer.SetupPacket[3] = (UCHAR)(req->wValue >> 8);
    urb.UrbControlTransfer.SetupPacket[4] = (UCHAR)req->wIndex;
    urb.UrbControlTransfer.SetupPacket[5] = (UCHAR)(req->wIndex >> 8);
    urb.UrbControlTransfer.SetupPacket[6] = (UCHAR)req->wLength;
    urb.UrbControlTransfer.SetupPacket[7] = (UCHAR)(req->wLength >> 8);

    NTSTATUS st = SubmitUrb(target, &urb);
    if (NT_SUCCESS(st)) {
        req->bytesReturned = urb.UrbControlTransfer.TransferBufferLength;
        *info = sizeof(USB_CTRL_REQ);
    } else req->bytesReturned = 0;
    return st;
}

static NTSTATUS DoBlock(PVOID buffer, ULONG inLen)
{
    if (buffer == NULL || inLen < sizeof(USB_BLOCK_REQ)) return STATUS_BUFFER_TOO_SMALL;
    USB_BLOCK_REQ *req = (USB_BLOCK_REQ *)buffer;
    if (req->DevIndex >= USB_MAX_DEVICES) return STATUS_INVALID_PARAMETER;

    PDEVICE_OBJECT target = NULL;
    KIRQL irql;
    KeAcquireSpinLock(&g_Lock, &irql);
    if (g_Devs[req->DevIndex].Active) {
        target = g_Devs[req->DevIndex].DeviceObject;
        g_Devs[req->DevIndex].Blocked = req->Block;
    }
    KeReleaseSpinLock(&g_Lock, irql);
    if (target == NULL) return STATUS_DEVICE_NOT_CONNECTED;
    return BlockDevice(target, req->Block);
}

static NTSTATUS DoDebug(PVOID buffer, ULONG outLen, PULONG info)
{
    if (buffer == NULL || outLen < sizeof(USB_DEBUG_INFO)) return STATUS_BUFFER_TOO_SMALL;
    USB_DEBUG_INFO *d = (USB_DEBUG_INFO *)buffer;
    KIRQL irql;
    RtlZeroMemory(d, sizeof(USB_DEBUG_INFO));
    KeAcquireSpinLock(&g_Lock, &irql);
    for (int i = 0; i < USB_MAX_DEVICES; i++) if (g_Devs[i].Active) d->DeviceCount++;
    KeReleaseSpinLock(&g_Lock, irql);
    d->HookInstalled  = g_PnpHook.Active ? 1 : 0;
    d->HubDriverFound = g_HubFound;
    d->LastScanCount  = g_ScanCount;
    d->LastScanStatus = g_ScanStatus;
    d->HookStatus     = g_HookStatus;
    d->Tracked        = g_Tracked;
    *info = sizeof(USB_DEBUG_INFO);
    return STATUS_SUCCESS;
}

static NTSTATUS CoreUsbDispatch(PVOID buffer, ULONG inLen, ULONG outLen,
                                ULONG code, PULONG info)
{
    switch (code) {
    case IOCTL_USB_ENUM_DEVICES:     return DoEnum(buffer, outLen, info);
    case IOCTL_USB_CONTROL_TRANSFER: return DoCtrl(buffer, inLen, info);
    case IOCTL_USB_BLOCK_DEVICE:     return DoBlock(buffer, inLen);
    case IOCTL_USB_DEBUG:            return DoDebug(buffer, outLen, info);
    default:                         return STATUS_INVALID_DEVICE_REQUEST;
    }
}

/* ================= 生命周期 ================= */
NTSTATUS CoreUsbInit(VOID)
{
    PDRIVER_OBJECT hubDrv = NULL;
    NTSTATUS st;

    KeInitializeSpinLock(&g_Lock);
    RtlZeroMemory(g_Devs, sizeof(g_Devs));
    RtlZeroMemory(&g_PnpHook, sizeof(g_PnpHook));

    st = FwHookResolveDriver(L"\\Driver\\USBHUB3", &hubDrv);
    g_HookStatus = (ULONG)st;
    if (!NT_SUCCESS(st) || hubDrv == NULL) {
        LOG_ERROR("resolve USBHUB3 failed: %08X", st);
        return st;
    }
    g_HubFound = 1;

    st = HookInstall(&g_PnpHook, HOOK_DISPATCH, hubDrv, MyHubPnp, IRP_MJ_PNP);
    if (!NT_SUCCESS(st)) {
        LOG_ERROR("install PnP hook failed: %08X", st);
        ObDereferenceObject(hubDrv);
        return st;
    }

    EnumExistingDevices();
    LOG_INFO("feature ready (hook=%d)", g_PnpHook.Active);
    return STATUS_SUCCESS;
}

VOID CoreUsbUnload(VOID)
{
    InterlockedExchange(&g_Unloading, 1);
    for (int i = 0; i < 200 && g_InHook != 0; i++) {
        LARGE_INTEGER d;
        d.QuadPart = -10000;
        KeDelayExecutionThread(KernelMode, FALSE, &d);
    }
    HookRemove(&g_PnpHook);
    LOG_INFO("feature unloaded");
}

static const FW_FEATURE g_CoreUsb = {
    "usb",
    IOCTL_USB_CONTROL_TRANSFER,
    4,
    CoreUsbDispatch
};

NTSTATUS CoreUsbRegister(VOID)
{
    return FwFeatureRegister(&g_CoreUsb);
}
