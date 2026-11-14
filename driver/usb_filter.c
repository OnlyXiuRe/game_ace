/*
 * usb_filter.c - 驱动入口 (框架装配)
 *
 * 结构:
 *   fw_log.c      环形日志 (3 环可读)
 *   fw_hook.c     通用 hook (驱动对象 dispatch 替换 + 多策略对象解析)
 *   fw_device.c   控制设备 + IOCTL 路由
 *   feat_usb.c    USB 特性 (第一个模块)
 *
 * 加新模块: 写 feat_xxx.c + 在 DriverEntry 里 Register/Init, 卸载时 Unload。
 */
#include "fw.h"
#include "feat.h"

#define FW_DRV_NAME "kernel"

static VOID DriverUnload(PDRIVER_OBJECT DriverObject)
{
    UNREFERENCED_PARAMETER(DriverObject);

    FW_INFO(FW_DRV_NAME, "unloading...");
    FeatUsbUnload();
    FwDeviceUnload();
    FW_INFO(FW_DRV_NAME, "unloaded");
}

NTSTATUS DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath)
{
    NTSTATUS st;
    UNREFERENCED_PARAMETER(RegistryPath);

    FwLogInit();
    FW_INFO(FW_DRV_NAME, "=== usb_filter framework starting ===");

    DriverObject->DriverUnload = DriverUnload;

    /* 1. 控制设备 (框架) */
    st = FwDeviceInit(DriverObject, L"\\Device\\usb_filter", L"\\DosDevices\\usb_filter");
    if (!NT_SUCCESS(st)) {
        FW_ERR(FW_DRV_NAME, "FwDeviceInit failed: %08X", st);
        return st;
    }

    /* 2. 注册特性 */
    st = FeatUsbRegister();
    if (!NT_SUCCESS(st))
        FW_ERR(FW_DRV_NAME, "FeatUsbRegister failed: %08X", st);

    /* 3. 初始化特性 */
    st = FeatUsbInit();
    if (!NT_SUCCESS(st))
        FW_ERR(FW_DRV_NAME, "FeatUsbInit failed: %08X", st);

    FW_INFO(FW_DRV_NAME, "=== framework ready ===");
    return STATUS_SUCCESS;
}
