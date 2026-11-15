/*
 * usb_hub.h - 通过 Hub 端口枚举所有 USB 设备 + 读完整描述符
 *
 * ==================================================================
 * 为什么走 Hub 而不是 hidclass
 * ==================================================================
 * 之前的弯路：想从 hidclass 拿 HID 报告描述符
 *   - IOCTL_HID_GET_REPORT_DESCRIPTOR  -> C0000010（要 FileObject->FsContext）
 *   - IoGetDeviceObjectPointer         -> C00000EF（FileObject 没走 CREATE）
 *   - ZwOpenFile + ZwDeviceIoControl   -> 拿不到
 *
 * 正确做法（本文实现）：
 *   所有发往设备的描述符请求必须由它的**父 Hub 驱动**代理转发。
 *   向 Hub 发 IOCTL_USB_GET_DESCRIPTOR_FROM_NODE_CONNECTION，
 *   指定 ConnectionIndex(=端口号) + SetupPacket，就能拿到该端口上
 *   设备的**任意描述符**：
 *     0x01 设备描述符   0x02 配置描述符
 *     0x03 字符串描述符 0x22 HID 报告描述符
 *
 * 这条路完全文档化，不需要 FileObject、不需要 hidclass、不受保护限制。
 */
#pragma once
#include <ntddk.h>

/* ==================================================================
 * 结果表
 * ================================================================== */
#define USB_FULL_MAX        32      /* 最多记录 32 个端口上的设备 */
#define USB_FULL_HUB_MAX    8       /* 最多 8 个 hub */

#define USB_STR_MAX         64      /* 字符串描述符最多 63 字符 */
#define USB_REPORT_DESC_MAX 256     /* 报告描述符最多 256 字节 */
#define USB_CFG_MAX         512     /* 配置描述符最多 512 字节 */

/* 设备速度（对应 USB_NODE_CONNECTION_INFORMATION_EX.Speed） */
#define USB_SPEED_UNKNOWN   0
#define USB_SPEED_LOW       1
#define USB_SPEED_FULL      2
#define USB_SPEED_HIGH      3
#define USB_SPEED_SUPER     4

typedef struct _USB_IFACE_INFO {
    UCHAR InterfaceNumber;
    UCHAR AltSetting;
    UCHAR Class;
    UCHAR SubClass;
    UCHAR Protocol;
    UCHAR EndpointCount;
    UCHAR HasReportDesc;            /* 1 = 拿到了 HID 报告描述符 */
    UCHAR Pad;
    USHORT ReportDescLen;
    UCHAR  ReportDesc[USB_REPORT_DESC_MAX];

    /* ---- HID 描述符 (0x21)：指纹维度 ---- */
    USHORT HidDescLen;              /* 0 = 不是 HID 接口 */
    USHORT HidBcdHID;               /* HID 协议版本，如 0x0111 */
    UCHAR  HidCountry;              /* 国家代码，多数为 0 */
    UCHAR  HidNumDesc;              /* 后面跟几个子描述符 */
    UCHAR  HasPhysDesc;             /* HID 物理描述符 (0x23) 是否可取 */
    UCHAR  PhysDescLen;
} USB_IFACE_INFO;

typedef struct _USB_DEV_FULL {
    /* 位置 */
    ULONG  HubIndex;                /* 第几个 hub */
    ULONG  PortNumber;              /* Hub 上的端口号（从 1 开始） */
    UCHAR  Speed;
    UCHAR  Reserved;

    /* 设备描述符 */
    USHORT Vid;
    USHORT Pid;
    USHORT BcdDevice;
    USHORT BcdUSB;
    UCHAR  DeviceClass;
    UCHAR  DeviceSubClass;
    UCHAR  DeviceProtocol;
    UCHAR  NumConfigurations;
    UCHAR  MaxPacketSize0;

    /* 配置描述符 */
    UCHAR  ConfigValue;
    UCHAR  ConfigAttributes;
    UCHAR  MaxPower;                /* 单位 2mA */
    UCHAR  NumInterfaces;
    USHORT ConfigTotalLength;

    /* 字符串描述符 */
    WCHAR  Manufacturer[USB_STR_MAX];
    WCHAR  Product[USB_STR_MAX];
    WCHAR  Serial[USB_STR_MAX];

    /* 接口特征（kmbox 判定关键：HID + CDC/厂商接口并存） */
    UCHAR        IfaceCount;
    USB_IFACE_INFO Ifaces[8];

    /* ---- 设备限定符 (0x06)：指纹维度 ---- */
    UCHAR  HasQualifier;            /* 1 = 设备支持高速且拿到了限定符 */
    UCHAR  QualClass;
    UCHAR  QualSubClass;
    UCHAR  QualProtocol;
    UCHAR  QualMaxPacket0;
    UCHAR  QualNumConfigs;
    USHORT QualBcdUSB;

    /* ---- BOS 描述符 (0x0F)：USB 2.1+ 能力集 ---- */
    UCHAR  HasBos;
    UCHAR  BosCapCount;             /* 里面有几个 Device Capability */
    USHORT BosTotalLen;

    /* Hub 符号链接（诊断用） */
    WCHAR  HubPath[128];
} USB_DEV_FULL;

typedef struct _USB_FULL_LIST {
    ULONG        Count;
    ULONG        HubCount;
    ULONG        LastStatus;
    ULONG        Reserved;
    USB_DEV_FULL Devs[USB_FULL_MAX];
} USB_FULL_LIST;

/* ==================================================================
 * 接口
 * ================================================================== */

/*
 * UsbHubEnumAll - 遍历所有 Hub 的所有端口，采集完整描述符
 *
 * ★ 必须在 PASSIVE_LEVEL 调用（内部发同步 IOCTL、可能分页 I/O）
 */
NTSTATUS UsbHubEnumAll(_Out_ USB_FULL_LIST *List);

/* 清空结果 */
VOID UsbHubClear(VOID);

/* 最后一次枚举的状态（0 = 成功） */
ULONG UsbHubLastStatus(VOID);

/* 判定设备是否为可疑复合形态（HID + CDC/厂商接口并存） */
BOOLEAN UsbHubIsSuspiciousComposite(_In_ const USB_DEV_FULL *Dev);

/* 取缓存里的第 Index 个设备（UsbHubEnumAll 之后有效） */
USB_DEV_FULL *UsbHubGetCached(_In_ ULONG Index);
ULONG         UsbHubCachedCount(VOID);

/* ==================================================================
 * 原始控制传输（不限于 GET_DESCRIPTOR）
 * ==================================================================
 * 目标由缓存里的设备下标定位（用它的 HubPath + PortNumber）。
 *
 * SetupPacket 完全由调用方填 —— 和 IOCTL_USB_GET_DESCRIPTOR_FROM_NODE_CONNECTION
 * 用的是同一个结构，区别只是 bRequest/bmRequest/wValue 不再固定。
 *
 * ★ 关键待验证点：hub 驱动是否校验 bRequest 一定是 GET_DESCRIPTOR(6)。
 *   如果不校验，就能发任意控制传输（GET_STATUS / SET_CONFIGURATION /
 *   厂商自定义 …），fuzz 和 diff 都能做。
 *   如果校验，返回 STATUS_INVALID_DEVICE_REQUEST，需要改走 usbccgp PDO。
 *
 * ★ PASSIVE_LEVEL
 */
NTSTATUS UsbHubRawControlToDevice(_In_ ULONG DevIndex,
                                  _In_ UCHAR bmRequest, _In_ UCHAR bRequest,
                                  _In_ USHORT wValue, _In_ USHORT wIndex,
                                  _Inout_updates_bytes_(wLength) PVOID Data,
                                  _In_ USHORT wLength,
                                  _Out_ PULONG Returned);

/*
 * UsbHubRawControlSelfTest - 对第一个非 hub 设备试几种控制请求并打日志
 *
 * 用来一次性判定 hub 是否允许任意 SetupPacket。
 */
VOID UsbHubRawControlSelfTest(VOID);

/* 自测 2：通过 GUID_DEVINTERFACE_USB_DEVICE 发 URB，验证 URB 通道是否可用 */
VOID UsbHubUrbProbeSelfTest(VOID);

/* 自测 3：用 URB_FUNCTION_CONTROL_TRANSFER 发任意 SetupPacket（fuzz 能力验证） */
VOID UsbUrbControlSelfTest(VOID);

/*
 * UsbUrbControlTransfer - 任意控制传输（内核版 libusb_control_transfer）
 *
 * DevIndex    : GUID_DEVINTERFACE_USB_DEVICE 枚举列表里的下标
 * BufLen      : 实际数据缓冲区字节数（<= USB_REPORT_DESC_MAX）
 * DeclaredLen : SetupPacket 里声明的 wLength，0 表示与 BufLen 相同
 *               —— 两者分开是为了忠实复现 wLength=65535 这类越界探测
 * ★ PASSIVE_LEVEL
 */
NTSTATUS UsbUrbControlTransfer(_In_ ULONG DevIndex,
                               _In_ UCHAR bmRequest, _In_ UCHAR bRequest,
                               _In_ USHORT wValue, _In_ USHORT wIndex,
                               _Inout_updates_bytes_opt_(BufLen) PVOID Data,
                               _In_ USHORT BufLen,
                               _In_ USHORT DeclaredLen,
                               _Out_ PULONG Got);
