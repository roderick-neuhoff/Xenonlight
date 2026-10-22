#pragma once

#include <stddef.h>
#include <stdint.h>

/* Decodes PNG/JPEG data into 0xAARRGGBB pixels (caller frees), or NULL. */
uint32_t *image_decode_argb(const unsigned char *data, size_t len, int *w, int *h);
