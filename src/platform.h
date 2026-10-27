/*
 * What src/app.c needs from each build: libxenon (src/main.c) and the
 * Xbox 360 SDK build (xdk/src/main_xdk.cpp). Everything else (UI, GameStream
 * client, video/audio/input callbacks behind video.h/audio.h/input.h) has the
 * same interface on both.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "config.h"

#ifdef __cplusplus
extern "C" {
#endif

uint32_t plat_now_ms(void);
/* Sleeps (or, with 0, just lets other threads run). */
void plat_sleep_ms(unsigned int ms);

typedef struct plat_thread plat_thread;
/* Runs fn(arg) concurrently with the caller; NULL if it couldn't start. */
plat_thread *plat_thread_start(void *(*fn)(void *), void *arg);
void plat_thread_join(plat_thread *t);

/* Decodes PNG/JPEG box art and scales it to tw x th, preferably on a core
 * that isn't running the UI. Returns ARGB pixels or NULL. */
uint32_t *plat_decode_art(const unsigned char *data, size_t len, int tw, int th);

/* The console's IP address for display, e.g. "192.168.1.99". */
const char *plat_console_ip(void);

/* Adjusts loaded settings to what this build supports (e.g. renderer). */
void plat_fixup_config(struct config *cfg);

/* Whether the "Renderer" setting means anything on this build. */
bool plat_has_renderer_choice(void);

/* Whether the "GPU picture" setting (GPU deblocking/sharpening) exists on this build. */
bool plat_has_gpu_filter(void);

/* One line of debug output. The SDK build broadcasts it on the LAN (UDP port
 * 47555, see xdk/scripts/log_listener.py); libxenon drops it. */
void plat_log(const char *line);

/* Decoder benchmark on a recorded stream (SDK build only). The run shows its
 * own progress and result screens. */
bool plat_benchmark_available(void);
void plat_run_benchmark(void);

/* Runs the whole app (splash, identity, menus). Platform init must be done. */
void app_main(void);

#ifdef __cplusplus
}
#endif
