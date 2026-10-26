/*
 * Moonlight for Xbox 360 (libxenon): hardware bring-up and the platform layer
 * for the shared app (src/app.c).
 *
 * Hardware thread 0 runs a cooperative scheduler (compat/xthread) that hosts
 * the UI, lwIP, and every moonlight-common-c thread, so plat_thread_* are
 * coroutines. Video decode and presentation run on the other hardware threads
 * (see video.c).
 *
 * The UI (src/ui) is software-rendered into the scanout framebuffer.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <xenos/xenos.h>
#include <console/console.h>
#include <xenon_soc/xenon_power.h>
#include <xenon_sound/sound.h>
#include <usb/usbmain.h>
#include <diskio/ata.h>
#include <network/network.h>
#include <libfat/fat.h>
#include <time/time.h>

#include "../compat/xthread.h"
#include "../compat/xsock.h"
#include "config.h"
#include "gpu.h"
#include "hw.h"
#include "platform.h"
#include "video.h"
#include "ui/image.h"
#include "ui/ui.h"

static char console_ip[20];

/* ------------------------------------------------------------ platform layer */

uint32_t plat_now_ms(void)
{
    return (uint32_t)(xthread_now_us() / 1000);
}

void plat_sleep_ms(unsigned int ms)
{
    if (ms == 0)
        xthread_yield();
    else
        xthread_sleep_us(ms * 1000ull);
}

/* Coroutines on hardware thread 0: they run whenever the UI sleeps or their
 * network waits yield, so both make progress. */
plat_thread *plat_thread_start(void *(*fn)(void *), void *arg)
{
    return (plat_thread *)xthread_create("task", fn, arg, 512 * 1024);
}

void plat_thread_join(plat_thread *t)
{
    xthread_join((xthread_t *)t, NULL);
}

/*
 * PNG decoding and scaling take a few hundred ms per cover on Xenon. Doing it
 * on hardware thread 0 would freeze input and the network, so it runs on the
 * renderer's hardware thread, which is idle outside a stream.
 */
static struct {
    const unsigned char *data;
    size_t len;
    int tw, th;
    uint32_t *result;
    volatile int done;
} art_job;

static void art_decode_job(void)
{
    int w, h;
    uint32_t *full = image_decode_argb(art_job.data, art_job.len, &w, &h);

    art_job.result = full ? image_scale_cover(full, w, h, art_job.tw, art_job.th) : NULL;
    free(full);
    hw_sync();
    art_job.done = 1;
}

uint32_t *plat_decode_art(const unsigned char *data, size_t len, int tw, int th)
{
    art_job.data = data;
    art_job.len = len;
    art_job.tw = tw;
    art_job.th = th;
    art_job.result = NULL;
    art_job.done = 0;
    hw_sync();

    if (hw_start(HW_THREAD_RENDER, art_decode_job)) {
        while (!art_job.done)
            xthread_sleep_us(5000);
        while (hw_running(HW_THREAD_RENDER))
            xthread_yield();
    } else {
        art_decode_job();   /* thread busy: decode here instead */
    }
    return art_job.result;
}

const char *plat_console_ip(void)
{
    return console_ip;
}

void plat_fixup_config(struct config *cfg)
{
    if (!gpu_ready() && cfg->renderer == RENDERER_GPU)
        cfg->renderer = RENDERER_CPU;
}

bool plat_has_renderer_choice(void)
{
    return true;
}

bool plat_has_gpu_filter(void)
{
    return false;
}

bool plat_has_thread_choice(void)
{
    return false;
}

void plat_log(const char *line)
{
    (void)line;   /* the UI owns the screen; no console to print to */
}

bool plat_benchmark_available(void)
{
    return false;
}

void plat_run_benchmark(void)
{
}

/* ------------------------------------------------------------ startup */

static void usb_pump(void)
{
    usb_do_poll();
}

static void splash(const char *status)
{
    ui_draw_splash(status, plat_now_ms());
    ui_present();
}

int main(void)
{
    int fb_w, fb_h;
    uint32_t ip;

    xenos_init(VIDEO_MODE_AUTO);
    console_init();
    printf("Moonlight for Xbox 360 starting...\n");

    /* Full CPU speed; also brings up hardware threads 1-5 for video. */
    xenon_make_it_faster(XENON_SPEED_FULL);
    xenon_sound_init();

    video_get_display_size(&fb_w, &fb_h);
    if (!ui_init(fb_w, fb_h)) {
        printf("UI init failed (out of memory?)\n");
        for (;;)
            ;
    }
    /* The UI owns the screen from here; console output would scribble on it. */
    console_close();
    xthread_init();

    gpu_init();   /* if it fails, plat_fixup_config falls back to the CPU */

    splash("Looking for USB storage...");
    usb_init();
    for (int i = 0; i < 30; i++) {
        usb_do_poll();
        mdelay(100);
    }
    xenon_ata_init();
    fatInitDefault();

    splash("Connecting to the network (this can take 15 seconds)...");
    network_init();
    xsock_init();
    xthread_add_pump(xsock_pump, 0);
    xthread_add_pump(usb_pump, 2000);
    ip = xsock_local_ip();
    snprintf(console_ip, sizeof(console_ip), "%u.%u.%u.%u", ip >> 24, (ip >> 16) & 255, (ip >> 8) & 255, ip & 255);

    storage_init();
    app_main();
    return 0;
}
