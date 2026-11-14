/*
 * driver_scan.c - 动态发现所有 USB 驱动
 *
 * ==================================================================
 * 为什么不用硬编码
 * ==================================================================
 *   最初我写了 4 个固定锚点 (USBHUB3 / usbhub / usbccgp / HidUsb),
 *   但不同机器的 USB 驱动集合不一样:
 *     - 有的机器没有 USB 3.0 控制器 → 没有 USBHUB3
 *     - 有的机器装了厂商驱动 → 有额外的 USB 驱动
 *     - 蓝牙 USB、摄像头、打印机都有各自的类驱动
 *
 *   所以正确做法是: 遍历 \Driver 目录, 用 DeviceType 过滤。
 *
 * ==================================================================
 * 原理
 * ==================================================================
 *   1. \Driver 是对象管理器里的一个 Directory 对象
 *      - 用 ZwOpenDirectoryObject 打开
 *      - 用 ZwQueryDirectoryObject 枚举里面的每一项
 *
 *   2. 每一项有 Name 和 TypeName
 *      - TypeName == L"Driver" 的才是驱动对象
 *
 *   3. 对每个驱动对象, 看它的 DeviceObject 链
 *      - DEVICE_OBJECT->DeviceType == FILE_DEVICE_USB (0x22) 就是 USB 驱动
 *      - 实测: usbhub3 的 FDO DeviceType = 0x22
 *
 *   4. 找到 USB 驱动后, 装 IRP_MJ_PNP hook
 *
 * ==================================================================
 * 验证数据 (WinDbg 实测)
 * ==================================================================
 *   \Driver 目录: 128 个驱动对象
 *   其中 USB 相关:
 *     usbuhci    USB 1.1 UHCI 控制器
 *     usbehci    USB 2.0 EHCI 控制器
 *     USBXHCI    USB 3.0 xHCI 控制器
 *     Ucx01000   USB 控制器扩展
 *     usbhub     USB 1.1/2.0 Hub
 *     USBHUB3    USB 3.0 Hub
 *     usbccgp    USB 复合设备父驱动
 *     HidUsb     HID USB 类驱动
 *     BTHUSB     蓝牙 USB
 *     BthEnum    蓝牙枚举
 *
 *   usbhub3 FDO: DeviceType = 0x22 (FILE_DEVICE_USB) ✓
 */
#include "module.h"
#include "game_base.h"
#include "driver_scan.h"
#include <ntstrsafe.h>

#define SCAN_MOD "scan"
/* ================= WDK 未声明的对象目录 API ================= */

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

#ifndef ARRAY_SIZE
#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))
#endif

/* DeviceType: 0x22 = FILE_DEVICE_USB */
#ifndef FILE_DEVICE_USB
#define FILE_DEVICE_USB 0x22
#endif

/* 目录枚举的缓冲区大小 */
#define SCAN_DIR_BUFFER  8192

/* 最多跟踪的 USB 驱动数 */
#define SCAN_MAX_DRIVERS 32

/* ================= 状态 ================= */

/* SCAN_USB_DRIVER 结构在 driver_scan.h 里定义, 这里直接用 */

static SCAN_USB_DRIVER  g_UsbDrivers[SCAN_MAX_DRIVERS];
static ULONG            g_UsbDriverCount = 0;
static KSPIN_LOCK       g_ScanLock;

/* ================= 内部 ================= */

/*
 * IsUsbDriver - 判断一个驱动对象是否管理 USB 设备
 *
 * 遍历它的设备链, 只要有任一设备的 DeviceType == FILE_DEVICE_USB 就算。
 */
static BOOLEAN IsUsbDriver(PDRIVER_OBJECT drv)
{
    if (drv == NULL) return FALSE;

    for (PDEVICE_OBJECT dev = drv->DeviceObject; dev != NULL; dev = dev->NextDevice) {
        if (dev->DeviceType == FILE_DEVICE_USB) {
            return TRUE;
        }
    }
    return FALSE;
}

/*
 * CountDevices - 统计驱动的设备数
 */
static ULONG CountDevices(PDRIVER_OBJECT drv)
{
    ULONG n = 0;
    for (PDEVICE_OBJECT dev = drv->DeviceObject; dev != NULL; dev = dev->NextDevice) n++;
    return n;
}

/*
 * ReleaseAll - 释放所有已记录的驱动引用
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
            g_UsbDrivers[i].Name.Buffer = NULL;
        }
    }
    g_UsbDriverCount = 0;
}

/* ================= 对外接口 ================= */

/*
 * DriverScanInit - 初始化扫描模块
 */
VOID DriverScanInit(VOID)
{
    KeInitializeSpinLock(&g_ScanLock);
    RtlZeroMemory(g_UsbDrivers, sizeof(g_UsbDrivers));
    g_UsbDriverCount = 0;
}

/*
 * DriverScanFindAll - 遍历 \Driver 目录, 找出所有 USB 驱动
 *
 * 返回: 找到的 USB 驱动数量
 *
 * 副作用: 每个找到的驱动都持有 ObReference, 调用 DriverScanRelease 释放
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

        st = ZwQueryDirectoryObject(hDir, buf, SCAN_DIR_BUFFER,
                                   FALSE,        /* ReturnSingleEntry */
                                   first,        /* RestartScan */
                                   &ctx, &len);
        first = FALSE;

        if (!NT_SUCCESS(st) || st == STATUS_NO_MORE_ENTRIES) break;

        info = (POBJECT_DIRECTORY_INFORMATION)buf;

        /* 只要 Driver 类型 */
        if (info->TypeName.Buffer == NULL) continue;
        if (_wcsicmp(info->TypeName.Buffer, L"Driver") != 0) continue;

        /* 拼完整路径 \Driver\XXX */
        RtlStringCchPrintfW(fullName, 256, L"\\Driver\\%wZ", &info->Name);

        /* 解析驱动对象 */
        if (!NT_SUCCESS(DriverLookupByName(fullName, &drv)) || drv == NULL) {
            continue;
        }

        /* 判断是否 USB 驱动 */
        if (IsUsbDriver(drv)) {
            ULONG devCount = CountDevices(drv);

            LOG_INFO("  USB driver: %wZ (DrvObj=%p devices=%lu)",
                     &info->Name, drv, devCount);

            if (g_UsbDriverCount < SCAN_MAX_DRIVERS) {
                SCAN_USB_DRIVER *slot = &g_UsbDrivers[g_UsbDriverCount];

                RtlZeroMemory(slot, sizeof(*slot));
                slot->Driver      = drv;          /* 保留引用 */
                slot->DeviceCount = devCount;
                slot->Name.MaximumLength = info->Name.Length + sizeof(WCHAR);
                slot->Name.Buffer = (PWCHAR)ExAllocatePoolWithTag(
                    NonPagedPool, slot->Name.MaximumLength, 'nSnD');
                if (slot->Name.Buffer != NULL) {
                    RtlCopyUnicodeString(&slot->Name, &info->Name);
                }

                g_UsbDriverCount++;
                found++;
            } else {
                LOG_WARN("  USB driver table full, skip %wZ", &info->Name);
                ObDereferenceObject(drv);
            }
        } else {
            /* 不是 USB 驱动, 释放引用 */
            ObDereferenceObject(drv);
        }
    }

    ExFreePoolWithTag(buf, 'cSnD');
    ZwClose(hDir);

    LOG_INFO("=== scan done: %lu USB driver(s) found ===", found);
    return found;
}

/*
 * DriverScanGet - 取第 i 个找到的 USB 驱动
 */
SCAN_USB_DRIVER *DriverScanGet(ULONG index)
{
    if (index >= g_UsbDriverCount) return NULL;
    return &g_UsbDrivers[index];
}

ULONG DriverScanCount(VOID)
{
    return g_UsbDriverCount;
}

/*
 * DriverScanRelease - 释放所有引用
 */
VOID DriverScanRelease(VOID)
{
    ReleaseAll();
}
