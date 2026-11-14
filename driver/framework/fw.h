/*
 * fw.h - 驱动框架核心
 *
 * 日志: log/game_log.h
 *   LOG_INFO(fmt, ...)  - 自动带 __FUNCTION__ 作函数名
 *
 * Hook: framework/hook/hook.h
 *   HookInstall / HookRemove / HookFindOriginal
 *
 * 特性: 每个检测能力实现一个 FW_FEATURE 并注册即可
 */
#pragma once
#include <ntddk.h>
#include "fw_user.h"
#include "game_log.h"
#include "hook.h"

/* ==================== 驱动对象解析 ====================
 * 按名字拿到 DRIVER_OBJECT (多策略回退, 定义在 fw_hook.c)
 */
NTSTATUS FwHookResolveDriver(_In_ const WCHAR *drvName, _Out_ PDRIVER_OBJECT *out);

/* ==================== 特性注册 ==================== */
#define FW_FEATURE_MAX 8

typedef NTSTATUS (*FW_FEATURE_DISPATCH)(PVOID buffer, ULONG inLen, ULONG outLen,
                                        ULONG ioctlCode, PULONG info);

typedef struct _FW_FEATURE {
    const CHAR           *Name;
    ULONG                 IoctlBase;   /* 本 feature 的 IOCTL 基址 */
    ULONG                 IoctlCount;  /* 本 feature 占用的 IOCTL 数量 */
    FW_FEATURE_DISPATCH   Dispatch;
} FW_FEATURE;

NTSTATUS          FwFeatureRegister(_In_ const FW_FEATURE *feat);
const FW_FEATURE *FwFeatureFind(_In_ ULONG ioctlCode);

/* ==================== 设备 / 框架生命周期 ==================== */
NTSTATUS FwDeviceInit(_In_ PDRIVER_OBJECT DriverObject, _In_ PCWSTR DeviceName, _In_ PCWSTR SymLink);
VOID     FwDeviceUnload(VOID);
