/*
 * The Android side of the CMVS engine: nothing but a way in.
 *
 * The engine is the C in this repository's src, the same code the desktop
 * runner builds. It opens the game's archives, runs the bytecode and composes a
 * frame into a buffer of pixels; this file starts a session, steps it, and
 * hands that buffer to Java. Nothing about CMVS is decided here, and nothing
 * about it is written twice.
 */
#include <jni.h>
#include <stdlib.h>
#include <string.h>

#include <aaudio/AAudio.h>
#include <android/log.h>

#include "session.h"

#define TAG "cmvs"

/*
 * Why a session that failed to open has no handle to carry its reason: the
 * error belongs to the attempt, not to a game. One buffer, read immediately
 * after a failed open, is what the plugin needs and all it needs.
 */
static char last_error[256];

static const char *java_string(JNIEnv *env, jstring value, const char **release)
{
    *release = NULL;
    if (value == NULL) return NULL;
    *release = (*env)->GetStringUTFChars(env, value, NULL);
    return *release;
}

/*
 * Sound: the device, which the engine deliberately does not open.
 *
 * The engine mixes and a frontend plays, because the two frontends open
 * different devices - SDL2 in the desktop runner, AAudio here - and AAudio asks
 * for samples on a thread of its own. That is safe: the mixer takes its own
 * lock and touches nothing else in the session, so the bytecode keeps running
 * on the main thread while this thread pulls blocks out of it.
 *
 * The stream and the session it feeds are file statics for the same reason
 * last_error is: the host runs one game at a time, and a handle for a device
 * that exists once would be a handle to carry through Java for nothing. The
 * stream is stopped and closed before the session is, so the callback cannot
 * be running when the mixer goes away.
 */
static AAudioStream *sound_stream;
static cmvs_session *sound_session;

static aaudio_data_callback_result_t feed_sound(AAudioStream *stream, void *user,
                                                void *frames, int32_t count)
{
    cmvs_session_mix(sound_session, (int16_t *) frames, count);
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

/*
 * A device is told a rate and answers with the one it took, so the session is
 * corrected to whatever came back rather than the engine assuming. No sound is
 * not a reason to refuse the game: a run with a silent device still reads.
 */
static void open_sound(cmvs_session *session)
{
    AAudioStreamBuilder *builder = NULL;
    aaudio_result_t opened;

    if (AAudio_createStreamBuilder(&builder) != AAUDIO_OK) {
        __android_log_print(ANDROID_LOG_WARN, TAG, "no sound: AAudio will not start");
        return;
    }
    AAudioStreamBuilder_setFormat(builder, AAUDIO_FORMAT_PCM_I16);
    AAudioStreamBuilder_setChannelCount(builder, 2);
    AAudioStreamBuilder_setPerformanceMode(builder, AAUDIO_PERFORMANCE_MODE_NONE);
    AAudioStreamBuilder_setDataCallback(builder, feed_sound, NULL);
    opened = AAudioStreamBuilder_openStream(builder, &sound_stream);
    AAudioStreamBuilder_delete(builder);
    if (opened != AAUDIO_OK) {
        sound_stream = NULL;
        __android_log_print(ANDROID_LOG_WARN, TAG, "no sound: %s",
                            AAudio_convertResultToText(opened));
        return;
    }
    sound_session = session;
    cmvs_session_audio_open(session, AAudioStream_getSampleRate(sound_stream));
    AAudioStream_requestStart(sound_stream);
    __android_log_print(ANDROID_LOG_INFO, TAG, "sound open at %d Hz",
                        cmvs_session_audio_rate(session));
}

/* The reader put the game down: the frame loop stops, and so must the sound,
 * or the music plays on over whatever they went to instead. */
static void pause_sound(int sounding)
{
    if (!sound_stream) return;
    if (sounding) AAudioStream_requestStart(sound_stream);
    else AAudioStream_requestPause(sound_stream);
}

static void close_sound(void)
{
    if (sound_stream) {
        AAudioStream_requestStop(sound_stream);
        AAudioStream_close(sound_stream);
        sound_stream = NULL;
    }
    sound_session = NULL;
}

JNIEXPORT jlong JNICALL
Java_dev_enginehost_plugin_cmvs_CmvsPlugin_nativeOpen(JNIEnv *env, jclass klass,
                                                      jstring folder, jstring script,
                                                      jstring saves)
{
    const char *folder_release = NULL, *script_release = NULL, *saves_release = NULL;
    const char *folder_text = java_string(env, folder, &folder_release);
    const char *script_text = java_string(env, script, &script_release);
    /*
     * The host's own folder for this game's saves. The engine puts the game's
     * real CSV2 and CSS1 files in the subfolder the boot script names inside
     * it, so what is written here is what the Windows game writes; with no
     * folder the engine saves nothing rather than writing into the game.
     */
    const char *saves_text = java_string(env, saves, &saves_release);
    cmvs_session *session;

    last_error[0] = 0;
    session = cmvs_session_open(folder_text, script_text, NULL, saves_text,
                                last_error, sizeof last_error);
    if (folder_release) (*env)->ReleaseStringUTFChars(env, folder, folder_release);
    if (script_release) (*env)->ReleaseStringUTFChars(env, script, script_release);
    if (saves_release) (*env)->ReleaseStringUTFChars(env, saves, saves_release);
    if (session == NULL) {
        __android_log_print(ANDROID_LOG_ERROR, TAG, "%s", last_error);
        return 0;
    }
    __android_log_print(ANDROID_LOG_INFO, TAG, "session open, %dx%d, in %s",
                        cmvs_session_width(session), cmvs_session_height(session),
                        cmvs_session_script(session));
    open_sound(session);
    return (jlong) (intptr_t) session;
}

/*
 * KEY_FUNCTION 13 and 14: quick save and quick load, on slot 999. They answer
 * with the reason when they fail rather than silently doing nothing, because on
 * a console a save that did not happen looks exactly like one that did.
 */
JNIEXPORT jstring JNICALL
Java_dev_enginehost_plugin_cmvs_CmvsPlugin_nativeQuickSave(JNIEnv *env, jclass klass,
                                                           jlong handle)
{
    cmvs_session *session = (cmvs_session *) (intptr_t) handle;
    char err[256] = {0};
    if (cmvs_session_quick_save(session, err, sizeof err)) return NULL;
    return (*env)->NewStringUTF(env, err[0] ? err : "the quick save did not happen");
}

JNIEXPORT jstring JNICALL
Java_dev_enginehost_plugin_cmvs_CmvsPlugin_nativeQuickLoad(JNIEnv *env, jclass klass,
                                                           jlong handle)
{
    cmvs_session *session = (cmvs_session *) (intptr_t) handle;
    char err[256] = {0};
    if (cmvs_session_quick_load(session, err, sizeof err)) return NULL;
    return (*env)->NewStringUTF(env, err[0] ? err : "there is no quick save");
}

JNIEXPORT jstring JNICALL
Java_dev_enginehost_plugin_cmvs_CmvsPlugin_nativeSaveFolder(JNIEnv *env, jclass klass,
                                                            jlong handle)
{
    const char *folder = cmvs_session_save_folder((cmvs_session *) (intptr_t) handle);
    return folder ? (*env)->NewStringUTF(env, folder) : NULL;
}

JNIEXPORT jstring JNICALL
Java_dev_enginehost_plugin_cmvs_CmvsPlugin_nativeError(JNIEnv *env, jclass klass)
{
    return (*env)->NewStringUTF(env, last_error[0] ? last_error : "the game could not be opened");
}

JNIEXPORT void JNICALL
Java_dev_enginehost_plugin_cmvs_CmvsPlugin_nativeClose(JNIEnv *env, jclass klass, jlong handle)
{
    close_sound();
    cmvs_session_close((cmvs_session *) (intptr_t) handle);
}

JNIEXPORT jint JNICALL
Java_dev_enginehost_plugin_cmvs_CmvsPlugin_nativeWidth(JNIEnv *env, jclass klass, jlong handle)
{
    return cmvs_session_width((cmvs_session *) (intptr_t) handle);
}

JNIEXPORT jint JNICALL
Java_dev_enginehost_plugin_cmvs_CmvsPlugin_nativeHeight(JNIEnv *env, jclass klass, jlong handle)
{
    return cmvs_session_height((cmvs_session *) (intptr_t) handle);
}

/*
 * Runs one frame and copies it out. The engine composes BGRA bytes and Android
 * wants ARGB ints; on a little-endian machine - which every Android device the
 * host builds for is - those are the same four bytes in the same order, so the
 * copy is a copy and not a conversion.
 */
JNIEXPORT jboolean JNICALL
Java_dev_enginehost_plugin_cmvs_CmvsPlugin_nativeFrame(JNIEnv *env, jclass klass,
                                                       jlong handle, jintArray out)
{
    cmvs_session *session = (cmvs_session *) (intptr_t) handle;
    char why[256] = {0};
    const uint8_t *pixels;
    jsize wanted;
    int rc;

    if (session == NULL || out == NULL) return JNI_FALSE;
    rc = cmvs_session_frame(session, why, sizeof why);
    if (rc < 0) __android_log_print(ANDROID_LOG_ERROR, TAG, "%s", why);

    pixels = cmvs_session_pixels(session);
    if (pixels == NULL) return JNI_FALSE;
    wanted = (jsize) (cmvs_session_width(session) * cmvs_session_height(session));
    if ((*env)->GetArrayLength(env, out) < wanted) return JNI_FALSE;
    (*env)->SetIntArrayRegion(env, out, 0, wanted, (const jint *) pixels);
    return rc > 0 ? JNI_TRUE : JNI_FALSE;
}

/*
 * Input. The engine wants the pointer in the GAME's coordinates, so the
 * conversion from the view's belongs to the plugin, which is the only part
 * that knows how the picture sits on the console's screen; this file just
 * carries the numbers across.
 *
 * The first pointer of a session is logged, and then every press with the
 * position it landed on. On hardware a tap that misses looks exactly like one
 * that never arrived unless the log carries the coordinates of each press, and
 * a press per tap is a handful of lines, not a flood.
 */
static int pointer_x;
static int pointer_y;
static int logged_pointer;

JNIEXPORT void JNICALL
Java_dev_enginehost_plugin_cmvs_CmvsPlugin_nativePointer(JNIEnv *env, jclass klass,
                                                         jlong handle, jint x, jint y)
{
    cmvs_session *session = (cmvs_session *) (intptr_t) handle;
    if (session == NULL) return;
    pointer_x = x;
    pointer_y = y;
    if (!logged_pointer) {
        logged_pointer = 1;
        __android_log_print(ANDROID_LOG_INFO, TAG, "pointer reached the engine at %d,%d", x, y);
    }
    cmvs_session_pointer(session, x, y);
}

JNIEXPORT void JNICALL
Java_dev_enginehost_plugin_cmvs_CmvsPlugin_nativeButton(JNIEnv *env, jclass klass,
                                                        jlong handle, jint button, jboolean down)
{
    cmvs_session *session = (cmvs_session *) (intptr_t) handle;
    if (session == NULL) return;
    if (down == JNI_TRUE) {
        __android_log_print(ANDROID_LOG_INFO, TAG, "button %d pressed at %d,%d",
                            button, pointer_x, pointer_y);
    }
    cmvs_session_button(session, button, down == JNI_TRUE);
}

JNIEXPORT void JNICALL
Java_dev_enginehost_plugin_cmvs_CmvsPlugin_nativeNavigate(JNIEnv *env, jclass klass,
                                                          jlong handle, jint direction)
{
    cmvs_session *session = (cmvs_session *) (intptr_t) handle;
    if (session == NULL) return;
    cmvs_session_navigate(session, direction);
}

/* Packs the two answers into one call so the frame loop asks once: the count of
 * menu items that have fired in the low bits, the last item in the high ones. */
JNIEXPORT jint JNICALL
Java_dev_enginehost_plugin_cmvs_CmvsPlugin_nativeMenuEvents(JNIEnv *env, jclass klass, jlong handle)
{
    cmvs_session *session = (cmvs_session *) (intptr_t) handle;
    int last = -1, count;
    if (session == NULL) return 0;
    count = cmvs_session_menu_events(session, &last);
    return (jint) ((count & 0xFFFF) | ((last & 0xFF) << 16));
}

JNIEXPORT void JNICALL
Java_dev_enginehost_plugin_cmvs_CmvsPlugin_nativeSound(JNIEnv *env, jclass klass,
                                                       jlong handle, jboolean sounding)
{
    pause_sound(sounding == JNI_TRUE);
}

JNIEXPORT jstring JNICALL
Java_dev_enginehost_plugin_cmvs_CmvsPlugin_nativeScript(JNIEnv *env, jclass klass, jlong handle)
{
    cmvs_session *session = (cmvs_session *) (intptr_t) handle;
    return (*env)->NewStringUTF(env, session ? cmvs_session_script(session) : "");
}

JNIEXPORT jint JNICALL
Java_dev_enginehost_plugin_cmvs_CmvsPlugin_nativeDrawn(JNIEnv *env, jclass klass, jlong handle)
{
    return cmvs_session_drawn((cmvs_session *) (intptr_t) handle);
}
