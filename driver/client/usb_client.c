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
#include "fw_user.h"
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
static const char *classify_error(DWORD err) {
    switch (err) {
    case ERROR_GEN_FAILURE:          return "GEN_FAIL";
    case ERROR_INVALID_PARAMETER:    return "INVALID_PARAM";
    case ERROR_SEM_TIMEOUT:          return "TIMEOUT";
    case ERROR_DEVICE_NOT_CONNECTED: return "DISCONNECTED";
    case ERROR_NOT_ENOUGH_MEMORY:    return "NOMEM";
    case ERROR_OPERATION_ABORTED:    return "ABORTED";
    default:                         return "OTHER";
    }
}

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
    return CreateFileW(L"\\\\.\\game_base", GENERIC_READ | GENERIC_WRITE,
                       FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                       OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
}

static int enum_devices(HANDLE h, USB_DEV_LIST *out) {
    DWORD ret = 0;
    if (!DeviceIoControl(h, IOCTL_USB_ENUM_DEVICES, NULL, 0, out, sizeof(*out), &ret, NULL))
        return -1;
    return (int)out->Count;
}

/* 对指定 devIndex 发控制传输: 成功返回字节数, 失败 -1 */
static int ctrl(HANDLE h, ULONG devIndex, uint8_t bm, uint8_t rq, uint16_t wv, uint16_t wi,
                unsigned char *d, uint16_t n) {
    ULONG size = sizeof(USB_CTRL_REQ) + n;
    USB_CTRL_REQ *req = (USB_CTRL_REQ *)calloc(1, size);
    req->DevIndex = devIndex;
    req->bmRequestType = bm;
    req->bRequest = rq;
    req->wValue = wv;
    req->wIndex = wi;
    req->wLength = n;
    if (n > 0 && !(bm & 0x80)) memcpy((PUCHAR)req + sizeof(USB_CTRL_REQ), d, n);
    DWORD ret = 0;
    BOOL ok = DeviceIoControl(h, IOCTL_USB_CONTROL_TRANSFER, req, size, req, size, &ret, NULL);
    int result;
    if (ok) {
        result = (int)req->bytesReturned;
        if (n > 0 && (bm & 0x80) && d)
            memcpy(d, (PUCHAR)req + sizeof(USB_CTRL_REQ), req->bytesReturned);
    } else {
        result = -1;
    }
    free(req);
    return result;
}

static int read_desc(HANDLE h, ULONG idx, uint8_t type, uint8_t index, uint16_t wIndex,
                     unsigned char *buf, uint16_t len) {
    return ctrl(h, idx, 0x80, 0x06, (uint16_t)((type << 8) | index), wIndex, buf, len);
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
static void cmd_fuzz_random(HANDLE h, ULONG idx, int nprobe) {
    static const uint8_t bms[] = {0x80,0x81,0x82,0xA0,0xA1,0xA2,0xC0,0xC1,0xC2};
    static const uint16_t wvs[] = {0x0000,0x0001,0x0100,0x0200,0x0300,0x0301,0x0302,0x0303,0xEE00,0xFFFF,0x1234};
    static const uint16_t wis[] = {0x0000,0x0001,0x0002,0xFFFF};
    unsigned char d[256];
    for (int i = 0; i < nprobe; i++) {
        uint8_t  bm = bms[rand() % 9];
        uint8_t  rq = (uint8_t)(rand() & 0xFF);
        uint16_t wv = wvs[rand() % 11];
        uint16_t wi = wis[rand() % 4];
        char sb[8], sr[8], swv[8], swi[8];
        sprintf(sb, "0x%02x", bm); sprintf(sr, "0x%02x", rq);
        sprintf(swv, "0x%04x", wv); sprintf(swi, "0x%04x", wi);
        int rt = ctrl(h, idx, bm, rq, wv, wi, d, 256);
        if (rt >= 0) emit(sb, sr, swv, swi, "", "ok", rt);
        else         emit(sb, sr, swv, swi, "", classify_error(GetLastError()), 0);
    }
}

static void cmd_diff(HANDLE h, ULONG idx) {
    struct { const char *nm; uint16_t wi, wv, wl; } ms[] = {
        {"wLength=0",0,0,0},{"wLength=1",0,0,1},{"wLength=255",0,0,255},
        {"wLength=256",0,0,256},{"wLength=512",0,0,512},{"wLength=4096",0,0,4096},
        {"wLength=65535",0,0,65535},{"wValue=FFFF",0,0xFFFF,256},{"wValue=7FFF",0,0x7FFF,256},
        {"wValue=8000",0,0x8000,256},{"wIndex=FFFF",0xFFFF,0,256},{"wIndex=00FF",0x00FF,0,256},
        {"wIndex=FF00",0xFF00,0,256}
    };
    int nm = (int)(sizeof(ms) / sizeof(ms[0]));
    static unsigned char d[65536];
    for (int bi = 0; bi < 3; bi++) {
        uint8_t bm = (uint8_t[]){0x80, 0xA0, 0xC0}[bi];
        for (int rq = 0; rq < 256; rq++)
            for (int mi = 0; mi < nm; mi++) {
                char bm_[8], r[8], wv[8], wi_[8], ex[32];
                sprintf(bm_, "0x%02x", bm); sprintf(r, "0x%02x", rq);
                sprintf(wv, "0x%04x", ms[mi].wv); sprintf(wi_, "0x%04x", ms[mi].wi);
                sprintf(ex, "s:%s", ms[mi].nm);
                int rt = ctrl(h, idx, bm, (uint8_t)rq, ms[mi].wv, ms[mi].wi, d, ms[mi].wl);
                if (rt >= 0) emit(bm_, r, wv, wi_, ex, "ok", rt);
                else         emit(bm_, r, wv, wi_, ex, classify_error(GetLastError()), 0);
            }
    }
}

/* ================= 扫描 ================= */
static void scan_one(HANDLE h, const USB_DEV_INFO *info, int nprobe, int verbose) {
    char key[64] = {0};
    g_mem = 1; g_len = 0;
    if (g_buf) g_buf[0] = 0;

    collect_descriptors(h, info->Index, key);
    cmd_fuzz_random(h, info->Index, nprobe);
    cmd_diff(h, info->Index);

    g_mem = 0;
    size_t count = 0; char hash[65] = {0};
    compute_hash(g_buf, &count, hash);

    printf("  [%lu] %ls\n", info->Index, info->HardwareId);
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

    int nprobe = DEFAULT_PROBES, verbose = 0;
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--n") && i + 1 < argc) nprobe = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--verbose")) verbose = 1;
    }
    if (nprobe < 16) nprobe = 16;
    if (nprobe > 8192) nprobe = 8192;

    HANDLE h = open_driver();
    if (h == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "打不开 \\\\.\\game_base (err=%lu), 驱动是否已加载?\n", GetLastError());
        return 1;
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

    if (!strcmp(argv[1], "scan")) {
        printf("========== 固件指纹扫描 (probes=%d) ==========\n", nprobe);
        for (int i = 0; i < n; i++) scan_one(h, &list.Devs[i], nprobe, verbose);
        CloseHandle(h);
        return 0;
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
        static FW_LOG_DUMP d;
        DWORD ret = 0;
        if (DeviceIoControl(h, IOCTL_FW_LOG_DUMP, NULL, 0, &d, sizeof(d), &ret, NULL)) {
            printf("日志条目: %lu\n", d.Count);
            for (ULONG i = 0; i < d.Count; i++) {
                const char *lv = d.Entries[i].Level == 2 ? "ERR" :
                                 d.Entries[i].Level == 1 ? "WRN" : "INF";
                printf("[%s][%s] %s\n", lv, d.Entries[i].Module, d.Entries[i].Msg);
            }
        } else {
            printf("LOG ioctl failed (err=%lu)\n", GetLastError());
        }
        CloseHandle(h);
        return 0;
    }

    fprintf(stderr, "用法: usb_client list | debug | log | scan [--n N] [--verbose] | block <idx> | unblock <idx>\n");
    CloseHandle(h);
    return 1;
}
