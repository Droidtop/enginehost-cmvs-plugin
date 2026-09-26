package dev.enginehost.plugin.cmvs;

import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.Rect;
import android.graphics.RectF;
import android.os.ParcelFileDescriptor;
import android.util.Log;
import android.view.Choreographer;
import android.view.MotionEvent;
import android.view.View;
import dev.enginehost.api.EngineControllerEvent;
import dev.enginehost.api.EngineFileBroker;
import dev.enginehost.api.EnginePlugin;
import dev.enginehost.api.EnginePluginSession;
import dev.enginehost.api.EngineProcess;
import dev.enginehost.api.EngineStepDriven;
import java.io.File;
import java.io.IOException;

/**
 * The Android wrapper around the CMVS engine.
 *
 * <p>The engine is the C in this repository's {@code src}, the same code the
 * desktop runner builds: it opens the game's CPZ archives, runs the PS3
 * bytecode and composes a frame. Everything this class does is hand it the game
 * folder, run it once per display frame, show the picture and pass the
 * reader's taps back. No part of the engine is repeated in Java.
 */
public final class CmvsPlugin implements EnginePlugin, EngineStepDriven {
    static {
        try {
            System.loadLibrary("cmvs");
        } catch (UnsatisfiedLinkError e) {
            // Sandbox layer 2, isolated launches ONLY (Enginehost
            // docs/engine-sandbox.md "Audio"): InMemoryDexClassLoader is
            // final and cannot override findLibrary the way the in-process
            // path's loader does, so loadLibrary always fails here under
            // isolation -- harmless there, since IsolatedRuntimeService
            // binds this library's native methods explicitly instead
            // (dlopen + a single RegisterNatives call, see
            // enginehost_register_natives in jni.c) before this class is
            // ever instantiated. A normal, in-process launch's loader DOES
            // have a working findLibrary, so a failure here means
            // something real -- a missing or broken .so -- and must not be
            // hidden behind a confusing later failure the way silently
            // swallowing it here would. EngineProcess.isIsolated() is
            // plugin-api's one canonical answer to "is this the isolated
            // process", shared by the host and every plugin (the same
            // mechanism enginehost-catsystem2-plugin uses).
            if (!EngineProcess.isIsolated()) throw e;
        }
    }

    private EnginePluginSession session;
    private long engine;
    private ScreenView view;

    @Override public void onCreate(EnginePluginSession session) throws Exception {
        this.session = session;
        String context = session.engineContext();
        if (!"cmvs".equals(session.engine()) || !("ps2".equals(context) || "ps3".equals(context))) {
            throw new IOException("Unsupported CMVS context");
        }
        EngineFileBroker gameBroker = session.host().gameBroker();
        if (gameBroker != null) {
            // Sandbox layer 2 (Enginehost docs/engine-sandbox.md): this
            // process cannot resolve the game or save folder itself, so
            // both cross to the host process over the broker instead of a
            // real path. There is no session.display() to attach a View
            // into either -- see step() below, which is how this plugin
            // runs without one. The host owns real audio output: this
            // process cannot reach AudioFlinger, so nativeOpenIsolated
            // never touches AAudio and instead renders into the shared
            // ring the host handed over, or plays silently when there is
            // none.
            engine = nativeOpenIsolated(gameBroker, session.execFile(), session.host().saveBroker(),
                    session.host().isolatedAudioBuffer(), session.host().isolatedAudioSampleRate());
        } else {
            // execFile names the boot script when a game does not use the
            // usual one. The engine falls back to start.ps3, which every
            // CMVS game ships loose beside its archives, so an empty
            // execFile is the normal case. The game's own saves go in the
            // folder the host keeps for this game, never the game folder
            // itself: that is the reader's install and stays read-only.
            File saves = session.host().saveDirectory();
            if (saves != null && !saves.isDirectory()) saves.mkdirs();
            engine = nativeOpen(session.gamePath(), session.execFile(),
                    saves == null ? null : saves.getAbsolutePath());
        }
        if (engine == 0) throw new IOException(nativeError());
        if (session.display() != null) {
            view = new ScreenView();
            session.display().addView(view, new android.view.ViewGroup.LayoutParams(-1, -1));
        }
    }

    @Override public int pixelWidth() { return engine == 0 ? 0 : nativeWidth(engine); }
    @Override public int pixelHeight() { return engine == 0 ? 0 : nativeHeight(engine); }

    /** The isolated runtime's frame pump; see EngineStepDriven. The in-process ScreenView drives the same two calls itself. */
    @Override public int step(int[] pixels) {
        if (engine == 0) return -1;
        if (!nativeStep(engine)) return -1;
        return nativeFrame(engine, pixels);
    }

    /** Already in this engine's own pixel space (EngineStepDriven); the in-process path does the same scaling in ScreenView. */
    @Override public void onPointerMove(int x, int y) {
        if (engine != 0) nativePointer(engine, x, y);
    }

    @Override public void onPointerUp(int x, int y) {
        if (engine != 0) nativeTouch(engine, x, y);
    }

    @Override public void onPause() {
        if (view != null) view.setRunning(false);
        if (engine != 0) nativeSetSounding(engine, false);
    }

    @Override public void onResume() {
        if (engine != 0) nativeSetSounding(engine, true);
        if (view != null) view.setRunning(true);
    }

    @Override public void onDestroy() {
        if (view != null) view.setRunning(false);
        if (engine != 0) {
            nativeClose(engine);
            engine = 0;
        }
    }

    @Override public boolean onControllerEvent(EngineControllerEvent event) {
        // The engine reads no bound gamepad functions yet beyond the
        // pointer and its own two buttons, which reach it through the
        // touch path (ScreenView.onTouchEvent / EngineStepDriven's
        // onPointerMove+onPointerUp) rather than a controller event.
        // Claiming the event here would only stop the host acting on it
        // for nothing this engine would do with it.
        return false;
    }

    /**
     * Shows the engine's picture, a frame at a time.
     *
     * <p>A CMVS game is authored for one fixed screen size, which cmvs.cfg
     * names, so the frame arrives at that size and is scaled to the
     * console's, centred, with the aspect ratio kept. Driven off the
     * display's own clock rather than a fixed timer, the same as
     * enginehost-catsystem2-plugin's own ScreenView.
     */
    private final class ScreenView extends View implements Choreographer.FrameCallback {
        private final int width;
        private final int height;
        private final int[] pixels;
        private final Bitmap frame;
        private final Paint paint = new Paint(Paint.FILTER_BITMAP_FLAG);
        private final Rect source;
        private final RectF destination = new RectF();
        private boolean running;

        ScreenView() {
            super(session.host().context());
            width = nativeWidth(engine);
            height = nativeHeight(engine);
            pixels = new int[width * height];
            frame = Bitmap.createBitmap(width, height, Bitmap.Config.ARGB_8888);
            source = new Rect(0, 0, width, height);
            setBackgroundColor(Color.BLACK);
            setFocusable(true);
            setFocusableInTouchMode(true);
            setRunning(true);
        }

        void setRunning(boolean wanted) {
            if (wanted == running) return;
            running = wanted;
            if (wanted) Choreographer.getInstance().postFrameCallback(this);
            else Choreographer.getInstance().removeFrameCallback(this);
        }

        @Override public void doFrame(long frameTimeNanos) {
            if (!running || engine == 0) return;
            boolean alive = nativeStep(engine);
            /*
             * The engine answers with the rows of the picture that are not
             * already on the screen, as the first row and how many: a
             * reader reading is watching one band of it change and most
             * frames change nothing at all, and a frame that changed
             * nothing is one the display is not asked to draw again.
             */
            int band = nativeFrame(engine, pixels);
            if (band != 0) {
                int top = band >>> 16;
                int rows = band & 0xffff;
                frame.setPixels(pixels, top * width, width, 0, top, width, rows);
                invalidate();
            }
            if (alive) {
                Choreographer.getInstance().postFrameCallback(this);
            } else {
                Log.i("cmvs", "the script ended in " + nativeScript(engine));
                running = false;
            }
        }

        private float scale() {
            return Math.min(getWidth() / (float) width, getHeight() / (float) height);
        }

        @Override protected void onDraw(Canvas canvas) {
            super.onDraw(canvas);
            float scale = scale();
            float left = (getWidth() - width * scale) / 2;
            float top = (getHeight() - height * scale) / 2;
            destination.set(left, top, left + width * scale, top + height * scale);
            canvas.drawBitmap(frame, source, destination, paint);
        }

        /**
         * A tap, in the game's own pixels. The game is authored for a fixed
         * screen and drawn scaled and centred on the console's, so a tap has
         * to be put back where the game would have seen it before the
         * engine can say which of its buttons it landed on.
         */
        @Override public boolean onTouchEvent(MotionEvent event) {
            if (engine == 0) return true;
            float scale = scale();
            if (scale <= 0) return true;
            int x = Math.round((event.getX() - (getWidth() - width * scale) / 2) / scale);
            int y = Math.round((event.getY() - (getHeight() - height * scale) / 2) / scale);
            x = Math.max(0, Math.min(width - 1, x));
            y = Math.max(0, Math.min(height - 1, y));
            switch (event.getActionMasked()) {
                case MotionEvent.ACTION_DOWN:
                case MotionEvent.ACTION_MOVE:
                    nativePointer(engine, x, y);
                    return true;
                case MotionEvent.ACTION_UP:
                    nativeTouch(engine, x, y);
                    return true;
                default:
                    return true;
            }
        }
    }

    private static native long nativeOpen(String folder, String script, String saveFolder);
    /** Sandbox layer 2 (Enginehost docs/engine-sandbox.md): gameBroker/saveBroker take the place of folder/a save path. */
    private static native long nativeOpenIsolated(EngineFileBroker gameBroker, String script, EngineFileBroker saveBroker,
            ParcelFileDescriptor audioBuffer, int audioSampleRate);
    private static native String nativeError();
    private static native void nativeClose(long engine);
    private static native void nativeSetSounding(long engine, boolean sounding);
    private static native int nativeWidth(long engine);
    private static native int nativeHeight(long engine);
    private static native boolean nativeStep(long engine);
    /** The rows of the picture that changed: the first row and how many, or 0. */
    private static native int nativeFrame(long engine, int[] pixels);
    private static native String nativeScript(long engine);
    /** The pointer, already scaled to the game's own pixels. */
    private static native void nativePointer(long engine, int x, int y);
    /** A tap: the pointer's own left button, pressed and released together. */
    private static native void nativeTouch(long engine, int x, int y);
}
</content>
