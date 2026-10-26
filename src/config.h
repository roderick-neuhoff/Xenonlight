#pragma once

#include <stdbool.h>
#include <stddef.h>

struct config {
    char host[64];
    char host_mac[18];  /* for Wake-on-LAN, learned from the host; "" if unknown */
    int width, height, fps;
    int bitrate_kbps;
    int packet_size;
    bool fast_decode;
    int renderer;       /* enum video_renderer: 0 GPU, 1 CPU, 2 CPU + display colour matrix */
    bool vsync;
    int gpu_filter;     /* SDK build: 0 off, 1 GPU deblocking, 2 deblocking + sharpening */
    bool sops;          /* let the host change its resolution to match */
    bool local_audio;   /* also play audio on the host PC */
    bool overlay;       /* SDK build: performance overlay on when a stream starts */
};

/* Finds a writable USB/HDD device and creates <dev>:/moonlight/. */
bool storage_init(void);
/* Directory for config and keys, e.g. "uda0:/moonlight", or NULL. */
const char *storage_dir(void);

/* Reads/writes an entire file; returns NULL/false on failure. */
char *storage_read(const char *name, size_t *len);
bool storage_write(const char *name, const void *data, size_t len);

void config_defaults(struct config *c);
void config_load(struct config *c);
bool config_save(const struct config *c);
