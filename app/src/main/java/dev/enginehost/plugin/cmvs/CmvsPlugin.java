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
import android.view.MotionEvent;
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
 * folder, run it once per display frame, show the picture, and report what the
 * reader touches. No part of the engine is repeated in Java.
 *
 * <p>The frame loop and every input arrive on the main thread - the loop is a
 * {@link Handler} on the main looper and touch and controller events are
 * delivered there - so the engine, which is not thread safe, is only ever
 * entered from one thread and needs no lock.
 */
public final class CmvsPlugin implements EnginePlugin {
    static {
        System.loadLibrary("cmvs");
    }

    /** ~60 frames a second, which is the rate the engine's own timers assume. */
    private static final long FRAME_MS = 16;

    /** How far a stick has to lean before it counts as one press of a direction. */
    private static final float STICK_STEP = 0.5f;

    private EnginePluginSession session;
    private long engine;
    private ScreenView view;
    private final Handler clock = new Handler(Looper.getMainLooper());
    private boolean running;

    /** A leaning stick is one press, not a press every frame, so each axis has
     * to fall back to the middle before it can step the selection again. */
    private int stickX;
    private int stickY;

    /** So the log says when a menu item fired rather than repeating the count. */
    private int menuEvents;
    private String script = "";

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
        script = nativeScript(engine);
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

    /**
     * The pad, in the engine's own terms.
     *
     * <p>A direction does not push a free pointer around: the engine's menus
     * move their selection along the item links and then warp the pointer onto
     * what they selected, which is what the original does with SetCursorPos.
     * Following that keeps the pad and the touchscreen one mechanism - the hit
     * test decides everything either way - instead of two that can disagree.
     * The menus here are vertical lists, so left steps back and right steps on,
     * the same as up and down.
     */
    @Override public boolean onControllerEvent(EngineControllerEvent event) {
        if (engine == 0) return false;
        String action = event.action();
        switch (action) {
            case "up":
            case "left":
                if (event.pressed()) nativeNavigate(engine, -1);
                return true;
            case "down":
            case "right":
                if (event.pressed()) nativeNavigate(engine, 1);
                return true;
            case "confirm":
                nativeButton(engine, 0, event.pressed());
                return true;
            case "cancel":
                nativeButton(engine, 1, event.pressed());
                return true;
            case "left_x":
                stickX = step(stickX, event.value());
                return true;
            case "left_y":
                stickY = step(stickY, event.value());
                return true;
            default:
                return false;
        }
    }

    /** Turns an axis reading into at most one step, remembering which way it
     * already leans. Android's Y axis grows downwards, so a positive reading is
     * down the list, which is what the engine's +1 means. */
    private int step(int leaning, float value) {
        int now = value <= -STICK_STEP ? -1 : value >= STICK_STEP ? 1 : 0;
        if (now != 0 && now != leaning) nativeNavigate(engine, now);
        return now;
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
            boolean alive = view.step();
            report();
            if (!alive) {
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
     * Says in the log when a menu answered a press and when the running script
     * changed. On hardware that is the whole difference between input that
     * never reached the engine and a menu that saw it and did nothing.
     */
    private void report() {
        int packed = nativeMenuEvents(engine);
        int count = packed & 0xFFFF;
        if (count != menuEvents) {
            menuEvents = count;
            session.host().log(Log.INFO, "cmvs", "menu item " + (packed >> 16) + " selected", null);
        }
        String now = nativeScript(engine);
        if (now != null && !now.equals(script)) {
            script = now;
            session.host().log(Log.INFO, "cmvs", "now running " + now, null);
        }
    }

    /**
     * Shows the engine's picture. A CMVS game is authored for one fixed screen
     * size, which cmvs.cfg names, so the frame arrives at that size and is
     * scaled to the console's, centred, with the aspect ratio kept.
     *
     * <p>The same three numbers place the picture and read a touch back out of
     * it, so a tap lands where the reader saw the caption whatever the console
     * does with the window.
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
            setFocusable(true);
        }

        /** Runs one frame. False once the script has ended. */
        boolean step() {
            boolean alive = nativeFrame(engine, pixels);
            frame.setPixels(pixels, 0, width, 0, 0, width, height);
            invalidate();
            return alive;
        }

        private float scale() {
            return Math.min(getWidth() / (float) width, getHeight() / (float) height);
        }

        /**
         * Says once, in the log, exactly how the picture sits on this console's
         * screen: the view's size, the scale and the letterbox margins. Anyone
         * who has to aim a tap at something in the game's own 1280x720 picture
         * can then compute where it is on the screen instead of guessing at a
         * scaling, which is what made a device run miss the START caption.
         */
        @Override protected void onSizeChanged(int w, int h, int oldW, int oldH) {
            super.onSizeChanged(w, h, oldW, oldH);
            float scale = Math.min(w / (float) width, h / (float) height);
            session.host().log(Log.INFO, "cmvs",
                    "view " + w + "x" + h + ", picture " + width + "x" + height
                            + " at scale " + scale + ", margins "
                            + ((w - width * scale) / 2) + "," + ((h - height * scale) / 2)
                            + " (screen = margin + game * scale)", null);
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
         * A touch is the pointer and the left button together: the finger's
         * position is reported first so that the frame which sees the press
         * hit-tests where the finger actually is, and both are in the game's
         * coordinates because that is the only thing the engine understands.
         * A touch outside the picture is still carried across, so dragging off
         * a caption and letting go cancels the press the way the original does.
         */
        @Override public boolean onTouchEvent(MotionEvent event) {
            if (engine == 0) return false;
            float scale = scale();
            if (scale <= 0) return false;
            int x = (int) ((event.getX() - (getWidth() - width * scale) / 2) / scale);
            int y = (int) ((event.getY() - (getHeight() - height * scale) / 2) / scale);
            nativePointer(engine, x, y);
            switch (event.getActionMasked()) {
                case MotionEvent.ACTION_DOWN:
                    nativeButton(engine, 0, true);
                    return true;
                case MotionEvent.ACTION_MOVE:
                    return true;
                case MotionEvent.ACTION_UP:
                case MotionEvent.ACTION_CANCEL:
                    nativeButton(engine, 0, false);
                    return true;
                default:
                    return false;
            }
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
    private static native void nativePointer(long engine, int x, int y);
    private static native void nativeButton(long engine, int button, boolean down);
    private static native void nativeNavigate(long engine, int direction);
    private static native int nativeMenuEvents(long engine);
}
