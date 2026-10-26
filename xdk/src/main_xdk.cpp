/*
 * Moonlight for Xbox 360, Xbox 360 SDK build: a regular title (.xex) that
 * starts from the dashboard or Aurora. Brings up graphics and the network,
 * provides the platform layer for the shared app (src/app.c) and keeps the
 * settings and client certificate next to the .xex (game:\moonlight).
 */
#include "xdk.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../src/config.h"
#include "../../src/platform.h"
#include "../../src/ui/canvas.h"
#include "../../src/ui/image.h"
#include "../../src/ui/ui.h"

static char console_ip[20];

/* ------------------------------------------------------------ platform layer */

uint32_t plat_now_ms(void)
{
    return (uint32_t)GetTickCount();
}

void plat_sleep_ms(unsigned int ms)
{
    Sleep(ms);
}

struct plat_thread {
    HANDLE handle;
    void *(*fn)(void *);
    void *arg;
};

static DWORD WINAPI thread_entry(LPVOID p)
{
    plat_thread *t = (plat_thread *)p;
    t->fn(t->arg);
    return 0;
}

plat_thread *plat_thread_start(void *(*fn)(void *), void *arg)
{
    plat_thread *t = (plat_thread *)calloc(1, sizeof(*t));

    if (t == NULL)
        return NULL;
    t->fn = fn;
    t->arg = arg;
    t->handle = CreateThread(NULL, 512 * 1024, thread_entry, t, 0, NULL);
    if (t->handle == NULL) {
        free(t);
        return NULL;
    }
    return t;
}

void plat_thread_join(plat_thread *t)
{
    WaitForSingleObject(t->handle, INFINITE);
    CloseHandle(t->handle);
    free(t);
}

/* Decodes on another core so the menu stays smooth while covers load. */
uint32_t *plat_decode_art(const unsigned char *data, size_t len, int tw, int th)
{
    DWORD prev = XSetThreadProcessor(GetCurrentThread(), HW_THREAD_ART);
    uint32_t *full, *scaled = NULL;
    int w, h;

    full = image_decode_argb(data, len, &w, &h);
    if (full)
        scaled = image_scale_cover(full, w, h, tw, th);
    free(full);
    if (prev != (DWORD)-1)
        XSetThreadProcessor(GetCurrentThread(), prev);
    return scaled;
}

const char *plat_console_ip(void)
{
    return console_ip;
}

void plat_fixup_config(struct config *cfg)
{
    cfg->renderer = 0;   /* always the GPU here */
}

bool plat_has_renderer_choice(void)
{
    return false;
}

bool plat_has_gpu_filter(void)
{
    return true;
}

bool plat_has_thread_choice(void)
{
    return true;
}

/* Debug log: each line is broadcast as one UDP datagram to port 47555 on the
 * LAN, so xdk/scripts/log_listener.py on the PC can show it live. */
#define LOG_PORT 47555
static SOCKET log_sock = INVALID_SOCKET;

static void log_init(void)
{
    BOOL on = TRUE;

    log_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (log_sock != INVALID_SOCKET)
        setsockopt(log_sock, SOL_SOCKET, SO_BROADCAST, (const char *)&on, sizeof(on));
}

void plat_log(const char *line)
{
    struct sockaddr_in to;
    char buf[600];
    int n;

    if (log_sock == INVALID_SOCKET)
        return;
    n = snprintf(buf, sizeof(buf), "%8lu %s", GetTickCount(), line);
    if (n > (int)sizeof(buf) - 1)
        n = (int)sizeof(buf) - 1;
    memset(&to, 0, sizeof(to));
    to.sin_family = AF_INET;
    to.sin_port = htons(LOG_PORT);
    to.sin_addr.s_addr = INADDR_BROADCAST;
    sendto(log_sock, buf, n, 0, (const struct sockaddr *)&to, sizeof(to));
}

/* ------------------------------------------------------------ storage */

/* game: is the folder the .xex was started from. */
static const char storage_root[] = "game:\\moonlight";
static bool storage_ok;

bool storage_init(void)
{
    DWORD attr;

    CreateDirectoryA(storage_root, NULL);
    attr = GetFileAttributesA(storage_root);
    storage_ok = attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY);
    return storage_ok;
}

const char *storage_dir(void)
{
    return storage_ok ? storage_root : NULL;
}

char *storage_read(const char *name, size_t *len)
{
    char path[128];
    FILE *f;
    char *buf;
    long size;

    if (!storage_ok)
        return NULL;
    snprintf(path, sizeof(path), "%s\\%s", storage_root, name);
    f = fopen(path, "rb");
    if (f == NULL)
        return NULL;
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);
    buf = (char *)malloc(size + 1);
    if (buf && fread(buf, 1, size, f) != (size_t)size) {
        free(buf);
        buf = NULL;
    }
    fclose(f);
    if (buf) {
        buf[size] = '\0';
        if (len)
            *len = (size_t)size;
    }
    return buf;
}

bool storage_write(const char *name, const void *data, size_t len)
{
    char path[128];
    FILE *f;
    bool ok;

    if (!storage_ok)
        return false;
    snprintf(path, sizeof(path), "%s\\%s", storage_root, name);
    f = fopen(path, "wb");
    if (f == NULL)
        return false;
    ok = fwrite(data, 1, len, f) == len;
    ok = (fclose(f) == 0) && ok;
    return ok;
}

/* ------------------------------------------------------------ startup */

static void splash(const char *status)
{
    ui_draw_splash(status, plat_now_ms());
    ui_present();
}

static void fatal(const char *title, const char *text)
{
    for (;;) {
        ui_draw_message(ICON_ERROR, title, text);
        ui_present();
        Sleep(500);
    }
}

/* Starts the network stack and waits (up to 20 s) for an IP address. */
static bool network_init(void)
{
    XNetStartupParams xnsp;
    WSADATA wsa;
    XNADDR addr;
    DWORD start = GetTickCount(), status = 0;

    ZeroMemory(&xnsp, sizeof(xnsp));
    xnsp.cfgSizeOfStruct = sizeof(xnsp);
    /* Plain TCP/UDP to the PC instead of Xbox Live's secure protocol. */
    xnsp.cfgFlags = XNET_STARTUP_BYPASS_SECURITY;
    /* The video stream arrives in bursts of up to a frame (~100 KB). */
    xnsp.cfgSockDefaultRecvBufsizeInK = 255;
    xnsp.cfgSockDefaultSendBufsizeInK = 64;
    /* The default of 32 TCP sockets is tight for the host search's parallel
     * connects (src/discover.c) plus box art and HTTPS requests. */
    xnsp.cfgSockMaxStreamSockets = 64;
    if (XNetStartup(&xnsp) != 0 || WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
        return false;

    while (GetTickCount() - start < 20000) {
        status = XNetGetTitleXnAddr(&addr);
        if (status != XNET_GET_XNADDR_PENDING && (status & (XNET_GET_XNADDR_DHCP | XNET_GET_XNADDR_STATIC)))
            break;
        Sleep(100);
    }
    if (!(status & (XNET_GET_XNADDR_DHCP | XNET_GET_XNADDR_STATIC)))
        return false;
    snprintf(console_ip, sizeof(console_ip), "%u.%u.%u.%u", addr.ina.S_un.S_un_b.s_b1, addr.ina.S_un.S_un_b.s_b2,
             addr.ina.S_un.S_un_b.s_b3, addr.ina.S_un.S_un_b.s_b4);
    return true;
}

void __cdecl main()
{
    if (!gfx_init())
        return;   /* nothing to show an error on */
    if (!ui_init(GFX_WIDTH, GFX_HEIGHT))
        return;

    splash("Connecting to the network...");
    if (!network_init())
        fatal("No network",
              "The console has no IP address. Connect a network cable or set up Wi-Fi in the dashboard's "
              "network settings, then start Moonlight again.");

    log_init();
    plat_log("Moonlight (Xbox 360 SDK build) started\n");
    if (!storage_init())
        plat_log("Storage: game:\\moonlight isn't writable; settings won't be saved\n");
    video_xdk_init();
    app_main();
}
