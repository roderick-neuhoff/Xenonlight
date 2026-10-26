#pragma once

#include <stdbool.h>
#include <Limelight.h>

void audio_init_callbacks(AUDIO_RENDERER_CALLBACKS *cb);

/* Whether this build can play 5.1 surround (the SDK build; libxenon is stereo only). */
bool audio_has_surround(void);
