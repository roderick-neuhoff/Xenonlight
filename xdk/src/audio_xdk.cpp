/*
 * Opus decode and playback through XAudio2 (48 kHz, 16-bit, native
 * big-endian samples), stereo or 5.1. Runs on Moonlight's audio thread.
 *
 * The 360's mastering voice is always 5.1 (the system downmixes for a stereo
 * TV), in the same channel order Moonlight's Opus streams decode to:
 * FL FR FC LFE BL BR. So a 6-channel source voice maps straight through.
 */
#include "xdk.h"

#include <xaudio2.h>
#include <string.h>

#include "../../src/audio.h"

#include <opus_multistream.h>

#define MAX_FRAME_SAMPLES 960   /* 20 ms at 48 kHz, the largest Moonlight uses */
#define MAX_CHANNELS 6
#define BUFFERS 16
/* Keep the queue short so audio stays in sync with the picture (~40-80 ms). */
#define MAX_QUEUED 8

static IXAudio2 *xaudio;
static IXAudio2MasteringVoice *master;
static IXAudio2SourceVoice *voice;
static OpusMSDecoder *decoder;
static int channels;
static int16_t pcm[BUFFERS][MAX_FRAME_SAMPLES * MAX_CHANNELS];
static int next_buffer;

static int audio_init(int config, const POPUS_MULTISTREAM_CONFIGURATION opus, void *ctx, int flags)
{
    WAVEFORMATEX wfx;
    int err;
    (void)config;
    (void)ctx;
    (void)flags;

    if (opus->channelCount != 2 && opus->channelCount != 6)
        return -1;
    channels = opus->channelCount;
    decoder = opus_multistream_decoder_create(opus->sampleRate, opus->channelCount, opus->streams,
                                              opus->coupledStreams, opus->mapping, &err);
    if (decoder == NULL)
        return -1;

    if (xaudio == NULL) {
        if (FAILED(XAudio2Create(&xaudio, 0, XboxThread5)) ||
            FAILED(xaudio->CreateMasteringVoice(&master))) {
            if (xaudio)
                xaudio->Release();
            xaudio = NULL;
            return -1;
        }
    }

    ZeroMemory(&wfx, sizeof(wfx));
    wfx.wFormatTag = WAVE_FORMAT_PCM;
    wfx.nChannels = (WORD)channels;
    wfx.nSamplesPerSec = opus->sampleRate;
    wfx.wBitsPerSample = 16;
    wfx.nBlockAlign = (WORD)(channels * 2);
    wfx.nAvgBytesPerSec = opus->sampleRate * channels * 2;
    if (FAILED(xaudio->CreateSourceVoice(&voice, &wfx))) {
        voice = NULL;
        return -1;
    }
    voice->Start(0);
    next_buffer = 0;
    return 0;
}

static void audio_cleanup(void)
{
    if (voice) {
        voice->Stop(0);
        voice->DestroyVoice();
        voice = NULL;
    }
    if (decoder) {
        opus_multistream_decoder_destroy(decoder);
        decoder = NULL;
    }
}

static void audio_decode_and_play(char *data, int len)
{
    XAUDIO2_VOICE_STATE state;
    XAUDIO2_BUFFER buf;
    int16_t *out = pcm[next_buffer];
    int samples;

    voice->GetState(&state);
    if (state.BuffersQueued >= MAX_QUEUED)
        return;   /* behind: drop this packet rather than add latency */

    samples = opus_multistream_decode(decoder, (const unsigned char *)data, len, out, MAX_FRAME_SAMPLES, 0);
    if (samples <= 0)
        return;

    ZeroMemory(&buf, sizeof(buf));
    buf.AudioBytes = samples * channels * sizeof(int16_t);
    buf.pAudioData = (const BYTE *)out;
    if (SUCCEEDED(voice->SubmitSourceBuffer(&buf)))
        next_buffer = (next_buffer + 1) % BUFFERS;
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
    return true;
}
