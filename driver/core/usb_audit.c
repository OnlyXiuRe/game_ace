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
#include "driver_scan.h"
#include "usb_node.h"
#include "usb_hub.h"
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
    HOOK_ENTRY       Hook;               /* hook 框架的条目 */
    PDRIVER_OBJECT   Driver;             /* hub 的驱动对象 (带引用) */
    PDRIVER_DISPATCH OriginalDispatch;   /* 原 IRP_MJ_PNP 函数 (自己保存副本) */
    ULONG            DeviceCount;        /* 该驱动的设备数 */
    ULONG            MaxStackDepth;      /* 最深设备栈 */
} HUB_SLOT;

/* 最多跟踪的 hub 驱动数 —— 和 driver_scan 保持一致 */
#define HUB_SLOT_MAX  32

static HUB_SLOT       g_HubSlots[HUB_SLOT_MAX];
static ULONG          g_HubSlotCount = 0;
static BOOLEAN        g_HubSlotUsed  = FALSE;   /* 至少有一个成功 */

static volatile LONG  g_Unloading = 0;   /* 卸载标记 */
static volatile LONG  g_InHook    = 0;   /* 正在 hook 回调里的线程数 */
static ULONG          g_HookStatus = 0xFFFFFFFF;
static ULONG          g_HubFound   = 0;
static ULONG          g_Tracked    = 0;
static ULONG          g_ScanCount  = 0;
static ULONG          g_ScanStatus = 0xFFFFFFFF;

/*
 * hidusb 驱动对象。
 * ResolveHidUsb 在枚举之前就解析好，EnumAllUsbHubs 用它判断
 * 「这个驱动是不是 HID 数据通路」，只给 HID 设备建节点。
 */
static PDRIVER_OBJECT g_HidUsbDriver = NULL;

/* ================= 前向声明 ================= */
static NTSTATUS UsbAuditDispatch(PVOID buffer, ULONG inLen, ULONG outLen,
                                 ULONG code, PULONG info);
static NTSTATUS MyHubPnp(PDEVICE_OBJECT DeviceObject, PIRP Irp);
static VOID     EnumAllUsbHubs(VOID);
static NTSTATUS DoEnum(PVOID buffer, ULONG outLen, PULONG info);
static NTSTATUS DoCtrl(PVOID buffer, ULONG inLen, PULONG info);
static NTSTATUS DoBlock(PVOID buffer, ULONG inLen);
static NTSTATUS DoDebug(PVOID buffer, ULONG outLen, PULONG info);
static NTSTATUS DoKmbox(PVOID buffer, ULONG outLen, PULONG info);
static NTSTATUS DoNodes(PVOID buffer, ULONG outLen, PULONG info);
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
 * QueryDeviceId - 从 _DEVICE_NODE->InstancePath 读设备标识
 *
 * ============================================================
 * 这里改了实现（对齐 ACE）
 * ============================================================
 * 旧实现用 IoGetDeviceProperty(DevicePropertyHardwareID)：
 *   - 要求 PASSIVE_LEVEL（内部走配置管理器 + 注册表分页 I/O）
 *   - hub 的 FDO 通常没有 HardwareID，直接返回空
 *   - DISPATCH_LEVEL 下调用 = 立刻蓝屏
 *
 * ACE 完全不走这条路。它读的是 _DEVICE_NODE->InstancePath：
 *   DeviceObject->DeviceObjectExtension(+0x138)
 *     -> DeviceNode(+0x28)
 *     -> InstancePath(+0x28, Buffer 在 +0x30)
 *
 * 纯内存访问，任何 IRQL 都安全，hub FDO 也能读到路径。
 * 路径形如 USB\VID_046D&PID_C077\5&2a3b1c4&0&1。
 */
static BOOLEAN QueryDeviceId(PDEVICE_OBJECT devObj, PUSHORT vid, PUSHORT pid,
                             PUSHORT rev, PWCHAR hidOut, ULONG hidChars)
{
    UNICODE_STRING *path = NULL;
    NTSTATUS        st;

    *vid = *pid = *rev = 0;

    st = UsbGetInstancePath(devObj, &path);
    if (!NT_SUCCESS(st) || path == NULL) {
        LOG_DEBUG("UsbGetInstancePath dev=%p -> %08X", devObj, st);
        return FALSE;
    }

    if (!UsbParseVidPid(path, vid, pid)) {
        LOG_DEBUG("InstancePath has no VID/PID: %wZ", path);
        return FALSE;
    }

    /* REV_ 可选，有就解 */
    {
        USHORT i, n = path->Length / sizeof(WCHAR);
        for (i = 0; i + 8 <= n; i++) {
            const WCHAR *p = path->Buffer + i;
            if (p[0]==L'R' && p[1]==L'E' && p[2]==L'V' && p[3]==L'_') {
                ParseHex4(p + 4, rev);
                break;
            }
        }
    }

    if (hidOut && hidChars) {
        ULONG chars = path->Length / sizeof(WCHAR);
        if (chars >= hidChars) chars = hidChars - 1;
        RtlStringCchCopyNW(hidOut, hidChars, path->Buffer, chars);
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
    for (ULONG i = 0; i < g_HubSlotCount; i++) {
        if (g_HubSlots[i].Driver == drv) {
            /*
             * 用自己的 OriginalDispatch 副本, 而不是 Hook.Original。
             * 原因: HookInstall 内部会在某个时刻回填 Hook.Original,
             *       如果 hook 已经生效但 Original 还没回填, 这里就会
             *       拿到 NULL, 进而把 PnP IRP 用错误状态完成 → 0xCA。
             *       OriginalDispatch 在登记 slot 时就已写好, 永远可用。
             */
            orig = g_HubSlots[i].OriginalDispatch;
            break;
        }
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
        KIRQL irql = KeGetCurrentIrql();

        /*
         * IRQL 说明（已按 ACE 的实现重写）:
         *
         *   旧实现里 START_DEVICE 必须判 PASSIVE_LEVEL，因为
         *   TrackDevice -> QueryDeviceId -> IoGetDeviceProperty 会走
         *   配置管理器 + 注册表，DISPATCH_LEVEL 下调用直接蓝屏。
         *
         *   现在 QueryDeviceId 改成读 _DEVICE_NODE->InstancePath，
         *   UsbNodeAdd 也只做内存读 + 自旋锁，所以两个 IRQL 都能跑。
         *   irql 仍然保留下来做日志，便于排查异常路径。
         */
        if (minor == IRP_MN_REMOVE_DEVICE) {
            InterlockedIncrement(&g_InHook);
            UsbNodeRemove(DeviceObject);          /* 节点表移除 */
            UntrackDevice(DeviceObject);          /* 旧表移除 */
            InterlockedDecrement(&g_InHook);
        } else if (minor == IRP_MN_START_DEVICE) {
            InterlockedIncrement(&g_InHook);

            /*
             * 先入节点表（类型先记 UNKNOWN，真正的鼠标/键盘分类
             * 由 UsbNodeClassifyAll 在 IOCTL 上下文里发 HID 描述符
             * 请求完成 —— 和 ACE 的 ClassifyHidDevice 一样，不能在
             * PnP 路径里发同步 URB）。
             */
            UsbNodeAdd(DeviceObject, HID_TYPE_NONE);
            TrackDevice(DeviceObject);

            InterlockedDecrement(&g_InHook);

            if (irql != PASSIVE_LEVEL)
                LOG_DEBUG("MyHubPnp: START at IRQL %lu (dev=%p) - still safe",
                          (ULONG)irql, DeviceObject);
        }
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
    PDEVICE_OBJECT cur  = top;
    PDEVICE_OBJECT prev = NULL;
    ULONG          d    = 0;

    if (top == NULL) { *depth = 0; return NULL; }

    while (cur != NULL) {
        PDRIVER_OBJECT drv = cur->DriverObject;
        PDEVICE_OBJECT lower;

        if (drv != NULL && drv->DriverName.Buffer != NULL) {
            LOG_INFO("    [stack %lu] dev=%p driver=%wZ", d, cur, &drv->DriverName);
        } else {
            LOG_INFO("    [stack %lu] dev=%p driver=<none>", d, cur);
        }

        /* 向下走一层, lower 自带一份引用 */
        lower = IoGetLowerDeviceObject(cur);

        /*
         * 释放"上一轮"的中间层。
         * 起始设备 top 是调用方持有的引用, 绝对不能在这里释放。
         */
        if (prev != NULL && prev != top) {
            ObDereferenceObject(prev);
        }

        prev = cur;
        d++;
        cur = lower;
    }

    *depth = d;

    /*
     * 循环结束, prev 是最底层:
     *   - prev == top  说明设备没有下层, 返回 NULL,
     *                  调用方什么都不用释放 (top 是它的引用)。
     *   - 否则 prev 还持有一份引用 (最后一次 IoGetLowerDeviceObject
     *     拿到的), 交给调用方 ObDereferenceObject。
     *
     * 注意: 不能像旧实现那样在循环里把 bottom 也 release 掉,
     *       否则调用方再 release 一次 = 重复释放 = 0x18 蓝屏。
     */
    return (prev == top) ? NULL : prev;
}

/*
 * EnumAllUsbHubs - 动态发现所有 USB 驱动并装 PnP hook
 *
 * 不再硬编码驱动名, 走 DriverScanFindAll:
 *   1. 遍历 \Driver 目录 (128 个驱动对象)
 *   2. 对每个驱动看 DeviceObject 链的 DeviceType
 *   3. DeviceType == FILE_DEVICE_USB (0x22) 的就是 USB 驱动
 *   4. 走设备链记录设备, 装 IRP_MJ_PNP hook
 *
 * 这样任何机器、任何厂商的 USB 驱动都能自动发现。
 */
static VOID EnumAllUsbHubs(VOID)
{
    ULONG found;

    g_ScanStatus = 0;
    g_ScanCount  = 0;
    g_HubSlotCount = 0;
    g_HubSlotUsed  = FALSE;

    found = DriverScanFindAll(NULL);

    if (found == 0) {
        LOG_ERROR("EnumAllUsbHubs: no USB driver discovered");
        g_HookStatus = STATUS_OBJECT_NAME_NOT_FOUND;
        return;
    }

    /* 对每个发现的 USB 驱动: 走设备链 + 装 hook */
    for (ULONG i = 0; i < DriverScanCount(); i++) {
        SCAN_USB_DRIVER *sd = DriverScanGet(i);
        PDRIVER_OBJECT   drv;
        ULONG            devCount = 0;
        ULONG            maxDepth = 0;
        NTSTATUS         st;
        BOOLEAN          isHid;

        if (sd == NULL || sd->Driver == NULL) continue;
        drv = sd->Driver;

        /*
         * 只有 hidusb 的设备才建档。
         * 原因：10 槽表装不下 24 个 USB 设备；而且 KMBox 判定只关心
         * HID 输入设备，hub/控制器/蓝牙这些建了也没用。
         */
        isHid = (g_HidUsbDriver != NULL && drv == g_HidUsbDriver);

        LOG_INFO("--- driver %wZ (DrvObj=%p)%s ---", &sd->Name, drv,
                 isHid ? "  [HID data path]" : "");
        LOG_INFO("    DriverStart=%p Size=%lu FirstDevice=%p",
                 drv->DriverStart, drv->DriverSize, drv->DeviceObject);

        /* 走设备链 */
        for (PDEVICE_OBJECT dev = drv->DeviceObject; dev != NULL; dev = dev->NextDevice) {
            ULONG          depth = 0;
            PDEVICE_OBJECT bottom;
            PDEVICE_OBJECT pathSrc;

            LOG_INFO("    device #%lu: %p (type=0x%lx)",
                     devCount, dev, dev->DeviceType);

            bottom = WalkDeviceStack(dev, &depth);
            if (depth > maxDepth) maxDepth = depth;

            /*
             * ★ 建档（这一步之前漏了，导致节点表为空）
             *
             * 只靠 PnP START_DEVICE 建档是不够的 —— 驱动加载时所有设备
             * 早就 START 完了，PnP 事件不会再触发。ACE 的做法就是在这里
             * 主动遍历已有设备建档。
             *
             * 键用 dev（设备栈顶部）：hidusb 的 URB 完成例程拿到的就是
             * 这个对象，必须用它才能查表命中。
             * 路径用 pathSrc（设备栈底部）：只有底部那层才有意义的
             * InstancePath 和 _DEVICE_NODE 父子链。
             */
            pathSrc = (bottom != NULL) ? bottom : dev;

            if (isHid) {
                TrackDevice(pathSrc);
                UsbNodeAddEx(dev, pathSrc, HID_TYPE_NONE);
                if (pathSrc != dev)
                    UsbNodeAddEx(pathSrc, pathSrc, HID_TYPE_NONE);
            }

            if (bottom != NULL)
                ObDereferenceObject(bottom);

            devCount++;
        }

        g_ScanCount += devCount;

        /* 装 hook */
        if (g_HubSlotCount < HUB_SLOT_MAX) {
            HUB_SLOT         *slot = &g_HubSlots[g_HubSlotCount];
            PDRIVER_DISPATCH  orig;

            RtlZeroMemory(slot, sizeof(*slot));

            /*
             * ============================================================
             * 顺序至关重要 —— 这里踩过 0xCA PNP_DETECTED_FATAL_ERROR
             * ============================================================
             * HookInstall 一旦返回, MajorFunction[27] 就指向 MyHubPnp。
             * 如果那时 slot 还没登记 (g_HubSlotCount 没加),
             * FindOrigByDevice 就查不到它, MyHubPnp 拿到 orig == NULL,
             * 于是用 STATUS_DEVICE_NOT_READY 把 PnP IRP 完成掉 ——
             * 设备的 PnP 状态机被打乱, 立刻 0xCA 蓝屏。
             *
             * 扫描 126 个驱动要几十毫秒, 这个窗口足够 PnP 管理器塞进
             * 一批 IRP, 所以必现。
             *
             * 正确顺序:
             *   1. 先读原派发函数
             *   2. 填好 slot 并 ObReferenceObject
             *   3. g_HubSlotCount++ (从此 FindOrigByDevice 能找到它)
             *   4. 最后才 HookInstall (此时才真正生效)
             * ============================================================
             */
            orig = (PDRIVER_DISPATCH)drv->MajorFunction[IRP_MJ_PNP];

            ObReferenceObject(drv);
            slot->Driver           = drv;
            slot->OriginalDispatch = orig;
            slot->DeviceCount      = devCount;
            slot->MaxStackDepth    = maxDepth;
            g_HubSlotCount++;                 /* 先登记 */

            st = HookInstall(&slot->Hook, HOOK_DISPATCH, drv, MyHubPnp, IRP_MJ_PNP);

            if (NT_SUCCESS(st)) {
                sd->Hooked    = TRUE;
                g_HubSlotUsed = TRUE;

                LOG_INFO("    hook OK: orig=%p handler=%p", orig, MyHubPnp);
            } else {
                /* 回滚: 把刚登记的 slot 撤掉, 绝不留下半成品 */
                g_HubSlotCount--;
                ObDereferenceObject(drv);
                RtlZeroMemory(slot, sizeof(*slot));
                LOG_ERROR("    hook failed: %08X", st);
            }
        } else {
            LOG_WARN("    slot table full, skip %wZ", &sd->Name);
        }
    }

    g_HubFound = g_HubSlotCount;
    g_HookStatus = g_HubSlotUsed ? 0 : STATUS_UNSUCCESSFUL;

    LOG_INFO("=== %lu driver(s) discovered, %lu hooked, %lu device(s) scanned ===",
             found, g_HubSlotCount, g_ScanCount);
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

/* ================= hidusb URB 拦截 =================
 *
 * 对齐 ACE：
 *   ACE sub_1400860A0 HookHidUsbInternalDeviceControl
 *       -> 替换 \Driver\hidusb 的 MajorFunction[15]
 *          (IRP_MJ_INTERNAL_DEVICE_CONTROL = 15)
 *   ACE 用 5 个独立的 slot handler（0x1400860E0/140/1A0/200/260），
 *   每个 slot 对应一个设备，超过 5 个打 "Find more then %d usbhub"。
 *   我们用「单 handler + DeviceObject→节点查表」达到同样效果，
 *   不需要复制 5 份代码。
 *
 *   ACE AntiEmuUsbIrpCompletionHandler (0x1400872D0):
 *       URB+40 的第一个字节 = Report ID
 *       -> 匹配 node+808 的 ReportId
 *       -> ExtractHidBitField 抽字段
 *       -> DispatchMouseReport -> SubmitMouseSample
 */

#define HIDUSB_DRIVER_NAME  L"\\Driver\\hidusb"

/* g_HidUsbDriver 已在上面状态区声明 */
static PDRIVER_DISPATCH g_HidUsbOrigIoctl = NULL;
static volatile LONG    g_HidUsbReportCount = 0;

/*
 * OnHidUsbCompletion - URB 完成回调
 *
 * 只在成功且是 IN 方向的 interrupt/bulk 传输时取数据。
 * 注意：这个回调可能在 DISPATCH_LEVEL 运行，只能做内存操作，
 * 不能发新 IRP、不能等待。
 */
static NTSTATUS OnHidUsbCompletion(PDEVICE_OBJECT DevObj, PIRP Irp, PVOID Context)
{
    PIO_STACK_LOCATION sl;
    PURB               urb;
    PUCHAR             data;
    ULONG              len;

    UNREFERENCED_PARAMETER(Context);

    if (NT_SUCCESS(Irp->IoStatus.Status) && g_Unloading == 0) {
        sl = IoGetCurrentIrpStackLocation(Irp);
        if (sl->Parameters.DeviceIoControl.IoControlCode == IOCTL_INTERNAL_USB_SUBMIT_URB) {
            urb = (PURB)sl->Parameters.Others.Argument1;

            /*
             * 只观察输入报告（鼠标/键盘数据），只计数。
             *
             * 报告描述符的截取已经移除 —— 之前那条路（从
             * URB_FUNCTION_GET_DESCRIPTOR_FROM_DEVICE 抄描述符）对
             * 已在线设备根本不会触发，因为 hidclass 只在设备启动时
             * 拉一次。要做位域抽取必须先把描述符拿到，见 usb_node.h
             * 顶部注释里的后续计划。
             */
            if (urb != NULL &&
                urb->UrbHeader.Function == URB_FUNCTION_BULK_OR_INTERRUPT_TRANSFER)
            {
                data = (PUCHAR)urb->UrbBulkOrInterruptTransfer.TransferBuffer;
                len  = urb->UrbBulkOrInterruptTransfer.TransferBufferLength;

                if (data != NULL && len > 0 && MmIsAddressValid(data)) {
                    InterlockedIncrement(&g_HidUsbReportCount);
                    UsbNodeNoteReport(DevObj, data, len);
                }
            }
        }
    }

    if (Irp->PendingReturned)
        IoMarkIrpPending(Irp);

    return STATUS_SUCCESS;
}

/*
 * MyHidUsbIoctl - hidusb 的 IRP_MJ_INTERNAL_DEVICE_CONTROL 拦截
 *
 * 只挂完成例程，不改栈、不改 IRP、不改返回值 —— 纯旁路观察，
 * 保证不破坏 hidusb 自己的状态机（这是踩过 0xCA 的教训）。
 */
static NTSTATUS MyHidUsbIoctl(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    PIO_STACK_LOCATION sl;

    if (g_HidUsbOrigIoctl == NULL) {
        Irp->IoStatus.Status      = STATUS_DEVICE_NOT_READY;
        Irp->IoStatus.Information = 0;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return STATUS_DEVICE_NOT_READY;
    }

    sl = IoGetCurrentIrpStackLocation(Irp);

    if (g_Unloading == 0 &&
        sl->Parameters.DeviceIoControl.IoControlCode == IOCTL_INTERNAL_USB_SUBMIT_URB)
    {
        PURB urb = (PURB)sl->Parameters.Others.Argument1;
        if (urb == NULL) return g_HidUsbOrigIoctl(DeviceObject, Irp);

        /*
         * 挂完成例程的两种情况：
         *   1. GET_DESCRIPTOR(HID Report)  —— 截描述符来建字段布局
         *   2. 中断 IN 传输              —— 截鼠标/键盘输入报告
         * 其它 URB 一律放行，不干预。
         */
        if (urb->UrbHeader.Function == URB_FUNCTION_GET_DESCRIPTOR_FROM_DEVICE &&
            urb->UrbControlDescriptorRequest.DescriptorType == 0x22 /* HID Report */)
        {
            IoSetCompletionRoutine(Irp, OnHidUsbCompletion, NULL, TRUE, TRUE, TRUE);
        } else if (urb->UrbHeader.Function == URB_FUNCTION_BULK_OR_INTERRUPT_TRANSFER &&
                   (urb->UrbBulkOrInterruptTransfer.TransferFlags & USBD_TRANSFER_DIRECTION_IN))
        {
            /*
             * 在当前栈位置挂完成例程。原驱动往下转发时会用
             * IoCopyCurrentIrpStackLocationToNext 把我们的栈位复制到
             * 下一层，它自己的完成例程挂在更低的位置，两者不冲突。
             */
            IoSetCompletionRoutine(Irp, OnHidUsbCompletion, NULL, TRUE, TRUE, TRUE);
        }
    }

    return g_HidUsbOrigIoctl(DeviceObject, Irp);
}

/*
 * HookHidUsb - 锚定 \Driver\hidusb 并替换 MajorFunction[15]
 *
 * 为什么必须 hook hidusb 而不是 usbhub：
 *   usbhub 只管端口/枚举，不碰 HID 报告；
 *   真正的鼠标/键盘数据流是
 *      设备 -> usbhub -> hidusb -> mouhid/kbdhid -> mouclass/kbdclass
 *   hidusb 是唯一能看到原始 HID 报告的一层。
 */
/*
 * ResolveHidUsb - 提前解析 \Driver\hidusb
 *
 * 必须在 EnumAllUsbHubs 之前调用：枚举时要靠 g_HidUsbDriver 判断
 * "这个驱动是不是 HID 数据通路的顶部"，只给 hidusb 的设备建档。
 * （不提前解析的话，24 个 USB 设备会把 10 槽节点表瞬间撑爆）
 */
static NTSTATUS ResolveHidUsb(VOID)
{
    PDRIVER_OBJECT drv = NULL;
    NTSTATUS       st;

    if (g_HidUsbDriver != NULL) return STATUS_SUCCESS;

    st = DriverLookupByName(HIDUSB_DRIVER_NAME, &drv);
    if (!NT_SUCCESS(st) || drv == NULL) {
        LOG_ERROR("ResolveHidUsb: resolve %ws failed %08X", HIDUSB_DRIVER_NAME, st);
        return st;
    }

    ObReferenceObject(drv);
    g_HidUsbDriver = drv;
    LOG_INFO("hidusb resolved: DrvObj=%p", drv);
    return STATUS_SUCCESS;
}

static NTSTATUS HookHidUsb(VOID)
{
    PDRIVER_OBJECT   drv;
    PDRIVER_DISPATCH orig;
    NTSTATUS         st;

    st = ResolveHidUsb();
    if (!NT_SUCCESS(st)) return st;

    drv = g_HidUsbDriver;
    if (drv == NULL) return STATUS_OBJECT_NAME_NOT_FOUND;

    orig = (PDRIVER_DISPATCH)drv->MajorFunction[IRP_MJ_INTERNAL_DEVICE_CONTROL];
    if (orig == NULL) {
        LOG_ERROR("HookHidUsb: hidusb MajorFunction[15] is NULL");
        return STATUS_PROCEDURE_NOT_FOUND;
    }

    /*
     * 顺序照搬 USBHUB3 那次踩坑的结论：
     *   1. 先保存原始函数
     *   2. 再让全局可见（g_HidUsbOrigIoctl 写好）
     *   3. 最后才真正替换 MajorFunction
     * 否则 hook 生效但原函数还没保存 -> 所有 HID 请求全部失败。
     */
    g_HidUsbOrigIoctl = orig;
    MemoryBarrier();

    InterlockedExchangePointer((PVOID volatile *)&drv->MajorFunction[IRP_MJ_INTERNAL_DEVICE_CONTROL],
                               (PVOID)MyHidUsbIoctl);

    LOG_INFO("hidusb hooked: drv=%p orig=%p handler=%p",
             drv, orig, MyHidUsbIoctl);
    return STATUS_SUCCESS;
}

static VOID UnhookHidUsb(VOID)
{
    if (g_HidUsbDriver == NULL) return;

    if (g_HidUsbOrigIoctl != NULL) {
        InterlockedExchangePointer(
            (PVOID volatile *)&g_HidUsbDriver->MajorFunction[IRP_MJ_INTERNAL_DEVICE_CONTROL],
            (PVOID)g_HidUsbOrigIoctl);
        LOG_INFO("hidusb unhooked");
    }

    ObDereferenceObject(g_HidUsbDriver);
    g_HidUsbDriver    = NULL;
    g_HidUsbOrigIoctl = NULL;
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

    d->HookInstalled  = g_HubSlotCount;
    d->HubDriverFound = g_HubFound;
    d->LastScanCount  = g_ScanCount;
    d->LastScanStatus = g_ScanStatus;
    d->HookStatus     = g_HookStatus;
    d->Tracked        = g_Tracked;

    *info = sizeof(USB_DEBUG_INFO);
    return STATUS_SUCCESS;
}

/*
 * DoKmbox - ★ KMBox 判定（对应 ACE AntiHw_FindSuspiciousKmbox）
 *
 * 流程：
 *   1. UsbNodeClassifyAll() 给未分类节点打上 鼠标/键盘 标记
 *      （发 HID 报告描述符请求，必须在 PASSIVE_LEVEL）
 *   2. UsbNodeDetectKmbox() 比对祖父设备，输出命中表
 */
static NTSTATUS DoKmbox(PVOID buffer, ULONG outLen, PULONG info)
{
    KMBOX_LIST   *out;
    KMBOX_RESULT  hits[KMBOX_RESULT_MAX];
    ULONG         hitCount = 0;
    NTSTATUS      st;

    if (buffer == NULL || outLen < sizeof(KMBOX_LIST))
        return STATUS_BUFFER_TOO_SMALL;

    out = (KMBOX_LIST *)buffer;
    RtlZeroMemory(out, sizeof(KMBOX_LIST));

    /* 先分类（IRQL 不够就跳过，不阻塞调用方） */
    if (KeGetCurrentIrql() == PASSIVE_LEVEL) {
        st = UsbNodeClassifyAll();
        if (!NT_SUCCESS(st))
            LOG_DEBUG("DoKmbox: ClassifyAll -> %08X", st);
    } else {
        LOG_WARN("DoKmbox: skip classification at IRQL %lu",
                 (ULONG)KeGetCurrentIrql());
    }

    RtlZeroMemory(hits, sizeof(hits));
    st = UsbNodeDetectKmbox(hits, KMBOX_RESULT_MAX_UM, &hitCount);
    if (!NT_SUCCESS(st)) return st;

    out->Count = hitCount;
    for (ULONG i = 0; i < hitCount && i < KMBOX_RESULT_MAX_UM; i++) {
        out->Hits[i].MouseVid = hits[i].MouseVid;
        out->Hits[i].MousePid = hits[i].MousePid;
        out->Hits[i].KbdVid   = hits[i].KbdVid;
        out->Hits[i].KbdPid   = hits[i].KbdPid;
        RtlStringCchCopyW(out->Hits[i].MouseProduct, HID_NAME_MAX_UM, hits[i].MouseProduct);
        RtlStringCchCopyW(out->Hits[i].KbdProduct,   HID_NAME_MAX_UM, hits[i].KbdProduct);
        RtlStringCchCopyW(out->Hits[i].SharedPath,   HID_PATH_MAX_UM, hits[i].SharedPath);
    }

    *info = sizeof(KMBOX_LIST);
    return STATUS_SUCCESS;
}

/*
 * DoNodes - 导出节点表快照
 */
static NTSTATUS DoNodes(PVOID buffer, ULONG outLen, PULONG info)
{
    HID_NODE_LIST *out;
    ULONG          n = 0;

    if (buffer == NULL || outLen < sizeof(HID_NODE_LIST))
        return STATUS_BUFFER_TOO_SMALL;

    out = (HID_NODE_LIST *)buffer;
    RtlZeroMemory(out, sizeof(HID_NODE_LIST));

    for (ULONG i = 0; i < HID_NODE_MAX && n < HID_NODE_MAX_UM; i++) {
        PHID_NODE node = UsbNodeGet(i);
        if (node == NULL) continue;

        out->Nodes[n].Slot       = node->Slot;
        out->Nodes[n].Vid        = node->Vid;
        out->Nodes[n].Pid        = node->Pid;
        out->Nodes[n].DeviceType = node->DeviceType;
        out->Nodes[n].Blocked    = node->Blocked;
        RtlStringCchCopyW(out->Nodes[n].ProductName,  HID_NAME_MAX_UM, node->ProductName);
        RtlStringCchCopyW(out->Nodes[n].InstancePath, HID_PATH_MAX_UM, node->InstancePath);
        n++;
    }
    out->Count = n;

    *info = sizeof(HID_NODE_LIST);
    return STATUS_SUCCESS;
}

/*
 * DoFullEnum - ★ 全量枚举（走 Hub 端口，采完整描述符）
 *
 * 对应 IOCTL_USB_FULL_ENUM。内部会遍历所有 hub 的所有端口，
 * 对已连接端口取：设备描述符 + 配置描述符 + 字符串 + HID 报告描述符。
 *
 * ★ 必须 PASSIVE_LEVEL（内部发同步 IOCTL）
 */
static NTSTATUS DoFullEnum(PVOID buffer, ULONG outLen, PULONG info)
{
    USB_FULL_LIST_UM *out;
    USB_FULL_LIST     list;
    NTSTATUS          st;

    if (buffer == NULL || outLen < sizeof(USB_FULL_LIST_UM))
        return STATUS_BUFFER_TOO_SMALL;

    if (KeGetCurrentIrql() != PASSIVE_LEVEL)
        return STATUS_INVALID_DEVICE_STATE;

    out = (USB_FULL_LIST_UM *)buffer;
    RtlZeroMemory(out, sizeof(USB_FULL_LIST_UM));

    RtlZeroMemory(&list, sizeof(list));
    st = UsbHubEnumAll(&list);

    out->LastStatus = (ULONG)st;
    out->HubCount   = list.HubCount;
    *info = sizeof(USB_FULL_LIST_UM);

    if (!NT_SUCCESS(st)) return st;

    for (ULONG i = 0; i < list.Count && i < USB_FULL_MAX_UM; i++) {
        USB_DEV_FULL *s = &list.Devs[i];
        USB_DEV_UM   *d = &out->Devs[i];
        ULONG         k;

        d->HubIndex          = s->HubIndex;
        d->PortNumber        = s->PortNumber;
        d->Speed             = s->Speed;
        d->Suspicious        = UsbHubIsSuspiciousComposite(s) ? 1 : 0;
        d->Vid               = s->Vid;
        d->Pid               = s->Pid;
        d->BcdDevice         = s->BcdDevice;
        d->BcdUSB            = s->BcdUSB;
        d->DeviceClass       = s->DeviceClass;
        d->DeviceSubClass    = s->DeviceSubClass;
        d->DeviceProtocol    = s->DeviceProtocol;
        d->NumConfigurations = s->NumConfigurations;
        d->ConfigValue       = s->ConfigValue;
        d->NumInterfaces     = s->NumInterfaces;
        d->ConfigTotalLength = s->ConfigTotalLength;

        RtlStringCchCopyW(d->Manufacturer, USB_STR_MAX_UM, s->Manufacturer);
        RtlStringCchCopyW(d->Product,      USB_STR_MAX_UM, s->Product);
        RtlStringCchCopyW(d->Serial,       USB_STR_MAX_UM, s->Serial);

        d->IfaceCount = (s->IfaceCount > USB_IFACE_MAX_UM)
                        ? USB_IFACE_MAX_UM : s->IfaceCount;
        for (k = 0; k < d->IfaceCount; k++) {
            d->Ifaces[k].Number    = s->Ifaces[k].InterfaceNumber;
            d->Ifaces[k].Class     = s->Ifaces[k].Class;
            d->Ifaces[k].SubClass  = s->Ifaces[k].SubClass;
            d->Ifaces[k].Protocol  = s->Ifaces[k].Protocol;
            d->Ifaces[k].Endpoints = s->Ifaces[k].EndpointCount;
            d->Ifaces[k].HasReport = s->Ifaces[k].HasReportDesc;
            d->Ifaces[k].ReportLen = s->Ifaces[k].ReportDescLen;
            d->Ifaces[k].HidDescLen  = s->Ifaces[k].HidDescLen;
            d->Ifaces[k].HidBcdHID   = s->Ifaces[k].HidBcdHID;
            d->Ifaces[k].HidCountry  = s->Ifaces[k].HidCountry;
            d->Ifaces[k].HidNumDesc  = s->Ifaces[k].HidNumDesc;
            d->Ifaces[k].HasPhysDesc = s->Ifaces[k].HasPhysDesc;
            d->Ifaces[k].PhysDescLen = s->Ifaces[k].PhysDescLen;
        }

        out->Count++;
    }

    return STATUS_SUCCESS;
}

/*
 * DoReportDesc - 取某个设备某个接口的 HID 报告描述符
 *
 * 输入 USB_REPORT_REQ，输出 USB_REPORT_RSP。
 * 数据来自上一次 UsbHubEnumAll 的缓存。
 */
static NTSTATUS DoReportDesc(PVOID buffer, ULONG inLen, ULONG outLen, PULONG info)
{
    USB_REPORT_RSP *rsp;
    USB_DEV_FULL   *dev;
    ULONG           devIdx, ifIdx, len;

    if (buffer == NULL) return STATUS_INVALID_PARAMETER;
    if (inLen  < sizeof(USB_REPORT_REQ)) return STATUS_BUFFER_TOO_SMALL;
    if (outLen < sizeof(USB_REPORT_RSP)) return STATUS_BUFFER_TOO_SMALL;

    /* METHOD_BUFFERED：先读请求，再覆盖写响应 */
    devIdx = ((USB_REPORT_REQ *)buffer)->DevIndex;
    ifIdx  = ((USB_REPORT_REQ *)buffer)->IfaceIndex;

    dev = UsbHubGetCached(devIdx);
    if (dev == NULL) return STATUS_INVALID_PARAMETER;
    if (ifIdx >= dev->IfaceCount || ifIdx >= USB_IFACE_MAX_UM)
        return STATUS_INVALID_PARAMETER;
    if (!dev->Ifaces[ifIdx].HasReportDesc)
        return STATUS_NOT_FOUND;

    rsp = (USB_REPORT_RSP *)buffer;
    RtlZeroMemory(rsp, sizeof(USB_REPORT_RSP));

    len = dev->Ifaces[ifIdx].ReportDescLen;
    if (len > USB_REPORT_MAX_UM) len = USB_REPORT_MAX_UM;

    rsp->DevIndex   = devIdx;
    rsp->IfaceIndex = ifIdx;
    rsp->Length     = len;
    RtlCopyMemory(rsp->Data, dev->Ifaces[ifIdx].ReportDesc, len);

    *info = sizeof(USB_REPORT_RSP);
    return STATUS_SUCCESS;
}

/*
 * DoRawControl - 任意控制传输（3 环 fuzz 用）
 *
 * 输入 USB_RAW_CTRL_REQ，原地返回 Status/Returned/Data。
 * 底层是 UsbUrbControlTransfer（URB_FUNCTION_CONTROL_TRANSFER +
 * USBD_DEFAULT_PIPE_TRANSFER），等价于 libusb_control_transfer。
 *
 * ★ PASSIVE_LEVEL
 */
static NTSTATUS DoRawControl(PVOID buffer, ULONG inLen, ULONG outLen, PULONG info)
{
    USB_RAW_CTRL_REQ *r;
    ULONG             got = 0;
    ULONG             buflen;
    USHORT            declared;
    NTSTATUS          st;

    if (buffer == NULL) return STATUS_INVALID_PARAMETER;
    if (inLen  < sizeof(USB_RAW_CTRL_REQ)) return STATUS_BUFFER_TOO_SMALL;
    if (outLen < sizeof(USB_RAW_CTRL_REQ)) return STATUS_BUFFER_TOO_SMALL;
    if (KeGetCurrentIrql() != PASSIVE_LEVEL) return STATUS_INVALID_DEVICE_STATE;

    r = (USB_RAW_CTRL_REQ *)buffer;

    /*
     * 声明长度与实际缓冲区分离：
     *   wLength 原样写进 SetupPacket（fuzz 要探 wLength=65535 这种越界值）
     *   实际缓冲区最大 USB_RAW_MAX_UM 字节
     */
    declared = r->wLength;
    buflen   = (declared > USB_RAW_MAX_UM) ? USB_RAW_MAX_UM : declared;

    st = UsbUrbControlTransfer(r->DevIndex, r->bmRequestType, r->bRequest,
                               r->wValue, r->wIndex,
                               buflen ? r->Data : NULL,
                               (USHORT)buflen, declared, &got);

    r->Status   = (ULONG)st;
    r->Returned = got;
    *info = sizeof(USB_RAW_CTRL_REQ);

    /*
     * 即使控制传输本身失败也返回 STATUS_SUCCESS —— 让 3 环读 r->Status
     * 区分「IOCTL 失败」和「设备 STALL」。fuzz 需要后者作为指纹信号。
     */
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
    case IOCTL_USB_KMBOX:            return DoKmbox(buffer, outLen, info);
    case IOCTL_USB_NODES:            return DoNodes(buffer, outLen, info);
    case IOCTL_USB_FULL_ENUM:        return DoFullEnum(buffer, outLen, info);
    case IOCTL_USB_REPORT_DESC:      return DoReportDesc(buffer, inLen, outLen, info);
    case IOCTL_USB_RAW_CONTROL:      return DoRawControl(buffer, inLen, outLen, info);
    default:                         return STATUS_INVALID_DEVICE_REQUEST;
    }
}

/* ================= 生命周期 ================= */

/*
 * DumpHex - 把描述符按 16 字节一行打到日志
 *
 * 注意：不要用 RtlStringCchPrintfA 的返回值做前进量 —— 它返回
 * NTSTATUS（成功=0），不是写入长度。之前就是这么写导致每次都从
 * 缓冲区开头覆盖，最后只剩最后一个字节。
 */
static const CHAR g_HexDigits[] = "0123456789ABCDEF";

static VOID DumpHex(const char *Tag, const UCHAR *Data, ULONG Len)
{
    CHAR  hex[16 * 3 + 4];
    ULONG i;

    if (Data == NULL || Len == 0) return;

    for (i = 0; i < Len; i += 16) {
        ULONG n = (Len - i > 16) ? 16 : (Len - i);
        ULONG p = 0;

        for (ULONG k = 0; k < n; k++) {
            hex[p++] = g_HexDigits[(Data[i + k] >> 4) & 0x0F];
            hex[p++] = g_HexDigits[Data[i + k] & 0x0F];
            hex[p++] = ' ';
        }
        hex[p] = 0;

        LOG_INFO("   %s[%03lu] %s", Tag, i, hex);
    }
}

/*
 * RunFullEnum - 全量枚举并打日志
 *
 * 在 UsbAuditInit 里跑一遍，这样驱动一加载日志里就有完整的设备列表
 * 和所有描述符，不需要 3 环去触发。
 */
static VOID RunFullEnum(VOID)
{
    USB_FULL_LIST *list;
    NTSTATUS       st;

    if (KeGetCurrentIrql() != PASSIVE_LEVEL) return;

    list = (USB_FULL_LIST *)ExAllocatePoolWithTag(NonPagedPool,
                                                  sizeof(USB_FULL_LIST), 'bHsU');
    if (list == NULL) {
        LOG_ERROR("RunFullEnum: alloc %lu bytes failed", (ULONG)sizeof(USB_FULL_LIST));
        return;
    }

    RtlZeroMemory(list, sizeof(USB_FULL_LIST));
    st = UsbHubEnumAll(list);

    if (!NT_SUCCESS(st)) {
        LOG_ERROR("RunFullEnum: UsbHubEnumAll -> %08X", st);
        ExFreePoolWithTag(list, 'bHsU');
        return;
    }

    LOG_INFO("=== full USB enumeration: %lu hub(s), %lu device(s) ===",
             list->HubCount, list->Count);

    for (ULONG i = 0; i < list->Count; i++) {
        USB_DEV_FULL *d = &list->Devs[i];

        LOG_INFO("USB[%lu] hub=%lu port=%lu VID=%04X PID=%04X rev=%04X "
                 "class=%02X/%02X/%02X speed=%u cfg#%u ifaces=%u%s",
                 i, d->HubIndex, d->PortNumber, d->Vid, d->Pid, d->BcdDevice,
                 d->DeviceClass, d->DeviceSubClass, d->DeviceProtocol,
                 d->Speed, d->ConfigValue, d->IfaceCount,
                 UsbHubIsSuspiciousComposite(d) ? "  <<< SUSPICIOUS COMPOSITE" : "");

        if (d->Manufacturer[0] || d->Product[0] || d->Serial[0]) {
            LOG_INFO("  mfg=%ws  product=%ws  serial=%ws",
                     d->Manufacturer[0] ? d->Manufacturer : L"-",
                     d->Product[0]      ? d->Product      : L"-",
                     d->Serial[0]       ? d->Serial       : L"-");
        }

        /* 扩展指纹维度：设备限定符 + BOS */
        if (d->HasQualifier) {
            LOG_INFO("  qualifier: bcdUSB=%04X class=%02X/%02X/%02X maxpkt0=%u cfgs=%u",
                     d->QualBcdUSB, d->QualClass, d->QualSubClass,
                     d->QualProtocol, d->QualMaxPacket0, d->QualNumConfigs);
        }
        if (d->HasBos) {
            LOG_INFO("  bos: total=%u caps=%u", d->BosTotalLen, d->BosCapCount);
        }

        for (ULONG k = 0; k < d->IfaceCount && k < 8; k++) {
            LOG_INFO("  iface %u: class=%02X/%02X/%02X eps=%u reportDesc=%u bytes",
                     d->Ifaces[k].InterfaceNumber,
                     d->Ifaces[k].Class, d->Ifaces[k].SubClass, d->Ifaces[k].Protocol,
                     d->Ifaces[k].EndpointCount,
                     d->Ifaces[k].ReportDescLen);

            if (d->Ifaces[k].HidDescLen != 0) {
                LOG_INFO("    HID: bcdHID=%04X country=%u numDesc=%u physDesc=%u",
                         d->Ifaces[k].HidBcdHID, d->Ifaces[k].HidCountry,
                         d->Ifaces[k].HidNumDesc,
                         d->Ifaces[k].HasPhysDesc ? d->Ifaces[k].PhysDescLen : 0);
            }

            if (d->Ifaces[k].HasReportDesc && d->Ifaces[k].ReportDescLen > 0) {
                DumpHex("REPORT", d->Ifaces[k].ReportDesc,
                        d->Ifaces[k].ReportDescLen);
            }
        }
    }

    ExFreePoolWithTag(list, 'bHsU');

    /*
     * ★ 验证：hub 是否允许任意 SetupPacket
     *   如果 GET_STATUS / GET_CONFIGURATION 都能通，说明
     *   IOCTL_USB_GET_DESCRIPTOR_FROM_NODE_CONNECTION 实际上
     *   是个通用控制传输通道，fuzz/diff 就能直接在驱动里做。
     */
    UsbHubRawControlSelfTest();

    /*
     * ★ 第二条路：走 GUID_DEVINTERFACE_USB_DEVICE + URB。
     *   如果通了，URB_FUNCTION_CONTROL_TRANSFER 就能发任意 SetupPacket。
     */
    UsbHubUrbProbeSelfTest();

    /* ★ 第三段：任意控制传输（fuzz/diff 的基础） */
    UsbUrbControlSelfTest();
}

NTSTATUS UsbAuditInit(VOID)
{
    KeInitializeSpinLock(&g_Lock);
    RtlZeroMemory(g_Devs, sizeof(g_Devs));
    RtlZeroMemory(g_HubSlots, sizeof(g_HubSlots));
    g_HubSlotCount = 0;
    g_HubSlotUsed  = FALSE;

    /* ACE 式 HID 节点表（10 槽，KMBox 判定用） */
    UsbNodeInit();

    DriverScanInit();

    LOG_INFO("=== USB audit: discovering USB drivers ===");

    /*
     * 先解析 hidusb —— 枚举时要靠它识别「HID 数据通路」，
     * 只给这些设备建档（10 槽表装不下所有 USB 设备）。
     */
    ResolveHidUsb();

    /*
     * ★ 全量枚举：走 Hub 端口，采集所有 USB 设备的完整描述符
     *   （设备描述符 + 配置描述符 + 字符串 + HID 报告描述符）
     *   放在 hook 之前 —— 以后装 hook 也不影响这次快照。
     */
    RunFullEnum();

    EnumAllUsbHubs();

    /*
     * 第二根锚：hidusb 的 IRP_MJ_INTERNAL_DEVICE_CONTROL。
     * 这根锚才看得到真正的 HID 鼠标/键盘报告流（ACE 也是这么做的）。
     * 失败不致命 —— KMBox 判定只需要节点表的祖父设备关系，
     * hidusb hook 只影响"输入样本采集"这部分能力。
     */
    {
        NTSTATUS hst = HookHidUsb();
        if (!NT_SUCCESS(hst))
            LOG_WARN("HookHidUsb failed: %08X (sample capture disabled)", hst);
    }

    if (!g_HubSlotUsed) {
        LOG_ERROR("=== USB audit init FAILED: no hub hooked ===");
        return STATUS_OBJECT_NAME_NOT_FOUND;
    }

    /*
     * 首次分类：把已经在线的 HID 设备打个 鼠标/键盘 标记。
     * 只在 PASSIVE_LEVEL 才有意义（发同步 IRP）。
     */
    if (KeGetCurrentIrql() == PASSIVE_LEVEL) {
        NTSTATUS cst = UsbNodeClassifyAll();
        LOG_INFO("initial ClassifyAll -> %08X", cst);
    }

    LOG_INFO("=== USB audit ready: hub hooked, %lu device(s) scanned, %lu tracked, "
             "%lu node(s) ===",
             g_ScanCount, g_Tracked, UsbNodeCount());
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

    /* 先卸 hidusb hook（它在数据通路上，必须最早恢复） */
    UnhookHidUsb();

    for (ULONG i = 0; i < g_HubSlotCount; i++) {
        if (g_HubSlots[i].Hook.Active) {
            HookRemove(&g_HubSlots[i].Hook);
            LOG_INFO("unhooked slot %lu", i);
        }
        if (g_HubSlots[i].Driver != NULL) {
            ObDereferenceObject(g_HubSlots[i].Driver);
            g_HubSlots[i].Driver = NULL;
        }
    }
    g_HubSlotCount = 0;
    g_HubSlotUsed  = FALSE;

    /* 清空节点表 */
    UsbNodeUnload();

    /* 释放 DriverScan 持有的引用和名字 */
    DriverScanRelease();

    LOG_INFO("USB audit unloaded");
}

/* ================= 模块注册 ================= */

static const MODULE_DESC g_UsbModule = {
    "usb",
    IOCTL_USB_CONTROL_TRANSFER,
    9,                  /* 0x800..0x808: CTRL/ENUM/BLOCK/DEBUG/KMBOX/NODES/FULLENUM/REPORT/RAWCTRL */
    UsbAuditDispatch
};

NTSTATUS UsbAuditRegister(VOID)
{
    return ModuleRegister(&g_UsbModule);
}
