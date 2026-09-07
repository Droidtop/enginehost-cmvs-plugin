#include "audio.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * stb_vorbis is the whole decoder, and it is INCLUDED rather than compiled on
 * its own so both builds pick it up from the sources they already glob: the
 * desktop makefile globs the src directory and the Android CMakeLists globs
 * directory, and neither has to name a vendored file.
 */
#define STB_VORBIS_NO_STDIO
#define STB_VORBIS_NO_PUSHDATA_API
#include "stb_vorbis.c"

#define CHANNELS (CMVS_SOUND_KINDS * CMVS_SOUND_BANKS)
#define SOURCE_FRAMES 1024
#define MIX_BLOCK 512

typedef struct {
    int in_use;

    uint8_t *file;              /* the Ogg stream, held while it plays */
    int file_size;
    stb_vorbis *decoder;
    int source_rate;
    int source_channels;
    int loop;

    /* Decoded source frames, and where in them the resampler is reading. */
    short source[SOURCE_FRAMES * 2];
    int source_have;
    int source_at;
    int drained;                /* the decoder has no more to give */

    /* Linear resampling from the source rate to the mixer's. */
    short current[2];
    short next[2];
    double phase;
    double step;
    int primed;

    /* Volume, 0 to 1, and the slide towards it. */
    double volume;
    double target;
    long fade_left;             /* output frames still to slide over */
    long fade_total;
    double fade_from;
    int stop_when_faded;
} channel;

struct cmvs_audio {
    cmvs_game *game;
    int rate;
    pthread_mutex_t lock;
    channel channel[CHANNELS];
};

static channel *slot_of(cmvs_audio *audio, int kind, int bank)
{
    if (kind < 0 || kind >= CMVS_SOUND_KINDS) return NULL;
    if (bank < 0 || bank >= CMVS_SOUND_BANKS) return NULL;
    return &audio->channel[kind * CMVS_SOUND_BANKS + bank];
}

static void release(channel *slot)
{
    if (slot->decoder) stb_vorbis_close(slot->decoder);
    free(slot->file);
    memset(slot, 0, sizeof *slot);
}

cmvs_audio *cmvs_audio_new(cmvs_game *game, int rate)
{
    cmvs_audio *audio = calloc(1, sizeof *audio);
    if (!audio) return NULL;
    audio->game = game;
    audio->rate = rate > 0 ? rate : 48000;
    if (pthread_mutex_init(&audio->lock, NULL) != 0) {
        free(audio);
        return NULL;
    }
    return audio;
}

void cmvs_audio_free(cmvs_audio *audio)
{
    int i;
    if (!audio) return;
    for (i = 0; i < CHANNELS; i++) release(&audio->channel[i]);
    pthread_mutex_destroy(&audio->lock);
    free(audio);
}

int cmvs_audio_rate(const cmvs_audio *audio) { return audio ? audio->rate : 0; }

static double clamp_volume(int volume)
{
    if (volume < 0) return 0.0;
    if (volume > 255) return 1.0;
    return volume / 255.0;
}

int cmvs_audio_play(cmvs_audio *audio, int kind, int bank, const char *name,
                    int loop, int volume)
{
    uint8_t *file;
    int size = 0, error = 0;
    stb_vorbis *decoder;
    stb_vorbis_info info;
    channel *slot;
    char why[256];

    if (!audio || !name || !name[0]) return 0;
    if (!slot_of(audio, kind, bank)) return 0;

    /* Read and decode outside the lock: this touches the card and takes a
     * while, and the mixer must not stall on a scene loading its music. */
    file = cmvs_game_sound(audio->game, name, &size, why, sizeof why);
    if (!file) return 0;
    decoder = stb_vorbis_open_memory(file, size, &error, NULL);
    if (!decoder) {
        free(file);
        return 0;
    }
    info = stb_vorbis_get_info(decoder);

    pthread_mutex_lock(&audio->lock);
    slot = slot_of(audio, kind, bank);
    release(slot);
    slot->in_use = 1;
    slot->file = file;
    slot->file_size = size;
    slot->decoder = decoder;
    slot->source_rate = (int) info.sample_rate;
    slot->source_channels = info.channels < 1 ? 1 : info.channels > 2 ? 2 : info.channels;
    slot->loop = loop;
    slot->step = slot->source_rate > 0
        ? (double) slot->source_rate / (double) audio->rate : 1.0;
    slot->volume = clamp_volume(volume);
    slot->target = slot->volume;
    pthread_mutex_unlock(&audio->lock);
    return 1;
}

void cmvs_audio_stop(cmvs_audio *audio, int kind, int bank, int ms)
{
    channel *slot;
    if (!audio) return;
    pthread_mutex_lock(&audio->lock);
    slot = slot_of(audio, kind, bank);
    if (slot && slot->in_use) {
        long frames = ms > 0 ? (long) ms * audio->rate / 1000 : 0;
        if (frames <= 0) {
            release(slot);
        } else {
            slot->fade_from = slot->volume;
            slot->target = 0.0;
            slot->fade_total = frames;
            slot->fade_left = frames;
            slot->stop_when_faded = 1;
        }
    }
    pthread_mutex_unlock(&audio->lock);
}

void cmvs_audio_stop_kind(cmvs_audio *audio, int kind, int ms)
{
    int bank;
    for (bank = 0; bank < CMVS_SOUND_BANKS; bank++)
        cmvs_audio_stop(audio, kind, bank, ms);
}

void cmvs_audio_stop_all(cmvs_audio *audio)
{
    int i;
    if (!audio) return;
    pthread_mutex_lock(&audio->lock);
    for (i = 0; i < CHANNELS; i++) release(&audio->channel[i]);
    pthread_mutex_unlock(&audio->lock);
}

int cmvs_audio_playing(const cmvs_audio *audio, int kind, int bank)
{
    cmvs_audio *a = (cmvs_audio *) audio;
    channel *slot;
    int playing;
    if (!audio) return 0;
    pthread_mutex_lock(&a->lock);
    slot = slot_of(a, kind, bank);
    playing = slot && slot->in_use;
    pthread_mutex_unlock(&a->lock);
    return playing;
}

void cmvs_audio_volume(cmvs_audio *audio, int kind, int bank, int volume)
{
    channel *slot;
    if (!audio) return;
    pthread_mutex_lock(&audio->lock);
    slot = slot_of(audio, kind, bank);
    if (slot && slot->in_use) {
        slot->volume = clamp_volume(volume);
        slot->target = slot->volume;
        slot->fade_left = 0;
        slot->fade_total = 0;
        slot->stop_when_faded = 0;
    }
    pthread_mutex_unlock(&audio->lock);
}

/* ---- mixing ---- */

/* Reads one source frame into out as stereo. Returns 0 when the sound is over. */
static int source_frame(channel *slot, short out[2])
{
    const short *frame;
    while (slot->source_at >= slot->source_have) {
        int frames;
        if (slot->drained) return 0;
        frames = stb_vorbis_get_samples_short_interleaved(
            slot->decoder, slot->source_channels, slot->source,
            SOURCE_FRAMES * slot->source_channels);
        if (frames <= 0 && slot->loop) {
            stb_vorbis_seek_start(slot->decoder);
            frames = stb_vorbis_get_samples_short_interleaved(
                slot->decoder, slot->source_channels, slot->source,
                SOURCE_FRAMES * slot->source_channels);
        }
        if (frames <= 0) {
            slot->drained = 1;
            return 0;
        }
        slot->source_have = frames;
        slot->source_at = 0;
    }
    frame = slot->source + (size_t) slot->source_at * slot->source_channels;
    out[0] = frame[0];
    out[1] = slot->source_channels >= 2 ? frame[1] : frame[0];
    slot->source_at++;
    return 1;
}

/*
 * Mixes one channel into a block of accumulated samples. Returns 0 when the
 * channel is finished and its slot should be freed, 1 while it plays on.
 */
static int mix_channel(channel *slot, int32_t *sum, int frames)
{
    int i, c;
    if (!slot->primed) {
        if (!source_frame(slot, slot->current)) return 0;
        if (!source_frame(slot, slot->next)) {
            slot->next[0] = slot->current[0];
            slot->next[1] = slot->current[1];
        }
        slot->phase = 0.0;
        slot->primed = 1;
    }
    for (i = 0; i < frames; i++) {
        while (slot->phase >= 1.0) {
            slot->current[0] = slot->next[0];
            slot->current[1] = slot->next[1];
            if (!source_frame(slot, slot->next)) return 0;
            slot->phase -= 1.0;
        }
        if (slot->fade_left > 0 && slot->fade_total > 0) {
            double done = (double) (slot->fade_total - slot->fade_left)
                        / (double) slot->fade_total;
            slot->volume = slot->fade_from + (slot->target - slot->fade_from) * done;
            slot->fade_left--;
            if (slot->fade_left == 0) {
                slot->volume = slot->target;
                if (slot->stop_when_faded) return 0;
            }
        }
        for (c = 0; c < 2; c++) {
            double value = slot->current[c]
                         + (slot->next[c] - slot->current[c]) * slot->phase;
            sum[i * 2 + c] += (int32_t) (value * slot->volume);
        }
        slot->phase += slot->step;
    }
    return 1;
}

void cmvs_audio_mix(cmvs_audio *audio, int16_t *out, int frames)
{
    int32_t sum[MIX_BLOCK * 2];
    int done, i;

    if (frames <= 0) return;
    memset(out, 0, (size_t) frames * 2 * sizeof *out);
    if (!audio) return;

    pthread_mutex_lock(&audio->lock);
    for (done = 0; done < frames; ) {
        int block = frames - done;
        if (block > MIX_BLOCK) block = MIX_BLOCK;
        memset(sum, 0, (size_t) block * 2 * sizeof *sum);
        for (i = 0; i < CHANNELS; i++) {
            channel *slot = &audio->channel[i];
            if (!slot->in_use) continue;
            if (!mix_channel(slot, sum, block)) release(slot);
        }
        for (i = 0; i < block * 2; i++) {
            int32_t value = sum[i];
            if (value > 32767) value = 32767;
            if (value < -32768) value = -32768;
            out[(done * 2) + i] = (int16_t) value;
        }
        done += block;
    }
    pthread_mutex_unlock(&audio->lock);
}
