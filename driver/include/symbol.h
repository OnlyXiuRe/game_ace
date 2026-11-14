/*
 * fw_resolve.h - 运行时函数解析 (参考 Kernel-Bridge 的 API 分层 + ACE 的运行期解析)
 *
 * 目的:
 *   1. 不依赖静态导入, 规避特征
 *   2. 解决 ObReferenceObjectByName 静态链接拿到错误地址的问题
 */
#pragma once
#include <ntddk.h>

/* 获取 ntoskrnl 基址 */
PVOID SymbolKernelBase(VOID);

/* 按名称从 ntoskrnl 导出表解析函数地址, 失败返回 NULL */
PVOID SymbolResolve(_In_ const char *funcName);

/* 带缓存与日志的解析 (同一名字只查一次) */
PVOID SymbolResolveCached(_In_ const char *funcName);

/* 清空解析缓存 (卸载时调用) */
VOID SymbolReset(VOID);
