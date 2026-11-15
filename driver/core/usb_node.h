/*
 * usb_node.h - HID 设备节点表 + KMBox 判定
 *
 * ==================================================================
 * 本文件只保留 **在目标 VM 上实际跑通并验证过** 的能力
 * ==================================================================
 *
 *   ✓ 枚举：DeviceObject -> DeviceObjectExtension(+0x138)
 *           -> DeviceNode(+0x28) -> InstancePath(+0x28)
 *           VM 日志验证：path=HID\VID_0E0F&PID_0003&MI_01\...
 *
 *   ✓ 解析：InstancePath -> VID/PID
 *           VM 日志验证：VID=0E0F PID=0003
 *
 *   ✓ 节点表：10 槽 + CAS
 *           VM 日志验证：6 node(s)
 *
 *   ✓ 祖父设备：DeviceNode(+0x10) -> Parent -> Parent
 *           VM 日志验证：gp=FFFFE308B74A1670
 *
 *   ✓ 分类：hidclass FDO(+0x18 AttachedDevice) + DeviceType(+0x48)
 *           WinDbg 验证：0x0F=FILE_DEVICE_MOUSE, 0x0B=FILE_DEVICE_KEYBOARD
 *
 *   ✓ KMBox 判定：鼠标节点与键盘节点共享祖父 _DEVICE_NODE
 *           （逻辑已实现，本 VM 无 KMBox，未实测命中）
 *
 * ==================================================================
 * 已移除（跑不通，不留死代码）
 * ==================================================================
 *   ✗ IOCTL_HID_GET_REPORT_DESCRIPTOR   -> C0000010，hidclass 要 FileObject
 *   ✗ IoGetDeviceObjectPointer 取描述符 -> C00000EF，FileObject 没走 CREATE
 *   ✗ ZwOpenFile + ZwDeviceIoControlFile-> 描述符拿不到
 *   ✗ HIDP_GetCollectionDescription     -> 没有描述符喂给它
 *
 *   报告描述符相关的能力（左键位域、移动增量）等拿到描述符后再做。
 *   届时走 ACE 的路子：
 *     hidusb FDO --+0x18--> hidclass FDO --+0x40--> DeviceExtension
 *                                                       --+0x40/+0x48-->
 *                                                         ReportDesc/Length
 */
#pragma once
#include <ntddk.h>

/* ==================================================================
 * 偏移 —— 每个都用 WinDbg `dt nt!_XXX` 在 18362 上核对过
 * ==================================================================
 *   dt nt!_DEVICE_OBJECT
 *     +0x018 AttachedDevice
 *     +0x040 DeviceExtension
 *     +0x048 DeviceType
 *     +0x138 DeviceObjectExtension
 *   dt nt!_DEVOBJ_EXTENSION
 *     +0x028 DeviceNode
 *   dt nt!_DEVICE_NODE
 *     +0x010 Parent
 *     +0x028 InstancePath (_UNICODE_STRING)
 * ================================================================== */

#define ACE_OFF_DEVOBJ_ATTACHED    0x018   /* -> 上层 _DEVICE_OBJECT* */
#define ACE_OFF_DEVOBJ_DEVTYPE     0x048   /* ULONG DeviceType */
#define ACE_OFF_DEVOBJ_EXTENSION   0x138   /* -> _DEVOBJ_EXTENSION* */

#define ACE_DEVICE_TYPE_MOUSE      0x0F    /* FILE_DEVICE_MOUSE */
#define ACE_DEVICE_TYPE_KEYBOARD   0x0B    /* FILE_DEVICE_KEYBOARD */

#define ACE_OFF_DEVEXT_DEVNODE     0x28    /* -> _DEVICE_NODE* */

#define ACE_OFF_DEVNODE_PARENT     0x10    /* -> 父 _DEVICE_NODE* */
#define ACE_OFF_DEVNODE_INSTPATH   0x28    /* UNICODE_STRING */

/* ==================================================================
 * 节点表
 * ================================================================== */
#define HID_NODE_MAX        10
#define HID_NAME_MAX        64
#define HID_PATH_MAX        260

#define HID_TYPE_NONE       0
#define HID_TYPE_KEYBOARD   1
#define HID_TYPE_MOUSE      2

typedef struct _HID_NODE {
    PDEVICE_OBJECT   DeviceObject;      /* 查表键（hidusb FDO / 路径来源设备） */
    PVOID            DeviceNode;        /* _DEVICE_NODE*（自身） */
    PVOID            ParentNode;        /* 父 _DEVICE_NODE*（诊断用） */
    PVOID            GrandParent;       /* 祖父 _DEVICE_NODE*（KMBox 判定用） */

    USHORT           Vid;
    USHORT           Pid;
    USHORT           BcdDevice;

    UCHAR            DeviceType;        /* HID_TYPE_* */
    UCHAR            SourceType;        /* _DEVICE_OBJECT.DeviceType 原始值，诊断用 */
    UCHAR            Blocked;
    UCHAR            Pad;

    WCHAR            ProductName[HID_NAME_MAX];
    WCHAR            InstancePath[HID_PATH_MAX];

    ULONG            Slot;
    volatile LONG    Active;            /* CAS 抢占标志 */
    volatile LONG    ReportCount;       /* 收到的输入报告数（URB hook 用） */
} HID_NODE, *PHID_NODE;

/* ==================================================================
 * KMBox 判定结果
 * ================================================================== */
#define KMBOX_RESULT_MAX    30

typedef struct _KMBOX_RESULT {
    USHORT  MouseVid;
    USHORT  MousePid;
    USHORT  KbdVid;
    USHORT  KbdPid;
    USHORT  Reserved;
    WCHAR   MouseProduct[HID_NAME_MAX];
    WCHAR   KbdProduct[HID_NAME_MAX];
    WCHAR   SharedPath[HID_PATH_MAX];
} KMBOX_RESULT;

/* ==================================================================
 * 接口
 * ================================================================== */

NTSTATUS UsbNodeInit(VOID);
VOID     UsbNodeUnload(VOID);

/*
 * UsbNodeAddEx - 建档
 *   DevObj     : 查表键（URB 完成例程拿到的 DeviceObject，通常是 hidusb FDO）
 *   PathSource : 读 InstancePath / 祖父节点的设备（设备栈底部那层）
 */
NTSTATUS UsbNodeAddEx(_In_ PDEVICE_OBJECT DevObj,
                      _In_ PDEVICE_OBJECT PathSource,
                      _In_ UCHAR DeviceType);
NTSTATUS UsbNodeAdd(_In_ PDEVICE_OBJECT DevObj, _In_ UCHAR DeviceType);
VOID     UsbNodeRemove(_In_ PDEVICE_OBJECT DevObj);

ULONG     UsbNodeCount(VOID);
PHID_NODE UsbNodeGet(_In_ ULONG Index);
PHID_NODE UsbNodeFindByDevice(_In_ PDEVICE_OBJECT DevObj);

/* 补分类（只对 UNKNOWN 的节点，走 DeviceType） */
NTSTATUS UsbNodeClassifyAll(VOID);

/* ★ KMBox 判定：鼠标与键盘是否共享祖父设备 */
NTSTATUS UsbNodeDetectKmbox(_Out_ KMBOX_RESULT *Results,
                            _In_  ULONG MaxResults,
                            _Out_ PULONG HitCount);

/* 记录一次输入报告（只计数，位域抽取等有描述符后再做） */
VOID  UsbNodeNoteReport(_In_ PDEVICE_OBJECT DevObj, _In_ const UCHAR *Data, _In_ ULONG Len);
ULONG UsbNodeReportCount(_In_ PDEVICE_OBJECT DevObj);

/* ==================================================================
 * 结构访问器（全部带 MmIsAddressValid 校验）
 * ================================================================== */
PVOID    UsbGetDeviceNode(_In_ PDEVICE_OBJECT DevObj);
PVOID    UsbGetGrandParentNode(_In_ PDEVICE_OBJECT DevObj);
NTSTATUS UsbGetInstancePath(_In_ PDEVICE_OBJECT DevObj, _Out_ PUNICODE_STRING *Path);
BOOLEAN  UsbParseVidPid(_In_ PCUNICODE_STRING Path, _Out_ PUSHORT Vid, _Out_ PUSHORT Pid);
