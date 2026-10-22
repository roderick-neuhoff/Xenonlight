/*
 * GPU presentation: uploads Y/U/V planes as textures and lets a Xenos pixel
 * shader do the colour conversion and aspect-correct scaling. Rendering is
 * double buffered; the finished frame is shown without an extra frame of
 * latency (render into the back buffer, wait for the GPU, then flip).
 *
 * gpu_init() runs once on hardware thread 0. During a stream every other call
 * comes from the render thread only (libxenon's Xe API isn't thread-safe).
 */
#pragma once

#include <stdbool.h>
#include <libavutil/frame.h>

bool gpu_init(void);
bool gpu_ready(void);

/* Prepares textures for a stream of the given size and clears the screen. */
bool gpu_stream_begin(int width, int height);
/* Draws one frame and shows it; with vsync the flip waits for vertical blank. */
void gpu_present(const AVFrame *frame, bool vsync);
/* Points the display back at the text console's framebuffer. */
void gpu_stream_end(void);
