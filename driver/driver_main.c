/*
 * driver_main.c - game_base.sys 驱动入口 (反作弊驱动框架装配)
 *
 * ==================================================================
 * game_base.sys 是模块化的反作弊驱动, 每个检测能力是一个 feature
 * ==================================================================
 *
 *   框架层 (framework/):
 *     fw.h            框架接口定义
 *     fw_hook.c       通用 hook (驱动对象 dispatch 替换 + 运行期解析)
 *     fw_device.c     控制设备 + IOCTL 路由
 *     fw_resolve.h    运行期函数解析接口
 *
 *   日志层 (log/):
 *     game_log.h      日志接口 + 宏 (Debug/Release 双版本)
 *     game_log.c      实现 (DbgPrintEx + 环形缓冲)
 *
 *   共享层 (include/):
 *     game_base.h     设备名常量 + 各 feature 的 IOCTL 与结构
 *     fw_user.h       日志结构 / 框架 IOCTL (UM + KM 共用)
 *
 *   功能层 (features/):
 *     feat_usb.c      USB 设备审计 (hook usbhub3 PnP, 枚举/探针/封锁)
 *     feat_xxx.c      后续扩展 (DMA / IOMMU / 模拟器检测 / 进程回调 ...)
 *
 *   客户端 (client/):
 *     usb_client.c    3 环调用工具
 *
 * ==================================================================
 * 加新模块的步骤
 * ==================================================================
 *   1. 写 features/feat_xxx.c, 实现一个 FW_FEATURE 结构
 *   2. 在 features/feat.h 加 3 个函数声明: Register / Init / Unload
 *   3. 在下面 DriverEntry 里加 Register + Init, DriverUnload 里加 Unload
 *
 *   框架层 (framework/) 不用改。
 */
#include "fw.h"
#include "feat.h"
#include "game_base.h"      /* GAME_BASE_DEVICE_NAME / GAME_BASE_SYMLINK_NAME */

/* 日志 tag: 用驱动名, 3 环里会显示 [game_base] */
#define FW_DRV_NAME "game_base"

/*
 * DriverUnload - 驱动卸载回调
 *
 * 调用时机: sc stop game_base (或系统关机)
 *
 * 必须按"逆序"清理, 且顺序很关键:
 *   1. 先卸 feature —— 它会恢复 hook, 必须在内核还在时完成
 *   2. 再删控制设备 —— 3 环此时不能再发 IOCTL 进来
 *
 * 顺序反了会: hook 还没恢复设备就没了 -> 清理代码访问已释放资源 -> 蓝屏
 */
static VOID DriverUnload(PDRIVER_OBJECT DriverObject)
{
    UNREFERENCED_PARAMETER(DriverObject);

    LOG_INFO_TAG(FW_DRV_NAME, "unloading...");

    /* 1. 卸载所有 feature (内部会恢复 hook、等 hook 回调退出) */
    FeatUsbUnload();

    /* 2. 删除控制设备 + 符号链接 */
    FwDeviceUnload();

    LOG_INFO_TAG(FW_DRV_NAME, "unloaded");
}

/*
 * DriverEntry - 驱动入口, 系统加载 .sys 时调用
 *
 * 执行顺序有严格依赖, 不能调换:
 *   1. 日志初始化   —— 后面所有步骤都要打日志, 必须最先
 *   2. 创建控制设备 —— 3 环要能打开才能看状态
 *   3. 注册 feature —— 只是登记到路由表, 不做事
 *   4. 初始化 feature —— 真正干活 (解析目标驱动、装 hook、扫设备)
 *
 * 返回值:
 *   STATUS_SUCCESS   加载成功
 *   其他 NTSTATUS    加载失败, 系统会卸载驱动, 3 环看到 sc start 报错
 *
 * 注意: 即使 feature 初始化失败也返回 STATUS_SUCCESS, 让驱动保持加载状态,
 *       这样 3 环仍能打开设备、读日志排查问题。失败原因会记在日志里,
 *       3 环用 `game_base_client log` 就能看到。
 */
NTSTATUS DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath)
{
    NTSTATUS st;
    UNREFERENCED_PARAMETER(RegistryPath);

    /* ---- 1. 日志必须最先初始化, 否则后面的日志会丢失 ---- */
    LogInit();
    LOG_INFO_TAG(FW_DRV_NAME, "=== game_base anti-cheat driver starting ===");

    /* 注册卸载回调, 否则驱动无法 sc stop */
    DriverObject->DriverUnload = DriverUnload;

    /* ---- 2. 创建控制设备 ----
     * 设备名和符号链接名来自 include/game_base.h 的 GAME_BASE_*_NAME 常量,
     * 不在这里硬编码字符串。3 环用 GAME_BASE_WIN32_PATH 打开。 */
    st = FwDeviceInit(DriverObject, GAME_BASE_DEVICE_NAME, GAME_BASE_SYMLINK_NAME);
    if (!NT_SUCCESS(st)) {
        LOG_ERROR_TAG(FW_DRV_NAME, "FwDeviceInit failed: %08X", st);
        return st;
    }

    /* ---- 3. 注册 feature (登记到路由表, 不做事) ---- */
    st = FeatUsbRegister();
    if (!NT_SUCCESS(st))
        LOG_ERROR_TAG(FW_DRV_NAME, "FeatUsbRegister failed: %08X", st);

    /* ---- 4. 初始化 feature (真正干活) ----
     * 失败不返回错误, 保持驱动加载状态, 让 3 环能读日志定位问题 */
    st = FeatUsbInit();
    if (!NT_SUCCESS(st))
        LOG_ERROR_TAG(FW_DRV_NAME, "FeatUsbInit failed: %08X", st);

    LOG_INFO_TAG(FW_DRV_NAME, "=== framework ready (1 feature active) ===");
    return STATUS_SUCCESS;
}
