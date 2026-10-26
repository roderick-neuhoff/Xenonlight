/*
 * GameStream HTTP protocol: server info, pairing, app list, launch, quit.
 * Ported from moonlight-embedded's libgamestream (GPLv3) to mbedTLS and a
 * tiny tag-extracting XML reader.
 */
#include "gamestream.h"
#include "http.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <mbedtls/aes.h>
#include <mbedtls/md.h>
#include <mbedtls/sha1.h>
#include <mbedtls/sha256.h>
#include <mbedtls/version.h>

/* mbedTLS 2.x (Xbox 360 SDK build) has public struct members and a
 * pk_sign without the output size. */
#ifndef MBEDTLS_PRIVATE
#  define MBEDTLS_PRIVATE(member) member
#endif
#ifndef MBEDTLS_PK_SIGNATURE_MAX_SIZE
#  define MBEDTLS_PK_SIGNATURE_MAX_SIZE MBEDTLS_MPI_MAX_SIZE
#endif

#define UNIQUE_ID "0123456789ABCDEF"
#define DEVICE_NAME "xbox360"
#define TIMEOUT_MS 10000
#define PAIR_TIMEOUT_MS (5 * 60 * 1000)   /* waits for the PIN to be typed on the PC */

static const struct client_identity *ident;
static char cert_hex[8192];
static char last_error[256];

static void set_error(const char *fmt, const char *arg)
{
    snprintf(last_error, sizeof(last_error), fmt, arg ? arg : "");
}

const char *gs_error(void)
{
    return last_error;
}

/* ---------------------------------------------------------------- helpers */

static void bytes_to_hex(const unsigned char *in, char *out, size_t len)
{
    static const char hex[] = "0123456789abcdef";
    size_t i;

    for (i = 0; i < len; i++) {
        out[i * 2] = hex[in[i] >> 4];
        out[i * 2 + 1] = hex[in[i] & 15];
    }
    out[len * 2] = '\0';
}

static int hex_val(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return 0;
}

static size_t hex_to_bytes(const char *in, unsigned char *out, size_t max)
{
    size_t n = strlen(in) / 2, i;

    if (n > max)
        n = max;
    for (i = 0; i < n; i++)
        out[i] = (unsigned char)((hex_val(in[i * 2]) << 4) | hex_val(in[i * 2 + 1]));
    return n;
}

static void make_uuid(char out[37])
{
    unsigned char b[16];
    char hex[33];

    crypto_random(b, sizeof(b));
    b[6] = (b[6] & 0x0F) | 0x40;
    b[8] = (b[8] & 0x3F) | 0x80;
    bytes_to_hex(b, hex, sizeof(b));
    snprintf(out, 37, "%.8s-%.4s-%.4s-%.4s-%.12s", hex, hex + 8, hex + 12, hex + 16, hex + 20);
}

/* Returns a malloc'd copy of the text of the first <tag>...</tag> at or after from. */
static char *xml_get_from(const char *from, const char *tag, const char **after)
{
    char open[64], close[64];
    const char *s, *e;
    char *r;

    snprintf(open, sizeof(open), "<%s>", tag);
    snprintf(close, sizeof(close), "</%s>", tag);
    s = strstr(from, open);
    if (s == NULL)
        return NULL;
    s += strlen(open);
    e = strstr(s, close);
    if (e == NULL)
        return NULL;
    r = (char *)malloc((size_t)(e - s) + 1);
    if (r == NULL)
        return NULL;
    memcpy(r, s, (size_t)(e - s));
    r[e - s] = '\0';
    if (after)
        *after = e + strlen(close);
    return r;
}

static char *xml_get(const char *doc, const char *tag)
{
    return xml_get_from(doc, tag, NULL);
}

/* Checks <root status_code="200" ...>; records status_message otherwise. */
static bool xml_status_ok(const char *doc)
{
    const char *p = strstr(doc, "status_code=\"");
    int code;

    if (p == NULL) {
        set_error("Malformed response from host", NULL);
        return false;
    }
    code = atoi(p + 13);
    if (code != 200) {
        const char *m = strstr(doc, "status_message=\"");
        char msg[160] = "unknown error";

        if (m) {
            const char *end = strchr(m + 16, '"');
            size_t n = end ? (size_t)(end - (m + 16)) : 0;
            if (n >= sizeof(msg))
                n = sizeof(msg) - 1;
            memcpy(msg, m + 16, n);
            msg[n] = '\0';
        }
        set_error("Host returned an error: %s", msg);
        return false;
    }
    return true;
}

static bool request(struct gs_server *srv, bool https, const char *path_and_query, int timeout_ms,
                    struct http_response *resp)
{
    if (!http_get(srv->address, https ? srv->https_port : srv->http_port, https, path_and_query,
                  ident, timeout_ms, resp)) {
        set_error("Could not reach %s (is Sunshine running and the IP correct?)", srv->address);
        return false;
    }
    if (resp->status != 200) {
        char code[16];
        snprintf(code, sizeof(code), "%d", resp->status);
        set_error("Host replied with HTTP %s", code);
        http_response_free(resp);
        return false;
    }
    return true;
}

/* ---------------------------------------------------------------- API */

bool gs_init(const struct client_identity *id)
{
    size_t len = strlen(id->cert_pem);

    ident = id;
    if (len * 2 + 1 > sizeof(cert_hex))
        return false;
    bytes_to_hex((const unsigned char *)id->cert_pem, cert_hex, len);
    return true;
}

static bool load_serverinfo(struct gs_server *srv, bool https)
{
    struct http_response r;
    char path[256], uuid[37];
    char *paired, *current, *appver, *state, *codec, *https_port, *gfever;
    bool ok = false;

    make_uuid(uuid);
    snprintf(path, sizeof(path), "/serverinfo?uniqueid=%s&uuid=%s", UNIQUE_ID, uuid);
    if (!request(srv, https, path, TIMEOUT_MS, &r))
        return false;
    if (!xml_status_ok(r.body)) {
        http_response_free(&r);
        return false;
    }

    paired = xml_get(r.body, "PairStatus");
    current = xml_get(r.body, "currentgame");
    appver = xml_get(r.body, "appversion");
    state = xml_get(r.body, "state");
    codec = xml_get(r.body, "ServerCodecModeSupport");
    https_port = xml_get(r.body, "HttpsPort");
    gfever = xml_get(r.body, "GfeVersion");
    {
        char *name = xml_get(r.body, "hostname"), *mac = xml_get(r.body, "mac");
        if (name) {
            snprintf(srv->hostname, sizeof(srv->hostname), "%s", name);
            free(name);
        }
        if (mac) {
            snprintf(srv->mac, sizeof(srv->mac), "%s", mac);
            free(mac);
        }
    }

    if (appver && state && current) {
        srv->paired = paired && strcmp(paired, "1") == 0;
        snprintf(srv->app_version, sizeof(srv->app_version), "%s", appver);
        snprintf(srv->gfe_version, sizeof(srv->gfe_version), "%s", gfever ? gfever : "");
        srv->server_major_version = atoi(appver);
        srv->is_nvidia = strstr(state, "MJOLNIR") != NULL;
        srv->codec_mode_support = codec ? atoi(codec) : SCM_H264;
        srv->current_game = strstr(state, "_SERVER_BUSY") ? atoi(current) : 0;
        if (https_port && atoi(https_port))
            srv->https_port = (unsigned short)atoi(https_port);
        ok = true;
    } else {
        set_error("Host sent incomplete server info", NULL);
    }

    free(paired);
    free(current);
    free(appver);
    free(state);
    free(codec);
    free(https_port);
    free(gfever);
    http_response_free(&r);
    return ok;
}

bool gs_server_info(struct gs_server *srv, const char *address)
{
    snprintf(srv->address, sizeof(srv->address), "%s", address);
    srv->http_port = 47989;
    if (!srv->https_port)
        srv->https_port = 47984;

    /*
     * Sunshine always reports PairStatus 0 over plain HTTP; only the HTTPS
     * endpoint (which checks our client certificate) says we're paired. So
     * try HTTPS first and fall back to HTTP when the host refuses us there,
     * which is what happens before pairing.
     */
    if (load_serverinfo(srv, true)) {
        srv->paired = true;
    } else {
        if (!load_serverinfo(srv, false))
            return false;
        srv->paired = false;
    }

    LiInitializeServerInformation(&srv->info);
    srv->info.address = srv->address;
    srv->info.serverInfoAppVersion = srv->app_version;
    srv->info.serverInfoGfeVersion = srv->gfe_version;
    srv->info.serverCodecModeSupport = srv->codec_mode_support;

    if (srv->server_major_version < 3) {
        set_error("Host software is too old", NULL);
        return false;
    }
    return true;
}

static void hash_for_version(int major, const unsigned char *in, size_t len, unsigned char out[32])
{
    if (major >= 7)
        mbedtls_sha256(in, len, out, 0);
    else
        mbedtls_sha1(in, len, out);
}

static void aes_ecb(const unsigned char key[16], bool encrypt, const unsigned char *in, unsigned char *out, size_t len)
{
    mbedtls_aes_context aes;
    size_t i;

    mbedtls_aes_init(&aes);
    if (encrypt)
        mbedtls_aes_setkey_enc(&aes, key, 128);
    else
        mbedtls_aes_setkey_dec(&aes, key, 128);
    for (i = 0; i + 16 <= len; i += 16)
        mbedtls_aes_crypt_ecb(&aes, encrypt ? MBEDTLS_AES_ENCRYPT : MBEDTLS_AES_DECRYPT, in + i, out + i);
    mbedtls_aes_free(&aes);
}

/* Sends one /pair step and checks <paired>1</paired>. Keeps r on success. */
static bool pair_step(struct gs_server *srv, bool https, const char *query, int timeout_ms, struct http_response *r)
{
    char *paired;
    bool ok;

    if (!request(srv, https, query, timeout_ms, r))
        return false;
    if (!xml_status_ok(r->body)) {
        http_response_free(r);
        return false;
    }
    paired = xml_get(r->body, "paired");
    ok = paired && strcmp(paired, "1") == 0;
    free(paired);
    if (!ok) {
        set_error("Pairing was rejected (wrong PIN?)", NULL);
        http_response_free(r);
    }
    return ok;
}

bool gs_pair(struct gs_server *srv, const char *pin)
{
    const size_t url_max = 16384;
    char *url = (char *)malloc(url_max);
    char uuid[37], *s;
    unsigned char salt[16], aes_key[32], salt_pin[20];
    unsigned char challenge[16], challenge_enc[16];
    unsigned char resp_enc[64], resp_dec[64];
    unsigned char client_secret[16];
    unsigned char hash[32], hash_enc[32];
    unsigned char *pairing_secret = NULL, *plaincert = NULL, *cr_buf = NULL, *signature = NULL;
    char hex[512];
    struct http_response r = { 0 };
    mbedtls_x509_crt server_cert;
    size_t sig_len, plaincert_len, ps_len, cr_len;
    int hash_len = srv->server_major_version >= 7 ? 32 : 20;
    bool ok = false;

    mbedtls_x509_crt_init(&server_cert);
    if (url == NULL)
        return false;

    if (srv->paired) {
        set_error("Already paired", NULL);
        goto out;
    }

    /* 1. Send our cert with a salt; the host shows a PIN prompt. */
    crypto_random(salt, sizeof(salt));
    bytes_to_hex(salt, hex, sizeof(salt));
    make_uuid(uuid);
    snprintf(url, url_max, "/pair?uniqueid=%s&uuid=%s&devicename=%s&updateState=1&phrase=getservercert&salt=%s&clientcert=%s",
             UNIQUE_ID, uuid, DEVICE_NAME, hex, cert_hex);
    if (!pair_step(srv, false, url, PAIR_TIMEOUT_MS, &r))
        goto out;
    s = xml_get(r.body, "plaincert");
    http_response_free(&r);
    if (s == NULL) {
        set_error("Host did not send its certificate (another pairing in progress?)", NULL);
        goto out;
    }
    plaincert = (unsigned char *)malloc(strlen(s) / 2 + 1);
    if (plaincert == NULL) {
        free(s);
        goto out;
    }
    plaincert_len = hex_to_bytes(s, plaincert, strlen(s) / 2);
    plaincert[plaincert_len] = '\0';
    free(s);
    if (mbedtls_x509_crt_parse(&server_cert, plaincert, plaincert_len + 1) != 0) {
        set_error("Could not parse the host's certificate", NULL);
        goto out;
    }

    /* AES key = first 16 bytes of hash(salt || PIN). */
    memcpy(salt_pin, salt, 16);
    memcpy(salt_pin + 16, pin, 4);
    hash_for_version(srv->server_major_version, salt_pin, sizeof(salt_pin), aes_key);

    /* 2. Encrypted random challenge. */
    crypto_random(challenge, sizeof(challenge));
    aes_ecb(aes_key, true, challenge, challenge_enc, sizeof(challenge));
    bytes_to_hex(challenge_enc, hex, sizeof(challenge_enc));
    make_uuid(uuid);
    snprintf(url, url_max, "/pair?uniqueid=%s&uuid=%s&devicename=%s&updateState=1&clientchallenge=%s",
             UNIQUE_ID, uuid, DEVICE_NAME, hex);
    if (!pair_step(srv, false, url, TIMEOUT_MS, &r))
        goto out;
    s = xml_get(r.body, "challengeresponse");
    http_response_free(&r);
    if (s == NULL || strlen(s) / 2 > sizeof(resp_enc)) {
        free(s);
        set_error("Bad challenge response from host", NULL);
        goto out;
    }
    memset(resp_enc, 0, sizeof(resp_enc));
    hex_to_bytes(s, resp_enc, sizeof(resp_enc));
    free(s);
    aes_ecb(aes_key, false, resp_enc, resp_dec, sizeof(resp_enc));

    /* 3. hash(server challenge || our cert signature || client secret). */
    crypto_random(client_secret, sizeof(client_secret));
    sig_len = ident->cert.MBEDTLS_PRIVATE(sig).len;
    cr_len = 16 + sig_len + sizeof(client_secret);
    cr_buf = (unsigned char *)malloc(cr_len);
    if (cr_buf == NULL)
        goto out;
    memcpy(cr_buf, resp_dec + hash_len, 16);
    memcpy(cr_buf + 16, ident->cert.MBEDTLS_PRIVATE(sig).p, sig_len);
    memcpy(cr_buf + 16 + sig_len, client_secret, sizeof(client_secret));
    memset(hash, 0, sizeof(hash));
    hash_for_version(srv->server_major_version, cr_buf, cr_len, hash);
    aes_ecb(aes_key, true, hash, hash_enc, sizeof(hash));
    bytes_to_hex(hash_enc, hex, sizeof(hash_enc));
    make_uuid(uuid);
    snprintf(url, url_max, "/pair?uniqueid=%s&uuid=%s&devicename=%s&updateState=1&serverchallengeresp=%s",
             UNIQUE_ID, uuid, DEVICE_NAME, hex);
    if (!pair_step(srv, false, url, TIMEOUT_MS, &r))
        goto out;
    s = xml_get(r.body, "pairingsecret");
    http_response_free(&r);
    if (s == NULL || strlen(s) / 2 <= 16) {
        free(s);
        set_error("Bad pairing secret from host", NULL);
        goto out;
    }
    ps_len = strlen(s) / 2;
    pairing_secret = (unsigned char *)malloc(ps_len);
    if (pairing_secret == NULL) {
        free(s);
        goto out;
    }
    hex_to_bytes(s, pairing_secret, ps_len);
    free(s);

    /* The host signed its secret with the cert it sent us: verify (MITM check). */
    {
        unsigned char h[32];
        mbedtls_sha256(pairing_secret, 16, h, 0);
        if (mbedtls_pk_verify(&server_cert.pk, MBEDTLS_MD_SHA256, h, 32,
                              pairing_secret + 16, ps_len - 16) != 0) {
            set_error("Host signature check failed (possible MITM, or wrong PIN)", NULL);
            goto out;
        }
    }

    /* 4. Send our secret signed with our key. */
    {
        unsigned char h[32];
        size_t olen = 0;
        unsigned char *cps;

        signature = (unsigned char *)malloc(MBEDTLS_PK_SIGNATURE_MAX_SIZE);
        if (signature == NULL)
            goto out;
        mbedtls_sha256(client_secret, sizeof(client_secret), h, 0);
#if MBEDTLS_VERSION_MAJOR >= 3
        if (mbedtls_pk_sign((mbedtls_pk_context *)&ident->key, MBEDTLS_MD_SHA256, h, 32, signature,
                            MBEDTLS_PK_SIGNATURE_MAX_SIZE, &olen, mbedtls_ctr_drbg_random, crypto_drbg()) != 0) {
#else
        if (mbedtls_pk_sign((mbedtls_pk_context *)&ident->key, MBEDTLS_MD_SHA256, h, 32, signature,
                            &olen, mbedtls_ctr_drbg_random, crypto_drbg()) != 0) {
#endif
            set_error("Signing failed", NULL);
            goto out;
        }
        cps = (unsigned char *)malloc(sizeof(client_secret) + olen);
        if (cps == NULL)
            goto out;
        memcpy(cps, client_secret, sizeof(client_secret));
        memcpy(cps + sizeof(client_secret), signature, olen);
        {
            char *cps_hex = (char *)malloc((sizeof(client_secret) + olen) * 2 + 1);
            if (cps_hex == NULL) {
                free(cps);
                goto out;
            }
            bytes_to_hex(cps, cps_hex, sizeof(client_secret) + olen);
            make_uuid(uuid);
            snprintf(url, url_max, "/pair?uniqueid=%s&uuid=%s&devicename=%s&updateState=1&clientpairingsecret=%s",
                     UNIQUE_ID, uuid, DEVICE_NAME, cps_hex);
            free(cps_hex);
        }
        free(cps);
    }
    if (!pair_step(srv, false, url, TIMEOUT_MS, &r))
        goto out;
    http_response_free(&r);

    /* 5. Confirm over HTTPS with our client certificate. */
    make_uuid(uuid);
    snprintf(url, url_max, "/pair?uniqueid=%s&uuid=%s&devicename=%s&updateState=1&phrase=pairchallenge",
             UNIQUE_ID, uuid, DEVICE_NAME);
    if (!pair_step(srv, true, url, TIMEOUT_MS, &r))
        goto out;
    http_response_free(&r);

    srv->paired = true;
    ok = true;

out:
    if (!ok)
        gs_unpair(srv);
    mbedtls_x509_crt_free(&server_cert);
    free(url);
    free(plaincert);
    free(cr_buf);
    free(pairing_secret);
    free(signature);
    return ok;
}

bool gs_unpair(struct gs_server *srv)
{
    struct http_response r;
    char path[256], uuid[37];

    make_uuid(uuid);
    snprintf(path, sizeof(path), "/unpair?uniqueid=%s&uuid=%s", UNIQUE_ID, uuid);
    if (!request(srv, false, path, TIMEOUT_MS, &r))
        return false;
    http_response_free(&r);
    srv->paired = false;
    return true;
}

int gs_applist(struct gs_server *srv, struct gs_app *apps, int max_apps)
{
    struct http_response r;
    char path[256], uuid[37];
    const char *p;
    char *block;
    int n = 0;

    make_uuid(uuid);
    snprintf(path, sizeof(path), "/applist?uniqueid=%s&uuid=%s", UNIQUE_ID, uuid);
    if (!request(srv, true, path, TIMEOUT_MS, &r))
        return -1;
    if (!xml_status_ok(r.body)) {
        http_response_free(&r);
        return -1;
    }

    p = r.body;
    while (n < max_apps && (block = xml_get_from(p, "App", &p)) != NULL) {
        char *title = xml_get(block, "AppTitle");
        char *id = xml_get(block, "ID");

        if (title && id) {
            apps[n].id = atoi(id);
            snprintf(apps[n].name, sizeof(apps[n].name), "%s", title);
            n++;
        }
        free(title);
        free(id);
        free(block);
    }
    http_response_free(&r);
    return n;
}

bool gs_launch(struct gs_server *srv, STREAM_CONFIGURATION *cfg, int app_id, bool sops, bool local_audio)
{
    struct http_response r;
    char path[1024], uuid[37], rikey[33];
    char *result, *session;
    uint32_t rikeyid;
    int fps;
    bool resume = srv->current_game != 0;

    crypto_random(cfg->remoteInputAesKey, sizeof(cfg->remoteInputAesKey));
    memset(cfg->remoteInputAesIv, 0, sizeof(cfg->remoteInputAesIv));
    crypto_random(cfg->remoteInputAesIv, sizeof(rikeyid));
    memcpy(&rikeyid, cfg->remoteInputAesIv, sizeof(rikeyid));   /* big-endian = network order */
    bytes_to_hex((const unsigned char *)cfg->remoteInputAesKey, rikey, sizeof(cfg->remoteInputAesKey));

    /* GFE misbehaves with fps > 60 when SOPS is on. */
    fps = (srv->is_nvidia && cfg->fps > 60) ? 0 : cfg->fps;

    make_uuid(uuid);
    snprintf(path, sizeof(path),
             "/%s?uniqueid=%s&uuid=%s&appid=%d&mode=%dx%dx%d&additionalStates=1&sops=%d&rikey=%s&rikeyid=%d"
             "&localAudioPlayMode=%d&surroundAudioInfo=%d&remoteControllersBitmap=%d&gcmap=%d%s",
             resume ? "resume" : "launch", UNIQUE_ID, uuid, app_id, cfg->width, cfg->height, fps,
             sops ? 1 : 0, rikey, (int)rikeyid, local_audio ? 1 : 0,
             SURROUNDAUDIOINFO_FROM_AUDIO_CONFIGURATION(cfg->audioConfiguration), 0xF, 0xF,
             LiGetLaunchUrlQueryParameters());

    /* Launching a game can take a while on the host side. */
    if (!request(srv, true, path, 60000, &r))
        return false;
    if (!xml_status_ok(r.body)) {
        http_response_free(&r);
        return false;
    }

    result = xml_get(r.body, "gamesession");
    if (result == NULL)
        result = xml_get(r.body, "resume");
    if (result == NULL || strcmp(result, "0") == 0) {
        free(result);
        set_error("Host refused to start the app", NULL);
        http_response_free(&r);
        return false;
    }
    free(result);

    session = xml_get(r.body, "sessionUrl0");
    free(srv->rtsp_session_url);
    srv->rtsp_session_url = session;
    srv->info.rtspSessionUrl = session;
    srv->current_game = app_id;

    http_response_free(&r);
    return true;
}

bool gs_quit(struct gs_server *srv)
{
    struct http_response r;
    char path[256], uuid[37];
    char *result;
    bool ok;

    make_uuid(uuid);
    snprintf(path, sizeof(path), "/cancel?uniqueid=%s&uuid=%s", UNIQUE_ID, uuid);
    if (!request(srv, true, path, 30000, &r))
        return false;
    ok = xml_status_ok(r.body);
    result = ok ? xml_get(r.body, "cancel") : NULL;
    ok = ok && result && strcmp(result, "0") != 0;
    free(result);
    http_response_free(&r);
    if (ok)
        srv->current_game = 0;
    else if (!last_error[0])
        set_error("Host refused to quit the app", NULL);
    return ok;
}

bool gs_app_art(struct gs_server *srv, int app_id, unsigned char **data, size_t *len)
{
    struct http_response r;
    char path[256], uuid[37];

    make_uuid(uuid);
    snprintf(path, sizeof(path), "/appasset?uniqueid=%s&uuid=%s&appid=%d&AssetType=2&AssetIdx=0",
             UNIQUE_ID, uuid, app_id);
    if (!request(srv, true, path, TIMEOUT_MS, &r))
        return false;
    if (r.len == 0) {
        http_response_free(&r);
        return false;
    }
    *data = (unsigned char *)r.body;
    *len = r.len;
    return true;
}

void gs_server_free(struct gs_server *srv)
{
    free(srv->rtsp_session_url);
    srv->rtsp_session_url = NULL;
    srv->info.rtspSessionUrl = NULL;
}
