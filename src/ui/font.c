#include "font.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#ifdef __GNUC__
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#include "../../third_party/stb/stb_truetype.h"
#ifdef __GNUC__
#pragma GCC diagnostic pop
#endif

/* The embedded TTF files (fonts.S, or a generated C array in the SDK build). */
#ifdef __cplusplus
extern "C" {
#endif
extern const unsigned char font_regular_ttf[];
extern const unsigned char font_bold_ttf[];
#ifdef __cplusplus
}
#endif

static stbtt_fontinfo faces[2];
static bool ready;

/* Glyph cache: open addressing on (face, size, codepoint). */
#define CACHE_SIZE 4096

struct glyph {
    uint32_t key_cp;
    uint16_t key_size;
    uint8_t key_face, used;
    int16_t xoff, yoff, w, h;
    float advance;
    uint8_t *bitmap;
};

static struct glyph cache[CACHE_SIZE];

bool font_init(void)
{
    if (ready)
        return true;
    if (!stbtt_InitFont(&faces[FONT_REGULAR], font_regular_ttf, stbtt_GetFontOffsetForIndex(font_regular_ttf, 0)) ||
        !stbtt_InitFont(&faces[FONT_BOLD], font_bold_ttf, stbtt_GetFontOffsetForIndex(font_bold_ttf, 0)))
        return false;
    ready = true;
    return true;
}

static float scale_for(int face, float px)
{
    return stbtt_ScaleForPixelHeight(&faces[face], px);
}

float font_ascent(int face, float px)
{
    int ascent, descent, gap;

    stbtt_GetFontVMetrics(&faces[face], &ascent, &descent, &gap);
    return ascent * scale_for(face, px);
}

static const struct glyph *get_glyph(int face, int size, uint32_t cp)
{
    uint32_t h = (cp * 2654435761u) ^ ((uint32_t)size * 40503u) ^ ((uint32_t)face << 30);
    unsigned i, n;

    for (n = 0, i = h % CACHE_SIZE; n < CACHE_SIZE; n++, i = (i + 1) % CACHE_SIZE) {
        struct glyph *g = &cache[i];

        if (g->used && g->key_cp == cp && g->key_size == size && g->key_face == face)
            return g;
        if (!g->used) {
            float scale = scale_for(face, (float)size);
            int adv, lsb, x0, y0, x1, y1;
            int idx = stbtt_FindGlyphIndex(&faces[face], (int)cp);

            if (idx == 0 && cp != ' ')
                idx = stbtt_FindGlyphIndex(&faces[face], '?');
            stbtt_GetGlyphHMetrics(&faces[face], idx, &adv, &lsb);
            stbtt_GetGlyphBitmapBox(&faces[face], idx, scale, scale, &x0, &y0, &x1, &y1);
            g->w = (int16_t)(x1 - x0);
            g->h = (int16_t)(y1 - y0);
            g->xoff = (int16_t)x0;
            g->yoff = (int16_t)y0;
            g->advance = adv * scale;
            g->bitmap = NULL;
            if (g->w > 0 && g->h > 0) {
                g->bitmap = (uint8_t *)malloc((size_t)g->w * g->h);
                if (g->bitmap)
                    stbtt_MakeGlyphBitmap(&faces[face], g->bitmap, g->w, g->h, g->w, scale, scale, idx);
            }
            g->key_cp = cp;
            g->key_size = (uint16_t)size;
            g->key_face = (uint8_t)face;
            g->used = 1;
            return g;
        }
    }
    return NULL;   /* cache full: glyph skipped */
}

/* Decodes one UTF-8 sequence; invalid bytes come back as '?'. */
static uint32_t next_cp(const char **s)
{
    const unsigned char *p = (const unsigned char *)*s;
    uint32_t cp;
    int n, i;

    if (p[0] < 0x80) { cp = p[0]; n = 1; }
    else if ((p[0] & 0xE0) == 0xC0) { cp = p[0] & 0x1F; n = 2; }
    else if ((p[0] & 0xF0) == 0xE0) { cp = p[0] & 0x0F; n = 3; }
    else if ((p[0] & 0xF8) == 0xF0) { cp = p[0] & 0x07; n = 4; }
    else { *s += 1; return '?'; }
    for (i = 1; i < n; i++) {
        if ((p[i] & 0xC0) != 0x80) {
            *s += i;
            return '?';
        }
        cp = (cp << 6) | (p[i] & 0x3F);
    }
    *s += n;
    return cp;
}

int font_text_width(int face, float px, const char *text)
{
    int size = (int)(px + 0.5f);
    float w = 0;

    while (*text) {
        const struct glyph *g = get_glyph(face, size, next_cp(&text));
        if (g)
            w += g->advance;
    }
    return (int)ceilf(w);
}

static float draw_run(canvas_t *c, int face, int size, float x, int y, uint32_t argb, const char *s, const char *end)
{
    while (*s && (end == NULL || s < end)) {
        const struct glyph *g = get_glyph(face, size, next_cp(&s));
        if (g == NULL)
            continue;
        if (c && g->bitmap)
            cv_mask(c, (int)floorf(x + 0.5f) + g->xoff, y + g->yoff, g->bitmap, g->w, g->h, g->w, argb);
        x += g->advance;
    }
    return x;
}

int font_draw(canvas_t *c, int face, float px, int x, int y, uint32_t argb, const char *text)
{
    return (int)draw_run(c, face, (int)(px + 0.5f), (float)x, y, argb, text, NULL);
}

int font_draw_fit(canvas_t *c, int face, float px, int x, int y, int max_w, uint32_t argb, const char *text)
{
    int size = (int)(px + 0.5f);
    const char *s = text, *cut = text;
    float w = 0, ell;

    if (font_text_width(face, px, text) <= max_w)
        return font_draw(c, face, px, x, y, argb, text);

    ell = (float)font_text_width(face, px, "...");
    while (*s) {
        const char *before = s;
        const struct glyph *g = get_glyph(face, size, next_cp(&s));
        if (g && w + g->advance + ell > max_w) {
            cut = before;
            break;
        }
        if (g)
            w += g->advance;
        cut = s;
    }
    x = (int)draw_run(c, face, size, (float)x, y, argb, text, cut);
    return font_draw(c, face, px, x, y, argb, "...");
}

int font_draw_wrapped(canvas_t *c, int face, float px, int x, int y, int w, float line_h, uint32_t argb,
                      const char *text)
{
    int size = (int)(px + 0.5f);
    float space = 0;
    const char *line = text;
    int lines = 0;

    {
        const struct glyph *g = get_glyph(face, size, ' ');
        if (g)
            space = g->advance;
    }

    while (*line) {
        const char *p = line, *last_break = NULL;
        float lw = 0;

        /* Greedy: take words while they fit; explicit \n ends the line. */
        while (*p && *p != '\n') {
            float ww = 0;
            while (*p && *p != ' ' && *p != '\n') {
                const struct glyph *g = get_glyph(face, size, next_cp(&p));
                if (g)
                    ww += g->advance;
            }
            if (lw > 0 && lw + space + ww > w)
                break;   /* the next line restarts at last_break */
            lw += (lw > 0 ? space : 0) + ww;
            last_break = p;
            while (*p == ' ')
                p++;
        }
        if (last_break == NULL)
            last_break = p;   /* single over-long word: draw it anyway */
        if (c)
            draw_run(c, face, size, (float)x, y + (int)(lines * line_h), argb, line, last_break);
        lines++;
        line = last_break;
        while (*line == ' ')
            line++;
        if (*line == '\n')
            line++;
    }
    return lines;
}
