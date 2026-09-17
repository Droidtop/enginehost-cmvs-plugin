/*
 * Decoder for CMV, the video container CMVS plays with command 0x308.
 *
 * A .cmv is a "CMVn" container whose payload is a run of JBPD chunks. JBPD is
 * the same transform codec as the JBP1 inside a PB3 - the same two 16-entry
 * Huffman trees, the same 16-byte run table, the same zigzag, the same IDCT and
 * the same YCbCr constants - wrapped differently, so this file shares jbp.c's
 * transform, trees, bitstream and pixel writer and only reads the wrapper.
 *
 * Transcribed from cmvs32.exe:
 *
 *   0x00432240   the container reader: the 0x2c-byte header, the frame table of
 *                (frames + 1) twenty-byte entries at 0x2c, and the MOVIE BLOCK
 *                that follows it, whose extent is the header's own +0x04 minus
 *                where the table ends.
 *   0x00518d20   the INTRA chunk, decoded once when the movie opens (reached
 *                from 0x00431060, which the player's start at 0x00431860 calls
 *                when the fourth magic byte is '9').
 *   0x00519250   a FRAME chunk, called per frame from 0x00431760 -> 0x00431310.
 *   0x0050c490   the Huffman builder, 0x0050c6b0 a symbol, 0x0050c700 a run.
 *   0x00545140   the IDCT and 0x0050e190 the pixel writer; both agree with the
 *                pair jbp.c already transcribes (0x00519c50 and 0x0051a080's
 *                own writer), which is why they are not transcribed twice.
 *   0x00432633   the audio: the LAST frame-table entry, the one whose codec is
 *                0, is an Ogg Vorbis stream, and the header's +0x28 says
 *                whether there is one.
 *
 * What makes a movie small: the movie block carries a MACROBLOCK MASK, one bit
 * per 16x16 macroblock of the whole picture. The intra codes the macroblocks
 * whose bit is SET - the part of the frame that never moves - and every frame
 * chunk codes the macroblocks whose bit is CLEAR, the part that does. In
 * ChronoClock's bg350a.cmv that is 2979 static and 621 moving macroblocks out
 * of 3600, which is exactly the 17874 and 3726 DC coefficients the intra's and
 * a frame's own headers declare. A delta frame carries a second, per-frame mask
 * as well, and a macroblock whose bit is set there is decoded and then NOT
 * written - 0x00519618 consumes its coefficients and skips the write - so the
 * picture keeps the previous frame's pixels in it.
 *
 * Because every frame chunk re-codes the whole moving set, a frame needs the
 * one before it only for the pixels its own mask leaves alone. That is why
 * 0x00431760 hands the decoder a flag saying whether this frame follows the
 * last one drawn, and why the per-frame mask is ignored when it does not.
 *
 * Pixels come out BGRA, top-down, stride 4 * width - the layout pb3.h uses, so
 * a decoded frame can be worn by a graphic object without a conversion.
 *
 * ONLY CMV9 CARRIES A MOVIE BLOCK. 0x004324ab reads the fourth magic byte and
 * builds the block for '9' alone. A CMV6 has no static mask and no intra: every
 * frame codes every macroblock, its DC count is the grid rather than a header
 * field (its own +0x24 is zero), and its per-frame mask sits inline with no
 * length in front of it. That is the whole difference between 0x00513a90 and
 * 0x00519250, which is why one reader here serves both.
 *
 * 0x00431509 is the jump table that picks a decoder per generation. CMV7 and
 * CMV8 are refused rather than half-read: their frames carry an alpha plane
 * (a CMV8 header says 32 bits per pixel and its frame table's expanded size is
 * width * height * 4 + 54) and their decoders run two helpers, 0x0050ff00 and
 * 0x005119c0, that nothing else calls. CMV5 is refused because no game here has
 * one to check a reading against.
 */
#ifndef CMVS_CMV_H
#define CMVS_CMV_H

#include <stddef.h>
#include <stdint.h>

typedef struct cmv_movie cmv_movie;

/*
 * Parses the container. `data` is borrowed and must outlive the movie. On
 * failure returns NULL and fills err with a sentence saying why.
 */
cmv_movie *cmv_open(const uint8_t *data, int size, char *err, size_t errlen);
void cmv_close(cmv_movie *m);

int cmv_width(const cmv_movie *m);
int cmv_height(const cmv_movie *m);
/* How many frame chunks the container holds (its +0x10). */
int cmv_frames(const cmv_movie *m);
/* The container's own +0x14 and +0x18. The frame clock at 0x00431c2c steps
 * (elapsed * fps) / 1000 / step frames at a time. */
int cmv_fps(const cmv_movie *m);
int cmv_step(const cmv_movie *m);
/* The macroblock grid the movie block declares, 80 x 45 for a 1280x720 movie. */
int cmv_mb_width(const cmv_movie *m);
int cmv_mb_height(const cmv_movie *m);

/*
 * Whether the macroblock at (x, y) of that grid is one of the STATIC ones: the
 * set the intra codes once and no frame chunk ever touches. The mask is walked
 * tile by tile rather than row by row, so this is also how a caller outside the
 * decoder can index it correctly.
 */
int cmv_static(const cmv_movie *m, int x, int y);

/*
 * Lays the intra picture down. This is what 0x00431060 does when the movie
 * opens, and it is what makes the static five sixths of the frame exist at all.
 */
int cmv_intra(cmv_movie *m, char *err, size_t errlen);

/*
 * Decodes frame `index` (0-based) onto the surface. `follows` is 0x00431760's
 * own flag: 1 when this frame is the one after the frame last drawn, which is
 * the only case in which the frame's own mask may leave pixels alone.
 */
int cmv_frame(cmv_movie *m, int index, int follows, char *err, size_t errlen);

/*
 * The surface, BGRA top-down. It is the macroblock grid's size, which is the
 * picture rounded up to sixteen, so the stride is asked for rather than assumed.
 * Owned by the movie.
 */
const uint8_t *cmv_pixels(const cmv_movie *m);
int cmv_stride(const cmv_movie *m);

/*
 * The audio stream the container carries, if any: the CMV6 movies in
 * ChronoClock's data/video hold an Ogg Vorbis track here and the CMV9
 * backgrounds in video.cpz hold none. Answers 0 when there is none. The bytes
 * belong to the buffer cmv_open was given.
 */
int cmv_audio(const cmv_movie *m, const uint8_t **data, int *size);

#endif
