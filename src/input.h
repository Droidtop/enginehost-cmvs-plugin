/*
 * The pointer and the engine's key functions, the way its input device holds them.
 *
 * These are not buttons. CMVS resolves every input through a table of 24 NAMED
 * FUNCTIONS, each with five alternative bindings read out of key.cfg's
 * [KEY_FUNCTION_01..24] (loader 0x0044CCD0, table at +0x31c, resolver
 * 0x00449020). A function's three flags live at +0x430 + 12*n:
 *
 *   +0  RELEASED   last frame's counter was above zero and this frame's is zero
 *   +4  HELD       this frame's counter is above zero
 *   +8  PRESSED    this frame's counter is exactly one, the first frame only
 *
 * so the table here is that table, one entry per function, indexed by the
 * engine's own function number. The accessors the menus and the commands call
 * are 0x00448B20 onwards, a getter and an edge-clearing setter per function in
 * KEY_FUNCTION order: the getter answers RELEASED and hands HELD back through
 * an out-parameter (0x00448B20), a second getter answers PRESSED (0x00448B40),
 * and the clear zeroes RELEASED and PRESSED (0x00448B50). That is how the
 * engine consumes an edge exactly once, and it is why a tap and a pad press
 * are the same thing here - the original never distinguishes them either.
 *
 * The full 24-function table, its evidence and the 128-entry input-code space
 * are in coordination agents/cmvs/KEY-FUNCTIONS.md. A frontend raises the ones
 * it can: a pointer and its two buttons are functions 01 and 02 by
 * ChronoClock's own key.cfg, and the rest stay quiet until a frontend binds
 * them, which is exactly what an unbound function does in the original.
 *
 * The pointer is in ENGINE coordinates - the game's own screen, 1280x720 for
 * ChronoClock - not in the window's. The original scales between the two
 * inside the menu poll, using the client size it was handed at bind time; here
 * the frontend does it, because the frontend is the only part that knows how
 * its picture is laid out on a real display. That is one conversion in one
 * place instead of the same one in the desktop runner, the JNI and the menus.
 */
#ifndef CMVS_INPUT_H
#define CMVS_INPUT_H

/* The engine's own function numbers, from KEY-FUNCTIONS.md's table. Function 0
 * is not a function: the entry is kept so the index is the engine's. */
enum {
    CMVS_FN_CONFIRM = 1,        /* 決定          advance the message, take a menu item */
    CMVS_FN_CANCEL = 2,         /* キャンセル    back out, open the in-message menu */
    CMVS_FN_UP = 3,             /* カーソル↑ */
    CMVS_FN_DOWN = 4,           /* カーソル↓ */
    CMVS_FN_LEFT = 5,           /* カーソル← */
    CMVS_FN_RIGHT = 6,          /* カーソル→ */
    CMVS_FN_HIDE_WINDOW = 7,    /* メッセージウィンドウ消去 */
    CMVS_FN_FORCE_SKIP = 8,     /* 強制スキップ */
    CMVS_FN_READ_SKIP = 9,      /* 既読スキップ */
    CMVS_FN_AUTO = 10,          /* 自動送り      auto advance */
    CMVS_FN_HISTORY = 11,       /* 履歴モード    open the backlog */
    CMVS_FN_VOICE = 12,         /* 音声再生      replay the line's voice */
    CMVS_FN_QUICK_SAVE = 13,    /* クイックセーブ */
    CMVS_FN_QUICK_LOAD = 14,    /* クイックロード */
    CMVS_FN_POPUP = 15,         /* ポップアップメニュー */
    CMVS_FN_HISTORY_UP = 16,    /* 履歴アップ */
    CMVS_FN_HISTORY_DOWN = 17,  /* 履歴ダウン */
    CMVS_FN_CONFIG = 18,        /* 設定画面 */
    CMVS_FN_SAVE_SCREEN = 19,   /* セーブ画面 */
    CMVS_FN_LOAD_SCREEN = 20,   /* ロード画面 */
    CMVS_FN_ADVANCE = 21,       /* 拡張メッセージ送り */
    CMVS_FN_CENTRE = 22,        /* Outerの中央移動 */
    CMVS_FN_VALUE_UP = 23,      /* メニュー増減アップ */
    CMVS_FN_VALUE_DOWN = 24,    /* メニュー増減ダウン */
    CMVS_FUNCTIONS = 25
};

typedef struct {
    int released, held, pressed;
} cmvs_function;

/* At most a press and its release can be waiting behind one pointer move. */
#define CMVS_INPUT_QUEUE 2

typedef struct {
    int x, y;                  /* the pointer, in engine coordinates */
    int have_pointer;          /* nothing has pointed at the screen yet */
    int moved;                 /* the pointer moved since the last poll */

    cmvs_function fn[CMVS_FUNCTIONS];

    /* Transitions held back for a later poll, in the order they arrived; see
     * cmvs_input_poll. */
    struct {
        unsigned char function;
        unsigned char down;
        unsigned char wait;    /* polls still to pass before it is raised */
    } queue[CMVS_INPUT_QUEUE];
    int queued;
} cmvs_input;

/*
 * The device poll, once at the top of every frame: 0x0044BEC0, which
 * 0x0045A8E0 makes before a single statement of the frame runs. The original
 * samples the mouse and the keyboard there, and that is where a press first
 * becomes visible to the script.
 *
 * It is also what keeps a POINTER THAT TELEPORTS honest. A mouse cannot report
 * a new position and a button going down in the same poll: the cursor is
 * already where it is before the button is pressed, so the poll that first
 * sees the button down is never the poll that first sees the position. A
 * finger has no such history - ACTION_DOWN carries a position and a press
 * together - and the game's own script counts on the difference: intproc.ps3's
 * toolbar procedure decides which icon the pointer is on in one frame and only
 * acts on a press in a later one, so a press delivered in the same poll as the
 * move it arrived with is thrown away (measured: one poll of separation is
 * enough, none is not). So a button transition raised in the same poll as a
 * pointer move waits here for the next poll, one transition per poll, in the
 * order it arrived. Nothing that arrives without a pointer move waits.
 */
void cmvs_input_poll(cmvs_input *in);

/* What a frontend calls. `button` is 0 for the left button (a tap, confirm)
 * and 1 for the right one (cancel); both are functions here, the ones
 * ChronoClock's key.cfg binds them to. */
void cmvs_input_move(cmvs_input *in, int x, int y);
void cmvs_input_button(cmvs_input *in, int button, int down);

/* A press of a direction: -1 up, +1 down. The menus move their selection on
 * these and warp the pointer to whatever they select, which is how a pad and a
 * pointer stay one mechanism rather than two. */
void cmvs_input_navigate(cmvs_input *in, int direction);

/* Any named function, for a frontend that has a button for it. The edge is
 * latched until a reader clears it, so a press shorter than a frame arrives. */
void cmvs_input_function(cmvs_input *in, int function, int down);

/* The two halves of 0x00448B20's contract: the RELEASED edge with HELD handed
 * back, and the clear that zeroes RELEASED and PRESSED together. */
int cmvs_input_released(const cmvs_input *in, int function, int *held);
int cmvs_input_pressed(const cmvs_input *in, int function);
int cmvs_input_held(const cmvs_input *in, int function);
void cmvs_input_clear(cmvs_input *in, int function);

#endif
