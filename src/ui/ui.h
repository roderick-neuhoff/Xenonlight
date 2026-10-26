/*
 * Moonlight's screens, drawn into an offscreen canvas. Each ui_draw_* call
 * renders one complete frame; the platform layer then shows it with
 * ui_present(). Layout is designed for 1280x720 and scaled to the display.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "canvas.h"

enum ui_icon {
    ICON_PLAY, ICON_LINK, ICON_UNLINK, ICON_MONITOR, ICON_GEAR, ICON_STOP, ICON_REFRESH,
    ICON_INFO, ICON_ERROR, ICON_CHECK, ICON_SEARCH, ICON_EDIT, ICON_POWER
};

enum ui_host_state { HOST_UNKNOWN, HOST_CONNECTING, HOST_OFFLINE, HOST_UNPAIRED, HOST_PAIRED };

struct ui_host {
    const char *name;           /* host name, or NULL if unknown */
    const char *address;        /* IP address, or "" */
    int state;                  /* enum ui_host_state */
    const char *running_app;    /* NULL when nothing is running */
    const char *detail;         /* error text or server version, may be NULL */
};

struct ui_menu_item {
    const char *title;
    const char *subtitle;
    int icon;
    bool enabled;
};

struct ui_app {
    const char *name;
    const uint32_t *art;        /* ui_art_size() pixels, or NULL for a placeholder */
    bool running;
};

struct ui_setting {
    const char *label;
    const char *value;
    const char *help;
};

struct ui_stat {
    const char *label;
    char value[64];
};

/* One line of the performance overlay; level 0 normal, 1 warning, 2 bad. */
struct ui_overlay_row {
    const char *label;
    char value[48];
    int level;
};

bool ui_init(int width, int height);
canvas_t *ui_canvas(void);
/* Size that box art should be scaled to before passing it to ui_draw_apps. */
void ui_art_size(int *w, int *h);
/* Columns in the app grid, for d-pad navigation. */
int ui_apps_columns(void);

void ui_draw_splash(const char *status, uint32_t t_ms);
void ui_draw_main(const struct ui_host *host, const struct ui_menu_item *items, int n, int sel,
                  const char *console_ip, uint32_t t_ms);
void ui_draw_apps(const struct ui_host *host, const struct ui_app *apps, int n, int sel, uint32_t t_ms);
void ui_draw_settings(const struct ui_setting *rows, int n, int sel);
/* Host chooser: found hosts plus fixed actions, scrolling when they don't fit. */
void ui_draw_hosts(const struct ui_menu_item *items, int n, int sel, const char *note);
void ui_draw_ip_editor(const int octets[4], int sel);
void ui_draw_pair(const struct ui_host *host, const char *pin, uint32_t t_ms);
void ui_draw_busy(const char *title, const char *message, uint32_t t_ms);
void ui_draw_message(int icon, const char *title, const char *text);
void ui_draw_stats(const char *title, const struct ui_stat *stats, int n, const char *note);

/* Performance overlay, drawn at 1:1 into its own canvas (not the screen):
 * ui_overlay_size gives the canvas size for n rows. */
void ui_overlay_size(int n, int *w, int *h);
void ui_draw_overlay(canvas_t *dst, const struct ui_overlay_row *rows, int n);

/* Platform: shows the canvas on screen (present_xenon.c / the PC preview). */
void ui_present(void);
