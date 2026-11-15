/* game_base.h - game_base.sys 与用户态共享定义
 * 反作弊驱动框架: 框架 IOCTL + 各 feature 的 IOCTL/结构
 * 包含方:
 *   - 驱动: #include <ntddk.h> 之后
 *   - 客户端: #include <windows.h> + <initguid.h> 之后
 */
#pragma once

/* ==================================================================
 * 驱动标识常量 —— 只在这里定义一次, 驱动和 3 环都引用这里
 * ================================================================== */

/* 内核设备名 (NT 对象管理器路径) */
#define GAME_BASE_DEVICE_NAME       L"\\Device\\game_base"

/* 符号链接名 (DosDevices 路径) */
#define GAME_BASE_SYMLINK_NAME      L"\\DosDevices\\game_base"

/* 3 环打开设备用的 Win32 路径 */
#define GAME_BASE_WIN32_PATH        L"\\\\.\\game_base"

/* 服务名 (ANSI) */
#define GAME_BASE_SERVICE_NAME      "game_base"

/* 驱动文件名 */
#define GAME_BASE_SYS_FILE          "game_base.sys"

/* ==================================================================
 * 设备与数据长度上限
 * ================================================================== */
#define USB_MAX_DEVICES   64
#define USB_MAX_PATH      260
#define USB_MAX_DATA      4096

/* ==================================================================
 * IOCTL
 *   CTRL     对指定设备发控制传输 (原始 URB)
 *   ENUM     查设备表 (hook 收集的 START/REMOVE 事件)
 *   BLOCK    封锁/解封指定设备 (伪造 PNP IRP)
 *   DEBUG    驱动内部状态
 *   KMBOX    KMBox 判定结果 (鼠标/键盘共享祖父设备)
 *   NODES    HID 节点表快照
 * ================================================================== */
#define IOCTL_USB_CONTROL_TRANSFER \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x800, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_USB_ENUM_DEVICES \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x801, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_USB_BLOCK_DEVICE \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x802, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_USB_DEBUG \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x803, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_USB_KMBOX \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x804, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_USB_NODES \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x805, METHOD_BUFFERED, FILE_ANY_ACCESS)

/* 全量枚举：走 Hub 端口，返回设备描述符+配置+字符串+接口列表 */
#define IOCTL_USB_FULL_ENUM \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x806, METHOD_BUFFERED, FILE_ANY_ACCESS)

/* 取单个设备的 HID 报告描述符 */
#define IOCTL_USB_REPORT_DESC \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x807, METHOD_BUFFERED, FILE_ANY_ACCESS)

/*
 * 任意控制传输（内核版 libusb_control_transfer）
 *
 * 走 URB_FUNCTION_CONTROL_TRANSFER + USBD_DEFAULT_PIPE_TRANSFER，
 * 不需要设备绑定 libusbK，也不需要 FileObject。
 */
#define IOCTL_USB_RAW_CONTROL \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x808, METHOD_BUFFERED, FILE_ANY_ACCESS)

/* ==================================================================
 * USB 设备表 (IOCTL_USB_ENUM_DEVICES)
 * ================================================================== */
typedef struct _USB_DEV_INFO {
    ULONG   Index;          /* 驱动内索引, CTRL/BLOCK 用它定位设备 */
    BOOLEAN Active;         /* 当前是否在线 */
    UCHAR   Reserved[3];
    USHORT  Vid;
    USHORT  Pid;
    USHORT  BcdDevice;      /* 固件版本 */
    USHORT  BusNumber;
    ULONG   DeviceAddress;
    WCHAR   HardwareId[128];
} USB_DEV_INFO;

typedef struct _USB_DEV_LIST {
    ULONG        Count;
    ULONG        Reserved;
    USB_DEV_INFO Devs[USB_MAX_DEVICES];
} USB_DEV_LIST;

/* 控制传输请求 (IOCTL_USB_CONTROL_TRANSFER)
 * 缓冲区布局: [USB_CTRL_REQ][数据] */
typedef struct _USB_CTRL_REQ {
    ULONG  DevIndex;
    UCHAR  bmRequestType;
    UCHAR  bRequest;
    USHORT wValue;
    USHORT wIndex;
    USHORT wLength;
    ULONG  bytesReturned;   /* 驱动回填 */
} USB_CTRL_REQ;

/* 封锁请求 (IOCTL_USB_BLOCK_DEVICE) */
typedef struct _USB_BLOCK_REQ {
    ULONG   DevIndex;
    BOOLEAN Block;
    UCHAR   Reserved[3];
} USB_BLOCK_REQ;

/* 调试信息 (IOCTL_USB_DEBUG) */
typedef struct _USB_DEBUG_INFO {
    ULONG HookInstalled;
    ULONG DeviceCount;
    ULONG LastScanCount;
    ULONG LastScanStatus;
    ULONG HubDriverFound;
    ULONG HookStatus;
    ULONG Tracked;
} USB_DEBUG_INFO;

/* ==================================================================
 * HID 节点表 (IOCTL_USB_NODES)
 * 布局对应 core/usb_node.h 的 HID_NODE，去掉内核指针
 * ================================================================== */
#define HID_NODE_MAX_UM     10
#define HID_NAME_MAX_UM     64
#define HID_PATH_MAX_UM     260
#define HID_TYPE_NONE_UM    0
#define HID_TYPE_KBD_UM     1
#define HID_TYPE_MOUSE_UM   2

typedef struct _HID_NODE_UM {
    ULONG  Slot;
    USHORT Vid;
    USHORT Pid;
    UCHAR  DeviceType;      /* HID_TYPE_*_UM */
    UCHAR  Blocked;
    UCHAR  Reserved[2];
    WCHAR  ProductName[HID_NAME_MAX_UM];
    WCHAR  InstancePath[HID_PATH_MAX_UM];
} HID_NODE_UM;

typedef struct _HID_NODE_LIST {
    ULONG       Count;
    ULONG       Reserved;
    HID_NODE_UM Nodes[HID_NODE_MAX_UM];
} HID_NODE_LIST;

/* ==================================================================
 * KMBox 判定结果 (IOCTL_USB_KMBOX)
 * 鼠标与键盘共享祖父设备即命中
 * ================================================================== */
#define KMBOX_RESULT_MAX_UM 30

typedef struct _KMBOX_HIT_UM {
    USHORT MouseVid;
    USHORT MousePid;
    USHORT KbdVid;
    USHORT KbdPid;
    USHORT Reserved;
    WCHAR  MouseProduct[HID_NAME_MAX_UM];
    WCHAR  KbdProduct[HID_NAME_MAX_UM];
    WCHAR  SharedPath[HID_PATH_MAX_UM];
} KMBOX_HIT_UM;

typedef struct _KMBOX_LIST {
    ULONG        Count;
    ULONG        Reserved;
    KMBOX_HIT_UM Hits[KMBOX_RESULT_MAX_UM];
} KMBOX_LIST;

/* ==================================================================
 * 全量枚举 (IOCTL_USB_FULL_ENUM)
 * 走 Hub 端口，采集设备描述符 + 配置描述符 + 字符串 + 接口列表
 * ================================================================== */
#define USB_FULL_MAX_UM      32
#define USB_STR_MAX_UM       64
#define USB_IFACE_MAX_UM     8

typedef struct _USB_IFACE_UM {
    UCHAR  Number;          /* bInterfaceNumber */
    UCHAR  Class;           /* bInterfaceClass: 3=HID, 2/0x0A=CDC, 0xFF=厂商 */
    UCHAR  SubClass;
    UCHAR  Protocol;
    UCHAR  Endpoints;
    UCHAR  HasReport;       /* 1 = 拿到了 HID 报告描述符 */
    USHORT ReportLen;

    /* HID 描述符 (0x21) 指纹维度 */
    USHORT HidDescLen;      /* 0 = 非 HID 接口 */
    USHORT HidBcdHID;
    UCHAR  HidCountry;
    UCHAR  HidNumDesc;
    UCHAR  HasPhysDesc;     /* HID 物理描述符 (0x23) 是否存在 */
    UCHAR  PhysDescLen;
} USB_IFACE_UM;

typedef struct _USB_DEV_UM {
    ULONG  HubIndex;
    ULONG  PortNumber;
    UCHAR  Speed;           /* 1=Low 2=Full 3=High 4=Super */
    UCHAR  Suspicious;      /* 1 = HID 与 CDC/厂商接口并存（kmbox 形态） */
    USHORT Vid;
    USHORT Pid;
    USHORT BcdDevice;
    USHORT BcdUSB;
    UCHAR  DeviceClass;
    UCHAR  DeviceSubClass;
    UCHAR  DeviceProtocol;
    UCHAR  NumConfigurations;
    UCHAR  ConfigValue;
    UCHAR  NumInterfaces;
    USHORT ConfigTotalLength;
    WCHAR  Manufacturer[USB_STR_MAX_UM];
    WCHAR  Product[USB_STR_MAX_UM];
    WCHAR  Serial[USB_STR_MAX_UM];

    /* 设备限定符 (0x06) 指纹维度 */
    UCHAR  HasQualifier;
    UCHAR  QualClass;
    UCHAR  QualSubClass;
    UCHAR  QualProtocol;
    UCHAR  QualMaxPacket0;
    UCHAR  QualNumConfigs;
    USHORT QualBcdUSB;

    /* BOS (0x0F) 指纹维度 */
    UCHAR  HasBos;
    UCHAR  BosCapCount;
    USHORT BosTotalLen;

    UCHAR  IfaceCount;
    UCHAR  Reserved[3];
    USB_IFACE_UM Ifaces[USB_IFACE_MAX_UM];
} USB_DEV_UM;

typedef struct _USB_FULL_LIST_UM {
    ULONG      Count;
    ULONG      HubCount;
    ULONG      LastStatus;      /* 0 = 成功 */
    ULONG      Reserved;
    USB_DEV_UM Devs[USB_FULL_MAX_UM];
} USB_FULL_LIST_UM;

/* 报告描述符请求 (IOCTL_USB_REPORT_DESC 输入) */
typedef struct _USB_REPORT_REQ {
    ULONG DevIndex;         /* USB_FULL_LIST_UM.Devs 的下标 */
    ULONG IfaceIndex;       /* 该设备的第几个接口（HID 接口才有） */
} USB_REPORT_REQ;

/* 报告描述符响应 */
#define USB_REPORT_MAX_UM   256

typedef struct _USB_REPORT_RSP {
    ULONG DevIndex;
    ULONG IfaceIndex;
    ULONG Length;
    ULONG Reserved;
    UCHAR Data[USB_REPORT_MAX_UM];
} USB_REPORT_RSP;

/* ==================================================================
 * 任意控制传输 (IOCTL_USB_RAW_CONTROL)
 * ==================================================================
 * DevIndex 用 USB_FULL_LIST_UM.Devs 的下标不行（那是 Hub 端口表），
 * 这里用 GUID_DEVINTERFACE_USB_DEVICE 枚举的下标（Usually 0..N）。
 * 3 环可以先调 IOCTL_USB_FULL_ENUM 拿到设备，再用索引试探。
 */
#define USB_RAW_MAX_UM      256

typedef struct _USB_RAW_CTRL_REQ {
    ULONG  DevIndex;        /* GUID_DEVINTERFACE_USB_DEVICE 枚举下标 */
    UCHAR  bmRequestType;   /* 0x80 = IN | Standard | Device */
    UCHAR  bRequest;        /* 6 = GET_DESCRIPTOR */
    USHORT wValue;
    USHORT wIndex;
    USHORT wLength;         /* 期望字节数，<= USB_RAW_MAX_UM */
    USHORT Reserved;
    ULONG  Status;          /* 返回：NTSTATUS */
    ULONG  Returned;        /* 返回：实际字节数 */
    UCHAR  Data[USB_RAW_MAX_UM];
} USB_RAW_CTRL_REQ;
