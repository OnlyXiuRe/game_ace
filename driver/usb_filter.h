/* usb_filter.h - 驱动与用户态共享定义 (v3: usbhub3 hook + 设备枚举/探测/封锁)
 * 包含方:
 *   - 驱动: #include <ntddk.h> + <wdf.h> 之后
 *   - 客户端: #include <windows.h> + <initguid.h> 之后
 */
#pragma once

#define USB_MAX_DEVICES   64
#define USB_MAX_PATH      260
#define USB_MAX_DATA      4096

/* ==================================================================
 * IOCTL
 *   ENUM     查设备表 (hook 收集的 START/REMOVE 事件)
 *   CTRL     对指定设备发控制传输 (原始 URB)
 *   BLOCK    封锁/解封指定设备 (伪造 PNP IRP)
 * ================================================================== */
#define IOCTL_USB_ENUM_DEVICES \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x801, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_USB_CONTROL_TRANSFER \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x800, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_USB_DEBUG \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x803, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_USB_BLOCK_DEVICE \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x802, METHOD_BUFFERED, FILE_ANY_ACCESS)

/* 单个设备信息 */
typedef struct _USB_DEV_INFO {
    ULONG   Index;          /* 驱动内索引, CTRL/BLOCK 用它定位设备 */
    BOOLEAN Active;         /* 当前是否在线 */
    UCHAR   Reserved[3];
    USHORT  Vid;
    USHORT  Pid;
    USHORT  BcdDevice;      /* 固件版本 */
    USHORT  BusNumber;
    ULONG   DeviceAddress;
    WCHAR   HardwareId[128];/* 例如 USB\VID_046D&PID_C077&REV_2301 */
} USB_DEV_INFO;

/* 设备列表 (ENUM 回填) */
typedef struct _USB_DEV_LIST {
    ULONG       Count;
    ULONG       Reserved;
    USB_DEV_INFO Devs[USB_MAX_DEVICES];
} USB_DEV_LIST;

/* 控制传输请求
 * 缓冲区布局: [USB_CTRL_REQ][数据]
 *   devIndex 定位设备; 失败返回 -1, bytesReturned=0
 */
typedef struct _USB_CTRL_REQ {
    ULONG  DevIndex;        /* 设备索引 (来自 ENUM) */
    UCHAR  bmRequestType;
    UCHAR  bRequest;
    USHORT wValue;
    USHORT wIndex;
    USHORT wLength;
    ULONG  bytesReturned;   /* 驱动回填 */
} USB_CTRL_REQ;

/* 封锁请求 */
typedef struct _USB_BLOCK_REQ {
    ULONG   DevIndex;
    BOOLEAN Block;          /* TRUE=封锁 FALSE=解封 */
    UCHAR   Reserved[3];
} USB_BLOCK_REQ;

/* 调试信息 */
typedef struct _USB_DEBUG_INFO {
    ULONG HookInstalled;   /* g_Hooked */
    ULONG DeviceCount;     /* 表里 Active 的数量 */
    ULONG LastScanCount;   /* 上次枚举扫描到的接口数 */
    ULONG LastScanStatus;  /* 扫描 API 返回码 */
    ULONG HubDriverFound;  /* ObReferenceObjectByName 是否成功 */
    ULONG HookStatus;      /* ObReferenceObjectByName 返回码 */
    ULONG Tracked;         /* 成功记录的设备数 */
} USB_DEBUG_INFO;
