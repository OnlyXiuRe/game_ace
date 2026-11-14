/*
 * driver_scan.c - 动态发现所有 USB 驱动
 *
 * ==================================================================
 * 判定依据: 总线类型 GUID, 不是驱动名
 * ==================================================================
 * 一开始我用 DEVICE_OBJECT->DeviceType == 0x22 判断 USB, 结果几乎所有
 * 驱动都被误判 (PEAUTH / DXGKrnl / SoftwareDevice / 甚至我们自己的
 * game_base)。原因有据可查 —— usbiodef.h 里就写着:
 *
 *     #define FILE_DEVICE_USB  FILE_DEVICE_UNKNOWN
 *
 * 也就是说 DeviceType 对 USB 在结构上就没有区分度。
 *
 * 正确做法是问总线枚举器: 这个设备挂在哪种总线上。Windows 为此提供
 * 了 DevicePropertyBusTypeGuid (wdm.h: = 0xc), USB 设备返回
 * GUID_BUS_TYPE_USB (wdmguid.h):
 *
 *     DEFINE_GUID(GUID_BUS_TYPE_USB, 0x9d7debbc, 0xc85d, 0x11d1,
 *                 0x9e, 0xb4, 0x00, 0x60, 0x08, 0xc3, 0xa1, 0x9a);
 *
 * 这是枚举器 (PCI / USB / ACPI / ROOT / SWD) 在创建设备时写入的类型
 * 标记, 和驱动文件名完全无关。
 *
 * ==================================================================
 * 为什么还要排除名单
 * ==================================================================
 * 总线判定会命中真实挂在 USB 上的所有设备, 包括蓝牙适配器:
 *
 *     BTHUSB   -> 设备 PDO 是 USB     (命中)
 *     BthEnum  -> 挂在 BTHUSB 下      (PDO 不是 USB, 不命中)
 *     RFCOMM   -> 挂在 BTHUSB 下      (不命中)
 *
 * 但 BTHUSB 这类是「功能驱动」不是 USB 总线管理者, 接管它们的
 * IRP_MJ_PNP 会破坏其状态机 (实测触发 0xCA PNP_DETECTED_FATAL_ERROR)。
 * 所以再加一层排除名单, 把消费者驱动剔掉。
 *
 * ==================================================================
 * 目录枚举的正确姿势
 * ==================================================================
 * ZwQueryDirectoryObject 必须用 ReturnSingleEntry = TRUE。
 * 传 FALSE 时内核把所有条目打包进缓冲区, 只能解析第一个结构,
 * 后面的要自己按 UNICODE_STRING 长度推算, 极易出错 (实测只扫到 1 个)。
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

/* 最多跟踪的 USB 驱动数 */
#define SCAN_MAX_DRIVERS 64

/* ================= WDK 未声明的内核 API ================= */

/* 对象目录条目 (ZwQueryDirectoryObject 的输出结构) */
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

/* ================= 常量 ================= */

/*
 * GUID_BUS_TYPE_USB - USB 总线类型标识 (来源 wdmguid.h)
 */
static const GUID g_BusTypeUsb =
    { 0x9d7debbc, 0xc85d, 0x11d1, { 0x9e, 0xb4, 0x00, 0x60, 0x08, 0xc3, 0xa1, 0x9a } };

/*
 * 排除名单 - 总线判定命中, 但不该 hook 的功能性驱动
 */
static const WCHAR *g_ExcludeDrivers[] = {
    L"bth",        /* 蓝牙栈: BTHUSB / BthEnum / BthPan / BthMini */
    L"rfcomm",     /* 蓝牙串口 */
    L"mouclass",   /* 鼠标类驱动 */
    L"kbdclass",   /* 键盘类驱动 */
    L"mouhid",     /* 鼠标 HID 功能驱动 */
    L"game_base",  /* 我们自己 */
};

/* ================= 状态 ================= */

static SCAN_USB_DRIVER  g_UsbDrivers[SCAN_MAX_DRIVERS];
static ULONG            g_UsbDriverCount = 0;

/* ================= 工具 ================= */

/*
 * ContainsNoCase - 不区分大小写的子串查找
 */
static BOOLEAN ContainsNoCase(_In_ PCWSTR haystack, _In_ PCWSTR needle)
{
    size_t hlen, nlen, i, j;

    if (haystack == NULL || needle == NULL) return FALSE;

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
 * WalkToPdo - 沿设备栈走到底层 PDO
 *
 * 返回: PDO (持有一份引用, 调用方负责 ObDereferenceObject);
 *       如果设备本身就是栈底, 返回 NULL (调用方直接用它)。
 */
static PDEVICE_OBJECT WalkToPdo(_In_ PDEVICE_OBJECT dev)
{
    PDEVICE_OBJECT cur = dev;
    PDEVICE_OBJECT lower;

    if (dev == NULL) return NULL;

    while ((lower = IoGetLowerDeviceObject(cur)) != NULL) {
        if (cur != dev) ObDereferenceObject(cur);
        cur = lower;
    }

    return (cur == dev) ? NULL : cur;
}

/*
 * IsUsbBusType - 用总线类型 GUID 判断一个设备对象是不是 USB 设备
 *
 * 这是「设备类型」的正确判定方式, 和驱动名完全无关。
 */
static BOOLEAN IsUsbBusType(_In_ PDEVICE_OBJECT pdo)
{
    GUID     busType;
    ULONG    len = 0;
    NTSTATUS st;

    if (pdo == NULL) return FALSE;

    RtlZeroMemory(&busType, sizeof(busType));
    st = IoGetDeviceProperty(pdo, DevicePropertyBusTypeGuid,
                             sizeof(busType), &busType, &len);
    if (!NT_SUCCESS(st)) return FALSE;

    return (RtlCompareMemory(&busType, &g_BusTypeUsb, sizeof(GUID)) == sizeof(GUID));
}

/*
 * ProbeBusType - 读设备的 BusTypeGuid 和 EnumeratorName, 仅用于日志
 *
 * 打印出来的数据可以验证「总线判定」是否比「名字匹配」更准确。
 */
static VOID ProbeBusType(_In_ PDEVICE_OBJECT dev, _In_ PCWSTR driverName)
{
    PDEVICE_OBJECT pdo;
    GUID           busType;
    ULONG          len = 0;
    NTSTATUS       st;
    WCHAR          enumName[64];
    NTSTATUS       st2;
    BOOLEAN        isUsb;

    if (dev == NULL) return;

    pdo = WalkToPdo(dev);
    {
        PDEVICE_OBJECT target = (pdo != NULL) ? pdo : dev;

        RtlZeroMemory(&busType, sizeof(busType));
        st = IoGetDeviceProperty(target, DevicePropertyBusTypeGuid,
                                 sizeof(busType), &busType, &len);

        RtlZeroMemory(enumName, sizeof(enumName));
        st2 = IoGetDeviceProperty(target, DevicePropertyEnumeratorName,
                                  sizeof(enumName) - sizeof(WCHAR),
                                  enumName, &len);

        isUsb = IsUsbBusType(target);

        enumName[ARRAY_SIZE(enumName) - 1] = 0;

        LOG_INFO("    probe %wZ: busguid=%08X-%04X-%04X st=%08X enum=%ws st=%08X usb=%d",
                 driverName,
                 busType.Data1, busType.Data2, busType.Data3, st,
                 NT_SUCCESS(st2) ? enumName : L"?",
                 st2,
                 isUsb);

        if (pdo != NULL) ObDereferenceObject(pdo);
    }
}

/*
 * DeviceIsUsb - 判断一个设备（不看驱动名）是不是 USB 设备
 */
static BOOLEAN DeviceIsUsb(_In_ PDEVICE_OBJECT dev)
{
    PDEVICE_OBJECT pdo;
    BOOLEAN        result;

    if (dev == NULL) return FALSE;

    pdo = WalkToPdo(dev);
    if (pdo != NULL) {
        result = IsUsbBusType(pdo);
        ObDereferenceObject(pdo);
    } else {
        result = IsUsbBusType(dev);   /* 设备本身就是栈底 */
    }
    return result;
}

/*
 * IsUsbDriver - 判断驱动是否管理 USB 设备
 *
 * 判据:
 *   1. 驱动名不在排除名单里
 *   2. 该驱动的任一设备, 其底层 PDO 的 BusTypeGuid == GUID_BUS_TYPE_USB
 */
static BOOLEAN IsUsbDriver(_In_ PDRIVER_OBJECT drv)
{
    WCHAR  name[128];
    USHORT chars;

    if (drv == NULL) return FALSE;

    /* ---- 1. 排除功能性消费者驱动 ---- */
    if (drv->DriverName.Buffer != NULL) {
        chars = (USHORT)(drv->DriverName.Length / sizeof(WCHAR));
        if (chars > 0) {
            if (chars >= ARRAY_SIZE(name)) chars = ARRAY_SIZE(name) - 1;
            RtlCopyMemory(name, drv->DriverName.Buffer, (ULONG)chars * sizeof(WCHAR));
            name[chars] = 0;

            for (ULONG i = 0; i < ARRAY_SIZE(g_ExcludeDrivers); i++) {
                if (ContainsNoCase(name, g_ExcludeDrivers[i])) return FALSE;
            }
        }
    }

    /* ---- 2. 总线类型判定 ---- */
    for (PDEVICE_OBJECT dev = drv->DeviceObject; dev != NULL; dev = dev->NextDevice) {
        if (DeviceIsUsb(dev)) return TRUE;
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
            g_UsbDrivers[i].Name.Buffer       = NULL;
            g_UsbDrivers[i].Name.Length       = 0;
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

    LOG_INFO("=== scanning \\Driver directory (bus-type GUID based) ===");

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
            USHORT           nameBytes = info->Name.Length;

            RtlZeroMemory(slot, sizeof(*slot));
            slot->Driver      = drv;          /* 保留引用 */
            slot->DeviceCount = CountDevices(drv);
            slot->Hooked      = FALSE;

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

            /* 打印第一个设备的总线类型, 用于验证判定依据 */
            ProbeBusType(drv->DeviceObject, &slot->Name);
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
