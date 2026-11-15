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

static void emit(const char *bm, const char *rq, const char *wv, const char *wi,
                 const char *ex, const char *st, long v) {
    char line[256];
    sprintf(line, "%s\t%s\t%s\t%s\t%s\t%s\t%ld\n", bm, rq, wv, wi, ex, st, v);
    emit_raw(line);
}

static void emit_hex(const char *tag, const unsigned char *d, int n) {
    static char line[8192];
    int p = sprintf(line, "%s\thex\t", tag);
    for (int i = 0; i < n && p < (int)sizeof(line) - 4; i++)
        p += sprintf(line + p, "%02x", d[i]);
    sprintf(line + p, "\n");
    emit_raw(line);
}

/* ================= 错误码粗分类 ================= */
/* 仅保留 Win32 错误码名字；驱动侧状态改用 nt_name() */

/* ================= 指纹计算 (排序/去重/哈希) ================= */
typedef struct { char k[4][64]; int k4i; long k4n; char k4s[64]; char st[12]; long v; int vnone; } Ent;
static int ent_cmp(const void *a, const void *b) {
    const Ent *x = a, *y = b; int c;
    for (int i = 0; i < 4; i++) { c = strcmp(x->k[i], y->k[i]); if (c) return c; }
    if (x->k4i && y->k4i) return (x->k4n > y->k4n) - (x->k4n < y->k4n);
    if (!x->k4i && !y->k4i) return strcmp(x->k4s, y->k4s);
    return x->k4i ? -1 : 1;
}
static void ap(char **b, size_t *c, size_t *l, const char *s) {
    size_t n = strlen(s);
    if (*l + n + 1 > *c) { while (*l + n + 1 > *c) *c = *c ? *c * 2 : 65536; *b = realloc(*b, *c); }
    memcpy(*b + *l, s, n); *l += n; (*b)[*l] = 0;
}
static void api(char **b, size_t *c, size_t *l, long v) { char t[32]; sprintf(t, "%ld", v); ap(b, c, l, t); }
static char *build_repr(Ent *e, size_t n) {
    char *b = 0; size_t c = 0, l = 0;
    ap(&b, &c, &l, "[");
    for (size_t i = 0; i < n; i++) {
        if (i) ap(&b, &c, &l, ", ");
        ap(&b, &c, &l, "((");
        for (int j = 0; j < 4; j++) {
            if (j) ap(&b, &c, &l, ", ");
            ap(&b, &c, &l, "'"); ap(&b, &c, &l, e[i].k[j]); ap(&b, &c, &l, "'");
        }
        ap(&b, &c, &l, ", ");
        if (e[i].k4i) api(&b, &c, &l, e[i].k4n);
        else { ap(&b, &c, &l, "'"); ap(&b, &c, &l, e[i].k4s); ap(&b, &c, &l, "'"); }
        ap(&b, &c, &l, "), ('");
        ap(&b, &c, &l, e[i].st);
        ap(&b, &c, &l, "', ");
        if (e[i].vnone) ap(&b, &c, &l, "None"); else api(&b, &c, &l, e[i].v);
        ap(&b, &c, &l, "))");
    }
    ap(&b, &c, &l, "]");
    return b;
}
static int parse_line(char *line, Ent *cur) {
    char *f[8]; int nf = 0; char *p = line; f[nf++] = p;
    for (; *p; p++) {
        if (*p == '\t') { *p = 0; f[nf++] = p + 1; if (nf >= 8) break; }
        if (*p == '\n' || *p == '\r') { *p = 0; break; }
    }
    if (nf < 6) return 0;
    memset(cur, 0, sizeof(Ent));
    strncpy(cur->k[0], f[0], 63); strncpy(cur->k[1], f[1], 63);
    strncpy(cur->k[2], f[2], 63); strncpy(cur->k[3], f[3], 63);
    if (f[4][0] == 'i' && f[4][1] == ':') { cur->k4i = 1; cur->k4n = atol(f[4] + 2); }
    else if (f[4][0] == 's' && f[4][1] == ':') strncpy(cur->k4s, f[4] + 2, 63);
    else strncpy(cur->k4s, f[4], 63);
    strncpy(cur->st, f[5], 11);
    if (nf >= 7 && strcmp(f[6], "None") == 0) { cur->vnone = 1; cur->v = 0; }
    else cur->v = nf >= 7 ? atol(f[6]) : 0;
    return 1;
}
static void compute_hash(const char *text, size_t *out_count, char hash_out[65]) {
    size_t cap = 262144, n = 0;
    Ent *e = (Ent *)malloc(cap * sizeof(Ent));
    const char *p = text;
    while (p && *p) {
        const char *eol = strchr(p, '\n');
        size_t len = eol ? (size_t)(eol - p) : strlen(p);
        if (len > 0) {
            char *line = (char *)malloc(len + 1);
            memcpy(line, p, len); line[len] = 0;
            if (n >= cap) { cap *= 2; e = (Ent *)realloc(e, cap * sizeof(Ent)); }
            if (parse_line(line, &e[n])) n++;
            free(line);
        }
        if (!eol) break;
        p = eol + 1;
    }
    qsort(e, n, sizeof(Ent), ent_cmp);
    size_t m = 0;
    for (size_t i = 0; i < n; i++) { if (i + 1 < n && ent_cmp(&e[i], &e[i + 1]) == 0) continue; e[m++] = e[i]; }
    char *repr = build_repr(e, m);
    sha256_hex((const uint8_t *)repr, strlen(repr), hash_out);
    if (out_count) *out_count = m;
    free(repr); free(e);
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

static int read_desc(HANDLE h, ULONG iface, uint8_t type, uint8_t index, uint16_t wIndex,
                     unsigned char *buf, uint16_t len) {
    return raw_ctrl(h, iface, 0x80, 0x06, (uint16_t)((type << 8) | index),
                    wIndex, buf, len, NULL);
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

/* ================= 描述符采集 ================= */
static void collect_descriptors(HANDLE h, ULONG idx, char key_out[64]) {
    unsigned char d[512];
    memset(d, 0, sizeof(d));
    int n = read_desc(h, idx, 0x01, 0, 0, d, 18);
    if (n >= 18 && d[0] == 18) {
        uint16_t bcdUSB = (uint16_t)(d[2] | (d[3] << 8));
        uint16_t vid = (uint16_t)(d[8] | (d[9] << 8));
        uint16_t pid = (uint16_t)(d[10] | (d[11] << 8));
        uint16_t rev = (uint16_t)(d[12] | (d[13] << 8));
        sprintf(key_out, "VID_%04X&PID_%04X&REV_%04X&USB_%04X", vid, pid, rev, bcdUSB);
        emit_hex("DEV_DESC", d, n);
    } else {
        strcpy(key_out, "VID_UNKNOWN");
        emit("DEV_DESC", "", "", "", "", "err", 0);
    }
    memset(d, 0, sizeof(d));
    n = read_desc(h, idx, 0x02, 0, 0, d, 9);
    if (n >= 9 && d[0] == 9) {
        uint16_t total = (uint16_t)(d[2] | (d[3] << 8));
        emit_hex("CFG_DESC9", d, n);
        if (total >= 9 && total <= sizeof(d)) {
            memset(d, 0, sizeof(d));
            int n2 = read_desc(h, idx, 0x02, 0, 0, d, (uint16_t)total);
            if (n2 > 0) emit_hex("CFG_DESC_FULL", d, n2);
        }
    } else emit("CFG_DESC9", "", "", "", "", "err", 0);

    memset(d, 0, sizeof(d));
    n = read_desc(h, idx, 0x22, 0, 0, d, 64);
    if (n > 0) emit_hex("REPORT_DESC", d, n);
    else emit("REPORT_DESC", "", "", "", "", "err", 0);

    memset(d, 0, sizeof(d));
    n = read_desc(h, idx, 0x03, 0, 0x0409, d, 4);
    if (n >= 4) emit_hex("STR_LANGID", d, n);
    for (int i = 1; i <= 3; i++) {
        memset(d, 0, sizeof(d));
        n = read_desc(h, idx, 0x03, (uint8_t)i, 0x0409, d, 255);
        if (n > 0) { char tag[32]; sprintf(tag, "STR_IDX%d", i); emit_hex(tag, d, n); }
    }
}

/* ================= 探针 ================= */

/* 把驱动返回的 NTSTATUS 压成短标签，作为指纹里的一维 */
static const char *nt_name(ULONG st) {
    switch (st) {
    case 0x00000000: return "ok";
    case 0xFFFFFFFF: return "ioctl_fail";
    case 0xC0000001: return "STALL";        /* STATUS_UNSUCCESSFUL */
    case 0xC0000010: return "INVALID_REQ";  /* STATUS_INVALID_DEVICE_REQUEST */
    case 0xC0000023: return "BUF_SMALL";    /* STATUS_BUFFER_TOO_SMALL */
    case 0xC00000BB: return "NOT_SUPPORTED";
    case 0xC0000004: return "INVALID_INFO";
    default:         return "other";
    }
}

/*
 * 行为探测 —— 对齐 kmboxdetect idscan 的 6 组只读 GET_DESCRIPTOR
 *
 * 之前这里是 cmd_fuzz_random(随机 IN 探针) + cmd_diff(3×256×13=9984 次
 * 任意 bRequest)。9984 次控制传输打到一个正被 mouhid 占用的活鼠标上，
 * 直接把它打到 PnP 移除 —— scan 完再 ifaces 就变成 0 接口。
 *
 * 所以改成和参考工具一致：6 组温和的只读描述符探测。这些请求
 * 全部是 bRequest=6(GET_DESCRIPTOR)，不会改变设备状态，扫完设备还在。
 */
static void cmd_behavior(HANDLE h, ULONG iface) {
    struct { const char *nm; uint8_t type, index; uint16_t wlen; } bs[] = {
        {"DEVDESC_LEN0",     0x01, 0, 0},
        {"DEV_QUAL",         0x06, 0, 0},
        {"BOS",              0x0F, 0, 0},
        {"CONFIG_IDX1",      0x02, 1, 0},
        {"DEVDESC_PARTIAL8", 0x01, 0, 8},
        {"DEVDESC_AFTER",    0x01, 0, 18},
    };
    unsigned char d[256];
    char wv[16], nm[32];

    for (int i = 0; i < (int)(sizeof(bs) / sizeof(bs[0])); i++) {
        ULONG stat = 0;
        int rt;

        sprintf(wv, "0x%04x", (uint16_t)((bs[i].type << 8) | bs[i].index));
        sprintf(nm, "s:%s", bs[i].nm);

        rt = raw_ctrl(h, iface, 0x80, 0x06,
                      (uint16_t)((bs[i].type << 8) | bs[i].index),
                      0, d, bs[i].wlen, &stat);

        if (rt >= 0) emit("0x80", "0x06", wv, "0x0000", nm, "ok", rt);
        else         emit("0x80", "0x06", wv, "0x0000", nm, nt_name(stat), 0);
    }
}

/* ================= 扫描 ================= */
static void scan_one(HANDLE h, const IFACE *f, int nprobe, int verbose) {
    char key[64] = {0};
    g_mem = 1; g_len = 0;
    if (g_buf) g_buf[0] = 0;

    collect_descriptors(h, f->Index, key);
    cmd_behavior(h, f->Index);

    g_mem = 0;
    size_t count = 0; char hash[65] = {0};
    compute_hash(g_buf, &count, hash);

    printf("  [%lu] VID_%04X&PID_%04X&REV_%04X&USB_%04X  class=%02X/%02X/%02X\n",
           f->Index, f->Vid, f->Pid, f->Rev, f->BcdUSB, f->Cls, f->Sub, f->Proto);
    printf("        基线键: %s\n", key[0] ? key : "(unknown)");
    printf("        条目数: %zu\n", count);
    printf("        指纹:   %s\n", hash);
    if (verbose) {
        printf("        ---- 转录预览 ----\n");
        int lines = 0;
        for (char *p = g_buf; p && *p && lines < 10; lines++) {
            char *nl = strchr(p, '\n');
            if (nl) *nl = 0;
            printf("        %s\n", p);
            if (!nl) break;
            p = nl + 1;
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

    /* ---- scan: 固件指纹扫描（走 IOCTL_USB_RAW_CONTROL）---- */
    if (argc >= 2 && !strcmp(argv[1], "scan")) {
        static IFACE f[16];
        int nf = probe_ifaces(h, f, 16);

        printf("========== 固件指纹扫描 (probes=%d, ifaces=%d) ==========\n", nprobe, nf);
        if (nf == 0) {
            printf("  (没有可用接口 —— 先跑 usb_client ifaces 看驱动通道)\n");
            CloseHandle(h);
            return 2;
        }
        for (int i = 0; i < nf; i++) scan_one(h, &f[i], nprobe, verbose);
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
