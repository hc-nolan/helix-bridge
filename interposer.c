/*
 * Fake Line 6 Interposer.dll for running HX Edit under Wine.
 *
 * HX Edit (32-bit) delay-loads C:\ProgramData\Line 6\Interposer\Interposer.dll
 * and talks to Helix devices through it. This replacement forwards everything
 * over TCP to helixd.py, a native Linux daemon that owns the USB device via
 * libusb.
 *
 * API (all cdecl, reverse engineered from HX Edit 3.82):
 *   int InterposerInit(void)
 *   int InterposerCleanup(void)
 *   int InterposerGetDescriptorCount(int *count, int *descSize)
 *   int InterposerGetDescriptors(void *buf, int bytes)      desc = 0x214 bytes
 *   int InterposerRegisterCallback(int handle, cb fn, void *ctx, int type, int flags)
 *   int InterposerOpenPipe(int devId, int *pipe)
 *   int InterposerClosePipe(int pipe)
 *   int InterposerGetApplicationId(int pipe, int *id)
 *   int InterposerMessage(int pipe, const msg *req, msg *resp, int respCap, int *respLen)
 *
 * Messages are { u32 size; u32 code; u32 arg; u8 payload[] }:
 *   0x207  control transfer: payload = setup packet (8 bytes) + OUT data;
 *          response payload = IN data
 *   0x483  bulk: arg==0 -> write payload to EP1 OUT
 *                arg==1 -> read; response payload = data from EP1 IN (may be empty)
 *   0x286  query; response payload = u32 (max transfer size, default 0x200)
 *   0x256  sent before ClosePipe
 * Callback type 5 on a pipe = "IN data available"; fn(ctx, msg{.code=5}).
 */
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>

#define DESC_SIZE 0x214
#define DEFAULT_PORT 47800

enum { OP_ENUM = 1, OP_CTRL = 2, OP_BULK_WRITE = 3, OP_SUBSCRIBE = 9 };

typedef void(__cdecl *icb_t)(void *ctx, void *msg);

static CRITICAL_SECTION g_lock;   /* command socket + state */
static CRITICAL_SECTION g_qlock;  /* IN queue */
static SOCKET g_cmd = INVALID_SOCKET, g_evt = INVALID_SOCKET;
static HANDLE g_evt_thread;
static volatile LONG g_running;
static int g_inited, g_next_pipe = 0x100;
static uint16_t g_vid, g_pid;
static FILE *g_log;

#define MAX_CB 16
static struct { int handle; icb_t fn; void *ctx; int type; } g_cbs[MAX_CB];

typedef struct chunk { struct chunk *next; uint32_t len, off; uint8_t data[]; } chunk;
static chunk *g_qhead, *g_qtail;

static void logf_(const char *fmt, ...)
{
    if (!g_log) return;
    SYSTEMTIME t; GetLocalTime(&t);
    fprintf(g_log, "%02d:%02d:%02d.%03d [%lu] ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds,
            GetCurrentThreadId());
    va_list ap; va_start(ap, fmt); vfprintf(g_log, fmt, ap); va_end(ap);
    fputc('\n', g_log); fflush(g_log);
}

static void loghex(const char *tag, const uint8_t *p, uint32_t n)
{
    if (!g_log || !getenv("HELIX_BRIDGE_HEX")) return;
    char buf[3 * 64 + 1]; uint32_t m = n < 64 ? n : 64;
    for (uint32_t i = 0; i < m; i++) sprintf(buf + 3 * i, "%02x ", p[i]);
    buf[3 * m] = 0;
    logf_("  %s (%u): %s%s", tag, n, buf, n > m ? "..." : "");
}

static int send_all(SOCKET s, const void *b, int n)
{
    const char *p = b;
    while (n > 0) { int r = send(s, p, n, 0); if (r <= 0) return -1; p += r; n -= r; }
    return 0;
}

static int recv_all(SOCKET s, void *b, int n)
{
    char *p = b;
    while (n > 0) { int r = recv(s, p, n, 0); if (r <= 0) return -1; p += r; n -= r; }
    return 0;
}

static SOCKET connect_daemon(void)
{
    int port = DEFAULT_PORT;
    const char *e = getenv("HELIX_BRIDGE_PORT");
    if (e) port = atoi(e);
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return s;
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET; a.sin_port = htons((u_short)port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(s, (struct sockaddr *)&a, sizeof a) != 0) { closesocket(s); return INVALID_SOCKET; }
    int one = 1; setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (char *)&one, sizeof one);
    return s;
}

/* Request: u32 len (op+payload), u8 op, payload. Response: u32 len, i32 status, payload. */
static int rpc(uint8_t op, const void *pl, uint32_t pllen, void *out, uint32_t outcap, uint32_t *outlen)
{
    int status = -1;
    EnterCriticalSection(&g_lock);
    if (g_cmd == INVALID_SOCKET) goto done;
    uint32_t len = 1 + pllen;
    if (send_all(g_cmd, &len, 4) || send_all(g_cmd, &op, 1) || (pllen && send_all(g_cmd, pl, pllen)))
        goto broken;
    uint32_t rlen; int32_t st;
    if (recv_all(g_cmd, &rlen, 4) || rlen < 4 || recv_all(g_cmd, &st, 4)) goto broken;
    rlen -= 4;
    uint32_t keep = rlen < outcap ? rlen : outcap;
    if (keep && recv_all(g_cmd, out, keep)) goto broken;
    for (uint32_t left = rlen - keep; left;) {
        char tmp[256]; int n = left < sizeof tmp ? left : sizeof tmp;
        if (recv_all(g_cmd, tmp, n)) goto broken;
        left -= n;
    }
    if (outlen) *outlen = keep;
    status = st;
    goto done;
broken:
    logf_("rpc: daemon connection lost");
    closesocket(g_cmd); g_cmd = INVALID_SOCKET;
done:
    LeaveCriticalSection(&g_lock);
    return status;
}

static void fire_callbacks(int type)
{
    uint32_t msg[3] = { 12, (uint32_t)type, 0 };
    icb_t fns[MAX_CB]; void *ctxs[MAX_CB]; int n = 0;
    EnterCriticalSection(&g_lock);
    for (int i = 0; i < MAX_CB; i++)
        if (g_cbs[i].fn && g_cbs[i].type == type) { fns[n] = g_cbs[i].fn; ctxs[n++] = g_cbs[i].ctx; }
    LeaveCriticalSection(&g_lock);
    for (int i = 0; i < n; i++) fns[i](ctxs[i], msg);
}

static DWORD WINAPI evt_thread(LPVOID unused)
{
    (void)unused;
    while (g_running) {
        uint32_t len;
        if (recv_all(g_evt, &len, 4)) break;
        chunk *c = malloc(sizeof *c + len);
        if (!c || (len && recv_all(g_evt, c->data, len))) { free(c); break; }
        c->next = NULL; c->len = len; c->off = 0;
        loghex("IN ", c->data, len);
        EnterCriticalSection(&g_qlock);
        if (g_qtail) g_qtail->next = c; else g_qhead = c;
        g_qtail = c;
        LeaveCriticalSection(&g_qlock);
        fire_callbacks(5);
    }
    logf_("event thread exiting");
    return 0;
}

/* Pop up to cap bytes of the next IN transfer. */
static uint32_t dequeue(uint8_t *dst, uint32_t cap)
{
    uint32_t n = 0;
    EnterCriticalSection(&g_qlock);
    chunk *c = g_qhead;
    if (c) {
        n = c->len - c->off;
        if (n > cap) { logf_("WARN: IN transfer %u > reader cap %u, splitting", n, cap); n = cap; }
        memcpy(dst, c->data + c->off, n);
        c->off += n;
        if (c->off >= c->len) { g_qhead = c->next; if (!g_qhead) g_qtail = NULL; free(c); }
    }
    LeaveCriticalSection(&g_qlock);
    return n;
}

static void open_log(void)
{
    const char *p = getenv("HELIX_BRIDGE_LOG");
    char def[MAX_PATH];
    if (!p) {
        snprintf(def, sizeof def, "C:\\ProgramData\\Line 6\\Interposer\\bridge.log");
        p = def;
    }
    g_log = fopen(p, "a");
}

__declspec(dllexport) int __cdecl InterposerInit(void)
{
    if (g_inited) return 0;
    InitializeCriticalSection(&g_lock);
    InitializeCriticalSection(&g_qlock);
    open_log();
    logf_("InterposerInit");
    WSADATA w; WSAStartup(MAKEWORD(2, 2), &w);
    g_cmd = connect_daemon();
    if (g_cmd == INVALID_SOCKET) {
        logf_("cannot reach helixd on 127.0.0.1 (is helixd.py running?)");
    } else {
        g_evt = connect_daemon();
        uint32_t len = 1; uint8_t op = OP_SUBSCRIBE;
        if (g_evt != INVALID_SOCKET && !send_all(g_evt, &len, 4) && !send_all(g_evt, &op, 1)) {
            g_running = 1;
            g_evt_thread = CreateThread(NULL, 0, evt_thread, NULL, 0, NULL);
        }
        logf_("connected to helixd");
    }
    g_inited = 1;
    return 0;
}

__declspec(dllexport) int __cdecl InterposerCleanup(void)
{
    logf_("InterposerCleanup");
    if (!g_inited) return 0;
    g_running = 0;
    if (g_evt != INVALID_SOCKET) { shutdown(g_evt, SD_BOTH); closesocket(g_evt); g_evt = INVALID_SOCKET; }
    if (g_evt_thread) { WaitForSingleObject(g_evt_thread, 2000); CloseHandle(g_evt_thread); g_evt_thread = NULL; }
    EnterCriticalSection(&g_lock);
    if (g_cmd != INVALID_SOCKET) { closesocket(g_cmd); g_cmd = INVALID_SOCKET; }
    memset(g_cbs, 0, sizeof g_cbs);
    LeaveCriticalSection(&g_lock);
    g_inited = 0;
    return 0;
}

__declspec(dllexport) int __cdecl InterposerGetDescriptorCount(int *count, int *descSize)
{
    uint8_t r[4]; uint32_t n = 0;
    int st = rpc(OP_ENUM, NULL, 0, r, sizeof r, &n);
    int present = (st == 0 && n == 4);
    if (present) { g_vid = r[0] | r[1] << 8; g_pid = r[2] | r[3] << 8; }
    if (count) *count = present ? 1 : 0;
    if (descSize) *descSize = DESC_SIZE;
    logf_("GetDescriptorCount -> %d (vid %04x pid %04x)", present, g_vid, g_pid);
    return 0;
}

__declspec(dllexport) int __cdecl InterposerGetDescriptors(void *buf, int bytes)
{
    logf_("GetDescriptors(%p, %d)", buf, bytes);
    if (!buf || bytes < DESC_SIZE) return -1;
    memset(buf, 0, bytes);
    uint32_t *d = buf;
    d[0] = DESC_SIZE;  /* unknown; size/version guess */
    d[1] = 1;          /* device id, passed to OpenPipe */
    d[2] = g_vid;
    /* HX Edit's Interposer path only instantiates PIDs 0x4241/0x4242/0x4244; other Helix models
     * (e.g. LT 0x424A) use the same device class via libusb, so present an accepted PID. */
    uint32_t pid = g_pid;
    const char *e = getenv("HELIX_BRIDGE_PID");
    if (e) pid = strtoul(e, NULL, 0);
    else if (pid != 0x4241 && pid != 0x4242 && pid != 0x4244) pid = 0x4241;
    d[3] = pid;
    logf_("descriptor: vid %04x pid %04x (real %04x)", g_vid, pid, g_pid);
    return 0;
}

__declspec(dllexport) int __cdecl InterposerRegisterCallback(int handle, icb_t fn, void *ctx, int type, int flags)
{
    logf_("RegisterCallback(handle %#x, fn %p, ctx %p, type %d, flags %#x)", handle, fn, ctx, type, flags);
    EnterCriticalSection(&g_lock);
    int slot = -1;
    for (int i = 0; i < MAX_CB; i++)
        if (g_cbs[i].fn && g_cbs[i].handle == handle && g_cbs[i].type == type && g_cbs[i].ctx == ctx) slot = i;
    if (!fn) {
        if (slot >= 0) g_cbs[slot].fn = NULL;
    } else {
        if (slot < 0) for (int i = 0; i < MAX_CB; i++) if (!g_cbs[i].fn) { slot = i; break; }
        if (slot >= 0) { g_cbs[slot].handle = handle; g_cbs[slot].fn = fn; g_cbs[slot].ctx = ctx; g_cbs[slot].type = type; }
    }
    LeaveCriticalSection(&g_lock);
    return 0;
}

__declspec(dllexport) int __cdecl InterposerOpenPipe(int devId, int *pipe)
{
    int p = InterlockedIncrement((LONG *)&g_next_pipe);
    if (pipe) *pipe = p;
    logf_("OpenPipe(dev %d) -> %#x", devId, p);
    return 0;
}

__declspec(dllexport) int __cdecl InterposerClosePipe(int pipe)
{
    logf_("ClosePipe(%#x)", pipe);
    EnterCriticalSection(&g_lock);
    for (int i = 0; i < MAX_CB; i++) if (g_cbs[i].handle == pipe) g_cbs[i].fn = NULL;
    LeaveCriticalSection(&g_lock);
    return 0;
}

__declspec(dllexport) int __cdecl InterposerGetApplicationId(int pipe, int *id)
{
    if (id) *id = 1;
    logf_("GetApplicationId(%#x) -> 1", pipe);
    return 0;
}

static int set_resp(uint32_t *resp, int cap, int *outlen, uint32_t code, const void *data, uint32_t n)
{
    if (!resp || cap < 12) { if (outlen) *outlen = 0; return 0; }
    if (n > (uint32_t)cap - 12) n = cap - 12;
    resp[0] = 12 + n; resp[1] = code; resp[2] = 0;
    if (n) memcpy((uint8_t *)resp + 12, data, n);
    if (outlen) *outlen = 12 + n;
    return 0;
}

__declspec(dllexport) int __cdecl InterposerMessage(int pipe, const uint32_t *req, uint32_t *resp, int cap, int *outlen)
{
    static int logged_cap;
    if (!req) return -1;
    uint32_t size = req[0], code = req[1], arg = req[2];
    const uint8_t *pl = (const uint8_t *)req + 12;
    uint32_t pllen = size > 12 ? size - 12 : 0;

    switch (code) {
    case 0x483:
        if (arg == 1) { /* read */
            if (!logged_cap) { logf_("bulk read cap = %d", cap); logged_cap = 1; }
            uint8_t *dst = resp ? (uint8_t *)resp + 12 : NULL;
            uint32_t n = (dst && cap > 12) ? dequeue(dst, cap - 12) : 0;
            if (resp && cap >= 12) { resp[0] = 12 + n; resp[1] = 0x483; resp[2] = 0; }
            if (outlen) *outlen = 12 + n;
            return 0;
        } else { /* write */
            loghex("OUT", pl, pllen);
            int32_t st = rpc(OP_BULK_WRITE, pl, pllen, NULL, 0, NULL);
            if (st < 0) { logf_("bulk write failed %d", st); return -1; }
            if (outlen) *outlen = 0;
            return 0;
        }
    case 0x207: { /* control transfer */
        if (pllen < 8) return -1;
        uint8_t bmrt = pl[0];
        uint16_t wlen = pl[6] | pl[7] << 8;
        uint8_t in[4096]; uint32_t got = 0;
        int32_t st = rpc(OP_CTRL, pl, pllen, in, sizeof in, &got);
        logf_("CTRL %02x %02x wValue %04x wIndex %04x wLength %u -> %d", bmrt, pl[1],
              pl[2] | pl[3] << 8, pl[4] | pl[5] << 8, wlen, st);
        if (st < 0) return -1;
        if (bmrt & 0x80) loghex("CTRL IN", in, got);
        return set_resp(resp, cap, outlen, 0x207, in, (bmrt & 0x80) ? got : 0);
    }
    case 0x286: {
        uint32_t v = 0x200;
        const char *e = getenv("HELIX_BRIDGE_Q286");
        if (e) v = strtoul(e, NULL, 0);
        logf_("Message 0x286 on %#x -> %#x", pipe, v);
        return set_resp(resp, cap, outlen, 0x286, &v, 4);
    }
    default:
        logf_("Message code %#x arg %#x size %u on pipe %#x (cap %d) -> ok", code, arg, size, pipe, cap);
        loghex("payload", pl, pllen);
        return set_resp(resp, cap, outlen, code, NULL, 0);
    }
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID r)
{
    (void)h; (void)r; (void)reason;
    return TRUE;
}
