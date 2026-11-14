/*
 * driver_scan.h - 动态发现所有 USB 驱动
 *
 * 设计目标: 不硬编码驱动名, 从 \Driver 目录遍历 + DeviceType 过滤。
 */
#ifndef GAME_BASE_DRIVER_SCAN_H
#define GAME_BASE_DRIVER_SCAN_H

#include <ntddk.h>

/* 一个被发现的 USB 驱动 */
typedef struct _SCAN_USB_DRIVER {
    PDRIVER_OBJECT  Driver;             /* 驱动对象 (持有引用) */
    UNICODE_STRING  Name;               /* 驱动名 (池分配) */
    ULONG           DeviceCount;        /* 设备数 */
    BOOLEAN         Hooked;             /* 是否已装 hook */
} SCAN_USB_DRIVER;

/*
 * DriverScanInit - 初始化
 */
VOID DriverScanInit(VOID);

/*
 * DriverScanFindAll - 遍历 \Driver 目录, 找出所有 USB 驱动
 *
 * 参数:
 *   hookContext  未使用, 保留给未来
 *
 * 返回: 找到的 USB 驱动数量
 */
ULONG DriverScanFindAll(PVOID hookContext);

/*
 * DriverScanGet - 取第 index 个驱动
 */
SCAN_USB_DRIVER *DriverScanGet(ULONG index);

/*
 * DriverScanCount - 已发现的驱动数
 */
ULONG DriverScanCount(VOID);

/*
 * DriverScanRelease - 释放所有驱动引用（卸载时调用）
 */
VOID DriverScanRelease(VOID);

#endif /* GAME_BASE_DRIVER_SCAN_H */
