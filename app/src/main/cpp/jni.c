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

JNIEXPORT jlong JNICALL
Java_dev_enginehost_plugin_cmvs_CmvsPlugin_nativeOpen(JNIEnv *env, jclass klass,
                                                      jstring folder, jstring script)
{
    const char *folder_release = NULL, *script_release = NULL;
    const char *folder_text = java_string(env, folder, &folder_release);
    const char *script_text = java_string(env, script, &script_release);
    cmvs_session *session;

    last_error[0] = 0;
    session = cmvs_session_open(folder_text, script_text, last_error, sizeof last_error);
    if (folder_release) (*env)->ReleaseStringUTFChars(env, folder, folder_release);
    if (script_release) (*env)->ReleaseStringUTFChars(env, script, script_release);
    if (session == NULL) {
        __android_log_print(ANDROID_LOG_ERROR, TAG, "%s", last_error);
        return 0;
    }
    __android_log_print(ANDROID_LOG_INFO, TAG, "session open, %dx%d, in %s",
                        cmvs_session_width(session), cmvs_session_height(session),
                        cmvs_session_script(session));
    return (jlong) (intptr_t) session;
}

JNIEXPORT jstring JNICALL
Java_dev_enginehost_plugin_cmvs_CmvsPlugin_nativeError(JNIEnv *env, jclass klass)
{
    return (*env)->NewStringUTF(env, last_error[0] ? last_error : "the game could not be opened");
}

JNIEXPORT void JNICALL
Java_dev_enginehost_plugin_cmvs_CmvsPlugin_nativeClose(JNIEnv *env, jclass klass, jlong handle)
{
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
