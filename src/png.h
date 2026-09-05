/*
 * A PNG writer, for looking at what the engine drew without a display.
 *
 * The desktop runner's --shot writes one of these; nothing in the engine needs
 * it at run time. It takes the same BGRA the rest of the engine works in.
 */
#ifndef CMVS_PNG_H
#define CMVS_PNG_H

#include <stddef.h>
#include <stdint.h>

/* Returns 1 on success. `pixels` is BGRA, top-down, stride 4 * width. */
int cmvs_png_write(const char *path, const uint8_t *pixels, int width, int height,
                   char *err, size_t errlen);

#endif
