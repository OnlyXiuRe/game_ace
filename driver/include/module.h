/*
 * fw.h - 驱动框架核心
 *
 * 日志: log/game_log.h
 *   LOG_INFO(fmt, ...)  - 自动带 __FUNCTION__ 作函数名
 *
 * Hook: framework/hook/hook.h
 *   HookInstall / HookRemove / HookFindOriginal
 *
 * 特性: 每个检测能力实现一个 MODULE_DESC 并注册即可
 */
#pragma once
#include <ntddk.h>
#include "log_types.h"
#include "game_log.h"
#include "hook.h"

/* ==================== 驱动对象解析 ====================
 * 按名字拿到 DRIVER_OBJECT (多策略回退, 定义在 fw_hook.c)
 */
NTSTATUS DriverLookupByName(_In_ const WCHAR *drvName, _Out_ PDRIVER_OBJECT *out);

/* 记录真实的 OBJECT_TYPE（在 DriverEntry 里、任何 DriverLookupByName 之前调用）
 * 因为静态链接的 IoDriverObjectType 符号可能和实际内核不匹配 */
VOID     DriverLookupSetDriverObject(_In_ PDRIVER_OBJECT driverObject);

/* ==================== 特性注册 ==================== */
#define MODULE_DESC_MAX 8

typedef NTSTATUS (*MODULE_DESC_DISPATCH)(PVOID buffer, ULONG inLen, ULONG outLen,
                                        ULONG ioctlCode, PULONG info);

typedef struct _MODULE_DESC {
    const CHAR           *Name;
    ULONG                 IoctlBase;   /* 本 feature 的 IOCTL 基址 */
    ULONG                 IoctlCount;  /* 本 feature 占用的 IOCTL 数量 */
    MODULE_DESC_DISPATCH   Dispatch;
} MODULE_DESC;

NTSTATUS          ModuleRegister(_In_ const MODULE_DESC *feat);
const MODULE_DESC *ModuleFind(_In_ ULONG ioctlCode);

/* ==================== 设备 / 框架生命周期 ==================== */
NTSTATUS ControlDeviceInit(_In_ PDRIVER_OBJECT DriverObject, _In_ PCWSTR DeviceName, _In_ PCWSTR SymLink);
VOID     ControlDeviceUnload(VOID);
