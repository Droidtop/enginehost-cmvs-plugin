/*
 * The game's sound: music, effects and voices.
 *
 * A CMVS script names a sound as a file: command 0x0a0 asks for "bgm37.ogg",
 * 0x0b0 for "sys101.ogg". Where that file lives is not in the name and not in
 * the engine binary either - a game keeps its music loose beside the pack
 * folder and its effects and voices inside archives - so the file layer is
 * asked for the name and it searches, exactly as it does for an image.
 *
 * The kinds are the ones the engine keeps separate subsystems for:
 *   MUSIC   one stream, the subsystem at +0xc58, commands 0x0a0..0x0a6.
 *   EFFECT  six banks, the subsystem at +0xc54, commands 0x0b0..0x0b7;
 *           0x004625a0 rejects a bank above 5 and puts bank N on channel N+9.
 *   VOICE   the line's own sound, started by the wait's first argument.
 *
 * The engine MIXES; it does not open a sound device, because the two frontends
 * open different ones (SDL2 on the desktop, AAudio on Android). The frontend
 * asks for the next block of samples from its own audio thread, so every
 * function here is safe to call while cmvs_audio_mix is running.
 */
#ifndef CMVS_AUDIO_H
#define CMVS_AUDIO_H

#include <stdint.h>

#include "game.h"

#define CMVS_SOUND_MUSIC  0
#define CMVS_SOUND_EFFECT 1
#define CMVS_SOUND_VOICE  2
#define CMVS_SOUND_KINDS  3

/* 0x004625a0 and 0x004627e0 both refuse a bank above five. */
#define CMVS_SOUND_BANKS 6

typedef struct cmvs_audio cmvs_audio;

/*
 * rate is the frontend's output rate in samples per second; a sound recorded at
 * another rate is resampled to it. Output is always two channels, interleaved
 * 16-bit. The game must outlive the mixer.
 */
cmvs_audio *cmvs_audio_new(cmvs_game *game, int rate);
void cmvs_audio_free(cmvs_audio *audio);

int cmvs_audio_rate(const cmvs_audio *audio);

/*
 * Starts a named file on a bank, replacing what it held. Returns 1, or 0 when
 * no archive and no loose file has that name - worth logging, because it means
 * a name the script built rather than one the game ships.
 */
int cmvs_audio_play(cmvs_audio *audio, int kind, int bank, const char *name,
                    int loop, int volume);

/* Stops a bank over `ms` milliseconds - the unit the fade commands are written
 * in (0x0a2 asks for 2000). Zero stops it at once. */
void cmvs_audio_stop(cmvs_audio *audio, int kind, int bank, int ms);
void cmvs_audio_stop_kind(cmvs_audio *audio, int kind, int ms);
void cmvs_audio_stop_all(cmvs_audio *audio);

/* Whether a bank is sounding, which is how "is the music still going" is asked
 * (command 0x0a3 answers this in sys[0]). */
int cmvs_audio_playing(const cmvs_audio *audio, int kind, int bank);

/* Volume 0..255, the range the engine's own arithmetic produces: the music
 * volume is (+0x614 * +0x618) >> 8 and an effect's is (+0x614 * +0x61c) >> 8. */
void cmvs_audio_volume(cmvs_audio *audio, int kind, int bank, int volume);

/*
 * Mixes the next `frames` stereo frames into out, which holds frames * 2
 * samples. Silence is written when nothing is playing, so a frontend can call
 * this unconditionally. Called from the frontend's audio thread.
 */
void cmvs_audio_mix(cmvs_audio *audio, int16_t *out, int frames);

#endif
