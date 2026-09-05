package dev.enginehost.plugin.cmvs;

import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.Rect;
import android.graphics.RectF;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;
import android.view.View;
import dev.enginehost.api.EngineControllerEvent;
import dev.enginehost.api.EnginePlugin;
import dev.enginehost.api.EnginePluginSession;
import java.io.IOException;

/**
 * The Android wrapper around the CMVS engine.
 *
 * <p>The engine is the C in this repository's {@code src}, the same code the
 * desktop runner builds: it opens the game's CPZ archives, runs the PS3
 * bytecode and composes a frame. Everything this class does is hand it the game
 * folder, run it once per display frame, and show the picture. No part of the
 * engine is repeated in Java.
 */
public final class CmvsPlugin implements EnginePlugin {
    static {
        System.loadLibrary("cmvs");
    }

    /** ~60 frames a second, which is the rate the engine's own timers assume. */
    private static final long FRAME_MS = 16;

    private EnginePluginSession session;
    private long engine;
    private ScreenView view;
    private final Handler clock = new Handler(Looper.getMainLooper());
    private boolean running;

    @Override public void onCreate(EnginePluginSession session) throws Exception {
        this.session = session;
        String context = session.engineContext();
        if (!"cmvs".equals(session.engine()) || !("ps2".equals(context) || "ps3".equals(context))) {
            throw new IOException("Unsupported CMVS context");
        }
        // execFile names the boot script when a game does not use the usual one.
        // The engine falls back to start.ps3, which every CMVS game ships loose
        // beside its archives, so an empty execFile is the normal case.
        engine = nativeOpen(session.gamePath(), session.execFile());
        if (engine == 0) throw new IOException(nativeError());
        view = new ScreenView();
        session.display().addView(view, new android.view.ViewGroup.LayoutParams(-1, -1));
        start();
    }

    @Override public void onPause() {
        stop();
    }

    @Override public void onResume() {
        if (engine != 0) start();
    }

    @Override public void onDestroy() {
        stop();
        if (engine != 0) {
            nativeClose(engine);
            engine = 0;
        }
    }

    @Override public boolean onControllerEvent(EngineControllerEvent event) {
        // The engine reads no input yet: the scripts poll for it through
        // commands that are not implemented, so nothing here would reach them.
        // Claiming the event would only stop the host acting on it.
        return false;
    }

    private void start() {
        if (running) return;
        running = true;
        clock.post(tick);
    }

    private void stop() {
        running = false;
        clock.removeCallbacks(tick);
    }

    private final Runnable tick = new Runnable() {
        @Override public void run() {
            if (!running || engine == 0) return;
            if (!view.step()) {
                // The script ran off its end. The last frame stays on screen
                // rather than the display going black, which is what the reader
                // should see when a game stops.
                session.host().log(Log.INFO, "cmvs", "The script ended in " + nativeScript(engine), null);
                running = false;
                return;
            }
            clock.postDelayed(this, FRAME_MS);
        }
    };

    /**
     * Shows the engine's picture. A CMVS game is authored for one fixed screen
     * size, which cmvs.cfg names, so the frame arrives at that size and is
     * scaled to the console's, centred, with the aspect ratio kept.
     */
    private final class ScreenView extends View {
        private final int width;
        private final int height;
        private final int[] pixels;
        private final Bitmap frame;
        private final Paint paint = new Paint(Paint.FILTER_BITMAP_FLAG);
        private final Rect source;
        private final RectF destination = new RectF();

        ScreenView() {
            super(session.host().context());
            width = nativeWidth(engine);
            height = nativeHeight(engine);
            pixels = new int[width * height];
            frame = Bitmap.createBitmap(width, height, Bitmap.Config.ARGB_8888);
            source = new Rect(0, 0, width, height);
            setBackgroundColor(Color.BLACK);
        }

        /** Runs one frame. False once the script has ended. */
        boolean step() {
            boolean alive = nativeFrame(engine, pixels);
            frame.setPixels(pixels, 0, width, 0, 0, width, height);
            invalidate();
            return alive;
        }

        @Override protected void onDraw(Canvas canvas) {
            super.onDraw(canvas);
            float scale = Math.min(getWidth() / (float) width, getHeight() / (float) height);
            float left = (getWidth() - width * scale) / 2;
            float top = (getHeight() - height * scale) / 2;
            destination.set(left, top, left + width * scale, top + height * scale);
            canvas.drawBitmap(frame, source, destination, paint);
        }
    }

    private static native long nativeOpen(String folder, String script);
    private static native String nativeError();
    private static native void nativeClose(long engine);
    private static native int nativeWidth(long engine);
    private static native int nativeHeight(long engine);
    private static native boolean nativeFrame(long engine, int[] pixels);
    private static native String nativeScript(long engine);
    private static native int nativeDrawn(long engine);
}
