/*
 * usb_node.c - HID 设备节点表 + KMBox 判定
 *
 * 只保留在目标 VM（Windows 10 18362 x64）上实测跑通的能力。
 * 所有结构偏移都用 WinDbg `dt nt!_XXX` 核对过，见 usb_node.h 注释。
 *
 * 对应 ACE-CORE.sys3 的实现：
 *   BuildHidDeviceNode       (0x14008E2C0)  枚举 + 读 InstancePath
 *   GetHidNodeTable          (qword_1459C21F0)  10 槽表
 *   AntiHw_FindSuspiciousKmbox(0x14000F0C0)  祖父设备比较
 */
#include "module.h"
#include "usb_node.h"
#include <ntstrsafe.h>

/* ==================================================================
 * 安全的内存访问
 * ================================================================== */

static PVOID SafeReadPtr(const void *Base, ULONG Offset)
{
    PVOID v;
    if (Base == NULL) return NULL;
    if (!MmIsAddressValid((PVOID)((PUCHAR)Base + Offset))) return NULL;
    v = *(PVOID *)((PUCHAR)Base + Offset);
    if (v == NULL || !MmIsAddressValid(v)) return NULL;
    return v;
}

PVOID UsbGetDeviceNode(PDEVICE_OBJECT DevObj)
{
    PVOID ext;

    if (DevObj == NULL || !MmIsAddressValid(DevObj)) return NULL;

    /* DeviceObject->DeviceObjectExtension (+0x138) */
    ext = SafeReadPtr(DevObj, ACE_OFF_DEVOBJ_EXTENSION);
    if (ext == NULL) return NULL;

    /* _DEVOBJ_EXTENSION->DeviceNode (+0x28) */
    return SafeReadPtr(ext, ACE_OFF_DEVEXT_DEVNODE);
}

PVOID UsbGetGrandParentNode(PDEVICE_OBJECT DevObj)
{
    PVOID node = UsbGetDeviceNode(DevObj);
    PVOID parent, gp;

    if (node == NULL) return NULL;

    parent = SafeReadPtr(node, ACE_OFF_DEVNODE_PARENT);
    if (parent == NULL) return NULL;

    /* 祖父为空就退回父节点：单层设备的父子关系同样可用 */
    gp = SafeReadPtr(parent, ACE_OFF_DEVNODE_PARENT);
    return (gp != NULL) ? gp : parent;
}

NTSTATUS UsbGetInstancePath(PDEVICE_OBJECT DevObj, PUNICODE_STRING *Path)
{
    PVOID           node;
    UNICODE_STRING *us;

    *Path = NULL;

    node = UsbGetDeviceNode(DevObj);
    if (node == NULL) return STATUS_NOT_FOUND;

    if (!MmIsAddressValid((PVOID)((PUCHAR)node + ACE_OFF_DEVNODE_INSTPATH)))
        return STATUS_INVALID_ADDRESS;

    us = (UNICODE_STRING *)((PUCHAR)node + ACE_OFF_DEVNODE_INSTPATH);

    if (us->Buffer == NULL || us->Length == 0 || (us->Length & 1))
        return STATUS_NOT_FOUND;
    if (!MmIsAddressValid(us->Buffer))
        return STATUS_INVALID_ADDRESS;
    if (us->Length > HID_PATH_MAX * sizeof(WCHAR))
        return STATUS_INVALID_BUFFER_SIZE;

    *Path = us;
    return STATUS_SUCCESS;
}

/* ==================================================================
 * InstancePath 解析：USB\VID_046D&PID_C077\...
 * ================================================================== */

static BOOLEAN HexChar(WCHAR c, PUSHORT v)
{
    if (c >= L'0' && c <= L'9') { *v = (USHORT)(c - L'0');      return TRUE; }
    if (c >= L'a' && c <= L'f') { *v = (USHORT)(c - L'a' + 10); return TRUE; }
    if (c >= L'A' && c <= L'F') { *v = (USHORT)(c - L'A' + 10); return TRUE; }
    return FALSE;
}

BOOLEAN UsbParseVidPid(PCUNICODE_STRING Path, PUSHORT Vid, PUSHORT Pid)
{
    USHORT  i, n;
    USHORT  vidVal = 0, pidVal = 0;
    BOOLEAN haveVid = FALSE, havePid = FALSE;

    *Vid = *Pid = 0;
    if (Path == NULL || Path->Buffer == NULL) return FALSE;

    n = Path->Length / sizeof(WCHAR);

    for (i = 0; i + 8 <= n; i++) {
        const WCHAR *p = Path->Buffer + i;

        if ((p[0]==L'V'||p[0]==L'v') && (p[1]==L'I'||p[1]==L'i') &&
            (p[2]==L'D'||p[2]==L'd') && p[3]==L'_')
        {
            USHORT a,b,c,d;
            if (HexChar(p[4],&a) && HexChar(p[5],&b) &&
                HexChar(p[6],&c) && HexChar(p[7],&d))
            {
                vidVal = (USHORT)((a<<12)|(b<<8)|(c<<4)|d);
                haveVid = TRUE;
            }
        }
        if ((p[0]==L'P'||p[0]==L'p') && (p[1]==L'I'||p[1]==L'i') &&
            (p[2]==L'D'||p[2]==L'd') && p[3]==L'_')
        {
            USHORT a,b,c,d;
            if (HexChar(p[4],&a) && HexChar(p[5],&b) &&
                HexChar(p[6],&c) && HexChar(p[7],&d))
            {
                pidVal = (USHORT)((a<<12)|(b<<8)|(c<<4)|d);
                havePid = TRUE;
            }
        }
    }

    if (!haveVid && !havePid) return FALSE;

    *Vid = vidVal;
    *Pid = pidVal;
    return TRUE;
}

/* ==================================================================
 * 分类：读 hidclass FDO 的 DeviceType
 * ==================================================================
 *
 * WinDbg 实测（18362）：
 *   B574D3F0 --+0x18--> b5720c40  DeviceType = 0x0F  (FILE_DEVICE_MOUSE)
 *   B7260060 --+0x18--> b72558e0  DeviceType = 0x0F  (FILE_DEVICE_MOUSE)
 *   B73DF110 --+0x18--> 0         没挂 hidclass，不是 HID 设备
 *
 * hidclass 自己就是靠这个字段决定往 mouclass 还是 kbdclass 上报，
 * 我们读同一个字段，结论和系统一致。一次内存读，任意 IRQL 安全。
 */
static UCHAR ClassifyByAttachedDevice(PDEVICE_OBJECT HidUsbFdo, PUCHAR RawType)
{
    PDEVICE_OBJECT upper;
    UCHAR          t = 0;

    if (RawType) *RawType = 0;
    if (HidUsbFdo == NULL || !MmIsAddressValid(HidUsbFdo)) return HID_TYPE_NONE;

    upper = (PDEVICE_OBJECT)SafeReadPtr(HidUsbFdo, ACE_OFF_DEVOBJ_ATTACHED);
    if (upper == NULL) return HID_TYPE_NONE;

    if (!MmIsAddressValid((PUCHAR)upper + ACE_OFF_DEVOBJ_DEVTYPE))
        return HID_TYPE_NONE;

    t = *(UCHAR *)((PUCHAR)upper + ACE_OFF_DEVOBJ_DEVTYPE);
    if (RawType) *RawType = t;

    if (t == ACE_DEVICE_TYPE_MOUSE)    return HID_TYPE_MOUSE;
    if (t == ACE_DEVICE_TYPE_KEYBOARD) return HID_TYPE_KEYBOARD;
    return HID_TYPE_NONE;
}

/* 前向声明（UsbNodeAddEx 里用） */
static UCHAR ClassifyByAttachedDevice(_In_ PDEVICE_OBJECT HidUsbFdo, _Out_opt_ PUCHAR RawType);

/* ==================================================================
 * 节点表
 * ================================================================== */

static KSPIN_LOCK     g_NodeLock;
static HID_NODE       g_Nodes[HID_NODE_MAX];
static volatile LONG  g_NodeCount = 0;

NTSTATUS UsbNodeInit(VOID)
{
    KeInitializeSpinLock(&g_NodeLock);
    RtlZeroMemory(g_Nodes, sizeof(g_Nodes));
    g_NodeCount = 0;
    return STATUS_SUCCESS;
}

VOID UsbNodeUnload(VOID)
{
    KIRQL irql;
    KeAcquireSpinLock(&g_NodeLock, &irql);
    RtlZeroMemory(g_Nodes, sizeof(g_Nodes));
    g_NodeCount = 0;
    KeReleaseSpinLock(&g_NodeLock, irql);
}

ULONG UsbNodeCount(VOID)
{
    ULONG n = 0;
    KIRQL irql;
    KeAcquireSpinLock(&g_NodeLock, &irql);
    for (ULONG i = 0; i < HID_NODE_MAX; i++)
        if (g_Nodes[i].Active) n++;
    KeReleaseSpinLock(&g_NodeLock, irql);
    return n;
}

PHID_NODE UsbNodeGet(ULONG Index)
{
    if (Index >= HID_NODE_MAX) return NULL;
    return g_Nodes[Index].Active ? &g_Nodes[Index] : NULL;
}

PHID_NODE UsbNodeFindByDevice(PDEVICE_OBJECT DevObj)
{
    if (DevObj == NULL) return NULL;
    for (ULONG i = 0; i < HID_NODE_MAX; i++)
        if (g_Nodes[i].Active && g_Nodes[i].DeviceObject == DevObj)
            return &g_Nodes[i];
    return NULL;
}

/*
 * UsbNodeAddEx - 建档
 *
 * 键用 DevObj（URB 完成例程拿到的对象），路径/祖父用 PathSource
 * （设备栈底部那层才有意义的 InstancePath 和父子链）。
 *
 * 全程只做内存读 + 自旋锁，任何 IRQL 都能调用。
 */
NTSTATUS UsbNodeAddEx(PDEVICE_OBJECT DevObj, PDEVICE_OBJECT PathSource, UCHAR DeviceType)
{
    UNICODE_STRING *path = NULL;
    USHORT          vid = 0, pid = 0;
    PVOID           node, gp;
    NTSTATUS        st;
    ULONG           slot;
    KIRQL           irql;
    UCHAR           rawType = 0;

    if (DevObj == NULL) return STATUS_INVALID_PARAMETER;
    if (PathSource == NULL) PathSource = DevObj;

    /* 查重 */
    for (ULONG i = 0; i < HID_NODE_MAX; i++) {
        if (g_Nodes[i].Active && g_Nodes[i].DeviceObject == DevObj) {
            if (DeviceType != HID_TYPE_NONE && g_Nodes[i].DeviceType == HID_TYPE_NONE)
                g_Nodes[i].DeviceType = DeviceType;
            return STATUS_SUCCESS;
        }
    }

    node = UsbGetDeviceNode(PathSource);
    gp   = UsbGetGrandParentNode(PathSource);
    st   = UsbGetInstancePath(PathSource, &path);
    if (NT_SUCCESS(st) && path != NULL)
        UsbParseVidPid(path, &vid, &pid);

    /* 分类（WinDbg 验证过的 DeviceType 快路） */
    if (DeviceType == HID_TYPE_NONE)
        DeviceType = ClassifyByAttachedDevice(DevObj, &rawType);

    /* CAS 抢槽 */
    for (slot = 0; slot < HID_NODE_MAX; slot++) {
        if (InterlockedCompareExchange(&g_Nodes[slot].Active, 1, 0) == 0)
            break;
    }
    if (slot >= HID_NODE_MAX) {
        LOG_WARN("node table full (%d), skip dev=%p", HID_NODE_MAX, DevObj);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    KeAcquireSpinLock(&g_NodeLock, &irql);

    RtlZeroMemory(&g_Nodes[slot], sizeof(HID_NODE));
    g_Nodes[slot].Active       = 1;
    g_Nodes[slot].Slot         = slot;
    g_Nodes[slot].DeviceObject = DevObj;
    g_Nodes[slot].DeviceNode   = node;
    g_Nodes[slot].ParentNode   = (node != NULL)
                                 ? SafeReadPtr(node, ACE_OFF_DEVNODE_PARENT) : NULL;
    g_Nodes[slot].GrandParent  = gp;
    g_Nodes[slot].Vid          = vid;
    g_Nodes[slot].Pid          = pid;
    g_Nodes[slot].DeviceType   = DeviceType;
    g_Nodes[slot].SourceType   = rawType;

    if (NT_SUCCESS(st) && path != NULL) {
        ULONG chars = path->Length / sizeof(WCHAR);
        if (chars >= HID_PATH_MAX) chars = HID_PATH_MAX - 1;
        RtlStringCchCopyNW(g_Nodes[slot].InstancePath, HID_PATH_MAX,
                           path->Buffer, chars);
        RtlStringCchCopyW(g_Nodes[slot].ProductName, HID_NAME_MAX,
                          g_Nodes[slot].InstancePath);
    }

    InterlockedIncrement(&g_NodeCount);

    KeReleaseSpinLock(&g_NodeLock, irql);

    LOG_INFO("node[%lu] add dev=%p src=%p type=%u(dt=0x%02X) VID=%04X PID=%04X "
             "gp=%p parent=%p path=%ws",
             slot, DevObj, PathSource, DeviceType, rawType, vid, pid,
             gp, g_Nodes[slot].ParentNode,
             g_Nodes[slot].InstancePath[0] ? g_Nodes[slot].InstancePath : L"<none>");

    return STATUS_SUCCESS;
}

NTSTATUS UsbNodeAdd(PDEVICE_OBJECT DevObj, UCHAR DeviceType)
{
    return UsbNodeAddEx(DevObj, DevObj, DeviceType);
}

VOID UsbNodeRemove(PDEVICE_OBJECT DevObj)
{
    KIRQL irql;

    if (DevObj == NULL) return;

    KeAcquireSpinLock(&g_NodeLock, &irql);
    for (ULONG i = 0; i < HID_NODE_MAX; i++) {
        if (g_Nodes[i].Active && g_Nodes[i].DeviceObject == DevObj) {
            LOG_INFO("node[%lu] remove dev=%p VID=%04X PID=%04X",
                     i, DevObj, g_Nodes[i].Vid, g_Nodes[i].Pid);
            RtlZeroMemory(&g_Nodes[i], sizeof(HID_NODE));
            if (g_NodeCount > 0) InterlockedDecrement(&g_NodeCount);
            break;
        }
    }
    KeReleaseSpinLock(&g_NodeLock, irql);
}

/*
 * UsbNodeClassifyAll - 给还是 UNKNOWN 的节点补分类
 *
 * 只走 DeviceType 快路。拿不到报告描述符的能力已经移除（见头文件）。
 */
NTSTATUS UsbNodeClassifyAll(VOID)
{
    ULONG done = 0;

    for (ULONG i = 0; i < HID_NODE_MAX; i++) {
        UCHAR t, raw = 0;

        if (!g_Nodes[i].Active) continue;
        if (g_Nodes[i].DeviceType != HID_TYPE_NONE) continue;
        if (g_Nodes[i].DeviceObject == NULL) continue;

        t = ClassifyByAttachedDevice(g_Nodes[i].DeviceObject, &raw);
        g_Nodes[i].SourceType = raw;

        if (t != HID_TYPE_NONE) {
            g_Nodes[i].DeviceType = t;
            done++;
            LOG_INFO("slot %lu -> %s (DeviceType=0x%02X) VID=%04X PID=%04X",
                     i, t == HID_TYPE_MOUSE ? "MOUSE" : "KEYBOARD", raw,
                     g_Nodes[i].Vid, g_Nodes[i].Pid);
        } else {
            /* dt=0 表示这个设备上层没挂 hidclass，不是 HID 输入设备 */
            LOG_DEBUG("slot %lu unclassified (DeviceType=0x%02X) VID=%04X PID=%04X",
                      i, raw, g_Nodes[i].Vid, g_Nodes[i].Pid);
        }
    }

    LOG_INFO("ClassifyAll: %lu classified (of %lu node(s))", done, UsbNodeCount());
    return STATUS_SUCCESS;
}

/* ==================================================================
 * ★ KMBox 判定
 * ==================================================================
 *
 * 复刻 ACE AntiHw_FindSuspiciousKmbox：
 *   鼠标节点和键盘节点共享同一个祖父 _DEVICE_NODE -> 命中
 *
 * 原理：KMBox 是单设备双接口（一个 USB 复合设备同时暴露 HID 鼠标
 * 和 HID 键盘），两者父/祖父必然相同；真实玩家的鼠标键盘是两个
 * 物理独立设备，祖父不同。
 */
NTSTATUS UsbNodeDetectKmbox(KMBOX_RESULT *Results, ULONG MaxResults, PULONG HitCount)
{
    ULONG hits = 0;
    KIRQL irql;

    if (HitCount == NULL) return STATUS_INVALID_PARAMETER;
    *HitCount = 0;
    if (Results == NULL || MaxResults == 0) return STATUS_INVALID_PARAMETER;

    KeAcquireSpinLock(&g_NodeLock, &irql);

    for (ULONG mi = 0; mi < HID_NODE_MAX && hits < MaxResults; mi++) {
        PHID_NODE m = &g_Nodes[mi];

        if (!m->Active || m->DeviceType != HID_TYPE_MOUSE) continue;
        if (m->GrandParent == NULL) continue;

        for (ULONG ki = 0; ki < HID_NODE_MAX && hits < MaxResults; ki++) {
            PHID_NODE k = &g_Nodes[ki];

            if (!k->Active || k->DeviceType != HID_TYPE_KEYBOARD) continue;
            if (k->GrandParent == NULL) continue;

            /* ★ 唯一判据 */
            if (m->GrandParent != k->GrandParent) continue;

            RtlZeroMemory(&Results[hits], sizeof(KMBOX_RESULT));
            Results[hits].MouseVid = m->Vid;
            Results[hits].MousePid = m->Pid;
            Results[hits].KbdVid   = k->Vid;
            Results[hits].KbdPid   = k->Pid;
            RtlStringCchCopyW(Results[hits].MouseProduct, HID_NAME_MAX, m->ProductName);
            RtlStringCchCopyW(Results[hits].KbdProduct,   HID_NAME_MAX, k->ProductName);
            RtlStringCchCopyW(Results[hits].SharedPath,   HID_PATH_MAX, m->InstancePath);

            LOG_WARN("KMBOX HIT: mouse %04X:%04X + kbd %04X:%04X share gp=%p",
                     m->Vid, m->Pid, k->Vid, k->Pid, m->GrandParent);

            hits++;
            break;      /* 一个鼠标只记一次 */
        }
    }

    KeReleaseSpinLock(&g_NodeLock, irql);

    *HitCount = hits;
    return STATUS_SUCCESS;
}

/* ==================================================================
 * 输入报告计数（只计数，位域抽取等有报告描述符后再做）
 * ==================================================================
 * ★ 在 URB 完成例程里调用，可能 DISPATCH_LEVEL —— 只做内存操作。
 */
VOID UsbNodeNoteReport(PDEVICE_OBJECT DevObj, const UCHAR *Data, ULONG Len)
{
    PHID_NODE node;

    if (DevObj == NULL || Data == NULL || Len == 0) return;
    if (KeGetCurrentIrql() > DISPATCH_LEVEL) return;

    node = UsbNodeFindByDevice(DevObj);
    if (node == NULL) return;

    InterlockedIncrement(&node->ReportCount);
}

ULONG UsbNodeReportCount(PDEVICE_OBJECT DevObj)
{
    PHID_NODE node = UsbNodeFindByDevice(DevObj);
    return node ? (ULONG)node->ReportCount : 0;
}
