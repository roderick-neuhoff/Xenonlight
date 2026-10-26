#pragma once

#include <stdbool.h>
#include <Limelight.h>
#include "crypto.h"

struct gs_app {
    int id;
    char name[64];
};

struct gs_server {
    char address[64];
    char hostname[64];          /* PC name from serverinfo, may be empty */
    char mac[18];               /* "aa:bb:cc:dd:ee:ff"; Sunshine sends zeros unless paired */
    unsigned short http_port, https_port;
    bool paired;
    int current_game;
    int server_major_version;
    bool is_nvidia;
    char app_version[32];
    char gfe_version[32];
    char *rtsp_session_url;     /* from launch/resume, freed by gs_server_free */
    int codec_mode_support;
    SERVER_INFORMATION info;    /* filled for LiStartConnection */
};

/* Last human-readable error from any gs_* call. */
const char *gs_error(void);

bool gs_init(const struct client_identity *id);
bool gs_server_info(struct gs_server *srv, const char *address);
bool gs_pair(struct gs_server *srv, const char *pin);
bool gs_unpair(struct gs_server *srv);
int  gs_applist(struct gs_server *srv, struct gs_app *apps, int max_apps);
bool gs_launch(struct gs_server *srv, STREAM_CONFIGURATION *cfg, int app_id, bool sops, bool local_audio);
bool gs_quit(struct gs_server *srv);
/* Fetches an app's box art (PNG) over HTTPS; *data is malloc'd. */
bool gs_app_art(struct gs_server *srv, int app_id, unsigned char **data, size_t *len);
void gs_server_free(struct gs_server *srv);
