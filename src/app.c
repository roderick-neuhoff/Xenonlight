/*
 * Moonlight's UI flow: splash, client identity, main menu, settings, pairing,
 * app grid with box art, and the streaming session. Shared by the libxenon
 * build and the Xbox 360 SDK build; platform specifics come from platform.h.
 *
 * Kept valid as both C99 and C++ (the SDK build compiles it as C++): explicit
 * casts from void*, no designated initialisers or compound literals.
 *
 * Slow operations (network requests, pairing, key generation) run in a
 * background thread while the UI keeps a spinner moving.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#include <Limelight.h>

#include "platform.h"
#include "audio.h"
#include "config.h"
#include "crypto.h"
#include "gamestream.h"
#include "input.h"
#include "video.h"
#include "ui/ui.h"

#define MAX_APPS 64

static struct config cfg;
static struct client_identity identity;
static struct gs_server server;
static bool have_identity;
static bool server_ok;
static char server_error[200];

/* App list of the current host, kept for names and box art. */
static struct gs_app apps[MAX_APPS];
static uint32_t *app_art[MAX_APPS];
static int app_count;

/* ------------------------------------------------------------ input */

static int wait_button(void)
{
    int b;

    while ((b = input_menu_pressed()) == 0)
        plat_sleep_ms(16);
    return b;
}

/* ------------------------------------------------------------ background tasks */

struct task {
    void *(*fn)(void *);
    void *arg;
    void *ret;
    volatile bool done;
};

static void *task_entry(void *p)
{
    struct task *t = (struct task *)p;

    t->ret = t->fn(t->arg);
    t->done = true;
    return NULL;
}

struct busy_view {
    const char *title;
    char message[200];
};

/* Set once video owns the screen, so the busy screen stops drawing. */
static volatile bool ui_suspended;

/* Runs fn(arg) in a background thread and animates draw(ctx, t) until it finishes. */
static void *run_task(void *(*fn)(void *), void *arg, void (*draw)(void *ctx, uint32_t t), void *ctx)
{
    struct task t;
    plat_thread *th;

    t.fn = fn;
    t.arg = arg;
    t.ret = NULL;
    t.done = false;
    th = plat_thread_start(task_entry, &t);
    if (th == NULL)
        return fn(arg);   /* couldn't start a thread: run it inline, without animation */
    while (!t.done) {
        if (!ui_suspended) {
            draw(ctx, plat_now_ms());
            ui_present();
        }
        plat_sleep_ms(40);
    }
    plat_thread_join(th);
    return t.ret;
}

static void draw_busy(void *ctx, uint32_t t)
{
    struct busy_view *v = (struct busy_view *)ctx;
    ui_draw_busy(v->title, v->message[0] ? v->message : NULL, t);
}

static void *run_busy(const char *title, const char *message, void *(*fn)(void *), void *arg)
{
    struct busy_view v;

    v.title = title;
    v.message[0] = '\0';
    if (message)
        snprintf(v.message, sizeof(v.message), "%s", message);
    return run_task(fn, arg, draw_busy, &v);
}

/* ------------------------------------------------------------ messages */

static void message(int icon, const char *title, const char *body)
{
    ui_draw_message(icon, title, body);
    ui_present();
    while (!(wait_button() & (A_FLAG | B_FLAG)))
        ;
}

/* ------------------------------------------------------------ host state */

static const char *app_name(int id)
{
    int i;

    for (i = 0; i < app_count; i++)
        if (apps[i].id == id)
            return apps[i].name;
    return NULL;
}

static struct ui_host host_view(void)
{
    static char detail[220], running[80];
    struct ui_host h;

    h.name = NULL;
    h.address = cfg.host;
    h.state = HOST_UNKNOWN;
    h.running_app = NULL;
    h.detail = NULL;
    if (!cfg.host[0])
        return h;
    if (!server_ok) {
        h.state = HOST_OFFLINE;
        h.detail = server_error[0] ? server_error : NULL;
        return h;
    }
    h.name = server.hostname;
    h.state = server.paired ? HOST_PAIRED : HOST_UNPAIRED;
    if (server.current_game) {
        const char *n = app_name(server.current_game);
        snprintf(running, sizeof(running), "%s", n ? n : "A game or app");
        h.running_app = running;
    }
    snprintf(detail, sizeof(detail), "%s %s", server.is_nvidia ? "GeForce Experience" : "Sunshine",
             server.app_version);
    h.detail = detail;
    return h;
}

static void *refresh_task(void *arg)
{
    (void)arg;
    gs_server_free(&server);
    memset(&server, 0, sizeof(server));
    server_ok = gs_server_info(&server, cfg.host);
    if (!server_ok)
        snprintf(server_error, sizeof(server_error), "%s", gs_error());
    return NULL;
}

static void refresh_server(void)
{
    char title[96];

    if (!cfg.host[0]) {
        server_ok = false;
        return;
    }
    snprintf(title, sizeof(title), "Connecting to %s", cfg.host);
    run_busy(title, "Checking the host's status...", refresh_task, NULL);
}

/* ------------------------------------------------------------ IP editor */

static bool edit_ip(char *ip, size_t len)
{
    int o[4] = { 192, 168, 1, 2 }, sel = 0;

    sscanf(ip, "%d.%d.%d.%d", &o[0], &o[1], &o[2], &o[3]);
    for (;;) {
        int b;

        ui_draw_ip_editor(o, sel);
        ui_present();
        b = wait_button();
        if (b & LEFT_FLAG) sel = (sel + 3) % 4;
        if (b & RIGHT_FLAG) sel = (sel + 1) % 4;
        if (b & UP_FLAG) o[sel] = (o[sel] + 1) & 255;
        if (b & DOWN_FLAG) o[sel] = (o[sel] + 255) & 255;
        if (b & RB_FLAG) o[sel] = (o[sel] + 10) & 255;
        if (b & LB_FLAG) o[sel] = (o[sel] + 246) & 255;
        if (b & B_FLAG)
            return false;
        if (b & A_FLAG) {
            snprintf(ip, len, "%d.%d.%d.%d", o[0], o[1], o[2], o[3]);
            return true;
        }
    }
}

/* ------------------------------------------------------------ settings */

static const int resolutions[][2] = { { 854, 480 }, { 1280, 720 }, { 1920, 1080 } };

enum { SET_RES, SET_FPS, SET_BITRATE, SET_FAST, SET_RENDERER, SET_VSYNC, SET_SOPS, SET_AUDIO, SET_BENCH, SET_COUNT };

static void settings_menu(void)
{
    int sel = 0;

    for (;;) {
        static const char *labels[SET_COUNT] = {
            "Resolution", "Frame rate", "Bitrate", "Fast decode", "Renderer", "V-Sync",
            "Optimize game settings", "Play audio on PC", "Decoder benchmark",
        };
        static const char *helps[SET_COUNT] = {
            "The host renders and encodes at this size. 854x480 decodes about twice as fast as "
            "1280x720; the GPU scales it to fit the TV.",
            "30 FPS is the reliable choice. 60 FPS needs fast decode and a low resolution or bitrate.",
            "Higher looks better but costs decode time. 5-8 Mbps suits 720p.",
            "Skips H.264's deblocking filter: much faster, slightly blockier picture.",
            "GPU: the Xenos GPU converts and scales each frame (fastest). CPU modes are fallbacks if "
            "the GPU path misbehaves.",
            "Wait for the TV's refresh before showing a frame (no tearing). Off gives slightly lower latency.",
            "Lets the host change its resolution and game settings to match the stream.",
            "Also play the game's audio on the host PC's speakers.",
            "Decodes the recording of your first stream with and without the console's vector "
            "unit and shows the time per frame. Takes about a minute.",
        };
        char val[SET_COUNT][48];
        struct ui_setting rows[SET_COUNT];
        int map[SET_COUNT];   /* row -> setting id */
        int b, i, n = 0, res = 0;

        for (i = 0; i < 3; i++)
            if (resolutions[i][0] == cfg.width && resolutions[i][1] == cfg.height)
                res = i;
        snprintf(val[SET_RES], 48, "%dx%d", cfg.width, cfg.height);
        snprintf(val[SET_FPS], 48, "%d FPS", cfg.fps);
        snprintf(val[SET_BITRATE], 48, "%d Mbps", cfg.bitrate_kbps / 1000);
        snprintf(val[SET_FAST], 48, "%s", cfg.fast_decode ? "On" : "Off");
        snprintf(val[SET_RENDERER], 48, "%s", cfg.renderer == RENDERER_GPU ? "GPU" :
                 cfg.renderer == RENDERER_CPU ? "CPU" : "CPU + colour matrix");
        snprintf(val[SET_VSYNC], 48, "%s", cfg.vsync ? "On" : "Off");
        snprintf(val[SET_SOPS], 48, "%s", cfg.sops ? "On" : "Off");
        snprintf(val[SET_AUDIO], 48, "%s", cfg.local_audio ? "On" : "Off");
        snprintf(val[SET_BENCH], 48, "Press A");

        for (i = 0; i < SET_COUNT; i++) {
            if (i == SET_RENDERER && !plat_has_renderer_choice())
                continue;
            if (i == SET_BENCH && !plat_benchmark_available())
                continue;
            rows[n].label = labels[i];
            rows[n].value = val[i];
            rows[n].help = helps[i];
            map[n++] = i;
        }

        ui_draw_settings(rows, n, sel);
        ui_present();
        b = wait_button();
        if (b & UP_FLAG) sel = (sel + n - 1) % n;
        if (b & DOWN_FLAG) sel = (sel + 1) % n;
        if (b & B_FLAG)
            break;
        if (b & (LEFT_FLAG | RIGHT_FLAG | A_FLAG)) {
            int d = (b & LEFT_FLAG) ? -1 : 1;

            switch (map[sel]) {
            case SET_RES:
                res = (res + d + 3) % 3;
                cfg.width = resolutions[res][0];
                cfg.height = resolutions[res][1];
                break;
            case SET_FPS: cfg.fps = cfg.fps == 30 ? 60 : 30; break;
            case SET_BITRATE:
                cfg.bitrate_kbps += d * 1000;
                if (cfg.bitrate_kbps < 1000) cfg.bitrate_kbps = 1000;
                if (cfg.bitrate_kbps > 30000) cfg.bitrate_kbps = 30000;
                break;
            case SET_FAST: cfg.fast_decode = !cfg.fast_decode; break;
            case SET_RENDERER: cfg.renderer = (cfg.renderer + d + RENDERER_COUNT) % RENDERER_COUNT; break;
            case SET_VSYNC: cfg.vsync = !cfg.vsync; break;
            case SET_SOPS: cfg.sops = !cfg.sops; break;
            case SET_AUDIO: cfg.local_audio = !cfg.local_audio; break;
            case SET_BENCH:
                if (b & A_FLAG)
                    plat_run_benchmark();
                break;
            }
        }
    }
    config_save(&cfg);   /* without storage this just keeps them until reboot */
}

/* ------------------------------------------------------------ pairing */

struct pair_job {
    char pin[5];
    bool ok;
};

static void *pair_task(void *arg)
{
    struct pair_job *j = (struct pair_job *)arg;
    j->ok = gs_pair(&server, j->pin);
    return NULL;
}

static void draw_pair(void *ctx, uint32_t t)
{
    struct pair_job *j = (struct pair_job *)ctx;
    struct ui_host h = host_view();
    ui_draw_pair(&h, j->pin, t);
}

static void do_pair(void)
{
    struct pair_job job;
    unsigned char r[4];
    int i;

    crypto_random(r, sizeof(r));
    for (i = 0; i < 4; i++)
        job.pin[i] = (char)('0' + r[i] % 10);
    job.pin[4] = '\0';
    job.ok = false;

    run_task(pair_task, &job, draw_pair, &job);
    if (job.ok)
        message(ICON_CHECK, "Paired", "This console can now stream from the host. You won't need to pair again.");
    else
        message(ICON_ERROR, "Pairing failed", gs_error());
}

/* ------------------------------------------------------------ streaming */

static volatile bool connection_terminated;
static volatile int termination_error;
static char stage_text[96];

/* Moonlight's log lines go to the platform's debug output, not the screen. */
static void cl_log(const char *fmt, ...)
{
    char line[512];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    plat_log(line);
}

static void cl_stage_starting(int stage)
{
    cl_log("Stage starting: %s\n", LiGetStageName(stage));
    snprintf(stage_text, sizeof(stage_text), "Starting %s...", LiGetStageName(stage));
    /* From here on the video renderer takes over the screen. */
    if (stage >= STAGE_VIDEO_STREAM_INIT)
        ui_suspended = true;
}

static void cl_stage_failed(int stage, int err)
{
    cl_log("Stage failed: %s (error %d)\n", LiGetStageName(stage), err);
    snprintf(stage_text, sizeof(stage_text), "%s failed (error %d)", LiGetStageName(stage), err);
}

static void cl_connection_terminated(int err)
{
    cl_log("Connection terminated (error %d)\n", err);
    termination_error = err;
    connection_terminated = true;
}

struct launch_job {
    int app_id;
    STREAM_CONFIGURATION sc;
    bool ok;
};

static void *launch_task(void *arg)
{
    struct launch_job *j = (struct launch_job *)arg;
    j->ok = gs_launch(&server, &j->sc, j->app_id, cfg.sops, cfg.local_audio);
    return NULL;
}

struct connect_job {
    STREAM_CONFIGURATION *sc;
    CONNECTION_LISTENER_CALLBACKS *cl;
    DECODER_RENDERER_CALLBACKS *dr;
    AUDIO_RENDERER_CALLBACKS *ar;
    int ret;
};

static void *connect_task(void *arg)
{
    struct connect_job *j = (struct connect_job *)arg;
    j->ret = LiStartConnection(&server.info, j->sc, j->cl, j->dr, j->ar, NULL, 0, NULL, 0);
    return NULL;
}

static void draw_connecting(void *ctx, uint32_t t)
{
    ui_draw_busy((const char *)ctx, stage_text, t);
}

static void show_stream_stats(void)
{
    struct video_stats vs;
    struct ui_stat st[6];
    char title[64];
    int n = 0;

    video_get_stats(&vs);
    if (connection_terminated && termination_error != ML_ERROR_GRACEFUL_TERMINATION)
        snprintf(title, sizeof(title), "Connection lost (error %d)", termination_error);
    else
        snprintf(title, sizeof(title), "Stream ended");

    st[n].label = "Frames received / decoded / shown";
    snprintf(st[n++].value, 64, "%u / %u / %u", vs.received, vs.decoded, vs.rendered);
    st[n].label = "Dropped (decoder too slow)";
    snprintf(st[n++].value, 64, "%u", vs.dropped);
    st[n].label = "Decode time per frame";
    snprintf(st[n++].value, 64, "%.1f ms (%d threads)", vs.avg_decode_us / 1000.0, vs.decode_threads);
    st[n].label = "Render time per frame";
    snprintf(st[n++].value, 64, "%.1f ms (%s%s)", vs.avg_render_us / 1000.0, video_renderer_name(vs.renderer),
             vs.renderer == RENDERER_GPU && cfg.vsync ? ", incl. v-sync" : "");
    st[n].label = "Video";
    snprintf(st[n++].value, 64, "%dx%d @ %d FPS on %dx%d", cfg.width, cfg.height, cfg.fps, vs.display_w, vs.display_h);
    st[n].label = "Frame budget";
    snprintf(st[n++].value, 64, "%.1f ms", 1000.0 / cfg.fps);

    ui_draw_stats(title, st, n, "The app keeps running on the PC. Use \"Quit running app\" to close it.");
    ui_present();
    while (!(wait_button() & (A_FLAG | B_FLAG)))
        ;
}

static void stream(int app_id, const char *app_title)
{
    struct launch_job lj;
    struct connect_job cj;
    CONNECTION_LISTENER_CALLBACKS cl;
    DECODER_RENDERER_CALLBACKS dr;
    AUDIO_RENDERER_CALLBACKS ar;
    struct video_options vo;
    char title[128], msg[200];

    memset(&lj, 0, sizeof(lj));
    LiInitializeStreamConfiguration(&lj.sc);
    lj.app_id = app_id;
    lj.sc.width = cfg.width;
    lj.sc.height = cfg.height;
    lj.sc.fps = cfg.fps;
    lj.sc.bitrate = cfg.bitrate_kbps;
    lj.sc.packetSize = cfg.packet_size;
    lj.sc.streamingRemotely = STREAM_CFG_AUTO;
    lj.sc.audioConfiguration = AUDIO_CONFIGURATION_STEREO;
    lj.sc.supportedVideoFormats = VIDEO_FORMAT_H264;
    lj.sc.clientRefreshRateX100 = 6000;
    lj.sc.colorSpace = COLORSPACE_REC_601;
    lj.sc.colorRange = COLOR_RANGE_LIMITED;
    /* Video decryption would cost CPU time we don't have; audio is cheap. */
    lj.sc.encryptionFlags = ENCFLG_AUDIO;

    snprintf(title, sizeof(title), "Starting %s", app_title);
    snprintf(msg, sizeof(msg), "%dx%d at %d FPS, %d Mbps", cfg.width, cfg.height, cfg.fps, cfg.bitrate_kbps / 1000);
    run_busy(title, msg, launch_task, &lj);
    if (!lj.ok) {
        message(ICON_ERROR, "Couldn't start the app", gs_error());
        return;
    }

    LiInitializeConnectionCallbacks(&cl);
    cl.stageStarting = cl_stage_starting;
    cl.stageFailed = cl_stage_failed;
    cl.connectionTerminated = cl_connection_terminated;
    cl.logMessage = cl_log;
    cl.rumble = input_rumble;
    memset(&vo, 0, sizeof(vo));
    vo.fast_decode = cfg.fast_decode;
    vo.renderer = cfg.renderer;
    vo.vsync = cfg.vsync;
    video_set_options(&vo);
    video_init_callbacks(&dr);
    audio_init_callbacks(&ar);

    connection_terminated = false;
    ui_suspended = false;
    snprintf(stage_text, sizeof(stage_text), "Connecting...");
    cj.sc = &lj.sc;
    cj.cl = &cl;
    cj.dr = &dr;
    cj.ar = &ar;
    cj.ret = -1;
    snprintf(title, sizeof(title), "Connecting to %s", server.hostname[0] ? server.hostname : server.address);
    run_task(connect_task, &cj, draw_connecting, title);

    if (cj.ret != 0) {
        ui_suspended = false;
        snprintf(msg, sizeof(msg), "%s\n\nCheck that UDP ports 47998-48010 aren't blocked by the PC's firewall.",
                 stage_text);
        message(ICON_ERROR, "Couldn't connect", msg);
        return;
    }

    input_stream_start();
    while (!connection_terminated && !input_quit_requested())
        plat_sleep_ms(50);
    input_stream_stop();
    LiStopConnection();
    ui_suspended = false;

    show_stream_stats();
}

/* ------------------------------------------------------------ apps */

static volatile bool art_cancel;
static volatile int art_version;

static void *art_loader(void *arg)
{
    int i, tw, th;
    (void)arg;

    ui_art_size(&tw, &th);
    for (i = 0; i < app_count && !art_cancel; i++) {
        unsigned char *png;
        size_t len;
        uint32_t *scaled;

        if (app_art[i])
            continue;
        if (!gs_app_art(&server, apps[i].id, &png, &len))
            continue;
        scaled = plat_decode_art(png, len, tw, th);
        free(png);
        if (scaled && !art_cancel) {
            app_art[i] = scaled;
            art_version++;
        } else {
            free(scaled);
        }
        plat_sleep_ms(0);
    }
    return NULL;
}

static void *applist_task(void *arg)
{
    int *n = (int *)arg;
    *n = gs_applist(&server, apps, MAX_APPS);
    return NULL;
}

static void clear_apps(void)
{
    int i;

    for (i = 0; i < MAX_APPS; i++) {
        free(app_art[i]);
        app_art[i] = NULL;
    }
    app_count = 0;
}

static void draw_apps(int sel)
{
    struct ui_app views[MAX_APPS];
    struct ui_host h = host_view();
    int i;

    for (i = 0; i < app_count; i++) {
        views[i].name = apps[i].name;
        views[i].art = app_art[i];
        views[i].running = server.current_game == apps[i].id;
    }
    ui_draw_apps(&h, views, app_count, sel, plat_now_ms());
    ui_present();
}

static void stop_art_loader(plat_thread **loader)
{
    if (*loader) {
        art_cancel = true;
        plat_thread_join(*loader);
        *loader = NULL;
    }
}

static void app_menu(void)
{
    plat_thread *loader = NULL;
    int n = -1, sel = 0, seen_version, cols = ui_apps_columns();
    char title[96];

    snprintf(title, sizeof(title), "Loading apps from %s", server.hostname[0] ? server.hostname : server.address);
    clear_apps();
    run_busy(title, NULL, applist_task, &n);
    if (n < 0) {
        message(ICON_ERROR, "Couldn't load the app list", gs_error());
        return;
    }
    app_count = n;
    for (sel = 0; sel < app_count; sel++)
        if (apps[sel].id == server.current_game)
            break;
    if (sel >= app_count)
        sel = 0;

    art_cancel = false;
    art_version = 0;
    loader = plat_thread_start(art_loader, NULL);
    seen_version = -1;

    for (;;) {
        int b;

        if (seen_version != art_version) {
            seen_version = art_version;
            draw_apps(sel);
        }
        b = input_menu_pressed();
        if (b == 0) {
            plat_sleep_ms(16);
            continue;
        }
        if (app_count > 0) {
            if ((b & LEFT_FLAG) && sel > 0) sel--;
            if ((b & RIGHT_FLAG) && sel < app_count - 1) sel++;
            if ((b & UP_FLAG) && sel - cols >= 0) sel -= cols;
            if ((b & DOWN_FLAG) && sel + cols < app_count) sel += cols;
        }
        if (b & B_FLAG)
            break;
        if ((b & X_FLAG) && app_count > 0 && server.current_game == apps[sel].id) {
            stop_art_loader(&loader);
            if (gs_quit(&server))
                server.current_game = 0;
            else
                message(ICON_ERROR, "Couldn't quit the app", gs_error());
        } else if ((b & A_FLAG) && app_count > 0) {
            if (server.current_game && server.current_game != apps[sel].id) {
                char m[200];
                snprintf(m, sizeof(m), "%s is still running on the host. Select it and press X to quit it first.",
                         app_name(server.current_game) ? app_name(server.current_game) : "Another app");
                message(ICON_INFO, "Another app is running", m);
            } else {
                stop_art_loader(&loader);
                stream(apps[sel].id, apps[sel].name);
                refresh_task(NULL);   /* the host's running app may have changed */
            }
        }
        seen_version = -1;   /* redraw after any input */
    }
    stop_art_loader(&loader);
}

/* ------------------------------------------------------------ main menu */

static void *quit_task(void *arg)
{
    bool *ok = (bool *)arg;
    *ok = gs_quit(&server);
    return NULL;
}

static void *unpair_task(void *arg)
{
    bool *ok = (bool *)arg;
    *ok = gs_unpair(&server);
    return NULL;
}

static void set_item(struct ui_menu_item *it, const char *title, const char *subtitle, int icon, bool enabled)
{
    it->title = title;
    it->subtitle = subtitle;
    it->icon = icon;
    it->enabled = enabled;
}

static void main_menu(void)
{
    enum { N_ITEMS = 7 };
    int sel = 0;

    if (cfg.host[0])
        refresh_server();

    for (;;) {
        char sub[N_ITEMS][160];
        struct ui_host h = host_view();
        bool paired = server_ok && server.paired;
        const char *running = server_ok && server.current_game ? (h.running_app ? h.running_app : "An app") : NULL;
        struct ui_menu_item items[N_ITEMS];
        int b;

        set_item(&items[0], "Start streaming", sub[0], ICON_PLAY, paired);
        set_item(&items[1], "Pair with host", sub[1], ICON_LINK, server_ok && !server.paired && have_identity);
        set_item(&items[2], "Host address", sub[2], ICON_MONITOR, true);
        set_item(&items[3], "Settings", sub[3], ICON_GEAR, true);
        set_item(&items[4], "Quit running app", sub[4], ICON_STOP, paired && running != NULL);
        set_item(&items[5], "Refresh", sub[5], ICON_REFRESH, cfg.host[0] != 0);
        set_item(&items[6], "Unpair", sub[6], ICON_UNLINK, paired);

        if (paired)
            snprintf(sub[0], 160, "Choose a game or app on %s", h.name && h.name[0] ? h.name : cfg.host);
        else
            snprintf(sub[0], 160, "%s", !cfg.host[0] ? "Set the host address first" :
                     server_ok ? "Pair with the host first" : "Host not reachable");
        snprintf(sub[1], 160, "%s", paired ? "Already paired" : "Enter a PIN in Sunshine on your PC");
        snprintf(sub[2], 160, "%s", cfg.host[0] ? cfg.host : "Not set");
        snprintf(sub[3], 160, "%dx%d \xC2\xB7 %d FPS \xC2\xB7 %d Mbps%s", cfg.width, cfg.height, cfg.fps,
                 cfg.bitrate_kbps / 1000, !plat_has_renderer_choice() ? "" :
                 cfg.renderer == RENDERER_GPU ? " \xC2\xB7 GPU" : " \xC2\xB7 CPU");
        if (running)
            snprintf(sub[4], 160, "%s is running", running);
        else
            snprintf(sub[4], 160, "Nothing is running");
        snprintf(sub[5], 160, "Check the host's status again");
        snprintf(sub[6], 160, "Forget this console on the host");

        ui_draw_main(&h, items, N_ITEMS, sel, plat_console_ip(), plat_now_ms());
        ui_present();

        b = wait_button();
        if (b & UP_FLAG) sel = (sel + N_ITEMS - 1) % N_ITEMS;
        if (b & DOWN_FLAG) sel = (sel + 1) % N_ITEMS;
        if (b & Y_FLAG) {
            refresh_server();
            continue;
        }
        if (!(b & A_FLAG))
            continue;
        if (!items[sel].enabled) {
            message(ICON_INFO, items[sel].title, items[sel].subtitle);
            continue;
        }

        switch (sel) {
        case 0:
            app_menu();
            break;
        case 1:
            do_pair();
            refresh_server();
            break;
        case 2:
            if (edit_ip(cfg.host, sizeof(cfg.host))) {
                config_save(&cfg);
                clear_apps();
                refresh_server();
            }
            break;
        case 3:
            settings_menu();
            break;
        case 4: {
            bool ok = false;
            run_busy("Quitting the app", NULL, quit_task, &ok);
            if (!ok)
                message(ICON_ERROR, "Couldn't quit the app", gs_error());
            refresh_server();
            break;
        }
        case 5:
            refresh_server();
            break;
        case 6: {
            bool ok = false;
            run_busy("Unpairing", NULL, unpair_task, &ok);
            message(ok ? ICON_CHECK : ICON_ERROR, ok ? "Unpaired" : "Unpair failed",
                    ok ? "Pair again to stream from this host." : gs_error());
            refresh_server();
            break;
        }
        }
    }
}

/* ------------------------------------------------------------ startup */

struct identity_job {
    bool ok;
};

static void identity_status(const char *msg)
{
    (void)msg;   /* shown through the splash's fixed text instead */
}

static void *identity_task(void *arg)
{
    struct identity_job *j = (struct identity_job *)arg;
    j->ok = crypto_load_identity(&identity, identity_status);
    return NULL;
}

static void draw_splash_task(void *ctx, uint32_t t)
{
    ui_draw_splash((const char *)ctx, t);
}

void app_main(void)
{
    struct identity_job ij;

    config_load(&cfg);
    plat_fixup_config(&cfg);

    if (!crypto_init()) {
        message(ICON_ERROR, "Crypto init failed", "The random number generator couldn't be seeded.");
        for (;;)
            plat_sleep_ms(1000);
    }
    ij.ok = false;
    run_task(identity_task, &ij, draw_splash_task, (void *)"Preparing this console's client certificate...");
    have_identity = ij.ok;
    if (have_identity)
        gs_init(&identity);
    else
        message(ICON_ERROR, "No client certificate", "The certificate couldn't be created, so pairing isn't possible.");

    main_menu();
}
