/*
 * Opus decode + playback through the Xenon audio DMA ring (48 kHz,
 * 16-bit stereo, little-endian). Runs on Moonlight's audio coroutine.
 */
#include "audio.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <opus_multistream.h>
#include <xenon_sound/sound.h>

#define MAX_FRAME_SAMPLES 960   /* 20 ms at 48 kHz, the largest Moonlight uses */
/* Keep at most this much queued so latency stays low (~60 ms). */
#define MAX_QUEUED_BYTES (48000 * 4 * 60 / 1000)

static OpusMSDecoder *decoder;
static int channels;
static int16_t pcm[MAX_FRAME_SAMPLES * 2];
static uint32_t dropped_frames;

static int audio_init(int config, const POPUS_MULTISTREAM_CONFIGURATION opus, void *ctx, int flags)
{
    int err;
    (void)config;
    (void)ctx;
    (void)flags;

    if (opus->channelCount != 2) {
        printf("audio: only stereo is supported (got %d channels)\n", opus->channelCount);
        return -1;
    }
    channels = opus->channelCount;
    decoder = opus_multistream_decoder_create(opus->sampleRate, opus->channelCount, opus->streams,
                                              opus->coupledStreams, opus->mapping, &err);
    if (decoder == NULL) {
        printf("audio: opus decoder init failed (%d)\n", err);
        return -1;
    }
    dropped_frames = 0;
    return 0;
}

static void audio_cleanup(void)
{
    if (decoder) {
        opus_multistream_decoder_destroy(decoder);
        decoder = NULL;
    }
}

static void audio_decode_and_play(char *data, int len)
{
    int samples, bytes, i;

    samples = opus_multistream_decode(decoder, (const unsigned char *)data, len, pcm, MAX_FRAME_SAMPLES, 0);
    if (samples <= 0)
        return;

    bytes = samples * channels * (int)sizeof(int16_t);

    /* The sound driver overwrites unplayed audio if we exceed free space. */
    if (xenon_sound_get_unplayed() > MAX_QUEUED_BYTES || xenon_sound_get_free() < bytes) {
        dropped_frames++;
        return;
    }

    for (i = 0; i < samples * channels; i++)
        pcm[i] = (int16_t)__builtin_bswap16((uint16_t)pcm[i]);

    xenon_sound_submit(pcm, bytes);
}

void audio_init_callbacks(AUDIO_RENDERER_CALLBACKS *cb)
{
    LiInitializeAudioCallbacks(cb);
    cb->init = audio_init;
    cb->cleanup = audio_cleanup;
    cb->decodeAndPlaySample = audio_decode_and_play;
    cb->capabilities = CAPABILITY_DIRECT_SUBMIT;
}

bool audio_has_surround(void)
{
    return false;   /* the audio DMA ring here is stereo */
}
