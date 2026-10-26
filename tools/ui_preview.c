/*
 * Renders every UI screen with sample data to PNG files, on the PC.
 *   gcc -O2 -Isrc tools/ui_preview.c src/ui/canvas.c src/ui/font.c src/ui/ui.c src/ui/fonts.S -lm
 *   ./a.out <output dir> [width height]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/ui/ui.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "../third_party/stb/stb_image_write.h"

static const char *out_dir;
static const char *current;

void ui_present(void)
{
    canvas_t *c = ui_canvas();
    unsigned char *rgb = malloc((size_t)c->w * c->h * 3);
    char path[512];
    int i;

    for (i = 0; i < c->w * c->h; i++) {
        rgb[i * 3 + 0] = (c->px[i] >> 16) & 255;
        rgb[i * 3 + 1] = (c->px[i] >> 8) & 255;
        rgb[i * 3 + 2] = c->px[i] & 255;
    }
    snprintf(path, sizeof(path), "%s/%s.png", out_dir, current);
    stbi_write_png(path, c->w, c->h, 3, rgb, c->w * 3);
    free(rgb);
    printf("wrote %s\n", path);
}

static void shot(const char *name)
{
    current = name;
    ui_present();
}

/* Synthetic "box art": a diagonal gradient with a few stripes. */
static uint32_t *fake_art(uint32_t a, uint32_t b)
{
    const int w = 600, h = 800;
    uint32_t *src = malloc(w * h * 4), *out;
    int x, y, tw, th;

    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++) {
            unsigned t = (unsigned)((x + y) * 255 / (w + h));
            uint32_t c = cv_mix(a, b, t);
            if (((x + y * 2) / 60) % 7 == 0)
                c = cv_mix(c, 0xFFFFFFFF, 40);
            src[y * w + x] = c;
        }
    ui_art_size(&tw, &th);
    out = image_scale_cover(src, w, h, tw, th);
    free(src);
    return out;
}

int main(int argc, char **argv)
{
    int w = argc > 3 ? atoi(argv[2]) : 1280, h = argc > 3 ? atoi(argv[3]) : 720;
    struct ui_host host = { "DESKTOP-GAMING", "192.168.1.98", HOST_PAIRED, "Steam Big Picture", "Sunshine 2026.914" };
    struct ui_menu_item items[] = {
        { "Start streaming", "Choose a game or app on DESKTOP-GAMING", ICON_PLAY, true },
        { "Pair with host", "Already paired", ICON_LINK, false },
        { "Host address", "192.168.1.98", ICON_MONITOR, true },
        { "Settings", "1280x720 \xC2\xB7 30 FPS \xC2\xB7 8 Mbps \xC2\xB7 GPU", ICON_GEAR, true },
        { "Quit running app", "Steam Big Picture is running", ICON_STOP, true },
        { "Refresh", "Check the host's status", ICON_REFRESH, true },
        { "Unpair", "Forget this console on the host", ICON_UNLINK, true },
    };
    struct ui_app apps[] = {
        { "Desktop", NULL, false },
        { "Steam Big Picture", NULL, true },
        { "Cyberpunk 2077", NULL, false },
        { "Forza Horizon 5", NULL, false },
        { "Elden Ring", NULL, false },
        { "Hollow Knight", NULL, false },
        { "Ultra Long Game Name That Does Not Fit", NULL, false },
        { "\xC3\x89lan", NULL, false },
    };
    struct ui_setting settings[] = {
        { "Resolution", "1280x720", "The host renders and encodes at this size. 854x480 decodes about twice as fast." },
        { "Frame rate", "30 FPS", NULL },
        { "Bitrate", "8 Mbps", NULL },
        { "Fast decode", "On", NULL },
        { "Renderer", "GPU", NULL },
        { "V-Sync", "On", NULL },
        { "Optimize game settings", "On", NULL },
        { "Play audio on PC", "Off", NULL },
    };
    struct ui_stat stats[] = {
        { "Frames received / decoded / shown", "" }, { "Dropped", "" }, { "Decode time", "" },
        { "Render time", "" }, { "Video", "" },
    };
    int oct[4] = { 192, 168, 1, 98 };

    out_dir = argc > 1 ? argv[1] : ".";
    if (!ui_init(w, h)) {
        fprintf(stderr, "ui_init failed\n");
        return 1;
    }

    apps[0].art = fake_art(HEX(0x1E3C72), HEX(0x2A5298));
    apps[2].art = fake_art(HEX(0xF7E018), HEX(0xD12E6F));
    apps[3].art = fake_art(HEX(0x11998E), HEX(0x38EF7D));

    ui_draw_splash("Preparing this console's client certificate...", 300); shot("01_splash");
    ui_draw_main(&host, items, 7, 0, "192.168.1.99", 0); shot("02_main");
    {
        struct ui_host h2 = { NULL, "192.168.1.98", HOST_OFFLINE, NULL,
                              "Could not reach 192.168.1.98 (is Sunshine running and the IP correct?)" };
        ui_draw_main(&h2, items, 7, 3, "192.168.1.99", 0); shot("03_main_offline");
    }
    ui_draw_apps(&host, apps, 8, 2, 0); shot("04_apps");
    ui_draw_settings(settings, 8, 0); shot("05_settings");
    ui_draw_ip_editor(oct, 3); shot("06_ip");
    ui_draw_pair(&host, "4821", 400); shot("07_pair");
    ui_draw_busy("Connecting to DESKTOP-GAMING", "Starting Steam Big Picture at 1280x720, 30 FPS...", 500); shot("08_busy");
    ui_draw_message(ICON_ERROR, "Couldn't start the stream",
                    "Error -1. Check that UDP ports 47998-48010 aren't blocked by the firewall on the PC."); shot("09_error");
    snprintf(stats[0].value, 64, "2225 / 2196 / 2195");
    snprintf(stats[1].value, 64, "27");
    snprintf(stats[2].value, 64, "14.2 ms (4 threads)");
    snprintf(stats[3].value, 64, "2.1 ms (GPU)");
    snprintf(stats[4].value, 64, "1280x720 @ 30 FPS on 1280x720");
    ui_draw_stats("Stream ended", stats, 5, "The app keeps running on the PC. Use \"Quit running app\" to close it."); shot("10_stats");
    {
        struct ui_menu_item hosts[] = {
            { "DESKTOP-GAMING", "192.168.1.98 \xC2\xB7 Sunshine 2026.914 \xC2\xB7 Paired \xC2\xB7 Current host", ICON_MONITOR, true },
            { "LIVING-ROOM", "192.168.1.120 \xC2\xB7 Sunshine 2025.122 \xC2\xB7 Not paired yet", ICON_MONITOR, true },
            { "Search again", "Start Sunshine on the PC first if it isn't listed", ICON_SEARCH, true },
            { "Enter an address", "192.168.1.98", ICON_EDIT, true },
        };
        struct ui_host off = { NULL, "192.168.1.98", HOST_OFFLINE, NULL, "Could not reach 192.168.1.98" };
        ui_draw_hosts(hosts, 4, 0, "2 found"); shot("11_hosts");
        items[5].title = "Wake PC";
        items[5].subtitle = "Send Wake-on-LAN to 2c:f0:5d:12:34:56 (Y checks again)";
        items[5].icon = ICON_POWER;
        ui_draw_main(&off, items, 7, 5, "192.168.1.99", 0); shot("13_wake");
    }
    {
        /* The dashboard build's 10 settings, with the longest help text selected. */
        struct ui_setting all[10];
        int i;
        for (i = 0; i < 8; i++)
            all[i] = settings[i];
        all[4].label = "GPU picture";
        all[4].value = "Deblock";
        all[8].label = "Performance overlay";
        all[8].value = "Off";
        all[8].help = "Shows decode time, frame rate, bitrate and network round trip over the stream from the "
                      "start. Back + LB + RB + X shows or hides it at any time.";
        all[9].label = "Decoder benchmark";
        all[9].value = "Press A";
        all[9].help = NULL;
        ui_draw_settings(all, 10, 8); shot("14_settings_10");
    }
    {
        /* Overlay over a stand-in frame, blended at 0.85 like GFX_SHADER_OVERLAY. */
        struct ui_overlay_row rows[7] = {
            { "Frames received / shown", "30.0 / 29.6 FPS", 0 }, { "Decode time", "28.4 ms of 33.3", 1 },
            { "Render time", "2.1 ms", 0 }, { "Bitrate", "7.8 Mbps", 0 },
            { "Dropped (decoder behind)", "2 (14 total)", 1 }, { "Lost packets (unfixable)", "0", 0 },
            { "Network round trip", "4 ms \xC2\xB1 1", 0 },
        };
        canvas_t ov, *c = ui_canvas();
        int ow, oh, x, y;
        ui_overlay_size(7, &ow, &oh);
        cv_init(&ov, ow, oh);
        ui_draw_overlay(&ov, rows, 7);
        ui_draw_busy("A game", "pretend this is the video", 0);
        for (y = 0; y < oh; y++)
            for (x = 0; x < ow; x++)
                c->px[(36 + y) * c->w + 48 + x] = cv_mix(c->px[(36 + y) * c->w + 48 + x], ov.px[y * ow + x], 217);
        shot("15_overlay");
        cv_free(&ov);
        ui_draw_busy("Searching for PCs running Sunshine", "Checked 120 of 253 addresses \xC2\xB7 found 1", 500); shot("12_search");
    }
    return 0;
}
