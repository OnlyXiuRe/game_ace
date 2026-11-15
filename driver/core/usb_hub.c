/*
 * usb_hub.c - 通过 Hub 端口枚举 USB 设备并读取完整描述符
 *
 * ==================================================================
 * 架构
 * ==================================================================
 *   1. IoGetDeviceInterfaces(GUID_DEVINTERFACE_USB_HUB)  拿所有 hub
 *   2. IoGetDeviceObjectPointer 打开每个 hub
 *   3. IOCTL_USB_GET_NODE_INFORMATION                    拿端口数
 *   4. 对每个端口 IOCTL_USB_GET_NODE_CONNECTION_INFORMATION_EX
 *         -> ConnectionStatus == DeviceConnected 就有设备
 *         -> 同时拿到设备描述符（VID/PID/class/speed）
 *   5. 对已连接端口，用 IOCTL_USB_GET_DESCRIPTOR_FROM_NODE_CONNECTION
 *      分别取：配置描述符(0x02)、字符串(0x03×3)、HID报告描述符(0x22)
 *
 * ★ 为什么必须经过 Hub：
 *   描述符请求必须由设备的父 Hub 驱动代理。直接对设备对象发
 *   GET_DESCRIPTOR 是不行的（会被 hidclass/usbccgp 拒掉，之前踩过）。
 *
 * ★ IRQL：全部 PASSIVE_LEVEL。
 */
#include <initguid.h>       /* DEFINE_GUID：只在本文件出现一次 */
#include "module.h"
#include "usb_hub.h"
#include <usbiodef.h>
#include <usbioctl.h>
#include <usb.h>
#include <usbdlib.h>
#include <ntstrsafe.h>

#ifndef ARRAY_SIZE
#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))
#endif

/* usb.h 里不一定有这个名字，兜一个 */
#ifndef USB_HID_REPORT_DESCRIPTOR_TYPE
#define USB_HID_REPORT_DESCRIPTOR_TYPE      0x22
#endif
#ifndef USB_HID_DESCRIPTOR_TYPE
#define USB_HID_DESCRIPTOR_TYPE             0x21
#endif
#ifndef USB_HID_PHYSICAL_DESCRIPTOR_TYPE
#define USB_HID_PHYSICAL_DESCRIPTOR_TYPE    0x23
#endif
#ifndef USB_DEVICE_QUALIFIER_DESCRIPTOR_TYPE
#define USB_DEVICE_QUALIFIER_DESCRIPTOR_TYPE 0x06
#endif
#ifndef USB_BOS_DESCRIPTOR_TYPE
#define USB_BOS_DESCRIPTOR_TYPE             0x0F
#endif

/* ==================================================================
 * 同步 IOCTL
 * ================================================================== */
static NTSTATUS SendSyncIoctl(PDEVICE_OBJECT DevObj, ULONG Code,
                              PVOID InBuf, ULONG InLen,
                              PVOID OutBuf, ULONG OutLen,
                              PULONG OutInfo)
{
    KEVENT          ev;
    IO_STATUS_BLOCK iosb;
    PIRP            irp;
    NTSTATUS        st;

    if (OutInfo) *OutInfo = 0;

    KeInitializeEvent(&ev, NotificationEvent, FALSE);
    RtlZeroMemory(&iosb, sizeof(iosb));

    irp = IoBuildDeviceIoControlRequest(Code, DevObj, InBuf, InLen,
                                        OutBuf, OutLen, FALSE, &ev, &iosb);
    if (irp == NULL) return STATUS_INSUFFICIENT_RESOURCES;

    st = IofCallDriver(DevObj, irp);
    if (st == STATUS_PENDING) {
        KeWaitForSingleObject(&ev, Executive, KernelMode, FALSE, NULL);
        st = iosb.Status;
    }

    if (OutInfo) *OutInfo = (ULONG)iosb.Information;
    return st;
}

/* ==================================================================
 * 通过 Hub 取任意描述符
 * ==================================================================
 * ConnectionIndex : 端口号（从 1 开始）
 * DescType        : 0x01 设备 / 0x02 配置 / 0x03 字符串 / 0x22 HID报告
 * DescIndex       : 描述符索引（字符串索引；配置索引；报告描述符填 0）
 * LangId          : 字符串用 0x0409；其它填 0
 * Out/OutLen      : 输出缓冲
 */
static NTSTATUS HubGetDescriptor(PDEVICE_OBJECT HubDev, ULONG Port,
                                 UCHAR DescType, UCHAR DescIndex, USHORT LangId,
                                 PVOID Out, ULONG OutLen, PULONG Returned)
{
    ULONG                   header = FIELD_OFFSET(USB_DESCRIPTOR_REQUEST, Data);
    ULONG                   total  = header + OutLen;
    PUSB_DESCRIPTOR_REQUEST req;
    NTSTATUS                st;
    ULONG                   info = 0;

    if (Returned) *Returned = 0;
    if (Out == NULL || OutLen == 0) return STATUS_INVALID_PARAMETER;

    req = (PUSB_DESCRIPTOR_REQUEST)ExAllocatePoolWithTag(NonPagedPool, total, 'bHsU');
    if (req == NULL) return STATUS_INSUFFICIENT_RESOURCES;

    RtlZeroMemory(req, total);
    req->ConnectionIndex         = Port;
    req->SetupPacket.bmRequest   = 0x80;                        /* 设备->主机, 标准 */
    req->SetupPacket.bRequest    = USB_REQUEST_GET_DESCRIPTOR;
    req->SetupPacket.wValue      = (USHORT)((DescType << 8) | DescIndex);
    req->SetupPacket.wIndex      = LangId;
    req->SetupPacket.wLength     = (USHORT)OutLen;

    st = SendSyncIoctl(HubDev, IOCTL_USB_GET_DESCRIPTOR_FROM_NODE_CONNECTION,
                       req, total, req, total, &info);

    if (NT_SUCCESS(st)) {
        RtlCopyMemory(Out, req->Data, OutLen);

        /*
         * 长度必须用 ioStatus.Information（实际传输字节数）。
         *
         * 之前用 Data[0] 当长度是错的 —— HID 报告描述符(0x22)
         * 是一串 item，没有 bLength 字段，Data[0] 只是第一个
         * item 的 prefix（例如 0x05 = Usage Page），
         * 所以拿到的"长度"是垃圾值。
         */
        if (Returned) {
            ULONG cap = (info > OutLen) ? OutLen : info;
            if (cap == 0) {
                /* 某些路径 Information 为 0，退回用描述符自身长度 */
                PUCHAR d = (PUCHAR)Out;
                if (DescType == USB_CONFIGURATION_DESCRIPTOR_TYPE)
                    cap = d[2] | (d[3] << 8);
                else if (DescType != USB_HID_REPORT_DESCRIPTOR_TYPE)
                    cap = d[0];
            }
            *Returned = cap;
        }
    }

    ExFreePoolWithTag(req, 'bHsU');
    return st;
}

/* ==================================================================
 * 字符串描述符 -> WCHAR
 * ==================================================================
 * 返回: [bLength][bDescriptorType=3][UTF-16LE ...]
 */
static VOID ParseStringDescriptor(const UCHAR *Desc, ULONG Len,
                                  PWCHAR Out, ULONG OutChars)
{
    ULONG i, chars;

    Out[0] = 0;
    if (Desc == NULL || Len < 4 || Desc[1] != USB_STRING_DESCRIPTOR_TYPE) return;

    chars = (Len - 2) / 2;
    if (chars > OutChars - 1) chars = OutChars - 1;

    for (i = 0; i < chars; i++) {
        /* 描述符里是 UTF-16LE，直接按 WCHAR 拷 */
        Out[i] = (WCHAR)(Desc[2 + i * 2] | (Desc[3 + i * 2] << 8));
    }
    Out[chars] = 0;
}

static VOID ReadStringDesc(PDEVICE_OBJECT HubDev, ULONG Port, UCHAR Index,
                           PWCHAR Out, ULONG OutChars)
{
    UCHAR    buf[128];
    ULONG    got = 0;

    Out[0] = 0;
    if (Index == 0) return;     /* 0 = 没有字符串 */

    RtlZeroMemory(buf, sizeof(buf));
    if (NT_SUCCESS(HubGetDescriptor(HubDev, Port, USB_STRING_DESCRIPTOR_TYPE,
                                    Index, 0x0409, buf, sizeof(buf), &got)))
    {
        ParseStringDescriptor(buf, sizeof(buf), Out, OutChars);
    }
}

/* ==================================================================
 * 解析配置描述符里的接口
 * ================================================================== */
static VOID ParseConfigInterfaces(PDEVICE_OBJECT HubDev, ULONG Port,
                                  const UCHAR *Cfg, ULONG CfgLen,
                                  USB_DEV_FULL *Dev)
{
    PUSB_CONFIGURATION_DESCRIPTOR cd = (PUSB_CONFIGURATION_DESCRIPTOR)Cfg;
    ULONG offset = 0;

    if (CfgLen < sizeof(USB_CONFIGURATION_DESCRIPTOR)) return;
    if (cd->bLength != sizeof(USB_CONFIGURATION_DESCRIPTOR)) return;

    Dev->ConfigValue       = cd->bConfigurationValue;
    Dev->ConfigAttributes  = cd->bmAttributes;
    Dev->MaxPower          = cd->MaxPower;
    Dev->NumInterfaces     = cd->bNumInterfaces;
    Dev->ConfigTotalLength = cd->wTotalLength;

    /* 手工遍历变长描述符链（比 USBD_ParseDescriptors 更直观，也避免 lib 依赖） */
    while (offset + 2 <= CfgLen && offset + 2 <= cd->wTotalLength) {
        const UCHAR *d = Cfg + offset;
        UCHAR blen = d[0];
        UCHAR btype = d[1];

        if (blen < 2 || offset + blen > CfgLen) break;

        if (btype == USB_INTERFACE_DESCRIPTOR_TYPE &&
            blen >= sizeof(USB_INTERFACE_DESCRIPTOR))
        {
            PUSB_INTERFACE_DESCRIPTOR id = (PUSB_INTERFACE_DESCRIPTOR)d;

            /*
             * 只取 alternate setting 0。
             * 一个接口可以有多个 alt setting（Bluetooth 就有 6 个），
             * bInterfaceNumber 相同，全记下来会重复计数。
             */
            if (id->bAlternateSetting == 0 &&
                Dev->IfaceCount < ARRAY_SIZE(Dev->Ifaces))
            {
                USB_IFACE_INFO *ii = &Dev->Ifaces[Dev->IfaceCount];

                RtlZeroMemory(ii, sizeof(*ii));
                ii->InterfaceNumber = id->bInterfaceNumber;
                ii->AltSetting      = id->bAlternateSetting;
                ii->Class           = id->bInterfaceClass;
                ii->SubClass        = id->bInterfaceSubClass;
                ii->Protocol        = id->bInterfaceProtocol;
                ii->EndpointCount   = id->bNumEndpoints;
                Dev->IfaceCount++;

                /*
                 * HID 接口（class 3）单独要一份报告描述符。
                 * wIndex = 接口号，wValue = (0x22 << 8) | 0。
                 */
                if (id->bInterfaceClass == 3) {
                    UCHAR    rep[USB_REPORT_DESC_MAX];
                    ULONG    got = 0;
                    NTSTATUS rst;

                    RtlZeroMemory(rep, sizeof(rep));
                    rst = HubGetDescriptor(HubDev, Port,
                                           USB_HID_REPORT_DESCRIPTOR_TYPE,
                                           0, id->bInterfaceNumber,
                                           rep, sizeof(rep), &got);

                    if (NT_SUCCESS(rst) && got > 0) {
                        if (got > USB_REPORT_DESC_MAX) got = USB_REPORT_DESC_MAX;
                        RtlCopyMemory(ii->ReportDesc, rep, got);
                        ii->ReportDescLen = (USHORT)got;
                        ii->HasReportDesc = 1;

                        LOG_INFO("    port %lu iface %u: HID report desc %lu bytes",
                                 Port, id->bInterfaceNumber, got);
                    } else {
                        /* 真实状态码 + 实际字节数，方便判断是拒绝还是返回空 */
                        LOG_WARN("    port %lu iface %u: report desc failed "
                                 "st=%08X got=%lu",
                                 Port, id->bInterfaceNumber, rst, got);
                    }

                    /*
                     * HID 描述符 (0x21) —— 指纹维度
                     *   [0] bLength  [1] 0x21  [2..3] bcdHID
                     *   [4] bCountryCode  [5] bNumDescriptors
                     *   [6..] DescriptorList[]: {type(1), len(2)} × N
                     */
                    {
                        UCHAR  hd[64];
                        ULONG  hgot = 0;

                        RtlZeroMemory(hd, sizeof(hd));
                        if (NT_SUCCESS(HubGetDescriptor(HubDev, Port,
                                                        USB_HID_DESCRIPTOR_TYPE,
                                                        0, id->bInterfaceNumber,
                                                        hd, sizeof(hd), &hgot))
                            && hgot >= 9 && hd[0] >= 9 && hd[1] == USB_HID_DESCRIPTOR_TYPE)
                        {
                            ii->HidDescLen = hd[0];
                            ii->HidBcdHID  = (USHORT)(hd[2] | (hd[3] << 8));
                            ii->HidCountry = hd[4];
                            ii->HidNumDesc = hd[5];

                            for (ULONG k = 0; k < hd[5]; k++) {
                                ULONG off = 6 + k * 3;
                                if (off + 3 > hd[0]) break;
                                if (hd[off] == USB_HID_PHYSICAL_DESCRIPTOR_TYPE) {
                                    ii->HasPhysDesc = 1;
                                    ii->PhysDescLen = (UCHAR)
                                        (hd[off + 1] | (hd[off + 2] << 8));
                                }
                            }

                            LOG_INFO("      HID desc: bcdHID=%04X country=%u numDesc=%u "
                                     "phys=%u",
                                     ii->HidBcdHID, ii->HidCountry, ii->HidNumDesc,
                                     ii->HasPhysDesc);
                        }
                    }
                }
            }
        }

        offset += blen;
    }
}

/* ==================================================================
 * 采集单个端口上的设备
 * ================================================================== */
static BOOLEAN QueryPortDevice(PDEVICE_OBJECT HubDev, ULONG HubIndex, ULONG Port,
                               USB_DEV_FULL *Dev)
{
    USB_NODE_CONNECTION_INFORMATION_EX conn;
    NTSTATUS                          st;
    UCHAR                             cfg[USB_CFG_MAX];
    ULONG                             got = 0;

    RtlZeroMemory(&conn, sizeof(conn));
    conn.ConnectionIndex = Port;

    st = SendSyncIoctl(HubDev, IOCTL_USB_GET_NODE_CONNECTION_INFORMATION_EX,
                       &conn, sizeof(conn), &conn, sizeof(conn), NULL);

    if (!NT_SUCCESS(st)) {
        LOG_DEBUG("port %lu: connection info failed %08X", Port, st);
        return FALSE;
    }
    if (conn.ConnectionStatus != DeviceConnected) return FALSE;

    RtlZeroMemory(Dev, sizeof(*Dev));
    Dev->HubIndex   = HubIndex;
    Dev->PortNumber = Port;
    Dev->Speed      = (conn.Speed <= USB_SPEED_SUPER) ? conn.Speed : USB_SPEED_UNKNOWN;

    /* 设备描述符（连接信息里已经带了一份） */
    Dev->Vid            = conn.DeviceDescriptor.idVendor;
    Dev->Pid            = conn.DeviceDescriptor.idProduct;
    Dev->BcdDevice      = conn.DeviceDescriptor.bcdDevice;
    Dev->BcdUSB         = conn.DeviceDescriptor.bcdUSB;
    Dev->DeviceClass    = conn.DeviceDescriptor.bDeviceClass;
    Dev->DeviceSubClass = conn.DeviceDescriptor.bDeviceSubClass;
    Dev->DeviceProtocol = conn.DeviceDescriptor.bDeviceProtocol;
    Dev->NumConfigurations = conn.DeviceDescriptor.bNumConfigurations;
    Dev->MaxPacketSize0 = conn.DeviceDescriptor.bMaxPacketSize0;

    /* 字符串描述符 */
    ReadStringDesc(HubDev, Port, conn.DeviceDescriptor.iManufacturer,
                   Dev->Manufacturer, USB_STR_MAX);
    ReadStringDesc(HubDev, Port, conn.DeviceDescriptor.iProduct,
                   Dev->Product, USB_STR_MAX);
    ReadStringDesc(HubDev, Port, conn.DeviceDescriptor.iSerialNumber,
                   Dev->Serial, USB_STR_MAX);

    /* 配置描述符：先 9 字节头，再全量 */
    RtlZeroMemory(cfg, sizeof(cfg));
    got = 0;
    if (NT_SUCCESS(HubGetDescriptor(HubDev, Port, USB_CONFIGURATION_DESCRIPTOR_TYPE,
                                    0, 0, cfg, sizeof(USB_CONFIGURATION_DESCRIPTOR),
                                    &got)))
    {
        PUSB_CONFIGURATION_DESCRIPTOR cd = (PUSB_CONFIGURATION_DESCRIPTOR)cfg;
        USHORT total = cd->wTotalLength;

        if (total >= sizeof(USB_CONFIGURATION_DESCRIPTOR) && total <= USB_CFG_MAX) {
            RtlZeroMemory(cfg, sizeof(cfg));
            got = 0;
            if (NT_SUCCESS(HubGetDescriptor(HubDev, Port,
                                            USB_CONFIGURATION_DESCRIPTOR_TYPE,
                                            0, 0, cfg, total, &got)))
            {
                ParseConfigInterfaces(HubDev, Port, cfg, total, Dev);
            }
        }
    } else {
        LOG_DEBUG("port %lu: config desc failed", Port);
    }

    /*
     * 设备限定符 (0x06) —— 只有高速能力的设备才有。
     *   [0] bLength(=10) [1] 0x06 [2..3] bcdUSB [4] class
     *   [5] subclass [6] protocol [7] maxpkt0 [8] numConfigs [9] reserved
     */
    {
        UCHAR q[16];
        ULONG qgot = 0;

        RtlZeroMemory(q, sizeof(q));
        if (NT_SUCCESS(HubGetDescriptor(HubDev, Port,
                                        USB_DEVICE_QUALIFIER_DESCRIPTOR_TYPE,
                                        0, 0, q, sizeof(q), &qgot))
            && qgot >= 10 && q[0] == 10 && q[1] == USB_DEVICE_QUALIFIER_DESCRIPTOR_TYPE)
        {
            Dev->HasQualifier   = 1;
            Dev->QualBcdUSB     = (USHORT)(q[2] | (q[3] << 8));
            Dev->QualClass      = q[4];
            Dev->QualSubClass   = q[5];
            Dev->QualProtocol   = q[6];
            Dev->QualMaxPacket0 = q[7];
            Dev->QualNumConfigs = q[8];

            LOG_INFO("  port %lu: DEVICE_QUALIFIER bcdUSB=%04X class=%02X/%02X/%02X "
                     "maxpkt0=%u cfgs=%u",
                     Port, Dev->QualBcdUSB, Dev->QualClass, Dev->QualSubClass,
                     Dev->QualProtocol, Dev->QualMaxPacket0, Dev->QualNumConfigs);
        }
    }

    /*
     * BOS 描述符 (0x0F) —— USB 2.1+ 才有。
     *   [0] bLength(=5) [1] 0x0F [2..3] wTotalLength [4] bNumDeviceCaps
     */
    {
        UCHAR b[USB_CFG_MAX];
        ULONG bgot = 0;

        RtlZeroMemory(b, sizeof(b));
        if (NT_SUCCESS(HubGetDescriptor(HubDev, Port,
                                        USB_BOS_DESCRIPTOR_TYPE,
                                        0, 0, b, sizeof(b), &bgot))
            && bgot >= 5 && b[0] >= 5 && b[1] == USB_BOS_DESCRIPTOR_TYPE)
        {
            Dev->HasBos      = 1;
            Dev->BosTotalLen = (USHORT)(b[2] | (b[3] << 8));
            Dev->BosCapCount = b[4];

            LOG_INFO("  port %lu: BOS total=%u caps=%u", Port,
                     Dev->BosTotalLen, Dev->BosCapCount);

            /* 把每个 Device Capability 的类型也打出来（指纹维度） */
            {
                ULONG off = b[0];
                UCHAR capIdx = 0;
                while (off + 3 <= bgot && off + 3 <= Dev->BosTotalLen) {
                    UCHAR clen = b[off];
                    if (clen < 3) break;
                    LOG_INFO("      BOS cap[%u] type=0x%02X len=%u",
                             capIdx, b[off + 1], clen);
                    capIdx++;
                    off += clen;
                }
            }
        }
    }

    LOG_INFO("  port %lu: VID=%04X PID=%04X class=%02X/%02X/%02X speed=%u ifaces=%u",
             Port, Dev->Vid, Dev->Pid, Dev->DeviceClass, Dev->DeviceSubClass,
             Dev->DeviceProtocol, Dev->Speed, Dev->IfaceCount);

    return TRUE;
}

/* ==================================================================
 * 主枚举
 * ================================================================== */
static KSPIN_LOCK        g_Lock;
static USB_DEV_FULL      g_Last[USB_FULL_MAX];
static volatile LONG     g_LastCount = 0;
static volatile LONG     g_LastStatus = 0;

/*
 * UsbHubEnumAll - 遍历所有 Hub 的所有端口
 *
 * ★ PASSIVE_LEVEL
 */
NTSTATUS UsbHubEnumAll(USB_FULL_LIST *List)
{
    PWSTR     links = NULL;
    NTSTATUS  st;
    ULONG     hubIndex = 0;
    ULONG     devCount = 0;
    ULONG     totalPorts = 0;
    KIRQL     irql;

    if (List == NULL) return STATUS_INVALID_PARAMETER;
    if (KeGetCurrentIrql() != PASSIVE_LEVEL) {
        LOG_WARN("UsbHubEnumAll requires PASSIVE_LEVEL (IRQL=%lu)",
                 (ULONG)KeGetCurrentIrql());
        return STATUS_INVALID_DEVICE_STATE;
    }

    RtlZeroMemory(List, sizeof(*List));

    st = IoGetDeviceInterfaces(&GUID_DEVINTERFACE_USB_HUB, NULL, 0, &links);
    if (!NT_SUCCESS(st) || links == NULL) {
        LOG_ERROR("IoGetDeviceInterfaces(USB_HUB) -> %08X", st);
        List->LastStatus = (ULONG)st;
        g_LastStatus = (LONG)st;
        return st;
    }

    /* links 是 MULTI_SZ：一串以 \0 结尾的字符串，最后以空串结束 */
    for (PWSTR p = links; *p != L'\0'; ) {
        UNICODE_STRING      name;
        PFILE_OBJECT        fo  = NULL;
        PDEVICE_OBJECT      hub = NULL;
        USB_NODE_INFORMATION ni;
        ULONG               ports = 0;

        RtlInitUnicodeString(&name, p);

        st = IoGetDeviceObjectPointer(&name, FILE_READ_DATA, &fo, &hub);
        if (NT_SUCCESS(st) && hub != NULL) {

            RtlZeroMemory(&ni, sizeof(ni));
            ni.NodeType = UsbHub;

            if (NT_SUCCESS(SendSyncIoctl(hub, IOCTL_USB_GET_NODE_INFORMATION,
                                         &ni, sizeof(ni), &ni, sizeof(ni), NULL)))
            {
                ports = ni.u.HubInformation.HubDescriptor.bNumberOfPorts;
                LOG_INFO("hub[%lu] ports=%lu", hubIndex, ports);
            } else {
                LOG_WARN("hub[%lu] GET_NODE_INFORMATION failed", hubIndex);
            }

            totalPorts += ports;

            for (ULONG port = 1; port <= ports; port++) {
                /*
                 * USB_DEV_FULL 现在约 3.2KB（8 个接口 × 报告描述符缓冲）。
                 * 不要放栈上 —— 内核栈只有 12KB，DriverEntry 的调用链
                 * DriverEntry -> UsbAuditInit -> RunFullEnum -> UsbHubEnumAll
                 * -> QueryPortDevice 叠加起来很容易爆栈。
                 */
                USB_DEV_FULL *d;

                if (devCount >= USB_FULL_MAX) break;

                d = (USB_DEV_FULL *)ExAllocatePoolWithTag(
                        NonPagedPool, sizeof(USB_DEV_FULL), 'bHsU');
                if (d == NULL) {
                    LOG_ERROR("alloc USB_DEV_FULL failed");
                    break;
                }

                RtlZeroMemory(d, sizeof(*d));

                if (QueryPortDevice(hub, hubIndex, port, d)) {
                    RtlStringCchCopyW(d->HubPath, 128, p);
                    List->Devs[devCount] = *d;
                    devCount++;
                }

                ExFreePoolWithTag(d, 'bHsU');
            }

            ObDereferenceObject(fo);    /* 同时释放 hub 引用 */
            hubIndex++;
        } else {
            LOG_DEBUG("open hub failed %08X", st);
        }

        /* 跳到 MULTI_SZ 里的下一个字符串 */
        while (*p) p++;
        p++;
    }

    ExFreePool(links);

    List->Count      = devCount;
    List->HubCount   = hubIndex;
    List->LastStatus = 0;

    /*
     * 缓存一份快照
     *
     * 不要放进自旋锁里：g_Last 约 32 × 3.2KB = 102KB，RtlZeroMemory
     * 加上 16KB 的拷贝在 DISPATCH_LEVEL 下持锁执行代价很大。
     * 缓存只在 PASSIVE_LEVEL 被读（IOCTL 处理路径），
     * 先把 Count 置 0、拷贝完再置真实值，读者看到的要么是空表
     * 要么是完整的新快照，不会读到半成品。
     */
    g_LastCount  = 0;
    RtlZeroMemory(g_Last, sizeof(g_Last));
    for (ULONG i = 0; i < devCount && i < USB_FULL_MAX; i++)
        g_Last[i] = List->Devs[i];
    g_LastStatus = 0;
    g_LastCount  = (LONG)devCount;

    LOG_INFO("=== USB hub enum done: %lu hub(s), %lu port(s), %lu device(s) ===",
             hubIndex, totalPorts, devCount);
    return STATUS_SUCCESS;
}

VOID UsbHubClear(VOID)
{
    KIRQL irql;
    KeAcquireSpinLock(&g_Lock, &irql);
    RtlZeroMemory(g_Last, sizeof(g_Last));
    g_LastCount = 0;
    KeReleaseSpinLock(&g_Lock, irql);
}

ULONG UsbHubLastStatus(VOID)
{
    return (ULONG)g_LastStatus;
}

/* 取缓存里的第 Index 个设备（UsbHubEnumAll 之后有效） */
USB_DEV_FULL *UsbHubGetCached(ULONG Index)
{
    if (Index >= (ULONG)g_LastCount || Index >= USB_FULL_MAX) return NULL;
    return &g_Last[Index];
}

ULONG UsbHubCachedCount(VOID)
{
    return (ULONG)g_LastCount;
}

/* ==================================================================
 * 原始控制传输
 * ==================================================================
 * 复用 USB_DESCRIPTOR_REQUEST 这个结构（hub 驱动按它解析），
 * 但 SetupPacket 全部由调用方决定，不再固定成 GET_DESCRIPTOR。
 */

/* 内部：对已打开的 hub 发控制传输 */
static NTSTATUS HubControlTransfer(PDEVICE_OBJECT HubDev, ULONG Port,
                                   UCHAR bmRequest, UCHAR bRequest,
                                   USHORT wValue, USHORT wIndex,
                                   PVOID Data, USHORT wLength, PULONG Returned)
{
    ULONG                   header = FIELD_OFFSET(USB_DESCRIPTOR_REQUEST, Data);
    ULONG                   total  = header + wLength;
    PUSB_DESCRIPTOR_REQUEST req;
    NTSTATUS                st;
    ULONG                   info = 0;

    if (Returned) *Returned = 0;
    if (Data == NULL || wLength == 0) return STATUS_INVALID_PARAMETER;

    req = (PUSB_DESCRIPTOR_REQUEST)ExAllocatePoolWithTag(NonPagedPool, total, 'bHsU');
    if (req == NULL) return STATUS_INSUFFICIENT_RESOURCES;

    RtlZeroMemory(req, total);
    req->ConnectionIndex       = Port;
    req->SetupPacket.bmRequest = bmRequest;
    req->SetupPacket.bRequest  = bRequest;
    req->SetupPacket.wValue    = wValue;
    req->SetupPacket.wIndex    = wIndex;
    req->SetupPacket.wLength   = wLength;

    if (bmRequest & 0x80) {
        /* IN：数据由设备返回 */
    } else {
        /* OUT：先把要发出去的数据放进 Data 区 */
        RtlCopyMemory(req->Data, Data, wLength);
    }

    st = SendSyncIoctl(HubDev, IOCTL_USB_GET_DESCRIPTOR_FROM_NODE_CONNECTION,
                       req, total, req, total, &info);

    if (NT_SUCCESS(st)) {
        RtlCopyMemory(Data, req->Data, wLength);
        if (Returned) *Returned = (info > wLength) ? wLength : info;
    }

    ExFreePoolWithTag(req, 'bHsU');
    return st;
}

NTSTATUS UsbHubRawControlToDevice(ULONG DevIndex,
                                  UCHAR bmRequest, UCHAR bRequest,
                                  USHORT wValue, USHORT wIndex,
                                  PVOID Data, USHORT wLength, PULONG Returned)
{
    USB_DEV_FULL   *dev;
    UNICODE_STRING  link;
    PFILE_OBJECT    fo  = NULL;
    PDEVICE_OBJECT  hub = NULL;
    NTSTATUS        st;

    if (Returned) *Returned = 0;
    if (KeGetCurrentIrql() != PASSIVE_LEVEL) return STATUS_INVALID_DEVICE_STATE;

    dev = UsbHubGetCached(DevIndex);
    if (dev == NULL) return STATUS_INVALID_PARAMETER;
    if (dev->HubPath[0] == 0) return STATUS_NOT_FOUND;

    RtlInitUnicodeString(&link, dev->HubPath);
    st = IoGetDeviceObjectPointer(&link, FILE_READ_DATA, &fo, &hub);
    if (!NT_SUCCESS(st) || hub == NULL) {
        LOG_WARN("RawControl: open hub failed %08X", st);
        return st;
    }

    st = HubControlTransfer(hub, dev->PortNumber, bmRequest, bRequest,
                            wValue, wIndex, Data, wLength, Returned);

    if (fo != NULL) ObDereferenceObject(fo);
    return st;
}

/* ==================================================================
 * 自测：判定 hub 是否允许任意 SetupPacket
 * ================================================================== */
typedef struct _CTRL_CASE {
    UCHAR  bmRequest;
    UCHAR  bRequest;
    USHORT wValue;
    USHORT wIndex;
    USHORT wLength;
    const char *Name;
} CTRL_CASE;

VOID UsbHubRawControlSelfTest(VOID)
{
    static const CTRL_CASE cases[] = {
        { 0x80, 6,  0x0100, 0, 18, "GET_DESCRIPTOR(Device)  [baseline]" },
        { 0x80, 0,  0x0000, 0,  2, "GET_STATUS(Device)" },
        { 0x80, 8,  0x0000, 0,  1, "GET_CONFIGURATION" },
        { 0x80, 10, 0x0000, 0,  2, "GET_INTERFACE" },
        { 0x80, 0xFF, 0x0000, 0, 8, "bRequest=0xFF (invalid)" },
    };

    ULONG target = 0xFFFFFFFF;
    UCHAR buf[64];
    ULONG got;

    if (KeGetCurrentIrql() != PASSIVE_LEVEL) return;
    if (UsbHubCachedCount() == 0) return;

    /* 找第一个「不是 hub」的设备（bDeviceClass != 9）作为测试目标 */
    for (ULONG i = 0; i < UsbHubCachedCount(); i++) {
        USB_DEV_FULL *d = UsbHubGetCached(i);
        if (d != NULL && d->DeviceClass != 0x09) { target = i; break; }
    }
    if (target == 0xFFFFFFFF) target = 0;

    {
        USB_DEV_FULL *d = UsbHubGetCached(target);
        LOG_INFO("=== RawControl self-test: dev[%lu] %04X:%04X port=%lu ===",
                 target, d->Vid, d->Pid, d->PortNumber);
    }

    for (ULONG i = 0; i < ARRAY_SIZE(cases); i++) {
        NTSTATUS st;

        RtlZeroMemory(buf, sizeof(buf));
        got = 0;

        st = UsbHubRawControlToDevice(target,
                                      cases[i].bmRequest, cases[i].bRequest,
                                      cases[i].wValue, cases[i].wIndex,
                                      buf, cases[i].wLength, &got);

        if (NT_SUCCESS(st)) {
            LOG_INFO("  [%u] %-38s -> OK  got=%lu  data=%02X %02X %02X %02X",
                     i, cases[i].Name, got, buf[0], buf[1], buf[2], buf[3]);
        } else {
            LOG_INFO("  [%u] %-38s -> FAIL %08X",
                     i, cases[i].Name, st);
        }
    }

    LOG_INFO("=== RawControl self-test done ===");
}

/* ==================================================================
 * 通用 URB 发送
 * ==================================================================
 * 两个参数都不能错（这是前面两次失败踩出来的）：
 *   1) IsInternalDeviceControl = TRUE  -> IRP_MJ_INTERNAL_DEVICE_CONTROL(15)
 *   2) Parameters.Others.Argument1 = Urb（不是 Type3InputBuffer）
 */
static NTSTATUS SendUrbToDevice(PDEVICE_OBJECT Dev, PVOID Urb, ULONG UrbLen)
{
    KEVENT          ev;
    IO_STATUS_BLOCK iosb;
    PIRP            irp;
    NTSTATUS        st;

    KeInitializeEvent(&ev, NotificationEvent, FALSE);
    RtlZeroMemory(&iosb, sizeof(iosb));

    irp = IoBuildDeviceIoControlRequest(IOCTL_INTERNAL_USB_SUBMIT_URB, Dev,
                                        Urb, UrbLen, NULL, 0,
                                        TRUE,       /* internal */
                                        &ev, &iosb);
    if (irp == NULL) return STATUS_INSUFFICIENT_RESOURCES;

    IoGetNextIrpStackLocation(irp)->Parameters.Others.Argument1 = Urb;
    irp->RequestorMode = KernelMode;

    st = IofCallDriver(Dev, irp);
    if (st == STATUS_PENDING) {
        KeWaitForSingleObject(&ev, Executive, KernelMode, FALSE, NULL);
        st = iosb.Status;
    }
    return st;
}

/* ==================================================================
 * 任意控制传输（fuzz / diff 的基础）
 * ==================================================================
 * DevIndex 是 GUID_DEVINTERFACE_USB_DEVICE 枚举列表里的下标。
 * SetupPacket 全部由调用方指定 —— 这就是 libusb_control_transfer 的内核等价物。
 *
 * ★ PASSIVE_LEVEL
 */
NTSTATUS UsbUrbControlTransfer(ULONG DevIndex,
                               UCHAR bmRequest, UCHAR bRequest,
                               USHORT wValue, USHORT wIndex,
                               PVOID Data, USHORT BufLen, USHORT DeclaredLen,
                               PULONG Got)
{
    PWSTR    links = NULL;
    NTSTATUS st;
    ULONG    idx = 0;
    BOOLEAN  done = FALSE;
    USHORT   declared;

    if (Got) *Got = 0;
    /* BufLen==0 合法：无数据阶段的控制传输（fuzz 会把 wLength=0 当探测点） */
    if (BufLen != 0 && Data == NULL) return STATUS_INVALID_PARAMETER;
    if (KeGetCurrentIrql() != PASSIVE_LEVEL) return STATUS_INVALID_DEVICE_STATE;

    declared = DeclaredLen ? DeclaredLen : BufLen;

    st = IoGetDeviceInterfaces(&GUID_DEVINTERFACE_USB_DEVICE, NULL, 0, &links);
    if (!NT_SUCCESS(st) || links == NULL) return st;

    for (PWSTR p = links; *p != L'\0' && !done; ) {
        UNICODE_STRING name;
        PFILE_OBJECT   fo  = NULL;
        PDEVICE_OBJECT dev = NULL;

        if (idx == DevIndex) {
            struct _URB_CONTROL_TRANSFER *urb;
            ULONG    urbLen = sizeof(struct _URB_CONTROL_TRANSFER);

            RtlInitUnicodeString(&name, p);
            st = IoGetDeviceObjectPointer(&name, FILE_READ_DATA, &fo, &dev);
            if (NT_SUCCESS(st) && dev != NULL) {
                urb = (void *)ExAllocatePoolWithTag(NonPagedPool, urbLen, 'bHsU');
                if (urb != NULL) {
                    RtlZeroMemory(urb, urbLen);

                    urb->Hdr.Length           = (USHORT)urbLen;
                    urb->Hdr.Function         = URB_FUNCTION_CONTROL_TRANSFER;

                    /*
                     * ★ TransferFlags 必须带 USBD_DEFAULT_PIPE_TRANSFER(0x08)
                     *
                     * _URB_CONTROL_TRANSFER 里有个 PipeHandle 字段，它是
                     * 「功能驱动」通过 URB_FUNCTION_SELECT_CONFIGURATION 拿到的
                     * 端点半句柄。我们不是 hidusb 的功能驱动，拿不到这个句柄，
                     * PipeHandle 只能是 0 —— hub 会把 0 当无效管道直接拒掉。
                     *
                     * USBD_DEFAULT_PIPE_TRANSFER 告诉 hub：本次传输走默认管道
                     * （端点 0），忽略 PipeHandle。这正是 libusb/WinUSB 做
                     * control transfer 时用的方式。
                     */
                    urb->TransferFlags = ((bmRequest & 0x80)
                                          ? USBD_TRANSFER_DIRECTION_IN
                                          : USBD_TRANSFER_DIRECTION_OUT)
                                       | USBD_DEFAULT_PIPE_TRANSFER
                                       | USBD_SHORT_TRANSFER_OK;

                    urb->TransferBufferLength = BufLen;
                    urb->TransferBuffer       = Data;
                    urb->TransferBufferMDL    = NULL;
                    urb->UrbLink              = NULL;
                    urb->SetupPacket[0]       = bmRequest;
                    urb->SetupPacket[1]       = bRequest;
                    urb->SetupPacket[2]       = (UCHAR)(wValue & 0xFF);
                    urb->SetupPacket[3]       = (UCHAR)(wValue >> 8);
                    urb->SetupPacket[4]       = (UCHAR)(wIndex & 0xFF);
                    urb->SetupPacket[5]       = (UCHAR)(wIndex >> 8);
                    urb->SetupPacket[6]       = (UCHAR)(declared & 0xFF);
                    urb->SetupPacket[7]       = (UCHAR)(declared >> 8);

                    st = SendUrbToDevice(dev, urb, urbLen);

                    if (NT_SUCCESS(st) && urb->Hdr.Status == USBD_STATUS_SUCCESS) {
                        if (Got) *Got = urb->TransferBufferLength;
                    } else if (NT_SUCCESS(st)) {
                        st = STATUS_UNSUCCESSFUL;   /* 用 URB 状态覆盖 */
                    }

                    ExFreePoolWithTag(urb, 'bHsU');
                } else {
                    st = STATUS_INSUFFICIENT_RESOURCES;
                }

                ObDereferenceObject(fo);
            }
            done = TRUE;
        }

        idx++;
        while (*p) p++;
        p++;
    }

    ExFreePool(links);
    return done ? st : STATUS_INVALID_PARAMETER;
}

/* ==================================================================
 * 自测 2：尝试通过 USB 设备接口发 URB
 * ==================================================================
 * 路径：IoGetDeviceInterfaces(GUID_DEVINTERFACE_USB_DEVICE)
 *       -> IoGetDeviceObjectPointer
 *       -> IOCTL_INTERNAL_USB_SUBMIT_URB (0x220003, METHOD_NEITHER)
 *       -> URB_FUNCTION_GET_DESCRIPTOR_FROM_DEVICE
 *
 * 为什么先测 GET_DESCRIPTOR：即使失败也无副作用（只读），能安全
 * 验证通道是否可用。通了就换成 URB_FUNCTION_CONTROL_TRANSFER，
 * 任意 SetupPacket 都能发，fuzz/diff 全部可做。
 */
VOID UsbHubUrbProbeSelfTest(VOID)
{
    PWSTR    links = NULL;
    NTSTATUS st;
    ULONG    opened = 0, ok = 0;

    if (KeGetCurrentIrql() != PASSIVE_LEVEL) return;

    st = IoGetDeviceInterfaces(&GUID_DEVINTERFACE_USB_DEVICE, NULL, 0, &links);
    if (!NT_SUCCESS(st) || links == NULL) {
        LOG_WARN("URB probe: IoGetDeviceInterfaces(USB_DEVICE) -> %08X", st);
        return;
    }

    LOG_INFO("=== URB probe via GUID_DEVINTERFACE_USB_DEVICE ===");

    for (PWSTR p = links; *p != L'\0' && opened < 3; ) {
        UNICODE_STRING name;
        PFILE_OBJECT   fo  = NULL;
        PDEVICE_OBJECT dev = NULL;

        RtlInitUnicodeString(&name, p);

        st = IoGetDeviceObjectPointer(&name, FILE_READ_DATA, &fo, &dev);
        if (NT_SUCCESS(st) && dev != NULL) {
            struct _URB_CONTROL_DESCRIPTOR_REQUEST *urb;
            UCHAR           buf[64];
            KEVENT          ev;
            IO_STATUS_BLOCK iosb;
            PIRP            irp;
            ULONG           urbLen = sizeof(struct _URB_CONTROL_DESCRIPTOR_REQUEST);
            NTSTATUS        ust;

            opened++;

            urb = (void *)ExAllocatePoolWithTag(NonPagedPool, urbLen, 'bHsU');
            if (urb != NULL) {
                RtlZeroMemory(urb, urbLen);
                RtlZeroMemory(buf, sizeof(buf));
                KeInitializeEvent(&ev, NotificationEvent, FALSE);
                RtlZeroMemory(&iosb, sizeof(iosb));

                urb->Hdr.Length           = (USHORT)urbLen;
                urb->Hdr.Function         = URB_FUNCTION_GET_DESCRIPTOR_FROM_DEVICE;
                urb->TransferBufferLength = sizeof(buf);
                urb->TransferBuffer       = buf;
                urb->DescriptorType       = USB_DEVICE_DESCRIPTOR_TYPE;
                urb->Index                = 0;
                urb->LanguageId           = 0;

                /*
                 * ★ 这两个参数都不能错
                 *
                 * 1) IsInternalDeviceControl 必须是 TRUE
                 *    IOCTL_INTERNAL_USB_SUBMIT_URB 走的是
                 *    IRP_MJ_INTERNAL_DEVICE_CONTROL(15)，不是 14。
                 *    传 FALSE 的话 IRP 主功能号不对，
                 *    hub 的 URB 分支根本进不去 -> C00000BB。
                 *
                 * 2) URB 指针要放到 Parameters.Others.Argument1（偏移 0）
                 *    而 IoBuildDeviceIoControlRequest 对 METHOD_NEITHER
                 *    放的是 Type3InputBuffer（偏移 0x18），两者不重叠。
                 */
                irp = IoBuildDeviceIoControlRequest(IOCTL_INTERNAL_USB_SUBMIT_URB,
                                                    dev, urb, urbLen,
                                                    NULL, 0,
                                                    TRUE,       /* internal */
                                                    &ev, &iosb);
                if (irp != NULL) {
                    PIO_STACK_LOCATION next = IoGetNextIrpStackLocation(irp);

                    next->Parameters.Others.Argument1 = urb;

                    irp->RequestorMode = KernelMode;

                    ust = IofCallDriver(dev, irp);
                    if (ust == STATUS_PENDING) {
                        KeWaitForSingleObject(&ev, Executive, KernelMode, FALSE, NULL);
                        ust = iosb.Status;
                    }

                    /* URB 自己还有一层状态 */
                    if (NT_SUCCESS(ust) && urb->Hdr.Status == USBD_STATUS_SUCCESS &&
                        urb->TransferBufferLength > 0)
                    {
                        ok++;
                        LOG_INFO("  [%lu] URB GET_DESCRIPTOR -> OK  len=%lu  "
                                 "bcdUSB=%02X%02X VID=%02X%02X PID=%02X%02X",
                                 opened - 1, (ULONG)urb->TransferBufferLength,
                                 buf[3], buf[2], buf[9], buf[8], buf[11], buf[10]);
                    } else {
                        LOG_INFO("  [%lu] URB GET_DESCRIPTOR -> FAIL irp=%08X "
                                 "urb=0x%08X len=%lu",
                                 opened - 1, ust, urb->Hdr.Status,
                                 (ULONG)urb->TransferBufferLength);
                    }
                } else {
                    LOG_INFO("  [%lu] IoBuildDeviceIoControlRequest failed", opened - 1);
                }

                ExFreePoolWithTag(urb, 'bHsU');
            }

            ObDereferenceObject(fo);
        }

        while (*p) p++;
        p++;
    }

    ExFreePool(links);
    LOG_INFO("=== URB probe done: %lu opened, %lu ok ===", opened, ok);
}

/* ==================================================================
 * 自测 3：用 URB_FUNCTION_CONTROL_TRANSFER 发任意 SetupPacket
 * ==================================================================
 * 这一段通过就说明 fuzz / diff 能力解锁 —— libusb_control_transfer
 * 能干的事，我们驱动也能干，而且不需要设备绑定 libusbK。
 */
VOID UsbUrbControlSelfTest(VOID)
{
    static const CTRL_CASE cases[] = {
        { 0x80, 6,  0x0100, 0, 18, "GET_DESCRIPTOR(Device)  [baseline]" },
        { 0x80, 0,  0x0000, 0,  2, "GET_STATUS(Device)" },
        { 0x80, 8,  0x0000, 0,  1, "GET_CONFIGURATION" },
        { 0x80, 10, 0x0000, 0,  1, "GET_INTERFACE" },
        { 0x80, 0x22, 0x0000, 0, 8, "bRequest=0x22 (custom)" },
    };

    ULONG probeIdx = 0xFFFFFFFF;
    UCHAR buf[64];
    ULONG got;
    NTSTATUS st;

    if (KeGetCurrentIrql() != PASSIVE_LEVEL) return;

    /* 先把编译器实际用的结构布局打出来，和 USB 规范对照 */
    LOG_INFO("URB_CONTROL_TRANSFER layout: sizeof=%lu "
             "Hdr=%lu TransferBuffer=%lu Length=%lu Flags=%lu "
             "UrbLink=%lu hca=%lu SetupPacket=%lu",
             (ULONG)sizeof(struct _URB_CONTROL_TRANSFER),
             (ULONG)FIELD_OFFSET(struct _URB_CONTROL_TRANSFER, Hdr),
             (ULONG)FIELD_OFFSET(struct _URB_CONTROL_TRANSFER, TransferBuffer),
             (ULONG)FIELD_OFFSET(struct _URB_CONTROL_TRANSFER, TransferBufferLength),
             (ULONG)FIELD_OFFSET(struct _URB_CONTROL_TRANSFER, TransferFlags),
             (ULONG)FIELD_OFFSET(struct _URB_CONTROL_TRANSFER, UrbLink),
             (ULONG)FIELD_OFFSET(struct _URB_CONTROL_TRANSFER, hca),
             (ULONG)FIELD_OFFSET(struct _URB_CONTROL_TRANSFER, SetupPacket));

    /* 逐个下标试，把每次结果都打出来 */
    for (ULONG i = 0; i < 4; i++) {
        RtlZeroMemory(buf, sizeof(buf));
        got = 0;
        st = UsbUrbControlTransfer(i, 0x80, 6, 0x0100, 0, buf, 18, 18, &got);
        LOG_INFO("  probe iface[%lu]: st=%08X got=%lu", i, st, got);
        if (NT_SUCCESS(st) && got >= 18) { probeIdx = i; break; }
    }

    if (probeIdx == 0xFFFFFFFF) {
        LOG_WARN("=== URB control self-test: no usable USB interface ===");
        return;
    }

    LOG_INFO("=== URB CONTROL_TRANSFER self-test: iface[%lu] ===", probeIdx);

    for (ULONG i = 0; i < ARRAY_SIZE(cases); i++) {
        RtlZeroMemory(buf, sizeof(buf));
        got = 0;

        st = UsbUrbControlTransfer(probeIdx,
                                   cases[i].bmRequest, cases[i].bRequest,
                                   cases[i].wValue, cases[i].wIndex,
                                   buf, cases[i].wLength, cases[i].wLength, &got);

        if (NT_SUCCESS(st)) {
            LOG_INFO("  [%u] %-38s -> OK  got=%lu  data=%02X %02X %02X %02X",
                     i, cases[i].Name, got, buf[0], buf[1], buf[2], buf[3]);
        } else {
            LOG_INFO("  [%u] %-38s -> FAIL %08X", i, cases[i].Name, st);
        }
    }

    LOG_INFO("=== URB control self-test done ===");
}

/* ==================================================================
 * kmbox 形态判定
 * ==================================================================
 * 正常鼠标/键盘：只有 HID(3) 接口。
 * kmbox 类硬件桥接器：HID(3) 与 CDC(2 / 0x0A) 或 厂商自定义(0xFF) 并存。
 */
BOOLEAN UsbHubIsSuspiciousComposite(const USB_DEV_FULL *Dev)
{
    BOOLEAN hasHid = FALSE, hasOther = FALSE;

    if (Dev == NULL) return FALSE;

    for (ULONG i = 0; i < Dev->IfaceCount && i < ARRAY_SIZE(Dev->Ifaces); i++) {
        UCHAR c = Dev->Ifaces[i].Class;

        if (c == 0x03)                                   hasHid = TRUE;
        else if (c == 0x02 || c == 0x0A || c == 0xFF)    hasOther = TRUE;
    }

    return (hasHid && hasOther);
}
