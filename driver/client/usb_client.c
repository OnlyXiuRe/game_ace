/*
 * usb_client.c - 3 环客户端: 通过 \\.\game_base 枚举设备 / 发探针 / 封锁
 *
 * 驱动侧 (game_base.sys) hook 了 usbhub3.sys 的 IRP_MJ_PNP,
 * 在设备 START/REMOVE 时记录设备, 并通过控制设备暴露给这里。
 *
 * 用法:
 *   usb_client list                列出驱动收集到的 USB 设备
 *   usb_client scan [--n N]        逐个设备采集固件指纹
 *   usb_client scan --verbose      带转录预览
 *   usb_client block <index>       封锁指定设备
 *   usb_client unblock <index>     解封
 *
 * 编译 (MSVC): cl /O2 /utf-8 usb_client.c setupapi.lib
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include "log_types.h"
#include "game_base.h"
#include "../../sha256.h"

#define DEFAULT_PROBES 512

/* ================= 转录缓冲 ================= */
static char  *g_buf = NULL;
static size_t g_len = 0, g_cap = 0;
static int    g_mem = 0;

static void emit_raw(const char *line) {
    size_t n = strlen(line);
    if (!g_mem) { fwrite(line, 1, n, stdout); return; }
    if (g_len + n + 1 > g_cap) {
        while (g_len + n + 1 > g_cap) g_cap = g_cap ? g_cap * 2 : 1048576;
        g_buf = (char *)realloc(g_buf, g_cap);
    }
    memcpy(g_buf + g_len, line, n);
    g_len += n;
    g_buf[g_len] = 0;
}

static void emit_hex(const char *tag, const unsigned char *d, int n) {
    static char line[8192];
    int p = sprintf(line, "%s\thex\t", tag);
    for (int i = 0; i < n && p < (int)sizeof(line) - 4; i++)
        p += sprintf(line + p, "%02x", d[i]);
    sprintf(line + p, "\n");
    emit_raw(line);
}

/* ================= 指纹计算 (排序/去重/哈希) ================= */
/* ================= 指纹计算 =================
 * 转录是确定性的（固定顺序 emit_field / emit_hex），没有随机探针，
 * 所以直接对原文做 SHA256，不需要旧的排序/去重/结构化解析。
 */
static void compute_hash(const char *text, size_t *out_count, char hash_out[65]) {
    size_t lines = 0;
    const char *p = text;

    while (p && *p) { if (*p == '\n') lines++; p++; }

    sha256_hex((const uint8_t *)text, strlen(text), hash_out);
    if (out_count) *out_count = lines;
}

/* ================= 驱动交互 ================= */
static HANDLE open_driver(void) {
    return CreateFileW(GAME_BASE_WIN32_PATH, GENERIC_READ | GENERIC_WRITE,
                       FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                       OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
}

static int enum_devices(HANDLE h, USB_DEV_LIST *out) {
    DWORD ret = 0;
    if (!DeviceIoControl(h, IOCTL_USB_ENUM_DEVICES, NULL, 0, out, sizeof(*out), &ret, NULL))
        return -1;
    return (int)out->Count;
}

/* ================= 任意控制传输 (IOCTL_USB_RAW_CONTROL) =================
 * iface 是 GUID_DEVINTERFACE_USB_DEVICE 枚举列表里的下标（见 ifaces 命令）。
 *
 * 返回：>=0 实际字节数（成功；设备 STALL 时返回 -1 但 status_out 有效）
 *       -1  IOCTL 本身失败 或 设备回 STALL（看 status_out 区分）
 *
 * wLength 和缓冲区分离：wLength 可以声明 65535 这种越界值，
 * 实际只给 256 字节缓冲 —— 和 kmboxdetect.exe 的 diff 探测一致。
 */
static int raw_ctrl(HANDLE h, ULONG iface, uint8_t bm, uint8_t rq, uint16_t wv, uint16_t wi,
                    unsigned char *d, uint16_t wlen, ULONG *status_out) {
    static USB_RAW_CTRL_REQ req;
    DWORD ret = 0;

    memset(&req, 0, sizeof(req));
    req.DevIndex      = iface;
    req.bmRequestType = bm;
    req.bRequest      = rq;
    req.wValue        = wv;
    req.wIndex        = wi;
    req.wLength       = wlen;

    if (wlen > 0 && !(bm & 0x80) && d) {
        ULONG n = wlen > USB_RAW_MAX_UM ? USB_RAW_MAX_UM : wlen;
        memcpy(req.Data, d, n);
    }

    if (!DeviceIoControl(h, IOCTL_USB_RAW_CONTROL, &req, sizeof(req),
                         &req, sizeof(req), &ret, NULL)) {
        if (status_out) *status_out = 0xFFFFFFFF;
        return -1;
    }
    if (status_out) *status_out = req.Status;

    /* 驱动即使设备 STALL 也返回 STATUS_SUCCESS，用它内部的 Status 判定 */
    if (req.Status != 0) return -1;

    if (wlen > 0 && (bm & 0x80) && d) {
        ULONG n = req.Returned > USB_RAW_MAX_UM ? USB_RAW_MAX_UM : req.Returned;
        if (n > wlen) n = wlen;
        memcpy(d, req.Data, n);
    }
    return (int)req.Returned;
}

/* ================= GUID 接口枚举 ================= */
typedef struct {
    ULONG Index;
    uint16_t Vid, Pid, Rev, BcdUSB;
    unsigned char Cls, Sub, Proto;
} IFACE;

/* 逐个下标试 GET_DESCRIPTOR(Device)，能返回合法 18 字节的就算一个可用接口 */
static int probe_ifaces(HANDLE h, IFACE *out, int max) {
    int n = 0;
    for (ULONG i = 0; i < 32 && n < max; i++) {
        unsigned char d[18];
        ULONG st = 0;
        memset(d, 0, sizeof(d));
        int r = raw_ctrl(h, i, 0x80, 0x06, 0x0100, 0, d, 18, &st);
        if (r < 18 || d[0] != 18 || d[1] != 0x01) continue;
        out[n].Index  = i;
        out[n].BcdUSB = (uint16_t)(d[2] | (d[3] << 8));
        out[n].Cls    = d[4];
        out[n].Sub    = d[5];
        out[n].Proto  = d[6];
        out[n].Vid    = (uint16_t)(d[8] | (d[9] << 8));
        out[n].Pid    = (uint16_t)(d[10] | (d[11] << 8));
        out[n].Rev    = (uint16_t)(d[12] | (d[13] << 8));
        n++;
    }
    return n;
}

static BOOL block_dev(HANDLE h, ULONG idx, BOOL on) {
    USB_BLOCK_REQ r;
    DWORD ret = 0;
    memset(&r, 0, sizeof(r));
    r.DevIndex = idx;
    r.Block = on ? TRUE : FALSE;
    return DeviceIoControl(h, IOCTL_USB_BLOCK_DEVICE, &r, sizeof(r), NULL, 0, &ret, NULL);
}

/* ================= 扫描（走 IOCTL_USB_FULL_ENUM，等价 kmboxdetect idscan）================= */
static void emit_field(const char *k, const char *v) {
    char line[1024];
    sprintf(line, "%s\t%s\n", k, v);
    emit_raw(line);
}

static void wstr_emit(const char *key, const WCHAR *w) {
    char u[512];
    if (!w || !w[0]) return;
    WideCharToMultiByte(CP_UTF8, 0, w, -1, u, sizeof(u), NULL, NULL);
    emit_field(key, u);
}

/* 对一台 Hub 设备做身份指纹 —— 数据全来自驱动 IOCTL_USB_FULL_ENUM */
static void scan_full_one(HANDLE h, const USB_DEV_UM *d, ULONG devIdx, int verbose) {
    char key[64], buf[256];
    ULONG k;

    g_mem = 1; g_len = 0;
    if (g_buf) g_buf[0] = 0;

    sprintf(key, "VID_%04X&PID_%04X&REV_%04X&USB_%04X",
            d->Vid, d->Pid, d->BcdDevice, d->BcdUSB);

    sprintf(buf, "hub=%lu port=%lu speed=%u", d->HubIndex, d->PortNumber, d->Speed);
    emit_field("LOC", buf);

    emit_field("DEV_DESC", key);

    sprintf(buf, "class=%02X/%02X/%02X cfg#%u ifaces=%u",
            d->DeviceClass, d->DeviceSubClass, d->DeviceProtocol,
            d->ConfigValue, d->IfaceCount);
    emit_field("CFG", buf);

    wstr_emit("MFG",     d->Manufacturer);
    wstr_emit("PRODUCT", d->Product);
    wstr_emit("SERIAL",  d->Serial);

    if (d->HasQualifier) {
        sprintf(buf, "bcdUSB=%04X class=%02X/%02X/%02X maxpkt0=%u cfgs=%u",
                d->QualBcdUSB, d->QualClass, d->QualSubClass, d->QualProtocol,
                d->QualMaxPacket0, d->QualNumConfigs);
        emit_field("QUAL", buf);
    }
    if (d->HasBos) {
        sprintf(buf, "total=%u caps=%u", d->BosTotalLen, d->BosCapCount);
        emit_field("BOS", buf);
    }

    for (k = 0; k < d->IfaceCount && k < USB_IFACE_MAX_UM; k++) {
        const USB_IFACE_UM *ii = &d->Ifaces[k];

        sprintf(buf, "iface%u class=%02X/%02X/%02X eps=%u",
                ii->Number, ii->Class, ii->SubClass, ii->Protocol, ii->Endpoints);
        emit_field("IFACE", buf);

        if (ii->HidDescLen) {
            sprintf(buf, "bcdHID=%04X country=%u numDesc=%u phys=%u",
                    ii->HidBcdHID, ii->HidCountry, ii->HidNumDesc,
                    ii->HasPhysDesc ? ii->PhysDescLen : 0);
            emit_field("HID", buf);
        }

        if (ii->HasReport) {
            USB_REPORT_REQ rq;
            USB_REPORT_RSP rsp;
            DWORD ret = 0;

            memset(&rq, 0, sizeof(rq));
            memset(&rsp, 0, sizeof(rsp));
            rq.DevIndex   = devIdx;
            rq.IfaceIndex = k;

            if (DeviceIoControl(h, IOCTL_USB_REPORT_DESC, &rq, sizeof(rq),
                                &rsp, sizeof(rsp), &ret, NULL) && rsp.Length > 0)
                emit_hex("REPORT_DESC", rsp.Data, rsp.Length);
        }
    }

    g_mem = 0;
    size_t count = 0; char hash[65] = {0};
    compute_hash(g_buf, &count, hash);

    printf("  [%lu] %s  %s\n", devIdx, key, d->Suspicious ? "  <<< 可疑复合" : "");
    printf("        端口: hub=%lu port=%lu  speed=%u\n", d->HubIndex, d->PortNumber, d->Speed);
    if (d->Manufacturer[0] || d->Product[0]) {
        char m[256], p[256];
        WideCharToMultiByte(CP_UTF8, 0, d->Manufacturer, -1, m, sizeof(m), NULL, NULL);
        WideCharToMultiByte(CP_UTF8, 0, d->Product, -1, p, sizeof(p), NULL, NULL);
        printf("        名称: %s %s\n", m, p);
    }
    printf("        条目数: %zu\n", count);
    printf("        指纹:   %s\n", hash);
    if (verbose) {
        printf("        ---- 转录预览 ----\n");
        int lines = 0;
        for (char *p2 = g_buf; p2 && *p2 && lines < 10; lines++) {
            char *nl = strchr(p2, '\n');
            if (nl) *nl = 0;
            printf("        %s\n", p2);
            if (!nl) break;
            p2 = nl + 1;
        }
    }
}

int main(int argc, char **argv) {
    SetConsoleOutputCP(CP_UTF8);
    srand((unsigned)time(NULL));

    /* 无参数 / -h / --help: 只打用法，不需要驱动 */
    if (argc < 2 || !strcmp(argv[1], "-h") || !strcmp(argv[1], "--help") ||
        !strcmp(argv[1], "help")) {
        fprintf(stderr,
            "用法: usb_client <命令>\n"
            "  kmbox               KMBox 判定（鼠标与键盘共享祖父设备）\n"
            "  nodes               HID 节点表快照（10 槽）\n"
            "  list                列出驱动收集到的 USB 设备\n"
            "  ifaces              GUID 接口表（raw control 用的下标）\n"
            "  debug               驱动内部状态\n"
            "  log                 驱动日志\n"
            "  scan [--n N] [--verbose]   固件指纹扫描（fuzz + diff）\n"
            "  block <idx>         封锁设备\n"
            "  unblock <idx>       解封设备\n");
        return argc < 2 ? 1 : 0;
    }

    int nprobe = DEFAULT_PROBES, verbose = 0;
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--n") && i + 1 < argc) nprobe = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--verbose")) verbose = 1;
    }
    if (nprobe < 16) nprobe = 16;
    if (nprobe > 8192) nprobe = 8192;

    HANDLE h = open_driver();
    if (h == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "打不开 %ws (err=%lu), 驱动是否已加载?\n", GAME_BASE_WIN32_PATH, GetLastError());
        return 1;
    }

    /* ================================================================
     * kmbox / nodes 不依赖设备枚举，放在 ENUM 之前，ENUM 失败也能用
     * ================================================================ */

    /* ---- kmbox: KMBox 判定（鼠标与键盘共享祖父设备）---- */
    if (argc >= 2 && !strcmp(argv[1], "kmbox")) {
        static KMBOX_LIST kl;
        DWORD ret = 0;

        if (!DeviceIoControl(h, IOCTL_USB_KMBOX, NULL, 0, &kl, sizeof(kl), &ret, NULL)) {
            printf("KMBOX ioctl 失败 (err=%lu)\n", GetLastError());
            CloseHandle(h);
            return 1;
        }

        printf("========== KMBox 判定 ==========\n");
        printf("判据: 鼠标节点与键盘节点共享同一个祖父 _DEVICE_NODE\n");
        printf("命中: %lu\n", kl.Count);

        if (kl.Count == 0) {
            printf("  (未发现可疑组合 —— 鼠标和键盘挂在不同的祖父设备下)\n");
        }

        for (ULONG i = 0; i < kl.Count; i++) {
            const KMBOX_HIT_UM *hit = &kl.Hits[i];
            printf("\n  [%lu] *** 可疑 ***\n", i);
            printf("      鼠标: VID_%04X PID_%04X\n", hit->MouseVid, hit->MousePid);
            printf("            %ls\n", hit->MouseProduct);
            printf("      键盘: VID_%04X PID_%04X\n", hit->KbdVid, hit->KbdPid);
            printf("            %ls\n", hit->KbdProduct);
            printf("      共享祖父设备路径:\n            %ls\n", hit->SharedPath);
        }

        CloseHandle(h);
        return kl.Count > 0 ? 0 : 2;
    }

    /* ---- nodes: HID 节点表快照 ---- */
    if (argc >= 2 && !strcmp(argv[1], "nodes")) {
        static HID_NODE_LIST nl;
        DWORD ret = 0;
        static const char *tname[] = { "NONE", "KEYBOARD", "MOUSE" };

        if (!DeviceIoControl(h, IOCTL_USB_NODES, NULL, 0, &nl, sizeof(nl), &ret, NULL)) {
            printf("NODES ioctl 失败 (err=%lu)\n", GetLastError());
            CloseHandle(h);
            return 1;
        }

        printf("========== HID 节点表 (%lu/%d) ==========\n", nl.Count, HID_NODE_MAX_UM);
        if (nl.Count == 0) {
            printf("  (空 —— 驱动还没收到 START_DEVICE，或 hub hook 没装上)\n");
        }
        for (ULONG i = 0; i < nl.Count; i++) {
            const HID_NODE_UM *nd = &nl.Nodes[i];
            const char *tn = (nd->DeviceType <= 2) ? tname[nd->DeviceType] : "?";
            printf("  [%lu] slot=%-2lu %-9s VID_%04X PID_%04X%s\n",
                   i, nd->Slot, tn, nd->Vid, nd->Pid,
                   nd->Blocked ? "  [BLOCKED]" : "");
            if (nd->InstancePath[0])
                printf("       路径: %ls\n", nd->InstancePath);
        }
        CloseHandle(h);
        return nl.Count > 0 ? 0 : 2;
    }

    /* ---- ifaces: GUID_DEVINTERFACE_USB_DEVICE 接口表 ---- */
    if (argc >= 2 && !strcmp(argv[1], "ifaces")) {
        static IFACE f[16];
        int nf = probe_ifaces(h, f, 16);

        printf("========== GUID_DEVINTERFACE_USB_DEVICE 接口 ==========\n");
        printf("可用接口: %d\n", nf);
        for (int i = 0; i < nf; i++)
            printf("  iface[%lu]  VID_%04X PID_%04X REV_%04X USB_%04X  class=%02X/%02X/%02X\n",
                   f[i].Index, f[i].Vid, f[i].Pid, f[i].Rev, f[i].BcdUSB,
                   f[i].Cls, f[i].Sub, f[i].Proto);
        if (nf == 0) printf("  (全空 —— 驱动是否加载? IOCTL_USB_RAW_CONTROL 是否可用?)\n");
        CloseHandle(h);
        return nf > 0 ? 0 : 2;
    }

    /* ---- scan: 全量枚举 + 身份指纹（等价 kmboxdetect idscan）---- */
    if (argc >= 2 && !strcmp(argv[1], "scan")) {
        static USB_FULL_LIST_UM full;
        DWORD ret = 0;

        if (!DeviceIoControl(h, IOCTL_USB_FULL_ENUM, NULL, 0,
                             &full, sizeof(full), &ret, NULL)) {
            printf("FULL_ENUM ioctl 失败 (err=%lu)\n", GetLastError());
            CloseHandle(h);
            return 1;
        }

        printf("========== 全量枚举指纹 (hub=%lu, 设备=%lu, last=0x%08lX) ==========\n",
               full.HubCount, full.Count, full.LastStatus);
        if (full.Count == 0) {
            printf("  (驱动没枚举到设备 —— 看 log 里的 UsbHubEnumAll)\n");
            CloseHandle(h);
            return 2;
        }

        for (ULONG i = 0; i < full.Count && i < USB_FULL_MAX_UM; i++)
            scan_full_one(h, &full.Devs[i], i, verbose);

        CloseHandle(h);
        return 0;
    }

    static USB_DEV_LIST list;
    int n = enum_devices(h, &list);
    if (n < 0) { fprintf(stderr, "ENUM 失败 (err=%lu)\n", GetLastError()); CloseHandle(h); return 1; }

    if (argc < 2 || !strcmp(argv[1], "list")) {
        printf("驱动收集到 %d 个 USB 设备\n", n);
        for (int i = 0; i < n; i++) {
            printf("  [%lu] %ls  (VID_%04X PID_%04X REV_%04X)%s\n",
                   list.Devs[i].Index, list.Devs[i].HardwareId,
                   list.Devs[i].Vid, list.Devs[i].Pid, list.Devs[i].BcdDevice,
                   list.Devs[i].Active ? "" : " [offline]");
        }
        CloseHandle(h);
        return n > 0 ? 0 : 2;
    }

    if (!strcmp(argv[1], "block") || !strcmp(argv[1], "unblock")) {
        if (argc < 3) { fprintf(stderr, "用法: usb_client block <index>\n"); CloseHandle(h); return 1; }
        ULONG idx = (ULONG)atoi(argv[2]);
        BOOL on = !strcmp(argv[1], "block");
        BOOL ok = block_dev(h, idx, on);
        printf("%s device %lu -> %s (err=%lu)\n", on ? "block" : "unblock", idx,
               ok ? "OK" : "FAILED", ok ? 0 : GetLastError());
        CloseHandle(h);
        return ok ? 0 : 1;
    }

    if (!strcmp(argv[1], "debug")) {
        USB_DEBUG_INFO d;
        DWORD ret = 0;
        if (DeviceIoControl(h, IOCTL_USB_DEBUG, NULL, 0, &d, sizeof(d), &ret, NULL)) {
            printf("hook_installed : %lu\n", d.HookInstalled);
            printf("hub_driver     : %lu\n", d.HubDriverFound);
            printf("device_count   : %lu\n", d.DeviceCount);
            printf("last_scan_count: %lu\n", d.LastScanCount);
            printf("last_scan_stat : 0x%08lX\n", d.LastScanStatus);
            printf("hook_status    : 0x%08lX\n", d.HookStatus);
            printf("tracked        : %lu\n", d.Tracked);
        } else {
            printf("DEBUG ioctl failed (err=%lu)\n", GetLastError());
        }
        CloseHandle(h);
        return 0;
    }

    if (!strcmp(argv[1], "log")) {
        static LOG_DUMP d;
        DWORD ret = 0;
        if (DeviceIoControl(h, IOCTL_FW_LOG_DUMP, NULL, 0, &d, sizeof(d), &ret, NULL)) {
            printf("=== game_base 日志 (%lu 条) ===\n", d.Count);
            for (ULONG i = 0; i < d.Count; i++) {
                const char *lv = d.Entries[i].Level == 3 ? "DBG" :
                                 d.Entries[i].Level == 2 ? "ERR" :
                                 d.Entries[i].Level == 1 ? "WRN" : "INF";
                /* 1601 起的 100ns 计数 -> FILETIME -> 本地时间 */
                FILETIME ft, lft;
                SYSTEMTIME st;
                ULONGLONG ts = d.Entries[i].Ts;
                ft.dwLowDateTime  = (DWORD)(ts & 0xFFFFFFFF);
                ft.dwHighDateTime = (DWORD)(ts >> 32);
                FileTimeToLocalFileTime(&ft, &lft);
                FileTimeToSystemTime(&lft, &st);
                printf("[%04d-%02d-%02d %02d:%02d:%02d.%03d][%s][%s] %s\n",
                       st.wYear, st.wMonth, st.wDay,
                       st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
                       lv, d.Entries[i].Func, d.Entries[i].Msg);
            }
        } else {
            printf("LOG ioctl failed (err=%lu)\n", GetLastError());
        }
        CloseHandle(h);
        return 0;
    }

    fprintf(stderr,
        "用法: usb_client <命令>\n"
        "  kmbox               KMBox 判定（鼠标与键盘共享祖父设备）\n"
        "  nodes               HID 节点表快照（10 槽）\n"
        "  list                列出驱动收集到的 USB 设备\n"
        "  debug               驱动内部状态\n"
        "  log                 驱动日志\n"
        "  scan [--n N] [--verbose]   固件指纹扫描\n"
        "  block <idx>         封锁设备\n"
        "  unblock <idx>       解封设备\n");
    CloseHandle(h);
    return 1;
}
