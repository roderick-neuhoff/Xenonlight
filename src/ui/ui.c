#include "ui.h"
#include "font.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------ theme */

#define C_BG_TOP     HEX(0x141C27)
#define C_BG_BOTTOM  HEX(0x07090D)
#define C_ACCENT     HEX(0x5B8DEF)
#define C_CARD       HEX(0x19212D)
#define C_CARD_SEL   HEX(0x223352)
#define C_BORDER     HEX(0x2A3544)
#define C_TEXT       HEX(0xEEF2F7)
#define C_TEXT_DIM   HEX(0x9BA7B6)
#define C_TEXT_FAINT HEX(0x5F6B7B)
#define C_GREEN      HEX(0x3DD68C)
#define C_YELLOW     HEX(0xF5B94A)
#define C_RED        HEX(0xF0605D)
#define C_BTN_A      HEX(0x5DB82E)
#define C_BTN_B      HEX(0xE0453A)
#define C_BTN_X      HEX(0x2E7FD9)
#define C_BTN_Y      HEX(0xE8B623)
#define C_SHADOW     ARGB(150, 0, 0, 0)

static canvas_t cv;     /* the frame being drawn */
static canvas_t bg;     /* pre-rendered background */
static float s;         /* design px -> screen px */
static float W, H;      /* design size (H is always 720) */

#define S(v) ((v) * s)
#define SI(v) ((int)((v) * s + 0.5f))

/* ------------------------------------------------------------ setup */

static void draw_background(void)
{
    int x, y;
    float gx = S(180), gy = S(-40), gr = S(760);

    cv_vgradient(&bg, 0, 0, bg.w, bg.h, C_BG_TOP, C_BG_BOTTOM);
    /* Soft accent glow from the top left. */
    for (y = 0; y < bg.h; y++) {
        for (x = 0; x < bg.w; x++) {
            float dx = x - gx, dy = y - gy;
            float d = sqrtf(dx * dx + dy * dy) / gr;
            if (d < 1.0f) {
                float a = (1.0f - d) * (1.0f - d) * 38.0f;
                bg.px[y * bg.w + x] = cv_mix(bg.px[y * bg.w + x], C_ACCENT, (unsigned)a);
            }
        }
    }
}

bool ui_init(int width, int height)
{
    if (!font_init())
        return false;
    if (!cv_init(&cv, width, height) || !cv_init(&bg, width, height))
        return false;
    s = height / 720.0f;
    H = 720.0f;
    W = width / s;
    draw_background();
    return true;
}

canvas_t *ui_canvas(void)
{
    return &cv;
}

#define TILE_W 150
#define TILE_H 200

#define TILE_GAP 22

void ui_art_size(int *w, int *h)
{
    *w = SI(TILE_W);
    *h = SI(TILE_H);
}

int ui_apps_columns(void)
{
    int cols = (int)((W - 128 + TILE_GAP) / (TILE_W + TILE_GAP));
    return cols < 1 ? 1 : cols;
}

/* ------------------------------------------------------------ helpers */

static void text(int face, float px, float x, float y, uint32_t color, const char *str)
{
    font_draw(&cv, face, S(px), SI(x), SI(y), color, str);
}

static void text_fit(int face, float px, float x, float y, float max_w, uint32_t color, const char *str)
{
    font_draw_fit(&cv, face, S(px), SI(x), SI(y), SI(max_w), color, str);
}

static float text_w(int face, float px, const char *str)
{
    return font_text_width(face, S(px), str) / s;
}

static void text_center(int face, float px, float cx, float y, uint32_t color, const char *str)
{
    text(face, px, cx - text_w(face, px, str) / 2, y, color, str);
}

static void text_right(int face, float px, float rx, float y, uint32_t color, const char *str)
{
    text(face, px, rx - text_w(face, px, str), y, color, str);
}

static int wrapped(int face, float px, float x, float y, float w, float line_h, uint32_t color, const char *str)
{
    return font_draw_wrapped(&cv, face, S(px), SI(x), SI(y), SI(w), S(line_h), color, str);
}

static int wrapped_lines(int face, float px, float w, const char *str)
{
    return font_draw_wrapped(NULL, face, S(px), 0, 0, SI(w), 0, 0, str);
}

static void rrect(float x, float y, float w, float h, float r, uint32_t c)
{
    cv_rrect(&cv, S(x), S(y), S(w), S(h), S(r), c);
}

static void card(float x, float y, float w, float h, float r, bool selected)
{
    if (selected)
        cv_shadow(&cv, S(x), S(y), S(w), S(h), S(r), S(22), WITH_ALPHA(C_ACCENT, 110));
    else
        cv_shadow(&cv, S(x), S(y + 4), S(w), S(h), S(r), S(16), C_SHADOW);
    rrect(x, y, w, h, r, selected ? C_CARD_SEL : C_CARD);
    cv_rrect_outline(&cv, S(x), S(y), S(w), S(h), S(r), selected ? S(2.5f) : S(1), selected ? C_ACCENT : C_BORDER);
}

static uint32_t state_color(int state)
{
    switch (state) {
    case HOST_PAIRED:   return C_GREEN;
    case HOST_UNPAIRED: return C_YELLOW;
    case HOST_OFFLINE:  return C_RED;
    default:            return C_TEXT_FAINT;
    }
}

static const char *state_text(int state)
{
    switch (state) {
    case HOST_PAIRED:     return "Paired";
    case HOST_UNPAIRED:   return "Not paired";
    case HOST_OFFLINE:    return "Offline";
    case HOST_CONNECTING: return "Connecting...";
    default:              return "No host set";
    }
}

/* ------------------------------------------------------------ icons */

struct icon_ctx { float cx, cy, k; };

static float crescent_sdf(float x, float y, const void *p)
{
    const struct icon_ctx *c = (const struct icon_ctx *)p;
    float ax = x - c->cx, ay = y - c->cy;
    float bx = x - (c->cx + c->k * 0.42f), by = y - (c->cy - c->k * 0.30f);
    float da = sqrtf(ax * ax + ay * ay) - c->k;
    float db = sqrtf(bx * bx + by * by) - c->k * 0.82f;
    return fmaxf(da, -db);
}

static void moon_logo(float cx, float cy, float r)
{
    struct icon_ctx k = { S(cx), S(cy), S(r) };
    cv_sdf(&cv, S(cx - r), S(cy - r), S(2 * r), S(2 * r), crescent_sdf, &k, C_ACCENT);
}

static float gear_sdf(float x, float y, const void *p)
{
    const struct icon_ctx *c = (const struct icon_ctx *)p;
    float dx = x - c->cx, dy = y - c->cy;
    float len = sqrtf(dx * dx + dy * dy);
    float teeth = cosf(8.0f * atan2f(dy, dx)) * 3.0f;
    float outer;

    teeth = teeth > 1 ? 1 : teeth < -1 ? -1 : teeth;
    outer = len - (c->k * 0.70f + c->k * 0.17f * teeth);
    return fmaxf(outer, -(len - c->k * 0.30f));
}

struct arc_ctx { float cx, cy, r, t, gap0, gap1; };

static float arc_sdf(float x, float y, const void *p)
{
    const struct arc_ctx *c = (const struct arc_ctx *)p;
    float dx = x - c->cx, dy = y - c->cy;
    float a = atan2f(dy, dx);

    if (a > c->gap0 && a < c->gap1) {
        float e0x = c->cx + cosf(c->gap0) * c->r, e0y = c->cy + sinf(c->gap0) * c->r;
        float e1x = c->cx + cosf(c->gap1) * c->r, e1y = c->cy + sinf(c->gap1) * c->r;
        float d0 = sqrtf((x - e0x) * (x - e0x) + (y - e0y) * (y - e0y));
        float d1 = sqrtf((x - e1x) * (x - e1x) + (y - e1y) * (y - e1y));
        return fminf(d0, d1) - c->t * 0.5f;
    }
    return fabsf(sqrtf(dx * dx + dy * dy) - c->r) - c->t * 0.5f;
}

/* Draws an icon centred at (cx, cy); k is its half-size in design px. */
static void icon(int which, float cx, float cy, float k, uint32_t color)
{
    float X = S(cx), Y = S(cy), K = S(k);
    struct icon_ctx ctx = { X, Y, K };

    switch (which) {
    case ICON_PLAY:
        cv_triangle(&cv, X - K * 0.45f, Y - K * 0.65f, X - K * 0.45f, Y + K * 0.65f, X + K * 0.7f, Y, color);
        break;
    case ICON_LINK:
    case ICON_UNLINK:
        cv_ring(&cv, X - K * 0.32f, Y, K * 0.42f, K * 0.17f, color);
        cv_ring(&cv, X + K * 0.32f, Y, K * 0.42f, K * 0.17f, color);
        if (which == ICON_UNLINK)
            cv_line(&cv, X - K * 0.75f, Y + K * 0.75f, X + K * 0.75f, Y - K * 0.75f, K * 0.17f, color);
        break;
    case ICON_MONITOR:
        cv_rrect_outline(&cv, X - K * 0.85f, Y - K * 0.65f, K * 1.7f, K * 1.1f, K * 0.15f, K * 0.16f, color);
        cv_line(&cv, X, Y + K * 0.45f, X, Y + K * 0.75f, K * 0.16f, color);
        cv_line(&cv, X - K * 0.4f, Y + K * 0.8f, X + K * 0.4f, Y + K * 0.8f, K * 0.16f, color);
        break;
    case ICON_GEAR:
        cv_sdf(&cv, X - K, Y - K, 2 * K, 2 * K, gear_sdf, &ctx, color);
        break;
    case ICON_STOP:
        cv_rrect(&cv, X - K * 0.55f, Y - K * 0.55f, K * 1.1f, K * 1.1f, K * 0.18f, color);
        break;
    case ICON_REFRESH: {
        struct arc_ctx a = { X, Y, K * 0.6f, K * 0.17f, -0.9f, -0.1f };
        float ex = X + cosf(-0.1f) * K * 0.6f, ey = Y + sinf(-0.1f) * K * 0.6f;
        cv_sdf(&cv, X - K, Y - K, 2 * K, 2 * K, arc_sdf, &a, color);
        cv_triangle(&cv, ex - K * 0.3f, ey, ex + K * 0.3f, ey, ex, ey + K * 0.35f, color);
        break;
    }
    case ICON_INFO:
    case ICON_ERROR: {
        const char *g = which == ICON_INFO ? "i" : "!";
        float px = k * 1.3f;
        text_center(FONT_BOLD, px, cx, cy + px * 0.36f, color, g);
        break;
    }
    case ICON_CHECK:
        cv_line(&cv, X - K * 0.5f, Y, X - K * 0.12f, Y + K * 0.4f, K * 0.2f, color);
        cv_line(&cv, X - K * 0.12f, Y + K * 0.4f, X + K * 0.55f, Y - K * 0.4f, K * 0.2f, color);
        break;
    case ICON_SEARCH:
        cv_ring(&cv, X - K * 0.18f, Y - K * 0.18f, K * 0.48f, K * 0.17f, color);
        cv_line(&cv, X + K * 0.2f, Y + K * 0.2f, X + K * 0.7f, Y + K * 0.7f, K * 0.22f, color);
        break;
    case ICON_POWER: {
        struct arc_ctx a = { X, Y + K * 0.05f, K * 0.6f, K * 0.17f, -2.25f, -0.89f };
        cv_sdf(&cv, X - K, Y - K, 2 * K, 2 * K, arc_sdf, &a, color);
        cv_line(&cv, X, Y - K * 0.75f, X, Y - K * 0.1f, K * 0.17f, color);
        break;
    }
    case ICON_EDIT:
        cv_line(&cv, X - K * 0.3f, Y + K * 0.3f, X + K * 0.5f, Y - K * 0.5f, K * 0.32f, color);
        cv_triangle(&cv, X - K * 0.72f, Y + K * 0.72f, X - K * 0.56f, Y + K * 0.1f, X - K * 0.1f, Y + K * 0.56f, color);
        break;
    }
}

static void spinner(float cx, float cy, float r, uint32_t t_ms)
{
    int head = (int)(t_ms / 85) % 10, i;

    for (i = 0; i < 10; i++) {
        float a = (float)i / 10.0f * 6.2831853f - 1.5707963f;
        int age = (head - i + 10) % 10;
        unsigned alpha = (unsigned)(255 - age * 22);
        cv_circle(&cv, S(cx + cosf(a) * r), S(cy + sinf(a) * r), S(r * 0.17f), WITH_ALPHA(C_ACCENT, alpha));
    }
}

/* ------------------------------------------------------------ chrome */

struct hint { const char *button; const char *label; };

static void keycap(float x, float baseline, const char *button, float *out_w)
{
    uint32_t color = 0;
    float cy = baseline - 6;

    if (!strcmp(button, "A")) color = C_BTN_A;
    else if (!strcmp(button, "B")) color = C_BTN_B;
    else if (!strcmp(button, "X")) color = C_BTN_X;
    else if (!strcmp(button, "Y")) color = C_BTN_Y;

    if (color) {
        cv_circle(&cv, S(x + 13), S(cy), S(13), color);
        text_center(FONT_BOLD, 15, x + 13, cy + 5.5f, HEX(0xFFFFFF), button);
        *out_w = 26;
    } else {
        float w = text_w(FONT_BOLD, 13, button) + 16;
        rrect(x, cy - 12, w, 24, 6, HEX(0x2C3646));
        text_center(FONT_BOLD, 13, x + w / 2, cy + 4.5f, C_TEXT, button);
        *out_w = w;
    }
}

static void footer(const struct hint *hints, int n, const char *right)
{
    float x = 64, y = H - 34;
    int i;

    cv_fill(&cv, 0, SI(H - 70), cv.w, 1, WITH_ALPHA(C_BORDER, 160));
    for (i = 0; i < n; i++) {
        float kw;
        keycap(x, y, hints[i].button, &kw);
        x += kw + 9;
        text(FONT_REGULAR, 17, x, y, C_TEXT_DIM, hints[i].label);
        x += text_w(FONT_REGULAR, 17, hints[i].label) + 30;
    }
    if (right)
        text_right(FONT_REGULAR, 15, W - 64, y, C_TEXT_FAINT, right);
}

static void header(const struct ui_host *host)
{
    moon_logo(84, 56, 19);
    text(FONT_BOLD, 30, 116, 67, C_TEXT, "Moonlight");
    text(FONT_REGULAR, 17, 116 + text_w(FONT_BOLD, 30, "Moonlight") + 10, 66, C_TEXT_DIM, "for Xbox 360");

    if (host) {
        char label[160];
        float tw, pw, px;

        if (host->state == HOST_UNKNOWN)
            snprintf(label, sizeof(label), "%s", state_text(host->state));
        else
            snprintf(label, sizeof(label), "%s  \xC2\xB7  %s",
                     host->name && host->name[0] ? host->name : host->address, state_text(host->state));
        tw = text_w(FONT_REGULAR, 16, label);
        pw = tw + 46;
        px = W - 64 - pw;
        rrect(px, 37, pw, 38, 19, WITH_ALPHA(C_CARD, 230));
        cv_rrect_outline(&cv, S(px), S(37), S(pw), S(38), S(19), S(1), C_BORDER);
        cv_circle(&cv, S(px + 21), S(56), S(5), state_color(host->state));
        text(FONT_REGULAR, 16, px + 34, 62, C_TEXT, label);
    }
}

static void begin_frame(void)
{
    cv_unclip(&cv);
    cv_copy(&cv, &bg);
}

/* Centred dialog card; returns its top-left through x/y. */
static void dialog(float w, float h, float *x, float *y)
{
    *x = (W - w) / 2;
    *y = (H - h) / 2 + 10;
    cv_fill(&cv, 0, 0, cv.w, cv.h, ARGB(110, 0, 0, 0));
    card(*x, *y, w, h, 20, false);
}

/* ------------------------------------------------------------ screens */

void ui_draw_splash(const char *status, uint32_t t_ms)
{
    begin_frame();
    moon_logo(W / 2, 270, 52);
    text_center(FONT_BOLD, 60, W / 2, 400, C_TEXT, "Moonlight");
    text_center(FONT_REGULAR, 24, W / 2, 440, C_TEXT_DIM, "for Xbox 360");
    if (status)
        text_center(FONT_REGULAR, 19, W / 2, 530, C_TEXT_DIM, status);
    spinner(W / 2, 585, 18, t_ms);
}

void ui_draw_main(const struct ui_host *host, const struct ui_menu_item *items, int n, int sel,
                  const char *console_ip, uint32_t t_ms)
{
    static const struct hint hints[] = { { "A", "Select" }, { "Y", "Refresh" } };
    char ip_label[64];
    float x = 64, y = 112, cw = 600, ch = 64, gap = 10;
    float px = 700, pw = W - 64 - px;
    int i;

    begin_frame();
    header(host);

    for (i = 0; i < n; i++) {
        float cy = y + i * (ch + gap);
        bool on = i == sel;
        uint32_t title_c = items[i].enabled ? C_TEXT : C_TEXT_FAINT;

        card(x, cy, cw, ch, 14, on);
        cv_circle(&cv, S(x + 38), S(cy + ch / 2), S(20), on ? C_ACCENT : WITH_ALPHA(C_ACCENT, 46));
        icon(items[i].icon, x + 38, cy + ch / 2, 11, on ? HEX(0xFFFFFF) : (items[i].enabled ? C_ACCENT : C_TEXT_FAINT));
        text_fit(FONT_BOLD, 20, x + 74, cy + 28, cw - 120, title_c, items[i].title);
        if (items[i].subtitle)
            text_fit(FONT_REGULAR, 15, x + 74, cy + 49, cw - 120, C_TEXT_DIM, items[i].subtitle);
        if (on) {
            cv_line(&cv, S(x + cw - 34), S(cy + ch / 2 - 8), S(x + cw - 26), S(cy + ch / 2), S(2.5f), C_ACCENT);
            cv_line(&cv, S(x + cw - 26), S(cy + ch / 2), S(x + cw - 34), S(cy + ch / 2 + 8), S(2.5f), C_ACCENT);
        }
    }

    /* Host panel. */
    card(px, y, pw, 300, 18, false);
    cv_circle(&cv, S(px + 52), S(y + 56), S(30), WITH_ALPHA(C_ACCENT, 40));
    icon(ICON_MONITOR, px + 52, y + 56, 15, C_ACCENT);
    if (host && host->state != HOST_UNKNOWN) {
        const char *name = host->name && host->name[0] ? host->name : "Gaming PC";
        float yy = y + 128;

        text_fit(FONT_BOLD, 26, px + 98, y + 52, pw - 120, C_TEXT, name);
        text(FONT_REGULAR, 17, px + 98, y + 76, C_TEXT_DIM, host->address);

        cv_circle(&cv, S(px + 32), S(yy - 6), S(6), state_color(host->state));
        text(FONT_BOLD, 18, px + 48, yy, state_color(host->state), state_text(host->state));
        yy += 40;
        if (host->running_app) {
            text(FONT_REGULAR, 15, px + 28, yy, C_TEXT_FAINT, "NOW RUNNING");
            text_fit(FONT_BOLD, 20, px + 28, yy + 26, pw - 56, C_TEXT, host->running_app);
            yy += 58;
        }
        if (host->detail)
            wrapped(FONT_REGULAR, 15, px + 28, yy, pw - 56, 21, C_TEXT_DIM, host->detail);
    } else {
        text(FONT_BOLD, 24, px + 98, y + 52, C_TEXT, "No host yet");
        wrapped(FONT_REGULAR, 16, px + 98, y + 78, pw - 120, 22, C_TEXT_DIM,
                "Choose \"Host address\" to find the PC running Sunshine on your network.");
    }

    /* Tip. */
    rrect(px, y + 320, pw, 84, 16, WITH_ALPHA(C_CARD, 150));
    text(FONT_BOLD, 15, px + 24, y + 350, C_TEXT_DIM, "TIP");
    wrapped(FONT_REGULAR, 15, px + 24, y + 374, pw - 48, 20, C_TEXT_DIM,
            "Hold Back + Start + LB + RB while streaming to return here.");

    snprintf(ip_label, sizeof(ip_label), "Console %s", console_ip ? console_ip : "");
    footer(hints, 2, ip_label);
    (void)t_ms;
}

static uint32_t name_color(const char *name)
{
    uint32_t h = 2166136261u;
    static const uint32_t palette[] = { 0x3A5BA0, 0x7A3FA0, 0x2F8A7A, 0xA0503A, 0x3F7FA0, 0x8A2F5A };
    while (*name)
        h = (h ^ (unsigned char)*name++) * 16777619u;
    return HEX(palette[h % 6]);
}

/* Placeholder art: vertical gradient in a colour derived from the name. */
static void placeholder_tile(const char *name, int px, int py, int pw, int ph)
{
    uint32_t base = name_color(name);
    uint32_t top = cv_mix(base, HEX(0xFFFFFF), 40), bottom = cv_mix(base, HEX(0x000000), 110);
    uint32_t *img = (uint32_t *)malloc((size_t)pw * ph * sizeof(uint32_t));
    int x, y;

    if (img == NULL)
        return;
    for (y = 0; y < ph; y++) {
        uint32_t c = cv_mix(top, bottom, (unsigned)(y * 255 / (ph - 1)));
        for (x = 0; x < pw; x++)
            img[y * pw + x] = c;
    }
    cv_image(&cv, img, pw, ph, px, py, S(12), 255);
    free(img);
}

static void app_tile(const struct ui_app *a, float x, float y, bool on)
{
    float w = TILE_W, h = TILE_H;

    if (on)
        cv_shadow(&cv, S(x), S(y), S(w), S(h), S(12), S(26), WITH_ALPHA(C_ACCENT, 140));
    else
        cv_shadow(&cv, S(x), S(y + 5), S(w), S(h), S(12), S(14), C_SHADOW);

    if (a->art) {
        cv_image(&cv, a->art, SI(TILE_W), SI(TILE_H), SI(x), SI(y), S(12), 255);
    } else {
        char initial[8] = { 0 };
        int n = 1;

        if (((unsigned char)a->name[0] & 0xE0) == 0xC0) n = 2;
        else if (((unsigned char)a->name[0] & 0xF0) == 0xE0) n = 3;
        memcpy(initial, a->name, n);
        placeholder_tile(a->name, SI(x), SI(y), SI(TILE_W), SI(TILE_H));
        text_center(FONT_BOLD, 72, x + w / 2, y + h / 2 + 24, WITH_ALPHA(HEX(0xFFFFFF), 210), initial);
    }

    /* Selection ring sits just outside the art. */
    if (on)
        cv_rrect_outline(&cv, S(x - 6), S(y - 6), S(w + 12), S(h + 12), S(16), S(3), C_ACCENT);
    if (a->running) {
        rrect(x + 10, y + 10, 82, 24, 12, C_GREEN);
        text_center(FONT_BOLD, 12, x + 51, y + 26.5f, HEX(0x08140E), "RUNNING");
    }
}

void ui_draw_apps(const struct ui_host *host, const struct ui_app *apps, int n, int sel, uint32_t t_ms)
{
    static const struct hint hints[] = { { "A", "Start" }, { "B", "Back" }, { "X", "Quit running app" } };
    const float gap = TILE_GAP, row_h = TILE_H + 52, top = 150, bottom = H - 80;
    int cols = ui_apps_columns();
    float x0;
    int rows_visible, first_row, i;
    char title[160], count[32];

    begin_frame();
    header(host);

    snprintf(title, sizeof(title), "%s", host && host->name && host->name[0] ? host->name : "Apps");
    snprintf(count, sizeof(count), n == 1 ? "1 app" : "%d apps", n);
    text(FONT_BOLD, 28, 64, 128, C_TEXT, title);
    text(FONT_REGULAR, 17, 64 + text_w(FONT_BOLD, 28, title) + 14, 127, C_TEXT_DIM, count);

    if (n == 0) {
        text_center(FONT_REGULAR, 20, W / 2, 380, C_TEXT_DIM, "This host has no apps configured.");
        footer(hints + 1, 1, NULL);
        return;
    }

    x0 = 64;
    rows_visible = (int)((bottom - top) / row_h);
    if (rows_visible < 1)
        rows_visible = 1;
    first_row = sel / cols - rows_visible + 1;
    if (first_row < 0)
        first_row = 0;

    cv_clip(&cv, 0, SI(top - 20), cv.w, SI(bottom - top + 20));
    for (i = 0; i < n; i++) {
        int row = i / cols - first_row, col = i % cols;
        float tx = x0 + col * (TILE_W + gap), ty = top + 8 + row * row_h;

        if (row < 0 || row > rows_visible)
            continue;
        if (i != sel) {
            app_tile(&apps[i], tx, ty, false);
            text_fit(FONT_REGULAR, 15, tx + 2, ty + TILE_H + 26, TILE_W - 4, C_TEXT_DIM, apps[i].name);
        }
    }
    /* Selected tile last so its glow overlaps the neighbours. */
    {
        int row = sel / cols - first_row, col = sel % cols;
        float tx = x0 + col * (TILE_W + gap), ty = top + 8 + row * row_h;
        app_tile(&apps[sel], tx, ty, true);
        text_fit(FONT_BOLD, 16, tx + 2, ty + TILE_H + 30, TILE_W + 20, C_TEXT, apps[sel].name);
    }
    cv_unclip(&cv);

    footer(hints, apps[sel].running ? 3 : 2, NULL);
    (void)t_ms;
}

void ui_draw_settings(const struct ui_setting *rows, int n, int sel)
{
    static const struct hint hints[] = { { "\xE2\x86\x90 \xE2\x86\x92", "Change" }, { "B", "Save & back" } };
    /* Tighter rows when there are many, so the help text stays clear of the footer. */
    float x = 64, y = 150, w = W - 128, rh = n > 8 ? 44.0f : 50.0f;
    int i;

    begin_frame();
    header(NULL);
    text(FONT_BOLD, 28, 64, 128, C_TEXT, "Settings");

    card(x, y, w, rh * n + 16, 16, false);
    for (i = 0; i < n; i++) {
        float ry = y + 8 + i * rh;
        bool on = i == sel;

        if (on) {
            rrect(x + 8, ry, w - 16, rh, 10, C_CARD_SEL);
            rrect(x + 8, ry + 10, 4, rh - 20, 2, C_ACCENT);
        } else if (i > 0) {
            cv_fill(&cv, SI(x + 24), SI(ry), SI(w - 48), 1, WITH_ALPHA(C_BORDER, 200));
        }
        text(FONT_REGULAR, 19, x + 30, ry + 32, on ? C_TEXT : C_TEXT_DIM, rows[i].label);
        if (on) {
            float vw = text_w(FONT_BOLD, 19, rows[i].value);
            float vx = x + w - 52 - vw;
            text(FONT_BOLD, 19, vx, ry + 32, C_TEXT, rows[i].value);
            /* < value > chevrons */
            cv_line(&cv, S(vx - 16), S(ry + rh / 2 - 6), S(vx - 22), S(ry + rh / 2), S(2.2f), C_ACCENT);
            cv_line(&cv, S(vx - 22), S(ry + rh / 2), S(vx - 16), S(ry + rh / 2 + 6), S(2.2f), C_ACCENT);
            cv_line(&cv, S(x + w - 36), S(ry + rh / 2 - 6), S(x + w - 30), S(ry + rh / 2), S(2.2f), C_ACCENT);
            cv_line(&cv, S(x + w - 30), S(ry + rh / 2), S(x + w - 36), S(ry + rh / 2 + 6), S(2.2f), C_ACCENT);
        } else {
            text_right(FONT_REGULAR, 19, x + w - 30, ry + 32, C_TEXT_DIM, rows[i].value);
        }
    }

    if (sel >= 0 && sel < n && rows[sel].help)
        wrapped(FONT_REGULAR, 16, x + 4, y + rh * n + 50, w - 8, 22, C_TEXT_DIM, rows[sel].help);

    footer(hints, 2, NULL);
}

void ui_draw_hosts(const struct ui_menu_item *items, int n, int sel, const char *note)
{
    static const struct hint hints[] = { { "A", "Select" }, { "Y", "Search again" }, { "B", "Back" } };
    enum { VISIBLE = 6 };
    float x = 64, y = 150, cw = W - 128, ch = 64, gap = 10;
    int i, first = sel >= VISIBLE ? sel - VISIBLE + 1 : 0;

    begin_frame();
    header(NULL);
    text(FONT_BOLD, 28, 64, 128, C_TEXT, "Choose a host");
    if (note)
        text_right(FONT_REGULAR, 16, W - 64, 128, C_TEXT_DIM, note);

    for (i = first; i < n && i < first + VISIBLE; i++) {
        float cy = y + (i - first) * (ch + gap);
        bool on = i == sel;

        card(x, cy, cw, ch, 14, on);
        cv_circle(&cv, S(x + 38), S(cy + ch / 2), S(20), on ? C_ACCENT : WITH_ALPHA(C_ACCENT, 46));
        icon(items[i].icon, x + 38, cy + ch / 2, 11, on ? HEX(0xFFFFFF) : C_ACCENT);
        text_fit(FONT_BOLD, 20, x + 74, cy + 28, cw - 120, C_TEXT, items[i].title);
        if (items[i].subtitle)
            text_fit(FONT_REGULAR, 15, x + 74, cy + 49, cw - 120, C_TEXT_DIM, items[i].subtitle);
        if (on) {
            cv_line(&cv, S(x + cw - 34), S(cy + ch / 2 - 8), S(x + cw - 26), S(cy + ch / 2), S(2.5f), C_ACCENT);
            cv_line(&cv, S(x + cw - 26), S(cy + ch / 2), S(x + cw - 34), S(cy + ch / 2 + 8), S(2.5f), C_ACCENT);
        }
    }
    /* More rows above/below: small arrows at the list's right edge. */
    if (first > 0)
        cv_triangle(&cv, S(W - 52), S(y - 6), S(W - 40), S(y - 6), S(W - 46), S(y - 14), C_TEXT_DIM);
    if (first + VISIBLE < n) {
        float by = y + VISIBLE * (ch + gap) - gap + 6;
        cv_triangle(&cv, S(W - 52), S(by), S(W - 40), S(by), S(W - 46), S(by + 8), C_TEXT_DIM);
    }

    footer(hints, 3, NULL);
}

void ui_draw_ip_editor(const int octets[4], int sel)
{
    static const struct hint hints[] = {
        { "\xE2\x86\x91 \xE2\x86\x93", "\xC2\xB1" "1" }, { "LB RB", "\xC2\xB1" "10" },
        { "\xE2\x86\x90 \xE2\x86\x92", "Move" }, { "A", "Save" }, { "B", "Cancel" }
    };
    float dw = 720, dh = 290, dx, dy, bw = 118, bh = 96, gap = 38, bx0;
    int i;

    begin_frame();
    header(NULL);
    dialog(dw, dh, &dx, &dy);

    text(FONT_BOLD, 26, dx + 40, dy + 58, C_TEXT, "Host PC address");
    text(FONT_REGULAR, 17, dx + 40, dy + 88, C_TEXT_DIM, "The IP address of the PC running Sunshine.");

    bx0 = dx + (dw - (4 * bw + 3 * gap)) / 2;
    for (i = 0; i < 4; i++) {
        float bx = bx0 + i * (bw + gap), by = dy + 140;
        char num[8];
        bool on = i == sel;

        snprintf(num, sizeof(num), "%d", octets[i]);
        rrect(bx, by, bw, bh, 14, on ? C_CARD_SEL : HEX(0x111821));
        cv_rrect_outline(&cv, S(bx), S(by), S(bw), S(bh), S(14), on ? S(3) : S(1), on ? C_ACCENT : C_BORDER);
        text_center(FONT_BOLD, 46, bx + bw / 2, by + bh / 2 + 16, on ? C_TEXT : C_TEXT_DIM, num);
        if (on) {
            cv_triangle(&cv, S(bx + bw / 2 - 9), S(by - 10), S(bx + bw / 2 + 9), S(by - 10), S(bx + bw / 2), S(by - 20), C_ACCENT);
            cv_triangle(&cv, S(bx + bw / 2 - 9), S(by + bh + 10), S(bx + bw / 2 + 9), S(by + bh + 10), S(bx + bw / 2), S(by + bh + 20), C_ACCENT);
        }
        if (i < 3)
            cv_circle(&cv, S(bx + bw + gap / 2), S(by + bh - 18), S(4.5f), C_TEXT_DIM);
    }
    footer(hints, 5, NULL);
}

void ui_draw_pair(const struct ui_host *host, const char *pin, uint32_t t_ms)
{
    float dw = 760, dh = 390, dx, dy, bw = 96, bh = 116, gap = 20, bx0;
    char title[160];
    int i;

    begin_frame();
    header(host);
    dialog(dw, dh, &dx, &dy);

    snprintf(title, sizeof(title), "Pair with %s", host && host->name && host->name[0] ? host->name :
             host ? host->address : "host");
    text_fit(FONT_BOLD, 26, dx + 40, dy + 58, dw - 80, C_TEXT, title);
    wrapped(FONT_REGULAR, 17, dx + 40, dy + 90, dw - 80, 24, C_TEXT_DIM,
            "On your PC, open the Sunshine web UI (https://localhost:47990), go to PIN and enter:");

    bx0 = dx + (dw - (4 * bw + 3 * gap)) / 2;
    for (i = 0; i < 4 && pin[i]; i++) {
        char d[2] = { pin[i], 0 };
        float bx = bx0 + i * (bw + gap), by = dy + 150;
        rrect(bx, by, bw, bh, 16, HEX(0x111821));
        cv_rrect_outline(&cv, S(bx), S(by), S(bw), S(bh), S(16), S(2), C_ACCENT);
        text_center(FONT_BOLD, 64, bx + bw / 2, by + bh / 2 + 23, C_TEXT, d);
    }

    spinner(dx + 60, dy + dh - 50, 11, t_ms);
    text(FONT_REGULAR, 17, dx + 84, dy + dh - 44, C_TEXT_DIM, "Waiting for the PIN to be entered on the PC...");
}

void ui_draw_busy(const char *title, const char *message, uint32_t t_ms)
{
    float dw = 640, dx, dy;
    int lines = message ? wrapped_lines(FONT_REGULAR, 18, dw - 150, message) : 0;
    float dh = 120 + lines * 25;

    begin_frame();
    header(NULL);
    dialog(dw, dh, &dx, &dy);
    spinner(dx + 64, dy + dh / 2, 20, t_ms);
    text_fit(FONT_BOLD, 24, dx + 110, dy + 56, dw - 140, C_TEXT, title);
    if (message)
        wrapped(FONT_REGULAR, 18, dx + 110, dy + 88, dw - 150, 25, C_TEXT_DIM, message);
}

void ui_draw_message(int which, const char *title, const char *body)
{
    static const struct hint hints[] = { { "A", "OK" } };
    float dw = 700, dx, dy;
    int lines = body ? wrapped_lines(FONT_REGULAR, 19, dw - 160, body) : 0;
    float dh = 150 + lines * 27;
    uint32_t color = which == ICON_ERROR ? C_RED : which == ICON_CHECK ? C_GREEN : C_ACCENT;

    if (dh < 190)
        dh = 190;
    begin_frame();
    header(NULL);
    dialog(dw, dh, &dx, &dy);
    cv_circle(&cv, S(dx + 70), S(dy + 72), S(28), WITH_ALPHA(color, 50));
    icon(which, dx + 70, dy + 72, 15, color);
    text_fit(FONT_BOLD, 26, dx + 120, dy + 72, dw - 160, C_TEXT, title);
    if (body)
        wrapped(FONT_REGULAR, 19, dx + 120, dy + 110, dw - 160, 27, C_TEXT_DIM, body);
    footer(hints, 1, NULL);
}

void ui_draw_stats(const char *title, const struct ui_stat *stats, int n, const char *note)
{
    static const struct hint hints[] = { { "A", "OK" } };
    float dw = 780, rh = 34, dx, dy;
    int note_lines = note ? wrapped_lines(FONT_REGULAR, 16, dw - 80, note) : 0;
    float dh = 120 + n * rh + note_lines * 22 + (note ? 20 : 0);
    int i;

    begin_frame();
    header(NULL);
    dialog(dw, dh, &dx, &dy);
    text(FONT_BOLD, 26, dx + 40, dy + 58, C_TEXT, title);
    for (i = 0; i < n; i++) {
        float ry = dy + 100 + i * rh;
        if (i > 0)
            cv_fill(&cv, SI(dx + 40), SI(ry - 23), SI(dw - 80), 1, WITH_ALPHA(C_BORDER, 200));
        text(FONT_REGULAR, 17, dx + 40, ry, C_TEXT_DIM, stats[i].label);
        text_right(FONT_BOLD, 17, dx + dw - 40, ry, C_TEXT, stats[i].value);
    }
    if (note)
        wrapped(FONT_REGULAR, 16, dx + 40, dy + 110 + n * rh + 10, dw - 80, 22, C_TEXT_FAINT, note);
    footer(hints, 1, NULL);
}
