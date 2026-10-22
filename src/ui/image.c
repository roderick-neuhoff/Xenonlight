#include "image.h"

#include <stdlib.h>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#define STBI_NO_HDR
#define STBI_NO_LINEAR
/* libxenon has no thread-local storage: __thread variables address through r2,
 * which holds garbage on bare metal and crashes. */
#define STBI_NO_THREAD_LOCALS
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#include "../../third_party/stb/stb_image.h"
#pragma GCC diagnostic pop

uint32_t *image_decode_argb(const unsigned char *data, size_t len, int *w, int *h)
{
    int n, i;
    unsigned char *rgba = stbi_load_from_memory(data, (int)len, w, h, &n, 4);
    uint32_t *out;

    if (rgba == NULL)
        return NULL;
    out = (uint32_t *)malloc((size_t)*w * *h * sizeof(uint32_t));
    if (out) {
        for (i = 0; i < *w * *h; i++) {
            const unsigned char *p = rgba + i * 4;
            out[i] = ((uint32_t)p[3] << 24) | ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];
        }
    }
    stbi_image_free(rgba);
    return out;
}
