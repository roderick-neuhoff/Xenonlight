#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void config_defaults(struct config *c)
{
    memset(c, 0, sizeof(*c));
    c->width = 1280;
    c->height = 720;
    c->fps = 30;
    c->bitrate_kbps = 8000;
    c->packet_size = 1392;
    c->fast_decode = true;
    c->renderer = 0;
    c->vsync = true;
    c->gpu_filter = 1;
    c->sops = true;
    c->local_audio = false;
}

static void trim(char *s)
{
    char *e;

    while (*s == ' ' || *s == '\t')
        memmove(s, s + 1, strlen(s));
    e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n'))
        *--e = '\0';
}

void config_load(struct config *c)
{
    char *text, *line, *save;

    config_defaults(c);
    text = storage_read("moonlight.ini", NULL);
    if (text == NULL)
        return;

    for (line = strtok_r(text, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        char *eq = strchr(line, '=');

        if (line[0] == '#' || line[0] == ';' || eq == NULL)
            continue;
        *eq = '\0';
        trim(line);
        trim(eq + 1);

        if (!strcmp(line, "host"))
            snprintf(c->host, sizeof(c->host), "%s", eq + 1);
        else if (!strcmp(line, "width"))
            c->width = atoi(eq + 1);
        else if (!strcmp(line, "height"))
            c->height = atoi(eq + 1);
        else if (!strcmp(line, "fps"))
            c->fps = atoi(eq + 1);
        else if (!strcmp(line, "bitrate"))
            c->bitrate_kbps = atoi(eq + 1);
        else if (!strcmp(line, "packet_size"))
            c->packet_size = atoi(eq + 1);
        else if (!strcmp(line, "fast_decode"))
            c->fast_decode = atoi(eq + 1) != 0;
        else if (!strcmp(line, "renderer")) {
            if (!strcmp(eq + 1, "cpu"))
                c->renderer = 1;
            else if (!strcmp(eq + 1, "cpu_hwcolor"))
                c->renderer = 2;
            else
                c->renderer = 0;
        }
        else if (!strcmp(line, "vsync"))
            c->vsync = atoi(eq + 1) != 0;
        else if (!strcmp(line, "gpu_filter"))
            c->gpu_filter = atoi(eq + 1);
        else if (!strcmp(line, "sops"))
            c->sops = atoi(eq + 1) != 0;
        else if (!strcmp(line, "local_audio"))
            c->local_audio = atoi(eq + 1) != 0;
    }
    free(text);

    /* A hand-edited file shouldn't be able to request something nonsensical. */
    if (c->width < 320 || c->width > 1920 || c->height < 240 || c->height > 1080) {
        c->width = 1280;
        c->height = 720;
    }
    if (c->fps < 10 || c->fps > 120)
        c->fps = 30;
    if (c->bitrate_kbps < 500 || c->bitrate_kbps > 100000)
        c->bitrate_kbps = 8000;
    if (c->packet_size < 256 || c->packet_size > 1500)
        c->packet_size = 1392;
    if (c->gpu_filter < 0 || c->gpu_filter > 2)
        c->gpu_filter = 1;
}

bool config_save(const struct config *c)
{
    char buf[1024];   /* ~400 bytes of text plus the values */
    int n = snprintf(buf, sizeof(buf),
        "# Moonlight for Xbox 360\n"
        "host=%s\n"
        "width=%d\n"
        "height=%d\n"
        "fps=%d\n"
        "# kbps\n"
        "bitrate=%d\n"
        "packet_size=%d\n"
        "# 1 = skip H.264 deblocking (much faster, slightly blockier)\n"
        "fast_decode=%d\n"
        "# gpu (default), cpu, or cpu_hwcolor (experimental)\n"
        "renderer=%s\n"
        "# GPU renderer: wait for vertical blank before showing a frame (no tearing)\n"
        "vsync=%d\n"
        "# Dashboard version: 0 off, 1 GPU deblocking, 2 deblocking + sharpening\n"
        "gpu_filter=%d\n"
        "sops=%d\n"
        "local_audio=%d\n",
        c->host, c->width, c->height, c->fps, c->bitrate_kbps, c->packet_size,
        c->fast_decode, c->renderer == 1 ? "cpu" : c->renderer == 2 ? "cpu_hwcolor" : "gpu",
        c->vsync, c->gpu_filter, c->sops, c->local_audio);
    if (n < 0 || n >= (int)sizeof(buf))
        return false;   /* never write a truncated file */
    return storage_write("moonlight.ini", buf, (size_t)n);
}
