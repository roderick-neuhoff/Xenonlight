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
#include "discover.h"
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
    /* A sleeping or switched-off PC would keep a blocking connect waiting
     * for many seconds (minutes on lwIP); a short probe answers quickly. */
    if (!discover_probe(cfg.host, 2000)) {
        server_ok = false;
        snprintf(server_error, sizeof(server_error),
                 "No answer from %s. Is the PC on, and is Sunshine running?", cfg.host);
        return NULL;
    }
    server_ok = gs_server_info(&server, cfg.host);
    if (!server_ok)
        snprintf(server_error, sizeof(server_error), "%s", gs_error());
    return NULL;
}

/* Remembers the host's MAC address (paired hosts report it) for Wake-on-LAN. */
static void learn_mac(const char *mac)
{
    if (wol_mac_valid(mac) && strcmp(cfg.host_mac, mac) != 0) {
        snprintf(cfg.host_mac, sizeof(cfg.host_mac), "%s", mac);
        config_save(&cfg);
    }
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
    if (server_ok)
        learn_mac(server.mac);
}

/* ------------------------------------------------------------ Wake-on-LAN */

#define WAKE_TIMEOUT_MS 60000
#define WAKE_RESEND_MS  10000

struct wake_job {
    uint32_t start;
    volatile bool cancel;
    bool sent, awake;
};

static void *wake_task(void *arg)
{
    struct wake_job *j = (struct wake_job *)arg;
    uint32_t last_send = plat_now_ms();

    j->sent = wol_send(cfg.host_mac, cfg.host);
    while (j->sent && !j->cancel && plat_now_ms() - j->start < WAKE_TIMEOUT_MS) {
        if (plat_now_ms() - last_send >= WAKE_RESEND_MS) {
            wol_send(cfg.host_mac, cfg.host);
            last_send = plat_now_ms();
        }
        /* A short probe first: a full serverinfo request to a sleeping PC
         * would sit in connect() for many seconds. */
        if (discover_probe(cfg.host, 1000)) {
            refresh_task(NULL);
            if (server_ok) {
                j->awake = true;
                break;
            }
        }
        plat_sleep_ms(500);
    }
    return NULL;
}

static void draw_wake(void *ctx, uint32_t t)
{
    struct wake_job *j = (struct wake_job *)ctx;
    char title[96], msg[160];

    if (input_menu_pressed() & B_FLAG)
        j->cancel = true;
    snprintf(title, sizeof(title), "Waking %s", cfg.host);
    if (j->cancel)
        snprintf(msg, sizeof(msg), "Stopping...");
    else
        snprintf(msg, sizeof(msg), "Sent the wake-up packet. Waiting for the PC to start (%u s). B stops.",
                 (unsigned)((t - j->start) / 1000));
    ui_draw_busy(title, msg, t);
}

static void wake_host(void)
{
    struct wake_job j;

    memset(&j, 0, sizeof(j));
    j.start = plat_now_ms();
    run_task(wake_task, &j, draw_wake, &j);
    if (j.awake) {
        learn_mac(server.mac);
        return;
    }
    if (!j.sent)
        message(ICON_ERROR, "Couldn't send Wake-on-LAN", "The console couldn't send the wake-up packet.");
    else if (!j.cancel)
        message(ICON_ERROR, "The PC didn't wake up",
                "Wake-on-LAN has to be enabled in the PC's BIOS and in its network adapter's settings, and "
                "most PCs only wake over Ethernet, not Wi-Fi.");
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

enum { SET_RES, SET_FPS, SET_BITRATE, SET_FAST, SET_RENDERER, SET_VSYNC, SET_GPU, SET_SOPS, SET_SURROUND, SET_AUDIO, SET_OVERLAY, SET_BENCH,
       SET_COUNT };

static const char *gpu_filter_names[3] = { "Off", "Deblock", "Deblock + sharpen" };

static void settings_menu(void)
{
    int sel = 0;

    for (;;) {
        static const char *labels[SET_COUNT] = {
            "Resolution", "Frame rate", "Bitrate", "Fast decode", "Renderer", "V-Sync", "GPU picture",
            "Optimize game settings", "Audio", "Play audio on PC", "Performance overlay", "Decoder benchmark",
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
            "With fast decode, the GPU smooths the block edges the decoder skipped, at no CPU cost. "
            "Sharpen also crisps up lower resolutions when they're scaled to the TV.",
            "Lets the host change its resolution and game settings to match the stream.",
            "5.1 surround needs a receiver or TV that takes it over HDMI or optical; the console mixes "
            "it down for stereo TVs. It costs a little more CPU time than stereo.",
            "Also play the game's audio on the host PC's speakers.",
            "Shows decode time, frame rate, bitrate and network round trip over the stream from the start. "
            "Back + LB + RB + X shows or hides it at any time.",
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
        snprintf(val[SET_GPU], 48, "%s", gpu_filter_names[cfg.gpu_filter >= 0 && cfg.gpu_filter <= 2 ? cfg.gpu_filter : 1]);
        snprintf(val[SET_SOPS], 48, "%s", cfg.sops ? "On" : "Off");
        snprintf(val[SET_AUDIO], 48, "%s", cfg.local_audio ? "On" : "Off");
        snprintf(val[SET_SURROUND], 48, "%s", cfg.surround ? "5.1 surround" : "Stereo");
        snprintf(val[SET_OVERLAY], 48, "%s", cfg.overlay ? "On" : "Off");
        snprintf(val[SET_BENCH], 48, "Press A");

        for (i = 0; i < SET_COUNT; i++) {
            if (i == SET_RENDERER && !plat_has_renderer_choice())
                continue;
            if (i == SET_BENCH && !plat_benchmark_available())
                continue;
            if (i == SET_GPU && !plat_has_gpu_filter())
                continue;
            if (i == SET_OVERLAY && !video_has_overlay())
                continue;
            if (i == SET_SURROUND && !audio_has_surround())
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
            case SET_GPU: cfg.gpu_filter = (cfg.gpu_filter + d + 3) % 3; break;
            case SET_SOPS: cfg.sops = !cfg.sops; break;
            case SET_AUDIO: cfg.local_audio = !cfg.local_audio; break;
            case SET_OVERLAY: cfg.overlay = !cfg.overlay; break;
            case SET_SURROUND: cfg.surround = !cfg.surround; break;
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

/* ------------------------------------------------------------ performance overlay */

#define OVERLAY_ROWS        7
#define OVERLAY_INTERVAL_MS 500

struct overlay {
    bool on;
    int toggles;                /* input_overlay_toggles() already handled */
    uint32_t last_ms;
    struct video_stats last;
    uint32_t last_fec_failed;
    canvas_t cv;
};

static uint32_t video_fec_failed(void)
{
    const RTP_VIDEO_STATS *rs = LiGetRTPVideoStats();
    return rs ? rs->packetCountFecFailed : 0;
}

static void overlay_draw(struct overlay *o)
{
    struct ui_overlay_row rows[OVERLAY_ROWS];
    struct video_stats vs;
    uint32_t now = plat_now_ms(), dt = now - o->last_ms, rtt, rtt_var, fec_failed = video_fec_failed();
    float budget = 1000.0f / cfg.fps, decode_ms, shown_fps;
    int n = 0;

    video_get_stats(&vs);
    if (dt == 0)
        dt = 1;
    decode_ms = vs.avg_decode_us / 1000.0f;
    shown_fps = (vs.rendered - o->last.rendered) * 1000.0f / dt;

    rows[n].label = "Frames received / shown";
    snprintf(rows[n].value, 48, "%.1f / %.1f FPS", (vs.received - o->last.received) * 1000.0f / dt, shown_fps);
    rows[n++].level = shown_fps < cfg.fps * 0.9f ? 1 : 0;
    rows[n].label = "Decode time";
    snprintf(rows[n].value, 48, "%.1f ms of %.1f", decode_ms, budget);
    rows[n++].level = decode_ms > budget ? 2 : decode_ms > budget * 0.8f ? 1 : 0;
    rows[n].label = "Render time";
    snprintf(rows[n].value, 48, "%.1f ms", vs.avg_render_us / 1000.0f);
    rows[n++].level = 0;
    rows[n].label = "Bitrate";
    snprintf(rows[n].value, 48, "%.1f Mbps", (vs.received_bytes - o->last.received_bytes) * 8.0f / 1000.0f / dt);
    rows[n++].level = 0;
    rows[n].label = "Dropped (decoder behind)";
    snprintf(rows[n].value, 48, "%u (%u total)", vs.dropped - o->last.dropped, vs.dropped);
    rows[n++].level = vs.dropped != o->last.dropped ? 1 : 0;
    rows[n].label = "Lost packets (unfixable)";
    snprintf(rows[n].value, 48, "%u", fec_failed - o->last_fec_failed);
    rows[n++].level = fec_failed != o->last_fec_failed ? 1 : 0;
    rows[n].label = "Network round trip";
    if (LiGetEstimatedRttInfo(&rtt, &rtt_var))
        snprintf(rows[n].value, 48, "%u ms \xC2\xB1 %u", rtt, rtt_var);
    else
        snprintf(rows[n].value, 48, "-");
    rows[n++].level = 0;

    ui_draw_overlay(&o->cv, rows, n);
    video_set_overlay(o->cv.px, o->cv.w, o->cv.h);
    o->last = vs;
    o->last_fec_failed = fec_failed;
    o->last_ms = now;
}

static void overlay_begin(struct overlay *o)
{
    int w, h;

    memset(o, 0, sizeof(*o));
    if (!video_has_overlay())
        return;
    ui_overlay_size(OVERLAY_ROWS, &w, &h);
    if (!cv_init(&o->cv, w, h)) {
        cv_free(&o->cv);
        return;
    }
    o->on = cfg.overlay;
    o->last_ms = plat_now_ms();
    video_get_stats(&o->last);
    o->last_fec_failed = video_fec_failed();
}

/* Called from the stream loop: handles the toggle combo and redraws twice a second. */
static void overlay_tick(struct overlay *o)
{
    int t = input_overlay_toggles();

    if (o->cv.px == NULL)
        return;
    if (t != o->toggles) {
        if ((t - o->toggles) & 1) {
            o->on = !o->on;
            if (!o->on)
                video_set_overlay(NULL, 0, 0);
        }
        o->toggles = t;
    }
    if (plat_now_ms() - o->last_ms < OVERLAY_INTERVAL_MS)
        return;
    if (o->on) {
        overlay_draw(o);
    } else {
        /* Keep the baseline fresh so the rates are right as soon as it's shown. */
        video_get_stats(&o->last);
        o->last_fec_failed = video_fec_failed();
        o->last_ms = plat_now_ms();
    }
}

static void overlay_end(struct overlay *o)
{
    video_set_overlay(NULL, 0, 0);
    cv_free(&o->cv);
}

static void stream(int app_id, const char *app_title)
{
    struct launch_job lj;
    struct connect_job cj;
    CONNECTION_LISTENER_CALLBACKS cl;
    DECODER_RENDERER_CALLBACKS dr;
    AUDIO_RENDERER_CALLBACKS ar;
    struct video_options vo;
    struct overlay ov;
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
    lj.sc.audioConfiguration = cfg.surround && audio_has_surround() ? AUDIO_CONFIGURATION_51_SURROUND
                                                                     : AUDIO_CONFIGURATION_STEREO;
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
    vo.gpu_filter = cfg.gpu_filter;
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
    overlay_begin(&ov);
    while (!connection_terminated && !input_quit_requested()) {
        overlay_tick(&ov);
        plat_sleep_ms(50);
    }
    overlay_end(&ov);
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

/* ------------------------------------------------------------ host search */

struct found_host {
    char address[16];
    char name[64];
    char detail[160];
    char mac[18];
};

static struct found_host found[DISCOVER_MAX];
static int found_count;

struct scan_job {
    struct discover_progress progress;
    volatile bool naming;       /* scan done, asking the hosts for their names */
    bool ok;                    /* false: the console has no usable address */
};

static void *scan_task(void *arg)
{
    struct scan_job *j = (struct scan_job *)arg;
    char addrs[DISCOVER_MAX][16];
    int i, n;

    found_count = 0;
    n = discover_hosts(plat_console_ip(), addrs, DISCOVER_MAX, &j->progress);
    j->ok = n >= 0;
    j->naming = true;
    for (i = 0; i < n; i++) {
        struct found_host *f = &found[found_count++];
        struct gs_server s;

        memset(&s, 0, sizeof(s));
        memset(f, 0, sizeof(*f));
        memcpy(f->address, addrs[i], sizeof(f->address));   /* same size, NUL-terminated */
        if (gs_server_info(&s, f->address)) {
            snprintf(f->name, sizeof(f->name), "%s", s.hostname[0] ? s.hostname : "Gaming PC");
            snprintf(f->detail, sizeof(f->detail), "%.15s \xC2\xB7 %s %s \xC2\xB7 %s", addrs[i],
                     s.is_nvidia ? "GeForce Experience" : "Sunshine", s.app_version,
                     s.paired ? "Paired" : "Not paired yet");
            memcpy(f->mac, s.mac, sizeof(f->mac));
        } else {
            /* Something answers on the GameStream port but not like a host we can use. */
            memcpy(f->name, f->address, sizeof(f->address));
            snprintf(f->detail, sizeof(f->detail), "%s", gs_error());
        }
        gs_server_free(&s);
    }
    return NULL;
}

static void draw_scan(void *ctx, uint32_t t)
{
    struct scan_job *j = (struct scan_job *)ctx;
    char msg[160];

    if (input_menu_pressed() & B_FLAG)
        j->progress.cancel = true;
    if (j->progress.cancel && !j->naming)
        snprintf(msg, sizeof(msg), "Stopping...");
    else if (j->naming)
        snprintf(msg, sizeof(msg), "Found %d. Asking for names...", j->progress.found);
    else
        snprintf(msg, sizeof(msg), "Checked %d of %d addresses \xC2\xB7 found %d \xC2\xB7 B stops", j->progress.checked,
                 j->progress.total ? j->progress.total : 253, j->progress.found);
    ui_draw_busy("Searching for PCs running Sunshine", msg, t);
}

static bool scan_network(void)
{
    struct scan_job j;

    memset(&j, 0, sizeof(j));
    run_task(scan_task, &j, draw_scan, &j);
    return j.ok;
}

/* mac may be NULL or unknown; a new address forgets the old host's MAC. */
static bool set_host(const char *address, const char *mac)
{
    if (strcmp(cfg.host, address) != 0) {
        snprintf(cfg.host, sizeof(cfg.host), "%s", address);
        cfg.host_mac[0] = '\0';
        config_save(&cfg);
        clear_apps();
    }
    learn_mac(mac);
    return true;
}

/* Lets the user pick a host found on the network or type an address.
 * Returns true when cfg.host was chosen (it may be unchanged). */
static bool host_menu(void)
{
    int sel = 0;

    if (!scan_network()) {
        message(ICON_INFO, "Can't search this network",
                "The console's address isn't known, so enter the PC's address instead.");
        {
            char ip[64];

            snprintf(ip, sizeof(ip), "%s", cfg.host);
            return edit_ip(ip, sizeof(ip)) && set_host(ip, NULL);
        }
    }

    for (;;) {
        struct ui_menu_item items[DISCOVER_MAX + 2];
        char note[64], current[DISCOVER_MAX][200];
        int n = found_count + 2, i, b;

        for (i = 0; i < found_count; i++) {
            snprintf(current[i], sizeof(current[i]), "%s%s", found[i].detail,
                     !strcmp(found[i].address, cfg.host) ? " \xC2\xB7 Current host" : "");
            set_item(&items[i], found[i].name, current[i], ICON_MONITOR, true);
        }
        set_item(&items[found_count], "Search again", "Start Sunshine on the PC first if it isn't listed",
                 ICON_SEARCH, true);
        set_item(&items[found_count + 1], "Enter an address",
                 cfg.host[0] ? cfg.host : "Type the PC's IP address with the d-pad", ICON_EDIT, true);
        if (found_count == 0)
            snprintf(note, sizeof(note), "No PCs found");
        else
            snprintf(note, sizeof(note), "%d found", found_count);
        if (sel >= n)
            sel = n - 1;

        ui_draw_hosts(items, n, sel, note);
        ui_present();
        b = wait_button();
        if (b & UP_FLAG) sel = (sel + n - 1) % n;
        if (b & DOWN_FLAG) sel = (sel + 1) % n;
        if (b & B_FLAG)
            return false;
        if ((b & Y_FLAG) || ((b & A_FLAG) && sel == found_count)) {
            scan_network();
            sel = 0;
            continue;
        }
        if (!(b & A_FLAG))
            continue;
        if (sel < found_count)
            return set_host(found[sel].address, found[sel].mac);
        {
            char ip[64];

            snprintf(ip, sizeof(ip), "%s", cfg.host);
            if (edit_ip(ip, sizeof(ip)))
                return set_host(ip, NULL);
        }
    }
}


static void main_menu(void)
{
    enum { N_ITEMS = 7 };
    int sel = 0;

    /* First start: look for the PC instead of asking for its address. */
    if (cfg.host[0] || host_menu())
        refresh_server();

    for (;;) {
        char sub[N_ITEMS][160];
        struct ui_host h = host_view();
        bool paired = server_ok && server.paired;
        const char *running = server_ok && server.current_game ? (h.running_app ? h.running_app : "An app") : NULL;
        struct ui_menu_item items[N_ITEMS];
        /* Offline host with a known MAC: "Refresh" (still on Y) becomes "Wake PC". */
        bool can_wake = cfg.host[0] && !server_ok && wol_mac_valid(cfg.host_mac);
        int b;

        set_item(&items[0], "Start streaming", sub[0], ICON_PLAY, paired);
        set_item(&items[1], "Pair with host", sub[1], ICON_LINK, server_ok && !server.paired && have_identity);
        set_item(&items[2], "Host address", sub[2], ICON_MONITOR, true);
        set_item(&items[3], "Settings", sub[3], ICON_GEAR, true);
        set_item(&items[4], "Quit running app", sub[4], ICON_STOP, paired && running != NULL);
        if (can_wake)
            set_item(&items[5], "Wake PC", sub[5], ICON_POWER, true);
        else
            set_item(&items[5], "Refresh", sub[5], ICON_REFRESH, cfg.host[0] != 0);
        set_item(&items[6], "Unpair", sub[6], ICON_UNLINK, paired);

        if (paired)
            snprintf(sub[0], 160, "Choose a game or app on %s", h.name && h.name[0] ? h.name : cfg.host);
        else
            snprintf(sub[0], 160, "%s", !cfg.host[0] ? "Set the host address first" :
                     server_ok ? "Pair with the host first" : "Host not reachable");
        snprintf(sub[1], 160, "%s", paired ? "Already paired" : "Enter a PIN in Sunshine on your PC");
        snprintf(sub[2], 160, "%s", cfg.host[0] ? cfg.host : "Not set \xC2\xB7 search the network or enter it");
        snprintf(sub[3], 160, "%dx%d \xC2\xB7 %d FPS \xC2\xB7 %d Mbps%s", cfg.width, cfg.height, cfg.fps,
                 cfg.bitrate_kbps / 1000, !plat_has_renderer_choice() ? "" :
                 cfg.renderer == RENDERER_GPU ? " \xC2\xB7 GPU" : " \xC2\xB7 CPU");
        if (running)
            snprintf(sub[4], 160, "%s is running", running);
        else
            snprintf(sub[4], 160, "Nothing is running");
        if (can_wake)
            snprintf(sub[5], 160, "Send Wake-on-LAN to %s (Y checks again)", cfg.host_mac);
        else
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
            if (host_menu())
                refresh_server();
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
            if (can_wake)
                wake_host();
            else
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
