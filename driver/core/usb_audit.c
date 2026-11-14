/*
 * usb_audit.c - USB 设备审计模块 (game_base 的第一个核心模块)
 *
 * ==================================================================
 * 职责
 * ==================================================================
 *   1. 锚定 \Driver\USBHUB3, 拿 DRIVER_OBJECT
 *   2. 替换它的 MajorFunction[IRP_MJ_PNP], 拦截 START/REMOVE 事件
 *   3. 走 DeviceObject->NextDevice 链表 + IoGetLowerDeviceObject 设备栈
 *   4. 提供 ENUM / CTRL(原始 URB) / BLOCK / DEBUG 四个 IOCTL
 *
 * ==================================================================
 * 对齐 ACE 的发现路径 (来自 IDA)
 * ==================================================================
 *   ACE sub_14000F600:  ObReferenceObjectByName(L"\\Driver\\USBHUB3")
 *   ACE sub_14000E630:  DriverObject->DeviceObject -> NextDevice 遍历
 *   ACE 字符串:         "UsbHub Stack: %ws %p DriverName: %ws"
 *   ACE sub_140086760:  InterlockedExchange64(&DrvObj->MajorFunction[27])
 *   ACE sub_14000E840:  伪造 PNP IRP 封锁设备
 */
#include "module.h"
#include "game_base.h"
#include <usb.h>
#include <usbdlib.h>
#include <usbioctl.h>
#include <ntstrsafe.h>

#define USB_MOD "usb"

#ifndef ARRAY_SIZE
#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))
#endif

/* WDK 的 ntddk.h 里没有这些声明, 手动补 */
extern PDEVICE_OBJECT IoGetLowerDeviceObject(PDEVICE_OBJECT DeviceObject);

/* ================= 状态 ================= */

/*
 * TRACKED_DEV - 一个被跟踪的 USB 设备
 */
typedef struct _TRACKED_DEV {
    PDEVICE_OBJECT DeviceObject;        /* 设备对象 */
    USHORT  Vid, Pid, BcdDevice;        /* 从 hardware id 解析 */
    USHORT  BusNumber;
    ULONG   DeviceAddress;
    BOOLEAN Active;
    BOOLEAN Blocked;
    WCHAR   HardwareId[128];
} TRACKED_DEV;

static KSPIN_LOCK     g_Lock;
static TRACKED_DEV    g_Devs[USB_MAX_DEVICES];

/*
 * HUB_SLOT - 一个 hub 驱动的 hook 槽
 */
typedef struct _HUB_SLOT {
    HOOK_ENTRY      Hook;               /* hook 框架的条目 */
    PDRIVER_OBJECT  Driver;             /* hub 的驱动对象 (带引用) */
    ULONG           DeviceCount;        /* 该驱动的设备数 */
    ULONG           MaxStackDepth;      /* 最深设备栈 */
} HUB_SLOT;

static HUB_SLOT       g_HubSlot;
static BOOLEAN        g_HubSlotUsed = FALSE;

static volatile LONG  g_Unloading = 0;   /* 卸载标记 */
static volatile LONG  g_InHook    = 0;   /* 正在 hook 回调里的线程数 */
static ULONG          g_HookStatus = 0xFFFFFFFF;
static ULONG          g_HubFound   = 0;
static ULONG          g_Tracked    = 0;
static ULONG          g_ScanCount  = 0;
static ULONG          g_ScanStatus = 0xFFFFFFFF;

/* ================= 前向声明 ================= */
static NTSTATUS UsbAuditDispatch(PVOID buffer, ULONG inLen, ULONG outLen,
                                 ULONG code, PULONG info);
static NTSTATUS MyHubPnp(PDEVICE_OBJECT DeviceObject, PIRP Irp);
static VOID     EnumAllUsbHubs(VOID);
static NTSTATUS DoEnum(PVOID buffer, ULONG outLen, PULONG info);
static NTSTATUS DoCtrl(PVOID buffer, ULONG inLen, PULONG info);
static NTSTATUS DoBlock(PVOID buffer, ULONG inLen);
static NTSTATUS DoDebug(PVOID buffer, ULONG outLen, PULONG info);

/* ================= 工具 ================= */

/* 解析 4 位十六进制 */
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

/* 从 HardwareID 解析 VID/PID/REV */
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

/*
 * QueryDeviceId - 从设备对象读 HardwareID 并解析 VID/PID
 *
 * 注意: hub 的 FDO 通常没有 HardwareID 属性, 需要传底层 PDO。
 */
static BOOLEAN QueryDeviceId(PDEVICE_OBJECT devObj, PUSHORT vid, PUSHORT pid,
                             PUSHORT rev, PWCHAR hidOut, ULONG hidChars)
{
    WCHAR buf[512];
    ULONG len = 0;
    NTSTATUS st;

    RtlZeroMemory(buf, sizeof(buf));
    st = IoGetDeviceProperty(devObj, DevicePropertyHardwareID,
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

/* 在设备表里按设备对象查找 */
static TRACKED_DEV *FindDev(PDEVICE_OBJECT devObj)
{
    for (int i = 0; i < USB_MAX_DEVICES; i++)
        if (g_Devs[i].Active && g_Devs[i].DeviceObject == devObj) return &g_Devs[i];
    return NULL;
}

/*
 * TrackDevice - 记录一个 USB 设备
 */
static VOID TrackDevice(PDEVICE_OBJECT devObj)
{
    USHORT vid = 0, pid = 0, rev = 0;
    WCHAR hid[128];
    KIRQL irql;

    if (devObj == NULL) return;

    RtlZeroMemory(hid, sizeof(hid));
    if (!QueryDeviceId(devObj, &vid, &pid, &rev, hid, 128)) return;

    KeAcquireSpinLock(&g_Lock, &irql);
    if (FindDev(devObj) == NULL) {
        for (int i = 0; i < USB_MAX_DEVICES; i++) {
            if (!g_Devs[i].Active) {
                RtlZeroMemory(&g_Devs[i], sizeof(TRACKED_DEV));
                g_Devs[i].DeviceObject = devObj;
                g_Devs[i].Vid = vid;
                g_Devs[i].Pid = pid;
                g_Devs[i].BcdDevice = rev;
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

/* 移除一个设备记录 */
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

/*
 * FindOrigByDevice - 从设备对象反查它所属 hub 的原始 PnP 派发函数
 *
 * 多 hub 场景下无法用单个全局 Original, 必须按 DriverObject 找对应槽。
 */
static PDRIVER_DISPATCH FindOrigByDevice(PDEVICE_OBJECT DeviceObject)
{
    PDRIVER_OBJECT   drv;
    PDRIVER_DISPATCH orig = NULL;
    KIRQL            irql;

    if (DeviceObject == NULL) return NULL;
    drv = DeviceObject->DriverObject;
    if (drv == NULL) return NULL;

    KeAcquireSpinLock(&g_Lock, &irql);
    if (g_HubSlotUsed && g_HubSlot.Driver == drv) {
        orig = (PDRIVER_DISPATCH)g_HubSlot.Hook.Original;
    }
    KeReleaseSpinLock(&g_Lock, irql);

    return orig;
}

/*
 * MyHubPnp - hub 的 IRP_MJ_PNP 拦截函数
 *
 * 拦截 START_DEVICE / REMOVE_DEVICE, 记录设备, 然后尾调用原派发函数。
 */
static NTSTATUS MyHubPnp(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    PIO_STACK_LOCATION sl;
    UCHAR              minor;
    PDRIVER_DISPATCH   orig;

    if (Irp == NULL) return STATUS_INVALID_PARAMETER;

    sl    = IoGetCurrentIrpStackLocation(Irp);
    minor = sl->MinorFunction;
    orig  = FindOrigByDevice(DeviceObject);

    if (orig == NULL) {
        LOG_WARN("MyHubPnp: no original dispatch for device %p", DeviceObject);
        Irp->IoStatus.Status      = STATUS_DEVICE_NOT_READY;
        Irp->IoStatus.Information = 0;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return STATUS_DEVICE_NOT_READY;
    }

    if (g_Unloading == 0) {
        InterlockedIncrement(&g_InHook);
        if (minor == IRP_MN_START_DEVICE)       TrackDevice(DeviceObject);
        else if (minor == IRP_MN_REMOVE_DEVICE) UntrackDevice(DeviceObject);
        InterlockedDecrement(&g_InHook);
    }

    return orig(DeviceObject, Irp);
}

/* ================= 设备栈遍历 ================= */

/*
 * WalkDeviceStack - 从某个设备沿设备栈向下走, 打印每一层的驱动名
 *
 * 参数:
 *   top    起始设备 (hub 的 FDO)
 *   depth  输出: 栈深度
 *
 * 返回: 最底层设备 (PDO), 调用方负责 ObDereferenceObject
 *
 * IoGetLowerDeviceObject 每次返回都带一个引用, 除了起始设备,
 * 每一层用完都要释放。
 */
static PDEVICE_OBJECT WalkDeviceStack(PDEVICE_OBJECT top, PULONG depth)
{
    PDEVICE_OBJECT cur = top;
    PDEVICE_OBJECT bottom = NULL;
    ULONG          d = 0;

    if (top == NULL) { *depth = 0; return NULL; }

    while (cur != NULL) {
        PDRIVER_OBJECT drv = cur->DriverObject;

        if (drv != NULL && drv->DriverName.Buffer != NULL) {
            LOG_INFO("    [stack %lu] dev=%p driver=%wZ", d, cur, &drv->DriverName);
        } else {
            LOG_INFO("    [stack %lu] dev=%p driver=<none>", d, cur);
        }

        bottom = cur;
        cur = IoGetLowerDeviceObject(cur);

        /* 起始设备不释放, 中间层释放 */
        if (bottom != top) {
            ObDereferenceObject(bottom);
        }
        d++;
    }

    *depth = d;
    return (bottom == top) ? NULL : bottom;
}

/*
 * EnumAllUsbHubs - 按 ACE 的路径枚举所有 hub 并装 hook
 *
 * 步骤 (和 ACE 一致):
 *   1. 锚定 \Driver\USBHUB3 → 拿 DRIVER_OBJECT
 *   2. 走 DeviceObject->NextDevice → 所有 hub 设备实例
 *   3. 对每个设备 IoGetLowerDeviceObject → 设备栈
 *   4. 装 IRP_MJ_PNP hook
 */
static VOID EnumAllUsbHubs(VOID)
{
    PDRIVER_OBJECT  drv = NULL;
    NTSTATUS        st;
    ULONG           devCount = 0;
    ULONG           maxDepth = 0;

    /* ---- 第 1 步: 锚定 USBHUB3 ---- */
    st = DriverLookupByName(L"\\Driver\\USBHUB3", &drv);
    g_HookStatus = (ULONG)st;

    if (!NT_SUCCESS(st) || drv == NULL) {
        LOG_ERROR("USBHUB3 driver not found: %08X", st);
        return;
    }

    LOG_INFO("found USBHUB3: DrvObj=%p Start=%p Size=%lu FirstDevice=%p",
             drv, drv->DriverStart, drv->DriverSize, drv->DeviceObject);

    /* ---- 第 2 步: 走设备链 ---- */
    for (PDEVICE_OBJECT dev = drv->DeviceObject; dev != NULL; dev = dev->NextDevice) {
        ULONG          depth  = 0;
        PDEVICE_OBJECT bottom;

        LOG_INFO("  hub device #%lu: %p", devCount, dev);

        /* ---- 第 3 步: 沿设备栈向下 ---- */
        bottom = WalkDeviceStack(dev, &depth);
        if (depth > maxDepth) maxDepth = depth;

        /* 底层是 PDO, 从它读 HardwareID */
        if (bottom != NULL) {
            TrackDevice(bottom);
            ObDereferenceObject(bottom);
        } else {
            TrackDevice(dev);
        }

        devCount++;
    }

    g_ScanCount = devCount;

    /* ---- 第 4 步: 装 PnP hook ---- */
    if (!g_HubSlotUsed) {
        RtlZeroMemory(&g_HubSlot, sizeof(g_HubSlot));
        st = HookInstall(&g_HubSlot.Hook, HOOK_DISPATCH, drv, MyHubPnp, IRP_MJ_PNP);

        if (NT_SUCCESS(st)) {
            g_HubSlot.Driver        = drv;
            g_HubSlot.DeviceCount   = devCount;
            g_HubSlot.MaxStackDepth = maxDepth;
            g_HubSlotUsed           = TRUE;
            g_HubFound              = 1;

            LOG_INFO("hook USBHUB3 ok: devices=%lu maxStackDepth=%lu orig=%p",
                     devCount, maxDepth, g_HubSlot.Hook.Original);
        } else {
            LOG_ERROR("hook USBHUB3 failed: %08X", st);
            ObDereferenceObject(drv);
        }
    }
}

/* ================= URB / Block ================= */

/*
 * SubmitUrb - 向某个设备提交一个 URB
 */
static NTSTATUS SubmitUrb(PDEVICE_OBJECT target, URB *urb)
{
    KEVENT          event;
    IO_STATUS_BLOCK iosb;
    PDEVICE_OBJECT  top;
    NTSTATUS        st;

    top = IoGetAttachedDeviceReference(target);
    if (top == NULL) return STATUS_UNSUCCESSFUL;

    KeInitializeEvent(&event, NotificationEvent, FALSE);
    RtlZeroMemory(&iosb, sizeof(iosb));

    PIRP irp = IoBuildDeviceIoControlRequest(IOCTL_INTERNAL_USB_SUBMIT_URB, top,
                                             urb, sizeof(URB), NULL, 0, TRUE, &event, &iosb);
    if (irp == NULL) {
        ObDereferenceObject(top);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    st = IofCallDriver(top, irp);
    if (st == STATUS_PENDING) {
        KeWaitForSingleObject(&event, Executive, KernelMode, FALSE, NULL);
        st = iosb.Status;
    }

    ObDereferenceObject(top);
    return st;
}

/*
 * BlockDevice - 伪造 PNP IRP 封锁/解封设备 (ACE 的做法)
 */
static NTSTATUS BlockDevice(PDEVICE_OBJECT target, BOOLEAN block)
{
    KEVENT          event;
    IO_STATUS_BLOCK iosb;
    PDEVICE_OBJECT  top;
    NTSTATUS        st;

    top = IoGetAttachedDeviceReference(target);
    if (top == NULL) return STATUS_UNSUCCESSFUL;

    KeInitializeEvent(&event, NotificationEvent, FALSE);
    RtlZeroMemory(&iosb, sizeof(iosb));

    PIRP irp = IoBuildSynchronousFsdRequest(IRP_MJ_PNP, top, NULL, 0, NULL, &event, &iosb);
    if (irp == NULL) {
        ObDereferenceObject(top);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

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

/* ================= IOCTL 处理 ================= */

static NTSTATUS DoEnum(PVOID buffer, ULONG outLen, PULONG info)
{
    USB_DEV_LIST *out;
    KIRQL         irql;
    ULONG         n = 0;

    if (buffer == NULL || outLen < sizeof(USB_DEV_LIST)) return STATUS_BUFFER_TOO_SMALL;

    out = (USB_DEV_LIST *)buffer;
    RtlZeroMemory(out, sizeof(USB_DEV_LIST));

    KeAcquireSpinLock(&g_Lock, &irql);
    for (int i = 0; i < USB_MAX_DEVICES; i++) {
        USB_DEV_INFO *d;
        if (!g_Devs[i].Active) continue;

        d = &out->Devs[n];
        d->Index = (ULONG)i;
        d->Active = TRUE;
        d->Vid = g_Devs[i].Vid;
        d->Pid = g_Devs[i].Pid;
        d->BcdDevice = g_Devs[i].BcdDevice;
        d->BusNumber = g_Devs[i].BusNumber;
        d->DeviceAddress = g_Devs[i].DeviceAddress;
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
    USB_CTRL_REQ   *req;
    PDEVICE_OBJECT  target = NULL;
    KIRQL           irql;
    PUCHAR          data;
    URB             urb;
    NTSTATUS        st;

    if (buffer == NULL || inLen < sizeof(USB_CTRL_REQ)) return STATUS_BUFFER_TOO_SMALL;

    req = (USB_CTRL_REQ *)buffer;
    if (req->DevIndex >= USB_MAX_DEVICES || req->wLength > USB_MAX_DATA)
        return STATUS_INVALID_PARAMETER;

    KeAcquireSpinLock(&g_Lock, &irql);
    if (g_Devs[req->DevIndex].Active) target = g_Devs[req->DevIndex].DeviceObject;
    KeReleaseSpinLock(&g_Lock, irql);

    if (target == NULL) return STATUS_DEVICE_NOT_CONNECTED;

    data = (PUCHAR)buffer + sizeof(USB_CTRL_REQ);

    RtlZeroMemory(&urb, sizeof(urb));
    urb.UrbHeader.Length   = sizeof(URB);
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

    st = SubmitUrb(target, &urb);
    if (NT_SUCCESS(st)) {
        req->bytesReturned = urb.UrbControlTransfer.TransferBufferLength;
        *info = sizeof(USB_CTRL_REQ);
    } else {
        req->bytesReturned = 0;
    }
    return st;
}

static NTSTATUS DoBlock(PVOID buffer, ULONG inLen)
{
    USB_BLOCK_REQ  *req;
    PDEVICE_OBJECT  target = NULL;
    KIRQL           irql;

    if (buffer == NULL || inLen < sizeof(USB_BLOCK_REQ)) return STATUS_BUFFER_TOO_SMALL;

    req = (USB_BLOCK_REQ *)buffer;
    if (req->DevIndex >= USB_MAX_DEVICES) return STATUS_INVALID_PARAMETER;

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
    USB_DEBUG_INFO *d;
    KIRQL           irql;

    if (buffer == NULL || outLen < sizeof(USB_DEBUG_INFO)) return STATUS_BUFFER_TOO_SMALL;

    d = (USB_DEBUG_INFO *)buffer;
    RtlZeroMemory(d, sizeof(USB_DEBUG_INFO));

    KeAcquireSpinLock(&g_Lock, &irql);
    for (int i = 0; i < USB_MAX_DEVICES; i++)
        if (g_Devs[i].Active) d->DeviceCount++;
    KeReleaseSpinLock(&g_Lock, irql);

    d->HookInstalled  = g_HubSlotUsed ? 1 : 0;
    d->HubDriverFound = g_HubFound;
    d->LastScanCount  = g_ScanCount;
    d->LastScanStatus = g_ScanStatus;
    d->HookStatus     = g_HookStatus;
    d->Tracked        = g_Tracked;

    *info = sizeof(USB_DEBUG_INFO);
    return STATUS_SUCCESS;
}

static NTSTATUS UsbAuditDispatch(PVOID buffer, ULONG inLen, ULONG outLen,
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

NTSTATUS UsbAuditInit(VOID)
{
    KeInitializeSpinLock(&g_Lock);
    RtlZeroMemory(g_Devs, sizeof(g_Devs));
    RtlZeroMemory(&g_HubSlot, sizeof(g_HubSlot));
    g_HubSlotUsed = FALSE;

    LOG_INFO("=== USB audit: resolving USBHUB3 ===");

    EnumAllUsbHubs();

    if (!g_HubSlotUsed) {
        LOG_ERROR("=== USB audit init FAILED: no hub hooked ===");
        return STATUS_OBJECT_NAME_NOT_FOUND;
    }

    LOG_INFO("=== USB audit ready: hub hooked, %lu device(s) scanned, %lu tracked ===",
             g_ScanCount, g_Tracked);
    return STATUS_SUCCESS;
}

VOID UsbAuditUnload(VOID)
{
    InterlockedExchange(&g_Unloading, 1);

    /* 等所有 hook 回调退出 */
    for (int i = 0; i < 200 && g_InHook != 0; i++) {
        LARGE_INTEGER d;
        d.QuadPart = -10000;
        KeDelayExecutionThread(KernelMode, FALSE, &d);
    }

    if (g_HubSlotUsed) {
        if (g_HubSlot.Hook.Active) {
            HookRemove(&g_HubSlot.Hook);
            LOG_INFO("unhooked USBHUB3");
        }
        if (g_HubSlot.Driver != NULL) {
            ObDereferenceObject(g_HubSlot.Driver);
            g_HubSlot.Driver = NULL;
        }
        g_HubSlotUsed = FALSE;
    }

    LOG_INFO("USB audit unloaded");
}

/* ================= 模块注册 ================= */

static const MODULE_DESC g_UsbModule = {
    "usb",
    IOCTL_USB_CONTROL_TRANSFER,
    4,
    UsbAuditDispatch
};

NTSTATUS UsbAuditRegister(VOID)
{
    return ModuleRegister(&g_UsbModule);
}
