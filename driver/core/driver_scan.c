/*
 * driver_scan.c - 动态发现所有 USB 驱动
 *
 * ==================================================================
 * 为什么不用 DeviceType 过滤 (踩过的坑)
 * ==================================================================
 *   最初我用 dev->DeviceType == 0x22 判断 USB 设备, 结果几乎所有驱动
 *   都被误判为 USB 驱动 (PEAUTH / DXGKrnl / SoftwareDevice / 甚至我们
 *   自己的 game_base)。
 *
 *   原因: WDK 里根本没有 FILE_DEVICE_USB 这个常量!
 *         FILE_DEVICE_UNKNOWN = 0x00000022
 *         USBHUB3 的 FDO DeviceType 也是 0x22 (即 UNKNOWN)
 *
 *   所以 DeviceType 完全是 "未分类" 的默认值, 无法区分 USB。
 *
 * ==================================================================
 * 实际可用的过滤条件
 * ==================================================================
 *   条件 A: 驱动名包含 "usb" (不区分大小写)
 *     usbuhci / usbehci / USBXHCI / usbhub / USBHUB3 / usbccgp
 *     HidUsb / BTHUSB / vmusbmouse / ucxcx0000...
 *
 *   条件 B: 设备的底层栈里有名字含 "usb" 的驱动
 *     覆盖 Ucx01000 (USB 控制器扩展, 名字里没有 usb,
 *     但它的下层设备挂在 USBXHCI 上)
 *
 *   两个条件任一满足即认为是 USB 驱动。
 *
 * ==================================================================
 * 目录枚举的正确姿势
 * ==================================================================
 *   ZwQueryDirectoryObject 必须用 ReturnSingleEntry = TRUE!
 *   传 FALSE 时内核把所有条目打包进缓冲区, 只能解析第一个结构,
 *   后面的需要自己按 UNICODE_STRING 长度推算, 极易出错。
 */
#include "module.h"
#include "game_base.h"
#include "driver_scan.h"
#include <ntstrsafe.h>

#define SCAN_MOD "scan"

#ifndef ARRAY_SIZE
#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))
#endif

/* 目录枚举的缓冲区大小 */
#define SCAN_DIR_BUFFER  4096

/* ================= WDK 未声明的内核 API ================= */

/* 对象目录条目（ZwQueryDirectoryObject 的输出结构） */
typedef struct _OBJECT_DIRECTORY_INFORMATION {
    UNICODE_STRING Name;
    UNICODE_STRING TypeName;
} OBJECT_DIRECTORY_INFORMATION, *POBJECT_DIRECTORY_INFORMATION;

NTSYSAPI NTSTATUS NTAPI ZwOpenDirectoryObject(
    _Out_ PHANDLE DirectoryHandle,
    _In_  ACCESS_MASK DesiredAccess,
    _In_  POBJECT_ATTRIBUTES ObjectAttributes);

NTSYSAPI NTSTATUS NTAPI ZwQueryDirectoryObject(
    _In_  HANDLE DirectoryHandle,
    _Out_writes_bytes_(BufferLength) PVOID Buffer,
    _In_  ULONG BufferLength,
    _In_  BOOLEAN ReturnSingleEntry,
    _In_  BOOLEAN RestartScan,
    _Inout_ PULONG Context,
    _Out_opt_ PULONG ReturnLength);

/* ntifs.h 有声明, ntddk.h 没有 */
extern PDEVICE_OBJECT IoGetLowerDeviceObject(_In_ PDEVICE_OBJECT DeviceObject);

/* 最多跟踪的 USB 驱动数 */
#define SCAN_MAX_DRIVERS 64

/* ================= 状态 ================= */

static SCAN_USB_DRIVER  g_UsbDrivers[SCAN_MAX_DRIVERS];
static ULONG            g_UsbDriverCount = 0;

/* ================= 工具 ================= */

/*
 * ContainsNoCase - 不区分大小写的子串查找
 *
 * 为什么不用 wcsstr: wcsstr 区分大小写, 而 \Driver\HidUsb 里是混合大小写,
 * 我们需要同时匹配 "usb" / "Usb" / "USB"。
 */
static BOOLEAN ContainsNoCase(_In_ PCWSTR haystack, _In_ PCWSTR needle)
{
    if (haystack == NULL || needle == NULL) return FALSE;

    size_t hlen, nlen, i, j;

    for (hlen = 0; haystack[hlen]; hlen++) {}
    for (nlen = 0; needle[nlen]; nlen++)   {}
    if (nlen == 0 || hlen < nlen) return FALSE;

    for (i = 0; i + nlen <= hlen; i++) {
        BOOLEAN match = TRUE;
        for (j = 0; j < nlen; j++) {
            WCHAR a = haystack[i + j];
            WCHAR b = needle[j];
            if (a >= L'A' && a <= L'Z') a += L'a' - L'A';
            if (b >= L'A' && b <= L'Z') b += L'a' - L'A';
            if (a != b) { match = FALSE; break; }
        }
        if (match) return TRUE;
    }
    return FALSE;
}

/*
 * DriverNameHasUsb - 驱动名是否包含 "usb"
 *
 * 注意: 不能把 DriverName.Buffer 直接当 C 串用。
 *       UNICODE_STRING 的 Buffer 只保证有 Length 个字节有效,
 *       虽然内核里驱动名通常带结尾 0, 但依赖这个假设会越界读。
 *       这里按 Length 拷贝到本地缓冲区后再比较。
 */
static BOOLEAN DriverNameHasUsb(_In_ PDRIVER_OBJECT drv)
{
    WCHAR  name[128];
    USHORT chars;

    if (drv == NULL) return FALSE;
    if (drv->DriverName.Buffer == NULL) return FALSE;

    chars = (USHORT)(drv->DriverName.Length / sizeof(WCHAR));
    if (chars == 0) return FALSE;
    if (chars >= ARRAY_SIZE(name)) chars = ARRAY_SIZE(name) - 1;

    RtlCopyMemory(name, drv->DriverName.Buffer, (ULONG)chars * sizeof(WCHAR));
    name[chars] = 0;

    return ContainsNoCase(name, L"usb");
}

/*
 * StackHasUsbDriver - 设备的底层栈里有没有名字含 usb 的驱动
 *
 * 用于覆盖 Ucx01000 这种名字不含 usb、但底层接着 USB 控制器的驱动。
 */
static BOOLEAN StackHasUsbDriver(_In_ PDEVICE_OBJECT top)
{
    PDEVICE_OBJECT cur  = top;
    PDEVICE_OBJECT next = NULL;
    BOOLEAN        hit  = FALSE;

    while (cur != NULL && !hit) {
        if (DriverNameHasUsb(cur->DriverObject)) hit = TRUE;

        next = IoGetLowerDeviceObject(cur);
        if (cur != top) ObDereferenceObject(cur);
        cur = next;
        if (hit && cur != NULL) {
            /* 命中了, 但 next 有引用需要释放 */
            ObDereferenceObject(cur);
            cur = NULL;
            break;
        }
    }

    /* 收尾: 如果循环因为 cur == NULL 结束, 没有额外引用需要释放 */
    return hit;
}

/*
 * IsUsbDriver - 判断一个驱动对象是否属于 USB 栈
 *
 * 策略: 驱动名含 "usb" 或 任一设备的底层栈含 "usb" 驱动
 */
static BOOLEAN IsUsbDriver(_In_ PDRIVER_OBJECT drv)
{
    if (drv == NULL) return FALSE;

    /* 条件 A: 驱动名自带 usb */
    if (DriverNameHasUsb(drv)) return TRUE;

    /* 条件 B: 设备栈里有 usb 驱动 */
    for (PDEVICE_OBJECT dev = drv->DeviceObject; dev != NULL; dev = dev->NextDevice) {
        if (StackHasUsbDriver(dev)) return TRUE;
    }
    return FALSE;
}

/*
 * CountDevices - 统计驱动的设备数
 */
static ULONG CountDevices(_In_ PDRIVER_OBJECT drv)
{
    ULONG n = 0;
    for (PDEVICE_OBJECT dev = drv->DeviceObject; dev != NULL; dev = dev->NextDevice) n++;
    return n;
}

/*
 * ReleaseAll - 释放所有已记录的驱动引用和名字内存
 */
static VOID ReleaseAll(VOID)
{
    for (ULONG i = 0; i < g_UsbDriverCount; i++) {
        if (g_UsbDrivers[i].Driver != NULL) {
            ObDereferenceObject(g_UsbDrivers[i].Driver);
            g_UsbDrivers[i].Driver = NULL;
        }
        if (g_UsbDrivers[i].Name.Buffer != NULL) {
            ExFreePool(g_UsbDrivers[i].Name.Buffer);
            g_UsbDrivers[i].Name.Buffer    = NULL;
            g_UsbDrivers[i].Name.Length    = 0;
            g_UsbDrivers[i].Name.MaximumLength = 0;
        }
    }
    g_UsbDriverCount = 0;
}

/* ================= 对外接口 ================= */

VOID DriverScanInit(VOID)
{
    RtlZeroMemory(g_UsbDrivers, sizeof(g_UsbDrivers));
    g_UsbDriverCount = 0;
}

/*
 * DriverScanFindAll - 遍历 \Driver 目录, 找出所有 USB 驱动
 *
 * 返回: 找到的 USB 驱动数量
 */
ULONG DriverScanFindAll(PVOID hookContext)
{
    UNICODE_STRING     dirName;
    OBJECT_ATTRIBUTES  oa;
    HANDLE             hDir = NULL;
    NTSTATUS           st;
    UCHAR             *buf = NULL;
    ULONG              ctx = 0;
    ULONG              len = 0;
    BOOLEAN            first = TRUE;
    ULONG              found = 0;
    ULONG              totalEntries = 0;

    UNREFERENCED_PARAMETER(hookContext);

    RtlInitUnicodeString(&dirName, L"\\Driver");
    InitializeObjectAttributes(&oa, &dirName,
                               OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE,
                               NULL, NULL);

    st = ZwOpenDirectoryObject(&hDir, DIRECTORY_QUERY, &oa);
    if (!NT_SUCCESS(st)) {
        LOG_ERROR("DriverScanFindAll: ZwOpenDirectoryObject(\\Driver) -> %08X", st);
        return 0;
    }

    buf = (UCHAR *)ExAllocatePoolWithTag(NonPagedPool, SCAN_DIR_BUFFER, 'cSnD');
    if (buf == NULL) {
        ZwClose(hDir);
        return 0;
    }

    LOG_INFO("=== scanning \\Driver directory ===");

    for (;;) {
        POBJECT_DIRECTORY_INFORMATION info;
        WCHAR fullName[256];
        PDRIVER_OBJECT drv = NULL;

        RtlZeroMemory(buf, SCAN_DIR_BUFFER);

        /* 关键: ReturnSingleEntry = TRUE, 一次取一个条目 */
        st = ZwQueryDirectoryObject(hDir, buf, SCAN_DIR_BUFFER,
                                   TRUE,         /* ReturnSingleEntry */
                                   first,        /* RestartScan */
                                   &ctx, &len);
        first = FALSE;

        if (st == STATUS_NO_MORE_ENTRIES) break;
        if (!NT_SUCCESS(st)) {
            LOG_WARN("ZwQueryDirectoryObject -> %08X at entry %lu", st, totalEntries);
            break;
        }

        info = (POBJECT_DIRECTORY_INFORMATION)buf;
        totalEntries++;

        if (info->TypeName.Buffer == NULL) continue;
        if (_wcsicmp(info->TypeName.Buffer, L"Driver") != 0) continue;

        RtlStringCchPrintfW(fullName, 256, L"\\Driver\\%wZ", &info->Name);

        if (!NT_SUCCESS(DriverLookupByName(fullName, &drv)) || drv == NULL) {
            continue;
        }

        if (!IsUsbDriver(drv)) {
            ObDereferenceObject(drv);
            continue;
        }

        /* 是 USB 驱动 */
        if (g_UsbDriverCount < SCAN_MAX_DRIVERS) {
            SCAN_USB_DRIVER *slot = &g_UsbDrivers[g_UsbDriverCount];
            USHORT nameBytes = info->Name.Length;

            RtlZeroMemory(slot, sizeof(*slot));
            slot->Driver      = drv;          /* 保留引用 */
            slot->DeviceCount = CountDevices(drv);
            slot->Hooked      = FALSE;

            /* 拷贝驱动名 */
            slot->Name.MaximumLength = (USHORT)(nameBytes + sizeof(WCHAR));
            slot->Name.Buffer = (PWCHAR)ExAllocatePoolWithTag(
                NonPagedPool, slot->Name.MaximumLength, 'nSnD');
            if (slot->Name.Buffer != NULL) {
                slot->Name.Length = nameBytes;
                RtlCopyMemory(slot->Name.Buffer, info->Name.Buffer, nameBytes);
                slot->Name.Buffer[nameBytes / sizeof(WCHAR)] = 0;
            }

            g_UsbDriverCount++;
            found++;

            LOG_INFO("  USB driver: %wZ (DrvObj=%p devices=%lu)",
                     &slot->Name, drv, slot->DeviceCount);
        } else {
            LOG_WARN("  table full (%lu), skip %wZ", SCAN_MAX_DRIVERS, &info->Name);
            ObDereferenceObject(drv);
        }
    }

    ExFreePoolWithTag(buf, 'cSnD');
    ZwClose(hDir);

    LOG_INFO("=== scan done: %lu entries scanned, %lu USB driver(s) found ===",
             totalEntries, found);
    return found;
}

SCAN_USB_DRIVER *DriverScanGet(ULONG index)
{
    if (index >= g_UsbDriverCount) return NULL;
    return &g_UsbDrivers[index];
}

ULONG DriverScanCount(VOID)
{
    return g_UsbDriverCount;
}

VOID DriverScanRelease(VOID)
{
    ReleaseAll();
}
