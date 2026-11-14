/*
 * fw.h - 驱动框架核心 (日志 / Hook / 特性注册)
 *
 * 设计目标: USB 只是第一个 feature, 后续可加 DMA / IOMMU / 模拟器检测等模块,
 *           每个模块只需实现一个 FW_FEATURE 并注册即可。
 */
#pragma once
#include <ntddk.h>
#include "fw_user.h"
#include "game_log.h"

/* ==================== 日志 (委托给 log/game_log.h) ==================== */
/* 统一用 GL_* 宏, 这里保留 FW_* 作为别名以兼容现有模块 */
#define FW_INFO(tag, fmt, ...)  GL_INFO(tag, fmt, ##__VA_ARGS__)
#define FW_WARN(tag, fmt, ...)  GL_WARN(tag, fmt, ##__VA_ARGS__)
#define FW_ERR(tag, fmt, ...)   GL_ERR (tag, fmt, ##__VA_ARGS__)
#define FW_DBG(tag, fmt, ...)   GL_DBG (tag, fmt, ##__VA_ARGS__)

/* ==================== Hook 模块 ==================== */
typedef struct _FW_HOOK {
    PDRIVER_OBJECT   Drv;           /* 目标驱动对象 */
    ULONG            Index;         /* MajorFunction 索引 */
    PDRIVER_DISPATCH Original;      /* 原始派发函数 */
    PDRIVER_DISPATCH Handler;       /* 我们的处理函数 */
    BOOLEAN          Active;
} FW_HOOK;

/* 按名称解析驱动对象 (内部尝试多种路径) */
NTSTATUS FwHookResolveDriver(_In_ const WCHAR *drvName, _Out_ PDRIVER_OBJECT *out);

/* 安装 / 移除 driver object dispatch hook */
NTSTATUS FwHookInstall(_Inout_ FW_HOOK *hook, _In_ PDRIVER_OBJECT drv,
                       _In_ ULONG index, _In_ PDRIVER_DISPATCH handler);
VOID     FwHookRemove(_Inout_ FW_HOOK *hook);

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
