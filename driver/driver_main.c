/*
 * driver_main.c - game_base.sys 驱动入口 (反作弊驱动框架装配)
 *
 * game_base.sys 是反作弊驱动, 模块化设计, 每个检测能力是一个 feature:
 *
 *   框架层:
 *     fw_log.c      环形日志 (3 环可读, 不依赖 DbgView)
 *     fw_hook.c     通用 hook (驱动对象 dispatch 替换 + 运行期解析)
 *     fw_device.c   控制设备 + IOCTL 路由
 *     fw_user.h     3 环共享定义 (日志结构 + IOCTL 码)
 *
 *   功能层 (feature):
 *     feat_usb.c    USB 设备审计 (hook usbhub3 PnP, 枚举/探针/封锁)
 *     feat_xxx.c    后续扩展 (DMA / IOMMU / 模拟器检测 / 进程回调 ...)
 *
 * 加新模块: 写 feat_xxx.c + 在 DriverEntry 里 Register/Init, 卸载时 Unload。
 */
#include "fw.h"
#include "feat.h"

#define FW_DRV_NAME "game_base"

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
    FW_INFO(FW_DRV_NAME, "=== game_base anti-cheat driver starting ===");

    DriverObject->DriverUnload = DriverUnload;

    /* 1. 控制设备 (框架) */
    st = FwDeviceInit(DriverObject, L"\\Device\\game_base", L"\\DosDevices\\game_base");
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

    FW_INFO(FW_DRV_NAME, "=== framework ready (1 feature active) ===");
    return STATUS_SUCCESS;
}
