#pragma once

#include <stdbool.h>
#include "canvas.h"

enum { FONT_REGULAR, FONT_BOLD };

bool font_init(void);

/* Sizes are pixel heights (ascent - descent). Text is UTF-8. */
int font_text_width(int face, float px, const char *text);
float font_ascent(int face, float px);

/* Draws text with its baseline at y. Returns the pen x after the text. */
int font_draw(canvas_t *c, int face, float px, int x, int y, uint32_t argb, const char *text);
/* Like font_draw but truncates with "..." to fit max_w. */
int font_draw_fit(canvas_t *c, int face, float px, int x, int y, int max_w, uint32_t argb, const char *text);
/* Word-wraps into a box of width w starting at baseline y; returns lines drawn.
 * With c == NULL it only counts lines. */
int font_draw_wrapped(canvas_t *c, int face, float px, int x, int y, int w, float line_h, uint32_t argb,
                      const char *text);
