/*
 * The Android side of the CMVS engine: nothing but a way in.
 *
 * The engine is the C in this repository's src, the same code the desktop
 * runner builds. It opens the game's archives, runs the bytecode and composes a
 * frame into a buffer of pixels; this file starts a session, steps it, and
 * hands that buffer to Java. Nothing about CMVS is decided here, and nothing
 * about it is written twice.
 */
#include <errno.h>
#include <jni.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include <aaudio/AAudio.h>
#include <android/log.h>

#include "broker.h"
#include "session.h"

#define TAG "cmvs"

/*
 * The isolated launch's audio ring (Enginehost docs/engine-sandbox.md
 * "Audio"): an isolated process cannot reach AudioFlinger to open its own
 * output at all, so a game running isolated never opens AAudio here.
 * Instead this file renders into a plain shared-memory ring the host reads
 * from and plays on a real AudioTrack it owns -- the same layout as
 * dev.enginehost.EngineHost#isolatedAudioBuffer's own doc comment and
 * IsolatedRuntimeHost.kt's IsolatedAudioBridge on the host side (identical
 * to enginehost-catsystem2-plugin's own jni.c: one wire format, not one
 * per engine): a 16-byte header (write position, read position, capacity,
 * reserved, each little-endian uint32) followed by AUDIO_RING_CAPACITY
 * bytes of ring data. This side only ever advances the write position; the
 * host only ever advances the read position.
 */
#define AUDIO_HEADER_SIZE 16
#define AUDIO_RING_CAPACITY (32 * 1024)
/* One chunk is 1024 stereo 16-bit frames -- 4096 bytes, the same size the
   host's own consumer thread reads in one pass. */
#define AUDIO_CHUNK_FRAMES 1024

/*
 * Why a session that failed to open has no handle to carry its reason: the
 * error belongs to the attempt, not to a game. One buffer, read immediately
 * after a failed open, is what the plugin needs and all it needs.
 */
static char last_error[256];

/*
 * A dev.enginehost.api.EngineFileBroker, reached from native code
 * (Enginehost docs/engine-sandbox.md "Host file service design"). Callbacks
 * can fire from whatever thread the engine is stepped on -- an isolated
 * runtime's step()/save calls arrive over Binder, which does not promise the
 * same pool thread twice -- so this holds a JavaVM and attaches per call
 * rather than assuming the JNIEnv that created it is still the right one.
 */
typedef struct {
    JavaVM *vm;
    jobject broker;       /* global ref */
    jclass broker_class;  /* global ref */
    jmethodID list_method;
    jmethodID open_read_method;
    jmethodID open_write_method;
    jmethodID commit_write_method;
    jmethodID delete_method;
} jni_broker;

typedef struct {
    cmvs_session *session;
    int width, height;
    /*
     * The frame the screen is already showing, BGRA, so nativeFrame can
     * hand Java only the rows that changed -- a reader reading a line of
     * dialogue is looking at a picture that mostly does not change, and
     * copying two and a half megabytes of pixels sixty times a second for
     * nothing would be work with no reader on the other end of it.
     */
    uint8_t *shown;
    /* Non-NULL only for an isolated launch (Enginehost docs/engine-sandbox.md). */
    jni_broker *game_broker;
    jni_broker *save_broker;
    cmvs_broker game_broker_ops;
    cmvs_broker save_broker_ops;
    AAudioStream *sound;
    /*
     * The isolated launch's audio ring, and the thread that renders into
     * it. Non-NULL/running only when nativeOpenIsolated was handed a real
     * host-side buffer; an isolated launch with none simply plays
     * silently rather than touching AAudio, which would hang.
     */
    void *audio_ring;
    size_t audio_ring_size;
    int audio_ring_fd;
    pthread_t audio_thread;
    volatile int audio_thread_running;
} session;

static jni_broker *jni_broker_create(JNIEnv *env, jobject broker) {
    if (broker == NULL) return NULL;
    jni_broker *jb = calloc(1, sizeof *jb);
    if (jb == NULL) return NULL;
    (*env)->GetJavaVM(env, &jb->vm);
    jb->broker = (*env)->NewGlobalRef(env, broker);
    jclass local_class = (*env)->GetObjectClass(env, broker);
    jb->broker_class = (jclass) (*env)->NewGlobalRef(env, local_class);
    (*env)->DeleteLocalRef(env, local_class);
    jb->list_method = (*env)->GetMethodID(
        env, jb->broker_class, "list", "(Ljava/lang/String;)[Ljava/lang/String;");
    jb->open_read_method = (*env)->GetMethodID(
        env, jb->broker_class, "openRead", "(Ljava/lang/String;)Landroid/os/ParcelFileDescriptor;");
    jb->open_write_method = (*env)->GetMethodID(
        env, jb->broker_class, "openWrite", "(Ljava/lang/String;)Landroid/os/ParcelFileDescriptor;");
    jb->commit_write_method = (*env)->GetMethodID(env, jb->broker_class, "commitWrite", "(Ljava/lang/String;)V");
    jb->delete_method = (*env)->GetMethodID(env, jb->broker_class, "delete", "(Ljava/lang/String;)V");
    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionClear(env);
        (*env)->DeleteGlobalRef(env, jb->broker);
        (*env)->DeleteGlobalRef(env, jb->broker_class);
        free(jb);
        return NULL;
    }
    return jb;
}

static void jni_broker_free(JNIEnv *env, jni_broker *jb) {
    if (jb == NULL) return;
    if (jb->broker != NULL) (*env)->DeleteGlobalRef(env, jb->broker);
    if (jb->broker_class != NULL) (*env)->DeleteGlobalRef(env, jb->broker_class);
    free(jb);
}

/* A JNIEnv valid on the calling thread, attaching it to the JVM if this is the first call on it. */
static JNIEnv *jni_broker_env(jni_broker *jb, int *attached) {
    JNIEnv *env = NULL;
    *attached = 0;
    if ((*jb->vm)->GetEnv(jb->vm, (void **) &env, JNI_VERSION_1_6) != JNI_OK) {
        if ((*jb->vm)->AttachCurrentThread(jb->vm, &env, NULL) != 0) return NULL;
        *attached = 1;
    }
    return env;
}

/* Takes a ParcelFileDescriptor's underlying fd as this process's own (dup'd) and releases the Java object. */
static int jni_pfd_take(JNIEnv *env, jobject pfd) {
    if (pfd == NULL) return -1;
    jclass pfd_class = (*env)->GetObjectClass(env, pfd);
    jmethodID get_fd = (*env)->GetMethodID(env, pfd_class, "getFd", "()I");
    jint raw_fd = (*env)->CallIntMethod(env, pfd, get_fd);
    int native_fd = (*env)->ExceptionCheck(env) ? -1 : dup((int) raw_fd);
    jmethodID close_method = (*env)->GetMethodID(env, pfd_class, "close", "()V");
    (*env)->CallVoidMethod(env, pfd, close_method);
    (*env)->ExceptionClear(env); /* close() may throw; the dup above already has its own fd either way */
    (*env)->DeleteLocalRef(env, pfd_class);
    return native_fd;
}

static int broker_list(void *ctx, const char *relative_path, char names[][256], int max_names) {
    jni_broker *jb = (jni_broker *) ctx;
    int attached;
    JNIEnv *env = jni_broker_env(jb, &attached);
    if (env == NULL) return -1;
    jstring jpath = (*env)->NewStringUTF(env, relative_path);
    jobjectArray result = (jobjectArray) (*env)->CallObjectMethod(env, jb->broker, jb->list_method, jpath);
    int count = -1;
    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionClear(env);
    } else {
        count = 0;
        if (result != NULL) {
            jsize n = (*env)->GetArrayLength(env, result);
            for (jsize i = 0; i < n && count < max_names; i++) {
                jstring entry = (jstring) (*env)->GetObjectArrayElement(env, result, i);
                const char *bytes = (*env)->GetStringUTFChars(env, entry, NULL);
                snprintf(names[count], 256, "%s", bytes);
                (*env)->ReleaseStringUTFChars(env, entry, bytes);
                (*env)->DeleteLocalRef(env, entry);
                count++;
            }
        }
    }
    if (result != NULL) (*env)->DeleteLocalRef(env, result);
    (*env)->DeleteLocalRef(env, jpath);
    if (attached) (*jb->vm)->DetachCurrentThread(jb->vm);
    return count;
}

static int broker_open_read(void *ctx, const char *relative_path) {
    jni_broker *jb = (jni_broker *) ctx;
    int attached;
    JNIEnv *env = jni_broker_env(jb, &attached);
    if (env == NULL) return -1;
    jstring jpath = (*env)->NewStringUTF(env, relative_path);
    jobject pfd = (*env)->CallObjectMethod(env, jb->broker, jb->open_read_method, jpath);
    int fd = -1;
    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionClear(env);
    } else {
        fd = jni_pfd_take(env, pfd);
    }
    if (pfd != NULL) (*env)->DeleteLocalRef(env, pfd);
    (*env)->DeleteLocalRef(env, jpath);
    if (attached) (*jb->vm)->DetachCurrentThread(jb->vm);
    return fd;
}

static int broker_open_write(void *ctx, const char *relative_path) {
    jni_broker *jb = (jni_broker *) ctx;
    int attached;
    JNIEnv *env = jni_broker_env(jb, &attached);
    if (env == NULL) return -1;
    jstring jpath = (*env)->NewStringUTF(env, relative_path);
    jobject pfd = (*env)->CallObjectMethod(env, jb->broker, jb->open_write_method, jpath);
    int fd = -1;
    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionClear(env);
    } else {
        fd = jni_pfd_take(env, pfd);
    }
    if (pfd != NULL) (*env)->DeleteLocalRef(env, pfd);
    (*env)->DeleteLocalRef(env, jpath);
    if (attached) (*jb->vm)->DetachCurrentThread(jb->vm);
    return fd;
}

static int broker_commit_write(void *ctx, const char *relative_path) {
    jni_broker *jb = (jni_broker *) ctx;
    int attached;
    JNIEnv *env = jni_broker_env(jb, &attached);
    if (env == NULL) return -1;
    jstring jpath = (*env)->NewStringUTF(env, relative_path);
    (*env)->CallVoidMethod(env, jb->broker, jb->commit_write_method, jpath);
    int ok = (*env)->ExceptionCheck(env) ? -1 : 0;
    if (ok != 0) (*env)->ExceptionClear(env);
    (*env)->DeleteLocalRef(env, jpath);
    if (attached) (*jb->vm)->DetachCurrentThread(jb->vm);
    return ok;
}

static int broker_remove(void *ctx, const char *relative_path) {
    jni_broker *jb = (jni_broker *) ctx;
    int attached;
    JNIEnv *env = jni_broker_env(jb, &attached);
    if (env == NULL) return -1;
    jstring jpath = (*env)->NewStringUTF(env, relative_path);
    (*env)->CallVoidMethod(env, jb->broker, jb->delete_method, jpath);
    int ok = (*env)->ExceptionCheck(env) ? -1 : 0;
    if (ok != 0) (*env)->ExceptionClear(env);
    (*env)->DeleteLocalRef(env, jpath);
    if (attached) (*jb->vm)->DetachCurrentThread(jb->vm);
    return ok;
}

static void fill_broker(cmvs_broker *out, jni_broker *jb) {
    out->ctx = jb;
    out->list = broker_list;
    out->open_read = broker_open_read;
    out->open_write = broker_open_write;
    out->commit_write = broker_commit_write;
    out->remove = broker_remove;
}

static session *from_handle(jlong handle) {
    return (session *) (intptr_t) handle;
}

/*
 * Sound. AAudio asks for samples on its own thread and the engine mixes into
 * whatever buffer it is handed, so this is the whole of it; the stream is
 * opened before it is started, so the mixer is always in place by the time the
 * first callback runs. A game with no sound device still plays: the engine
 * mixes silence either way and nothing here is required for it to run.
 */
static aaudio_data_callback_result_t feed_audio(AAudioStream *stream, void *user,
                                                void *frames, int32_t count) {
    (void) stream;
    session *state = (session *) user;
    cmvs_session_mix(state->session, (int16_t *) frames, count);
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

static void open_sound(session *state) {
    AAudioStreamBuilder *builder = NULL;
    if (AAudio_createStreamBuilder(&builder) != AAUDIO_OK) {
        __android_log_print(ANDROID_LOG_WARN, TAG, "no sound: AAudio will not start");
        return;
    }
    AAudioStreamBuilder_setFormat(builder, AAUDIO_FORMAT_PCM_I16);
    AAudioStreamBuilder_setChannelCount(builder, 2);
    AAudioStreamBuilder_setDataCallback(builder, feed_audio, state);
    AAudioStream *stream = NULL;
    aaudio_result_t opened = AAudioStreamBuilder_openStream(builder, &stream);
    AAudioStreamBuilder_delete(builder);
    if (opened != AAUDIO_OK || stream == NULL) {
        __android_log_print(ANDROID_LOG_WARN, TAG, "no sound: %s",
                            AAudio_convertResultToText(opened));
        return;
    }
    cmvs_session_audio_open(state->session, AAudioStream_getSampleRate(stream));
    state->sound = stream;
    AAudioStream_requestStart(stream);
    __android_log_print(ANDROID_LOG_INFO, TAG, "sound at %d Hz", AAudioStream_getSampleRate(stream));
}

/*
 * The isolated launch's audio producer, running on its own thread: mixes
 * one chunk at a time and copies it into the shared ring, backing off
 * briefly when the host has not read enough of the last chunk yet. The
 * write position is the only thing this thread touches in the header;
 * the host's read position is only ever read here.
 */
static void *audio_produce(void *arg) {
    session *state = (session *) arg;
    uint8_t *ring = (uint8_t *) state->audio_ring;
    _Atomic uint32_t *write_pos = (_Atomic uint32_t *) (ring + 0);
    _Atomic uint32_t *read_pos = (_Atomic uint32_t *) (ring + 4);
    int16_t chunk[AUDIO_CHUNK_FRAMES * 2];
    size_t needed = sizeof chunk;
    while (state->audio_thread_running) {
        uint32_t w = atomic_load_explicit(write_pos, memory_order_relaxed);
        uint32_t r = atomic_load_explicit(read_pos, memory_order_acquire);
        uint32_t used = w - r;
        uint32_t free_bytes = (uint32_t) AUDIO_RING_CAPACITY - used;
        if (free_bytes < needed) {
            usleep(5000);
            continue;
        }
        cmvs_session_mix(state->session, chunk, AUDIO_CHUNK_FRAMES);
        uint32_t start = w % (uint32_t) AUDIO_RING_CAPACITY;
        if ((size_t) start + needed <= (size_t) AUDIO_RING_CAPACITY) {
            memcpy(ring + AUDIO_HEADER_SIZE + start, chunk, needed);
        } else {
            size_t first = (size_t) AUDIO_RING_CAPACITY - start;
            memcpy(ring + AUDIO_HEADER_SIZE + start, chunk, first);
            memcpy(ring + AUDIO_HEADER_SIZE, (const uint8_t *) chunk + first, needed - first);
        }
        atomic_store_explicit(write_pos, w + (uint32_t) needed, memory_order_release);
    }
    return NULL;
}

/*
 * Sound for an isolated launch: no AAudio at all, since this process
 * cannot reach AudioFlinger to open it. audio_fd is a host-owned shared
 * region, already sized and zeroed by IsolatedRuntimeHost's
 * IsolatedAudioBridge; -1 means the host itself could not set one up, in
 * which case the game plays silently rather than touching AAudio.
 */
static void open_sound_bridged(session *state, int audio_fd, int sample_rate) {
    if (audio_fd < 0 || sample_rate <= 0) {
        __android_log_print(ANDROID_LOG_WARN, TAG,
            "no sound: the host had no isolated audio buffer to hand over");
        return;
    }
    size_t map_size = AUDIO_HEADER_SIZE + AUDIO_RING_CAPACITY;
    void *ring = mmap(NULL, map_size, PROT_READ | PROT_WRITE, MAP_SHARED, audio_fd, 0);
    if (ring == MAP_FAILED) {
        __android_log_print(ANDROID_LOG_WARN, TAG, "no sound: mmap of the audio buffer failed (%s)", strerror(errno));
        close(audio_fd);
        return;
    }
    cmvs_session_audio_open(state->session, sample_rate);
    state->audio_ring = ring;
    state->audio_ring_size = map_size;
    state->audio_ring_fd = audio_fd;
    state->audio_thread_running = 1;
    if (pthread_create(&state->audio_thread, NULL, audio_produce, state) != 0) {
        __android_log_print(ANDROID_LOG_WARN, TAG, "no sound: could not start the audio thread");
        state->audio_thread_running = 0;
        munmap(ring, map_size);
        close(audio_fd);
        state->audio_ring = NULL;
        state->audio_ring_fd = -1;
        return;
    }
    __android_log_print(ANDROID_LOG_INFO, TAG, "sound at %d Hz, bridged to the host", sample_rate);
}

static void close_sound(session *state) {
    if (state->sound != NULL) {
        AAudioStream_requestStop(state->sound);
        AAudioStream_close(state->sound);
        state->sound = NULL;
    }
    if (state->audio_thread_running) {
        state->audio_thread_running = 0;
        pthread_join(state->audio_thread, NULL);
    }
    if (state->audio_ring != NULL) {
        munmap(state->audio_ring, state->audio_ring_size);
        state->audio_ring = NULL;
    }
    if (state->audio_ring_fd >= 0) {
        close(state->audio_ring_fd);
        state->audio_ring_fd = -1;
    }
}

static void close_session(JNIEnv *env, session *state) {
    if (state == NULL) return;
    close_sound(state);
    cmvs_session_close(state->session);
    free(state->shown);
    /* Freed last: nothing above calls back into a broker, only closes
       fds/FILE*s that already came from one. */
    jni_broker_free(env, state->game_broker);
    jni_broker_free(env, state->save_broker);
    free(state);
}

/*
 * Everything past having a cmvs_session open: sizes the diff buffer and
 * starts sound. nativeOpen and nativeOpenIsolated differ only in how the
 * session itself is obtained (a real folder or a pair of host brokers);
 * everything downstream of that is this one mechanism, not two.
 */
static jlong finish_open(JNIEnv *env, session *state, int isolated, int audio_fd, int audio_rate) {
    state->width = cmvs_session_width(state->session);
    state->height = cmvs_session_height(state->session);
    state->shown = calloc((size_t) state->width * state->height, 4);
    if (state->shown == NULL) {
        snprintf(last_error, sizeof last_error, "out of memory");
        close_session(env, state);
        return 0;
    }
    __android_log_print(ANDROID_LOG_INFO, TAG, "session open, %dx%d, in %s",
                        state->width, state->height, cmvs_session_script(state->session));
    if (isolated) open_sound_bridged(state, audio_fd, audio_rate);
    else open_sound(state);
    return (jlong) (intptr_t) state;
}

JNIEXPORT jlong JNICALL
Java_dev_enginehost_plugin_cmvs_CmvsPlugin_nativeOpen(JNIEnv *env, jclass klass,
                                                      jstring folder, jstring script)
{
    (void) klass;
    const char *folder_text = folder == NULL ? NULL : (*env)->GetStringUTFChars(env, folder, NULL);
    const char *script_text = script == NULL ? NULL : (*env)->GetStringUTFChars(env, script, NULL);
    jlong handle = 0;

    last_error[0] = 0;
    session *state = calloc(1, sizeof *state);
    if (state == NULL) {
        snprintf(last_error, sizeof last_error, "out of memory");
    } else {
        state->audio_ring_fd = -1; /* calloc leaves 0, which is a real fd (stdin) */
        state->session = cmvs_session_open(folder_text, script_text, NULL, NULL,
                                           last_error, sizeof last_error);
        if (state->session == NULL) {
            __android_log_print(ANDROID_LOG_ERROR, TAG, "%s", last_error);
            close_session(env, state);
        } else {
            handle = finish_open(env, state, /*isolated=*/0, /*audio_fd=*/-1, /*audio_rate=*/0);
        }
    }
    if (folder_text != NULL) (*env)->ReleaseStringUTFChars(env, folder, folder_text);
    if (script_text != NULL) (*env)->ReleaseStringUTFChars(env, script, script_text);
    return handle;
}

/*
 * Sandbox layer 2 (Enginehost docs/engine-sandbox.md), the CatSystem2 route
 * applied to this engine: the same open, with the game folder and the save
 * folder each a host-brokered EngineFileBroker instead of a real path --
 * this process, under android:isolatedProcess, cannot resolve either path
 * itself. audio_buffer is the host's shared ring, or null when the host
 * could not make one; either way this never opens AAudio itself.
 */
JNIEXPORT jlong JNICALL
Java_dev_enginehost_plugin_cmvs_CmvsPlugin_nativeOpenIsolated(
        JNIEnv *env, jclass klass, jobject game_broker, jstring script,
        jobject save_broker, jobject audio_buffer, jint audio_sample_rate)
{
    (void) klass;
    const char *script_text = script == NULL ? NULL : (*env)->GetStringUTFChars(env, script, NULL);
    jlong handle = 0;

    last_error[0] = 0;
    session *state = calloc(1, sizeof *state);
    if (state == NULL) {
        snprintf(last_error, sizeof last_error, "out of memory");
    } else {
        state->audio_ring_fd = -1;
        state->game_broker = jni_broker_create(env, game_broker);
        if (state->game_broker == NULL) {
            snprintf(last_error, sizeof last_error, "no host file broker for the game folder");
            __android_log_print(ANDROID_LOG_ERROR, TAG, "%s", last_error);
            close_session(env, state);
        } else {
            fill_broker(&state->game_broker_ops, state->game_broker);
            const cmvs_broker *save_ops = NULL;
            if (save_broker != NULL) {
                state->save_broker = jni_broker_create(env, save_broker);
                if (state->save_broker != NULL) {
                    fill_broker(&state->save_broker_ops, state->save_broker);
                    save_ops = &state->save_broker_ops;
                }
            }
            state->session = cmvs_session_open_via_broker(&state->game_broker_ops, script_text, NULL,
                                                           save_ops, last_error, sizeof last_error);
            if (state->session == NULL) {
                __android_log_print(ANDROID_LOG_ERROR, TAG, "%s", last_error);
                close_session(env, state);
            } else {
                /* Takes and closes the Java-side ParcelFileDescriptor; -1 when there was none. */
                int audio_fd = jni_pfd_take(env, audio_buffer);
                handle = finish_open(env, state, /*isolated=*/1, audio_fd, (int) audio_sample_rate);
            }
        }
    }
    if (script_text != NULL) (*env)->ReleaseStringUTFChars(env, script, script_text);
    return handle;
}

JNIEXPORT jstring JNICALL
Java_dev_enginehost_plugin_cmvs_CmvsPlugin_nativeError(JNIEnv *env, jclass klass)
{
    (void) klass;
    return (*env)->NewStringUTF(env, last_error[0] ? last_error : "the game could not be opened");
}

JNIEXPORT void JNICALL
Java_dev_enginehost_plugin_cmvs_CmvsPlugin_nativeClose(JNIEnv *env, jclass klass, jlong handle)
{
    (void) klass;
    close_session(env, from_handle(handle));
}

/* The reader left the game; the music should not follow them out of it. */
JNIEXPORT void JNICALL
Java_dev_enginehost_plugin_cmvs_CmvsPlugin_nativeSetSounding(JNIEnv *env, jclass klass, jlong handle, jboolean sounding)
{
    (void) env;
    (void) klass;
    session *state = from_handle(handle);
    if (state == NULL || state->sound == NULL) return;
    if (sounding) AAudioStream_requestStart(state->sound);
    else AAudioStream_requestPause(state->sound);
}

JNIEXPORT jint JNICALL
Java_dev_enginehost_plugin_cmvs_CmvsPlugin_nativeWidth(JNIEnv *env, jclass klass, jlong handle)
{
    (void) env;
    (void) klass;
    session *state = from_handle(handle);
    return state == NULL ? 0 : (jint) state->width;
}

JNIEXPORT jint JNICALL
Java_dev_enginehost_plugin_cmvs_CmvsPlugin_nativeHeight(JNIEnv *env, jclass klass, jlong handle)
{
    (void) env;
    (void) klass;
    session *state = from_handle(handle);
    return state == NULL ? 0 : (jint) state->height;
}

/* One frame of the running script. Answers false once it has run off its end. */
JNIEXPORT jboolean JNICALL
Java_dev_enginehost_plugin_cmvs_CmvsPlugin_nativeStep(JNIEnv *env, jclass klass, jlong handle)
{
    (void) env;
    (void) klass;
    session *state = from_handle(handle);
    char why[256] = {0};
    int rc;
    if (state == NULL) return JNI_FALSE;
    rc = cmvs_session_frame(state->session, why, sizeof why);
    if (rc < 0) __android_log_print(ANDROID_LOG_ERROR, TAG, "%s", why);
    return rc > 0 ? JNI_TRUE : JNI_FALSE;
}

/*
 * The composed frame, as the rows that changed since the picture Java
 * already has: the first row that differs and how many, the same shape
 * enginehost-catsystem2-plugin's own nativeFrame answers. The engine
 * composes BGRA bytes and Android wants ARGB ints; on a little-endian
 * machine -- every Android device this builds for -- those are the same
 * four bytes in the same order, so both the diff and the copy work a whole
 * pixel row at a time without caring which name is on the bytes.
 */
JNIEXPORT jint JNICALL
Java_dev_enginehost_plugin_cmvs_CmvsPlugin_nativeFrame(JNIEnv *env, jclass klass, jlong handle, jintArray out)
{
    (void) klass;
    session *state = from_handle(handle);
    if (state == NULL || out == NULL) return 0;
    const uint8_t *pixels = cmvs_session_pixels(state->session);
    if (pixels == NULL) return 0;

    size_t row_bytes = (size_t) state->width * 4;
    int first = -1, last = -1;
    for (int row = 0; row < state->height; row++) {
        const uint8_t *made = pixels + (size_t) row * row_bytes;
        uint8_t *showing = state->shown + (size_t) row * row_bytes;
        if (memcmp(made, showing, row_bytes) == 0) continue;
        memcpy(showing, made, row_bytes);
        if (first < 0) first = row;
        last = row;
    }
    if (first < 0) return 0;
    jsize wanted = (jsize) ((size_t) (last - first + 1) * (size_t) state->width);
    if ((*env)->GetArrayLength(env, out) < (jsize) ((size_t) (last + 1) * (size_t) state->width)) return 0;
    (*env)->SetIntArrayRegion(env, out, (jsize) ((size_t) first * (size_t) state->width), wanted,
                              (const jint *) (pixels + (size_t) first * row_bytes));
    return (jint) (((uint32_t) first << 16) | (uint32_t) (last - first + 1));
}

JNIEXPORT jstring JNICALL
Java_dev_enginehost_plugin_cmvs_CmvsPlugin_nativeScript(JNIEnv *env, jclass klass, jlong handle)
{
    (void) klass;
    session *state = from_handle(handle);
    return (*env)->NewStringUTF(env, state ? cmvs_session_script(state->session) : "");
}

/*
 * The pointer, in the game's own pixels. Java has already undone the
 * scaling of the game's fixed screen onto the console's, exactly as
 * enginehost-catsystem2-plugin's own nativePointer documents.
 */
JNIEXPORT void JNICALL
Java_dev_enginehost_plugin_cmvs_CmvsPlugin_nativePointer(JNIEnv *env, jclass klass, jlong handle, jint x, jint y)
{
    (void) env;
    (void) klass;
    session *state = from_handle(handle);
    if (state == NULL) return;
    cmvs_session_pointer(state->session, x, y);
}

/*
 * A tap: the pointer's own left button, pressed and released together. The
 * engine's input model (src/input.h) reads a press and its release as two
 * separate edges a menu or the dialogue advance consumes once each, so both
 * are raised here rather than a single synthetic "click" - the same
 * function 01 ChronoClock's key.cfg binds a mouse click to either way.
 */
JNIEXPORT void JNICALL
Java_dev_enginehost_plugin_cmvs_CmvsPlugin_nativeTouch(JNIEnv *env, jclass klass, jlong handle, jint x, jint y)
{
    (void) env;
    (void) klass;
    session *state = from_handle(handle);
    if (state == NULL) return;
    __android_log_print(ANDROID_LOG_INFO, TAG, "a tap at %d,%d in the game's own picture", (int) x, (int) y);
    cmvs_session_pointer(state->session, x, y);
    cmvs_session_button(state->session, 0, 1);
    cmvs_session_button(state->session, 0, 0);
}

/*
 * Sandbox layer 2, isolated launches only (Enginehost docs/engine-sandbox.md
 * "Audio", the InMemoryDexClassLoader pivot): under Android 14's "safer
 * dynamic code loading", ART refuses a path-based dex file this process
 * itself made even sealed non-writable, so the host loads this plugin's dex
 * via InMemoryDexClassLoader instead, which is `final` and cannot override
 * findLibrary the way the in-process path's loader does, so this library's
 * own native methods are never auto-bound there and the static
 * initialiser's own System.loadLibrary("cmvs") always fails under isolation
 * (caught and ignored, CmvsPlugin.java).
 *
 * IsolatedRuntimeService dlopens this library directly (a /proc/self/fd
 * path to the same host-verified .so every other launch shape uses) and
 * dlsyms this EXACT, fixed, non-JNI symbol name -- not
 * Java_..._nativeOpen-style, so the JVM never tries to auto-bind it --
 * calling it once with a JNIEnv and this plugin's own Class object,
 * already correctly resolved by ordinary Java reflection on the host's
 * side. No FindClass, no classloader ambiguity: RegisterNatives binds
 * these exact, already-linked function pointers to that jclass directly,
 * regardless of which classloader loaded this library or defined that
 * class. Every one of this plugin's own native methods is listed here,
 * once, so nothing needs a second declaration to stay in sync as they
 * change -- the addresses are the same functions this file already
 * defines for the ordinary JNI auto-binding path. Identical mechanism to
 * enginehost-catsystem2-plugin's own enginehost_register_natives.
 */
JNIEXPORT void JNICALL
enginehost_register_natives(JNIEnv *env, jclass clazz) {
    static const JNINativeMethod methods[] = {
        {"nativeOpen", "(Ljava/lang/String;Ljava/lang/String;)J",
         (void *) Java_dev_enginehost_plugin_cmvs_CmvsPlugin_nativeOpen},
        {"nativeOpenIsolated",
         "(Ldev/enginehost/api/EngineFileBroker;Ljava/lang/String;Ldev/enginehost/api/EngineFileBroker;"
         "Landroid/os/ParcelFileDescriptor;I)J",
         (void *) Java_dev_enginehost_plugin_cmvs_CmvsPlugin_nativeOpenIsolated},
        {"nativeClose", "(J)V", (void *) Java_dev_enginehost_plugin_cmvs_CmvsPlugin_nativeClose},
        {"nativeSetSounding", "(JZ)V", (void *) Java_dev_enginehost_plugin_cmvs_CmvsPlugin_nativeSetSounding},
        {"nativeError", "()Ljava/lang/String;", (void *) Java_dev_enginehost_plugin_cmvs_CmvsPlugin_nativeError},
        {"nativeWidth", "(J)I", (void *) Java_dev_enginehost_plugin_cmvs_CmvsPlugin_nativeWidth},
        {"nativeHeight", "(J)I", (void *) Java_dev_enginehost_plugin_cmvs_CmvsPlugin_nativeHeight},
        {"nativeStep", "(J)Z", (void *) Java_dev_enginehost_plugin_cmvs_CmvsPlugin_nativeStep},
        {"nativeFrame", "(J[I)I", (void *) Java_dev_enginehost_plugin_cmvs_CmvsPlugin_nativeFrame},
        {"nativeScript", "(J)Ljava/lang/String;", (void *) Java_dev_enginehost_plugin_cmvs_CmvsPlugin_nativeScript},
        {"nativePointer", "(JII)V", (void *) Java_dev_enginehost_plugin_cmvs_CmvsPlugin_nativePointer},
        {"nativeTouch", "(JII)V", (void *) Java_dev_enginehost_plugin_cmvs_CmvsPlugin_nativeTouch},
    };
    (*env)->RegisterNatives(env, clazz, methods, (jint) (sizeof(methods) / sizeof(methods[0])));
}
</content>
