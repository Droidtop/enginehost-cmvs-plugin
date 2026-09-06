#include "interp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "commands.h"
#include "menu.h"
#include "scene.h"
#include "vm.h"

#define MAX_SLOTS      8
#define STACK_BYTES    (256 * 1024)
#define MAX_DEPTH      256
#define GLOBALS        0x4000     /* the int array at 0x596d78 */
#define FLAGS          0x10000    /* the bit array at 0x5bcd80 */
#define SCRIPT_VARS    0x4000     /* the per-script area at +0x3aec */
#define COMMANDS       0x400
#define FGLOBALS       2048       /* the float array at 0x5bad80 */
#define GSTRINGS       128        /* the string array at 0x59ad80 */
#define GSTRING_SIZE   1024       /* 0x473c30: index << 10, so 1 KB apiece */
#define SCRATCH        4096       /* the accumulator at +0x13d60 */

typedef struct {
    cmvs_script script;
    int loaded;
    char name[128];
    int32_t vars[SCRIPT_VARS / 4];
} cmvs_slot;

struct cmvs_interp {
    cmvs_game *game;
    cmvs_scene *scene;
    cmvs_menus *menus;           /* the six at +0xc6c */
    cmvs_input input;            /* the device at +0xcb8 */

    cmvs_slot slot[MAX_SLOTS];
    int current;                 /* +0x3390 */
    int pc;                      /* +0x3394 */
    /*
     * +0x3398, and it is not a slot: 0x0045AC97 increments it every time a
     * command returns bit 0x2000 and zeroes it on every other return, so it
     * counts the frames a repeating command has been repeating for.
     * 0x00463560 saves it on the frame it builds and 0x0414 restores it,
     * which is why it travels with a call.
     */
    int repeat;

    uint8_t stack[STACK_BYTES];  /* +0x13c20 */
    int sp;                      /* +0x13c24, a byte offset */
    int frame[MAX_DEPTH];        /* +0x13c28 */
    int depth;                   /* +0x13d28 */

    /* The ten millisecond timers at +0x339c. 0x0045A8E0 adds the frame's
     * elapsed time to all ten every time the engine calls the interpreter,
     * command 0x00B resets one and command 0x00C reads one into sys[0]. That
     * is how every wait in the game is written. */
    int32_t timer[10];
    int frame_ms;

    /*
     * How many times a menu poll has answered with an item, and which item the
     * last one was. Nothing in the engine reads these: they are here so a
     * frontend can say in its log that a press reached the menu and what it
     * selected, which on a console is the difference between input that never
     * arrived and a menu that ignored it.
     */
    int menu_events;
    int menu_last;

    /*
     * The 64 registered procedures at +0x33c8, 0x1c bytes apiece. Command
     * 0x088 puts the current script and a label into one of them and command
     * 0x08a calls it: this is how a script hands the engine a piece of itself
     * to run later, and how menu.ps3 gets from START to the game.
     */
    struct {
        int used;
        int slot;
        int pc;
        int32_t a, b, c;
    } proc[64];

    int32_t acc;                 /* +0x13d30 */
    int flag;                    /* +0x13d2c bit 0 */
    int32_t sys[16];             /* +0x13d34 onwards, what token 0x10F reads */

    int32_t globals[GLOBALS];
    float fglobals[FGLOBALS];
    uint8_t flags[FLAGS / 8];

    /*
     * The string machine. Operands in the 0x0201 grammar append their text to
     * one accumulator (+0x13d60) as a side effect of being resolved, which is
     * how the language concatenates; the statement itself evaluates to a handle
     * to where the text is stored, never to the text.
     */
    char scratch[SCRATCH];
    char gstring[GSTRINGS][GSTRING_SIZE];

    int running;       /* still executing this frame */
    int alive;         /* the script has not run off its end */
    int trace;
    long statements;
    int command_seen[COMMANDS];
    int command_known[COMMANDS];
};

static void fail(char *err, size_t errlen, const char *msg)
{
    if (err && errlen) snprintf(err, errlen, "%s", msg);
}

cmvs_interp *cmvs_interp_new(cmvs_game *game)
{
    cmvs_interp *in = calloc(1, sizeof *in);
    if (!in) return NULL;
    in->game = game;
    in->current = -1;
    in->frame_ms = 16;
    in->scene = cmvs_scene_new(game, cmvs_game_width(game), cmvs_game_height(game));
    if (!in->scene) { free(in); return NULL; }
    in->menus = cmvs_menus_new();
    if (!in->menus) { cmvs_scene_free(in->scene); free(in); return NULL; }
    return in;
}

void cmvs_interp_free(cmvs_interp *in)
{
    int i;
    if (!in) return;
    for (i = 0; i < MAX_SLOTS; i++)
        if (in->slot[i].loaded) cmvs_script_close(&in->slot[i].script);
    cmvs_menus_free(in->menus);
    cmvs_scene_free(in->scene);
    free(in);
}

void cmvs_interp_trace(cmvs_interp *in, int on) { in->trace = on; }
cmvs_scene *cmvs_interp_scene(cmvs_interp *in) { return in->scene; }
cmvs_input *cmvs_interp_input(cmvs_interp *in) { return in ? &in->input : NULL; }
int cmvs_interp_menu_events(const cmvs_interp *in, int *last_item)
{
    if (last_item) *last_item = in ? in->menu_last : -1;
    return in ? in->menu_events : 0;
}

long cmvs_interp_statements(const cmvs_interp *in) { return in->statements; }

static const cmvs_script *code(const cmvs_interp *in)
{
    return &in->slot[in->current].script;
}

static int word_at(const cmvs_interp *in, int at)
{
    const cmvs_script *s = code(in);
    if (at < 0 || at + 2 > s->code_size) return -1;
    return s->code[at] | (s->code[at + 1] << 8);
}

static int32_t dword_at(const cmvs_interp *in, int at)
{
    const cmvs_script *s = code(in);
    if (at < 0 || at + 4 > s->code_size) return 0;
    return (int32_t) ((uint32_t) s->code[at] | ((uint32_t) s->code[at + 1] << 8)
                    | ((uint32_t) s->code[at + 2] << 16) | ((uint32_t) s->code[at + 3] << 24));
}

static int32_t stack_get(const cmvs_interp *in, int at)
{
    int32_t v = 0;
    if (at < 0 || at + 4 > STACK_BYTES) return 0;
    memcpy(&v, in->stack + at, 4);
    return v;
}

static void stack_set(cmvs_interp *in, int at, int32_t v)
{
    if (at < 0 || at + 4 > STACK_BYTES) return;
    memcpy(in->stack + at, &v, 4);
}

static void push(cmvs_interp *in, int32_t v)
{
    stack_set(in, in->sp, v);
    in->sp += 4;
    if (in->sp > STACK_BYTES - 4) in->sp = STACK_BYTES - 4;
}

static int32_t pop(cmvs_interp *in)
{
    in->sp -= 4;
    if (in->sp < 0) in->sp = 0;
    return stack_get(in, in->sp);
}

/* ------------------------------------------------- variables and operands */

static int32_t script_var(const cmvs_interp *in, int32_t offset)
{
    const cmvs_slot *s = &in->slot[in->current];
    int32_t v = 0;
    if (offset < 0 || offset + 4 > SCRIPT_VARS) return 0;
    memcpy(&v, (const uint8_t *) s->vars + offset, 4);
    return v;
}

static void set_script_var(cmvs_interp *in, int32_t offset, int32_t v)
{
    cmvs_slot *s = &in->slot[in->current];
    if (offset < 0 || offset + 4 > SCRIPT_VARS) return;
    memcpy((uint8_t *) s->vars + offset, &v, 4);
}

static int32_t global_get(const cmvs_interp *in, int32_t i)
{
    return (i >= 0 && i < GLOBALS) ? in->globals[i] : 0;
}

static void global_set(cmvs_interp *in, int32_t i, int32_t v)
{
    if (i >= 0 && i < GLOBALS) in->globals[i] = v;
}

static int32_t flag_get(const cmvs_interp *in, int32_t i)
{
    if (i < 0 || i >= FLAGS) return 0;
    return (in->flags[i >> 3] >> (i & 7)) & 1;
}

static void flag_set(cmvs_interp *in, int32_t i, int32_t v)
{
    if (i < 0 || i >= FLAGS) return;
    if (v) in->flags[i >> 3] |= (uint8_t) (1u << (i & 7));
    else   in->flags[i >> 3] &= (uint8_t) ~(1u << (i & 7));
}

/* ---------------------------------------------------------------- strings */

/*
 * Where a handle points. This is 0x0045e530, the routine every command calls to
 * turn one of its arguments into a char*, and it is the whole string model:
 * four storages, told apart by the top two bits.
 */
static const char *string_text(cmvs_interp *in, int32_t value)
{
    uint32_t u = (uint32_t) value, off = u & 0x3FFFFFFFu;
    switch (u & CMVS_STR_TAG) {
    case CMVS_STR_POOL:
        return in->current >= 0 ? cmvs_script_string(code(in), u) : NULL;
    case CMVS_STR_SCRIPT:
        if (in->current < 0 || off >= SCRIPT_VARS) return NULL;
        return (const char *) in->slot[in->current].vars + off;
    case CMVS_STR_GLOBAL:
        return in->gstring[off % GSTRINGS];
    default: {
        int at = in->frame[in->depth] + (int) off;
        if (at < 0 || at >= STACK_BYTES) return NULL;
        return (const char *) in->stack + at;
    }
    }
}

/*
 * The same four storages, writable. lstrcpyA, lstrcatA and the substring
 * command all write THROUGH a handle their caller passes, so a command needs
 * the char* string_text hands out and the room left after it. The pool is the
 * script's own constant text and is the one storage that is not writable; a
 * command handed a pool handle as its destination writes nothing, which is
 * what the original does too because the pool is in the loaded script image.
 */
static char *string_buffer(cmvs_interp *in, int32_t value, size_t *room)
{
    uint32_t u = (uint32_t) value, off = u & 0x3FFFFFFFu;
    switch (u & CMVS_STR_TAG) {
    case CMVS_STR_SCRIPT:
        if (in->current < 0 || off >= SCRIPT_VARS) return NULL;
        *room = (size_t) (SCRIPT_VARS - off);
        return (char *) in->slot[in->current].vars + off;
    case CMVS_STR_GLOBAL:
        *room = GSTRING_SIZE;
        return in->gstring[off % GSTRINGS];
    case CMVS_STR_STACK: {
        int at = in->frame[in->depth] + (int) off;
        if (at < 0 || at >= STACK_BYTES) return NULL;
        *room = (size_t) (STACK_BYTES - at);
        return (char *) in->stack + at;
    }
    default:
        return NULL;
    }
}

/* cp932's lead bytes: the test 0x00406c20 makes, and the reason the substring
 * command counts characters rather than bytes. */
static int cmvs_lead_byte(unsigned char c)
{
    return (c >= 0x81 && c <= 0x9F) || (c >= 0xE0 && c <= 0xFC);
}

/*
 * 0x00406cd0, the engine's own string comparison, and it answers "these are
 * the same" rather than an ordering: 1 when both strings end together, 0 at
 * the first difference. A double-byte character is matched byte for byte; a
 * single-byte one is folded to upper case first (the exe adds 0xE0 to a byte
 * in 'a'..'z', which is the same wrap), so a script's comparison against a
 * letter does not depend on the case the text is written in.
 */
static int same_text(const char *a, const char *b)
{
    while (*a) {
        unsigned char ca = (unsigned char) *a, cb;
        if (cmvs_lead_byte(ca)) {
            if (ca != (unsigned char) *b) return 0;
            if (!a[1] || !b[1]) return 0;
            ca = (unsigned char) a[1];
            cb = (unsigned char) b[1];
            a += 2;
            b += 2;
        } else {
            cb = (unsigned char) *b;
            a++;
            b++;
            if (ca >= 0x61 && ca <= 0x7A) ca -= 0x20;
            if (cb >= 0x61 && cb <= 0x7A) cb -= 0x20;
        }
        if (ca != cb) return 0;
    }
    return *b == 0;
}

/*
 * The handle an operand token stands for, from the epilogue at 0x00459992: the
 * token decides the tag and the operand is the offset. 0x122 is the one that
 * holds a handle rather than being one, so it is read out of the stack.
 */
static int32_t string_handle(cmvs_interp *in, int token, int32_t operand)
{
    uint32_t off = (uint32_t) operand & 0x3FFFFFFFu;
    switch (token) {
    case 0x120: return operand;
    case 0x121: return (int32_t) (CMVS_STR_GLOBAL | off);
    case 0x122: return stack_get(in, in->frame[in->depth] - operand);
    case 0x125: return (int32_t) (CMVS_STR_SCRIPT | off);
    case 0x127: return (int32_t) (CMVS_STR_STACK | off);
    default: return 0;
    }
}

/* Whether such a token leaves anything in the accumulator at all: the epilogue
 * jumps straight to its exit for every token it does not list. */
static int string_token(int token)
{
    return token == 0x120 || token == 0x121 || token == 0x122
        || token == 0x125 || token == 0x127;
}

static void string_append(cmvs_interp *in, const char *text)
{
    size_t have, add;
    if (!text) return;
    have = strlen(in->scratch);
    add = strlen(text);
    if (have + 1 >= sizeof in->scratch) return;
    if (have + add + 1 > sizeof in->scratch) add = sizeof in->scratch - 1 - have;
    memcpy(in->scratch + have, text, add);
    in->scratch[have + add] = 0;
}

/*
 * The store side of the three writable string tokens (0x004584f6, 0x0045850f,
 * 0x00458531). Each copies the accumulator into its own storage and ignores the
 * value argument, which is why a string assignment passes zero for it.
 */
static void string_store(cmvs_interp *in, int token, int32_t operand)
{
    char *dst;
    size_t cap;
    switch (token) {
    case 0x121:
        dst = in->gstring[(uint32_t) operand % GSTRINGS];
        cap = GSTRING_SIZE;
        break;
    case 0x125:
        if (in->current < 0 || operand < 0 || operand >= SCRIPT_VARS) return;
        dst = (char *) in->slot[in->current].vars + operand;
        cap = (size_t) (SCRIPT_VARS - operand);
        break;
    case 0x127: {
        int at = in->frame[in->depth] + operand;
        if (at < 0 || at >= STACK_BYTES) return;
        dst = (char *) in->stack + at;
        cap = (size_t) (STACK_BYTES - at);
        break;
    }
    default: return;
    }
    snprintf(dst, cap, "%s", in->scratch);
}

/* The float storages (0x00458162, 0x00458188, 0x0045819f, 0x004581c6) share
 * their bytes with the integer ones and differ only in being read through an
 * ftol, so the conversion is where the float-ness lives. */
static int32_t float_bits(const void *p)
{
    float f;
    memcpy(&f, p, sizeof f);
    return (int32_t) f;
}

static int32_t float_stack(const cmvs_interp *in, int at)
{
    if (at < 0 || at + 4 > STACK_BYTES) return 0;
    return float_bits(in->stack + at);
}

/*
 * The value of one expression node, from the resolver at 0x457fc0. Its jump
 * table (0x458354 into 0x45830C) is what says which token means which storage:
 * 0x100 immediate, 0x101 an int global, 0x102 a flag, 0x103 and 0x122 locals
 * below the frame, 0x104..0x107 script variables, 0x108..0x10B locals above it,
 * 0x10E a label out of the index, 0x10F a system value, 0x120 a string.
 */
static int32_t resolve(cmvs_interp *in, int token, int32_t operand)
{
    int base = in->frame[in->depth];
    switch (token) {
    case 0x100: case 0x12A: return operand;
    case 0x101: return global_get(in, operand);
    case 0x102: return flag_get(in, operand);
    case 0x103: return stack_get(in, base - operand);
    case 0x104: case 0x106: case 0x107: return script_var(in, operand);
    case 0x108: case 0x10A: case 0x10B: return stack_get(in, base + operand);
    case 0x10E: {
        const cmvs_script *s = code(in);
        if (operand >= 0 && operand < s->index_count) return (int32_t) s->index[operand];
        return 0;
    }
    case 0x10F:
        return (operand >= 0 && operand < (int32_t) (sizeof in->sys / sizeof in->sys[0]))
             ? in->sys[operand] : 0;
    /*
     * The FIVE string operands evaluate to NOTHING. Each appends its text to
     * the accumulator and falls into `mov eax, edi` with edi zeroed at the top
     * of the resolver, which is how concatenation is expressed without a value
     * ever carrying the text.
     */
    case 0x120: case 0x121: case 0x125: case 0x127:
        string_append(in, string_text(in, string_handle(in, token, operand)));
        return 0;
    /*
     * 0x122 is not an integer local. Its handler (0x0045822a) reads a local
     * BELOW the frame, treats what it finds as a string handle - it switches on
     * the same top two bits string_text does, at 0x45827c / 0x458272 /
     * 0x45828d / 0x4582be - and appends the text it names. That is how a script
     * name reaches a called procedure: the caller pushes the handle, the callee
     * says `gstring[n] = arg` and the assignment copies the text across.
     */
    case 0x122:
        string_append(in, string_text(in, stack_get(in, base - operand)));
        return 0;
    case 0x129: return float_stack(in, base - operand);
    case 0x12B:
        return (operand >= 0 && operand < FGLOBALS) ? (int32_t) in->fglobals[operand] : 0;
    case 0x12C: return float_stack(in, base + operand);
    case 0x12E:
        if (in->current < 0 || operand < 0 || operand + 4 > SCRIPT_VARS) return 0;
        return float_bits((const uint8_t *) in->slot[in->current].vars + operand);
    /* Every other token in the table's range lands on the default handler,
     * which returns this same zero. */
    default: return 0;
    }
}

/* The store side, from 0x4583b0: the same storage classes, written instead. */
static void assign(cmvs_interp *in, int token, int32_t operand, int32_t value)
{
    int base = in->frame[in->depth];
    switch (token) {
    case 0x101: global_set(in, operand, value); break;
    case 0x102: flag_set(in, operand, value); break;
    case 0x103: stack_set(in, base - operand, value); break;
    case 0x104: case 0x106: case 0x107: set_script_var(in, operand, value); break;
    case 0x108: case 0x10A: case 0x10B: stack_set(in, base + operand, value); break;
    case 0x10F:
        if (operand >= 0 && operand < (int32_t) (sizeof in->sys / sizeof in->sys[0]))
            in->sys[operand] = value;
        break;
    case 0x121: case 0x125: case 0x127: string_store(in, token, operand); break;
    default: break;
    }
}

/* ------------------------------------------------------------ expressions */

/*
 * The token stream is postfix: a node is pushed for every operand token, and an
 * operator token (0x160..0x17E) rewrites the two below it. The stack holds
 * nodes rather than values because an assignment needs its destination token
 * and operand, not just the value the destination currently has.
 */
typedef struct { int token; int32_t operand; } node;

#define MAX_NODES 128

typedef struct {
    node n[MAX_NODES];
    int count;
} nodes;

static void node_push(nodes *st, int token, int32_t operand)
{
    if (st->count < MAX_NODES) {
        st->n[st->count].token = token;
        st->n[st->count].operand = operand;
        st->count++;
    }
}

static int32_t binary(int token, int32_t a, int32_t b)
{
    switch (token) {
    case 0x160: return a * b;
    case 0x161: return b ? a / b : 0;
    case 0x162: return b ? a % b : 0;
    case 0x163: return a + b;
    case 0x164: return a - b;
    case 0x165: return a & b;
    case 0x166: return a | b;
    case 0x167: return a ^ b;
    case 0x168: return (int32_t) ((uint32_t) a << (b & 31));
    case 0x169: return a >> (b & 31);
    case 0x16A: return a > b;
    case 0x16B: return a >= b;
    case 0x16C: return a < b;
    case 0x16D: return a <= b;
    case 0x16E: return a && b;
    case 0x16F: return a || b;
    case 0x171: return a == b;
    case 0x172: return a != b;
    default: return b;
    }
}

/* The compound assignments at 0x459768, in the order the exe lists them. */
static int32_t compound(int token, int32_t a, int32_t b)
{
    switch (token) {
    case 0x175: return a + b;
    case 0x176: return a - b;
    case 0x177: return a * b;
    case 0x178: return b ? a / b : 0;
    case 0x179: return a & b;
    case 0x17A: return a | b;
    case 0x17B: return b ? a % b : 0;
    case 0x17C: return a ^ b;
    case 0x17D: return (int32_t) ((uint32_t) a << (b & 31));
    case 0x17E: return a >> (b & 31);
    default: return b;
    }
}

static int eval_expression(cmvs_interp *in, int grammar, int at, int depth, int32_t *result);

/* Applies one token to the node stack. Returns 0 if the stream is malformed. */
static int apply(cmvs_interp *in, nodes *st, int token, int32_t operand)
{
    if (token >= 0x160 && token <= 0x172 && token != 0x170) {
        int32_t a, b;
        if (st->count < 2) return 0;
        a = resolve(in, st->n[st->count - 2].token, st->n[st->count - 2].operand);
        b = resolve(in, st->n[st->count - 1].token, st->n[st->count - 1].operand);
        st->count--;
        st->n[st->count - 1].token = 0x100;
        st->n[st->count - 1].operand = binary(token, a, b);
        return 1;
    }
    if (token == 0x170) {
        int32_t v;
        if (st->count < 2) return 0;
        v = resolve(in, st->n[st->count - 1].token, st->n[st->count - 1].operand);
        assign(in, st->n[st->count - 2].token, st->n[st->count - 2].operand, v);
        st->count--;
        st->n[st->count - 1].token = 0x100;
        st->n[st->count - 1].operand = v;
        return 1;
    }
    if (token == 0x173 || token == 0x174) {
        int32_t v;
        if (st->count < 1) return 0;
        v = resolve(in, st->n[st->count - 1].token, st->n[st->count - 1].operand);
        v += token == 0x173 ? 1 : -1;
        assign(in, st->n[st->count - 1].token, st->n[st->count - 1].operand, v);
        st->n[st->count - 1].token = 0x100;
        st->n[st->count - 1].operand = v;
        return 1;
    }
    if (token >= 0x175 && token <= 0x17E) {
        int32_t a, b, v;
        if (st->count < 2) return 0;
        a = resolve(in, st->n[st->count - 2].token, st->n[st->count - 2].operand);
        b = resolve(in, st->n[st->count - 1].token, st->n[st->count - 1].operand);
        v = compound(token, a, b);
        assign(in, st->n[st->count - 2].token, st->n[st->count - 2].operand, v);
        st->count--;
        st->n[st->count - 1].token = 0x100;
        st->n[st->count - 1].operand = v;
        return 1;
    }
    node_push(st, token, operand);
    return 1;
}

/*
 * The base token an operand parses AS. An indexed variable is not a token the
 * resolver ever sees: the parsers at 0x00458eb0 and 0x00459ab0 rewrite it to
 * the plain token of the same storage and fold the index into the operand, so
 * that by the time a node exists the subscript is already an offset. Reading
 * the resolver alone would say these tokens mean nothing, because they never
 * reach it.
 */
static int operand_base_token(int token)
{
    switch (token) {
    case 0x105: return 0x104;   /* 0x004593f6 */
    case 0x109: return 0x108;   /* 0x00459424 */
    case 0x110: return 0x106;   /* 0x00459452 */
    case 0x111: return 0x107;   /* 0x004594a2 */
    case 0x112: return 0x10A;   /* 0x00459459 */
    case 0x113: return 0x10B;   /* 0x0045950b */
    case 0x12A: return 0x100;   /* 0x00459512: a float literal, made an int */
    case 0x12D: return 0x12C;   /* 0x00459552 */
    case 0x12F: return 0x12E;   /* 0x0045955c */
    case 0x134: return 0x130;   /* 0x0045a629, the float grammar's own */
    case 0x135: return 0x131;   /* 0x0045a647 */
    case 0x136: return 0x132;   /* 0x0045a633 */
    case 0x137: return 0x133;   /* 0x0045a65b */
    default: return token;      /* 0x107, 0x10B, 0x131 and 0x133 keep theirs */
    }
}

/*
 * Reads one operand token into a node. Three subscript shapes exist and the
 * difference is only the stride: an index scaled by four for a dword array, an
 * index scaled by a WORD the token carries at +6 for a row of a table, and that
 * same row followed by a second index for the column.
 */
static int read_operand(cmvs_interp *in, int grammar, int at, int depth,
                        int *token_out, int32_t *operand_out, int *next)
{
    const cmvs_script *s = code(in);
    int token = word_at(in, at), len, nested;
    int32_t operand = 0, stride = 4;
    if (token < 0) return 0;
    cmvs_token_shape((cmvs_grammar) grammar, token, &len, &nested);
    if (len >= 6) operand = dword_at(in, at + 2);
    if (len >= 8) stride = word_at(in, at + 6);
    if (token == 0x12A) {
        /* 0x00459512 loads the operand as a float and rounds it. */
        float f;
        uint32_t bits = (uint32_t) operand;
        memcpy(&f, &bits, sizeof f);
        operand = (int32_t) f;
    }
    at += len;
    if (nested) {
        int32_t index = 0;
        int end = cmvs_expression_end(s, CMVS_GRAMMAR_200, at + 2);
        if (end < 0 || !eval_expression(in, CMVS_GRAMMAR_200, at + 2, depth + 1, &index))
            return 0;
        at = end;
        if (token == 0x101 || token == 0x102 || token == 0x12B) {
            operand = index;          /* the index IS the operand: an array of one */
        } else {
            operand += index * stride;
            if (token == 0x111 || token == 0x113 || token == 0x135 || token == 0x137) {
                int32_t column = 0;
                end = cmvs_expression_end(s, CMVS_GRAMMAR_200, at + 2);
                if (end < 0 || !eval_expression(in, CMVS_GRAMMAR_200, at + 2, depth + 1, &column))
                    return 0;
                at = end;
                operand += column * 4;
            }
        }
    }
    *token_out = operand_base_token(token);
    *operand_out = operand;
    *next = at;
    return 1;
}

static int run_tokens(cmvs_interp *in, int grammar, int at, int depth, nodes *st, int *end)
{
    for (;;) {
        int token = word_at(in, at), next = at;
        int32_t operand = 0;
        if (token < 0) return 0;
        if (token == 0x020F) { *end = at + 2; return 1; }
        if (token == 0x0200 && grammar != CMVS_GRAMMAR_201) {
            int32_t v = 0;
            if (!eval_expression(in, CMVS_GRAMMAR_200, at + 2, depth + 1, &v)) return 0;
            next = cmvs_expression_end(code(in), CMVS_GRAMMAR_200, at + 2);
            if (next < 0) return 0;
            node_push(st, 0x100, v);
            at = next;
            continue;
        }
        if (!read_operand(in, grammar, at, depth, &token, &operand, &next)) return 0;
        at = next;
        if (!apply(in, st, token, operand)) return 0;
    }
}

/*
 * The 0x0201 statement (0x00459790) is the string grammar, and it is a
 * different machine from the other two. Its whole vocabulary is three shapes:
 *
 *  - an operand token, which APPENDS its text to the accumulator and is worth
 *    nothing as a value (that is how the language concatenates);
 *  - a binary operator, every one of which is concatenation here: it resolves
 *    the left node then the right one and collapses both into a blank node;
 *  - 0x170, assignment: resolve the right node, then copy the accumulator into
 *    the storage the left node names.
 *
 * What the statement leaves in the interpreter's accumulator is decided by the
 * FIRST node alone (the epilogue at 0x00459992), and it is a handle to that
 * node's storage. So a string argument is always pushed as a bare variable
 * reference; a concatenation blanks the first node and the epilogue then
 * leaves the accumulator untouched, which is why `set_acc` exists here.
 */
static int run_string_expression(cmvs_interp *in, int at, int *end, int *set_acc,
                                 int32_t *result)
{
    node st[MAX_NODES];
    int count = 0;

    in->scratch[0] = 0;   /* 0x004597aa empties it before the first token */
    for (;;) {
        int token = word_at(in, at), len, nested;
        int32_t operand = 0;
        if (token < 0) return 0;
        if (token == 0x020F) { at += 2; break; }
        cmvs_token_shape(CMVS_GRAMMAR_201, token, &len, &nested);
        if (nested) {
            /* 0x004598fb: token 0x121's index is a nested 0x200 expression. */
            int32_t v = 0;
            int next = cmvs_expression_end(code(in), CMVS_GRAMMAR_200, at + len + 2);
            if (next < 0 || !eval_expression(in, CMVS_GRAMMAR_200, at + len + 2, 1, &v))
                return 0;
            operand = v;
            at = next;
        } else {
            if (len >= 6) operand = dword_at(in, at + 2);
            at += len;
        }
        if (token >= 0x160 && token <= 0x172) {
            if (count < 2) return 0;
            if (token == 0x170) {
                resolve(in, st[count - 1].token, st[count - 1].operand);
                string_store(in, st[count - 2].token, st[count - 2].operand);
            } else {
                resolve(in, st[count - 2].token, st[count - 2].operand);
                resolve(in, st[count - 1].token, st[count - 1].operand);
            }
            st[count - 2].token = 0;
            st[count - 2].operand = 0;
            count--;
        } else if (count < MAX_NODES) {
            st[count].token = token;
            st[count].operand = operand;
            count++;
        }
    }
    *end = at;
    *set_acc = count > 0 && string_token(st[0].token);
    *result = *set_acc ? string_handle(in, st[0].token, st[0].operand) : 0;
    return 1;
}

static int eval_expression(cmvs_interp *in, int grammar, int at, int depth, int32_t *result)
{
    nodes st;
    int end = 0;
    if (depth > 32) return 0;
    st.count = 0;
    if (!run_tokens(in, grammar, at, depth, &st, &end)) return 0;
    *result = st.count > 0
            ? resolve(in, st.n[st.count - 1].token, st.n[st.count - 1].operand) : 0;
    return 1;
}

/* ---------------------------------------------------------------- commands */

/*
 * The engine convention, read off handlers like 0x46bb90: a command reads its
 * arguments from under the stack top and does NOT pop them; the bytecode drops
 * them itself with 0x0412. So arg(n, i) is the i-th of n pushed values.
 */
static int32_t arg(const cmvs_interp *in, int n, int i)
{
    return stack_get(in, in->sp - 4 * (n - i));
}

/* A command argument as text. Only the tagged storages are certain to be
 * strings; an untagged value is a string-pool offset, but it is also what every
 * small integer looks like, so the caller decides whether to believe it. */
static const char *as_string(cmvs_interp *in, int32_t v)
{
    if (((uint32_t) v & CMVS_STR_TAG) == 0) return NULL;
    return string_text(in, v);
}

/*
 * A command handler returns bit flags, the two that matter being 0x8000 "the
 * script stops here" and 0x4000 "step the pc past me". Command 0x000 returns
 * 0xC000: it is how a script yields back to the engine, which is why the boot
 * script ends on it rather than running off its own end.
 */
static int command_builtin(cmvs_interp *in, int command);
static int load_slot(cmvs_interp *in, int slot, const char *name, char *err, size_t errlen);
static int enter_script(cmvs_interp *in, const char *name, char *err, size_t errlen);

static int do_command(cmvs_interp *in, int command)
{
    if (command >= 0 && command < COMMANDS) in->command_seen[command]++;
    if (in->trace) {
        /* The ABI byte count is how many arguments the bytecode pushed, so the
         * trace can name them all instead of guessing at the top two. */
        int abi = (command >= 0 && command < CMVS_COMMANDS) ? cmvs_command_abi[command] : 0;
        int n = abi > 0 ? CMVS_CMD_ARGS(abi) / 4 : 0, i;
        fprintf(stderr, "  %-12s cmd 0x%03x (", in->slot[in->current].name, command);
        for (i = 0; i < n; i++) {
            int32_t v = arg(in, n, i);
            const char *text = as_string(in, v);
            if (text) { fprintf(stderr, "%s\"%s\"", i ? ", " : "", text); continue; }
            fprintf(stderr, "%s%d", i ? ", " : "", v);
            /* An untagged value may still be a pool offset. Only believe it
             * when it lands on the START of a pooled string, or every small
             * integer in the trace acquires a spurious quotation. */
            if (v > 0 && code(in)->strings && v < code(in)->strings_size
                && code(in)->strings[v - 1] == 0) {
                text = string_text(in, v);
                if (text && *text) fprintf(stderr, "=\"%s\"", text);
            }
        }
        fprintf(stderr, ")  sp=%d acc=%d", in->sp, in->acc);
        fputc(0x0A, stderr);
    }
    {
        int abi = (command >= 0 && command < CMVS_COMMANDS) ? cmvs_command_abi[command] : -1;
        int flags = command_builtin(in, command);
        if (abi < 0) abi = CMVS_CMD_ADVANCE;   /* extractor gap; see commands.c */
        /* A handler that answers with CMVS_CMD_OWN has decided its whole
         * return and the table's single constant does not apply to it. */
        if (!(flags & CMVS_CMD_OWN)) flags |= abi;
        flags &= ~CMVS_CMD_OWN;
        /*
         * The argument pop is the engine's own bookkeeping and it belongs to
         * the SAME branch as the pc step: 0x0045AC80 tests bit 0x4000 and only
         * then does it add two to the pc and subtract the low byte from the
         * stack pointer. A command that does not step past itself has not
         * consumed its arguments either, which is exactly what lets a wait
         * re-enter next frame with them still under the stack top.
         */
        if (flags & CMVS_CMD_ADVANCE) {
            in->sp -= CMVS_CMD_ARGS(flags);
            if (in->sp < 0) in->sp = 0;
        }
        if (flags & CMVS_CMD_STOP) in->running = 0;
        in->repeat = (flags & CMVS_CMD_REPEAT) ? in->repeat + 1 : 0;
        return flags;
    }
}


/*
 * The built-in commands. None of these is guessed: each was read off its
 * handler in cmvs32.exe, and the address is on the case.
 *
 * The archive mounts (0x010..0x01A) are recorded rather than acted on, because
 * cmvs_game already opens every archive under the pack folder; keeping a second
 * mount table would be two mechanisms for one job.
 *
 * Arguments sit under the stack top and are NOT popped by the command: arg(n,i)
 * is the i-th of n in the order the bytecode pushed them, so the LAST one is
 * what a handler reads as [sp-4]. In the drawing commands that last argument is
 * always the object and the one before it the part, with -1 meaning the object
 * itself.
 */
static int command_builtin(cmvs_interp *in, int command)
{
    /* A command knows its own arguments are strings, so it decodes them
     * unconditionally (0x0045e530). as_string is the trace's guess; this is
     * not a guess. */
    const char *name = string_text(in, arg(in, 1, 0));

    switch (command) {
    case 0x000:
        return CMVS_CMD_STOP;
    case 0x00B: {   /* 0x0045E750: timer[arg] = 0 */
        int32_t t = arg(in, 1, 0);
        if (t >= 0 && t < 10) in->timer[t] = 0;
        in->command_known[command] = 1;
        return 0;
    }
    case 0x00C: {   /* 0x0045E780: sys[0] = timer[arg] */
        int32_t t = arg(in, 1, 0);
        in->sys[0] = (t >= 0 && t < 10) ? in->timer[t] : 0;
        in->command_known[command] = 1;
        return 0;
    }
    case 0x080: {
        /* 0x0046FA90: read the name, load it over slot 0 (0x0046EF20 sets
         * +0x3390 to 0, the pc to the script's own header entry at 0x20 and
         * the stack, depth and frame pointer to zero), then compose a frame.
         * It returns 0: no advance, because the pc now belongs to the new
         * script. This is how start.ps3 reaches main.ps3 and main.ps3 reaches
         * logo.ps3 - it is a jump between scripts, not a call. */
        char why[256];
        char wanted[128];
        if (!name) return 0;
        snprintf(wanted, sizeof wanted, "%s", name);
        if (!enter_script(in, wanted, why, sizeof why)) {
            if (in->trace) fprintf(stderr, "  cannot enter %s: %s\n", wanted, why);
            return CMVS_CMD_ADVANCE;
        }
        in->command_known[command] = 1;
        return 0;
    }
    /* ------------------------------------------------------- graphic objects */
    case 0x020:   /* 0x0045e8f0: a fresh object in the table at +0x77c */
        in->command_known[command] = cmvs_scene_object(in->scene, arg(in, 1, 0));
        return 0;
    case 0x021:   /* 0x0045e9b0: and the object is gone, parts and all */
        cmvs_scene_drop(in->scene, arg(in, 1, 0), -1);
        in->command_known[command] = 1;
        return 0;
    case 0x02F:   /* 0x0045ea10: all 256 of them, the layers untouched */
        cmvs_scene_drop_all(in->scene);
        in->command_known[command] = 1;
        return 0;
    case 0x027:   /* 0x0045eb50: is it there? The script asks before it draws */
        in->sys[0] = cmvs_scene_exists(in->scene, arg(in, 2, 1), arg(in, 2, 0));
        in->command_known[command] = 1;
        return 0;
    case 0x022:   /* 0x00433cb0: a fresh part, an object of the same class */
        in->command_known[command] =
            cmvs_scene_part(in->scene, arg(in, 2, 1), arg(in, 2, 0));
        return 0;
    case 0x030:   /* 0x0045ee10: decode a PB3 into the object (0x420840) */
        in->sys[0] = cmvs_scene_bitmap(in->scene, arg(in, 2, 1),
                                       string_text(in, arg(in, 2, 0)));
        in->command_known[command] = 1;
        return 0;
    case 0x033: {
        /*
         * 0x0045f1b0: how big is the bitmap this object holds? sys[0] is one
         * when there is an image at all (0x13d34), sys[1] its width (+0x68 of
         * the image, into 0x13d38), sys[2] its height (+0x6c, into 0x13d3c)
         * and sys[3] whether it carries alpha (+0x70, into 0x13d40).
         * snky01.ps3 asks this of every background and every character sprite
         * before it writes the source rectangle, so an unanswered 0x033 is
         * what made the scene draw a six-pixel-wide sliver of its sky.
         */
        int w = 0, h = 0, alpha = 0;
        int got = cmvs_scene_bitmap_size(in->scene, arg(in, 1, 0), -1, &w, &h, &alpha);
        in->sys[0] = got ? 1 : 0;
        in->sys[1] = w;
        in->sys[2] = h;
        in->sys[3] = alpha;
        in->command_known[command] = 1;
        return 0;
    }
    case 0x040:   /* 0x0045f650: give the object a draw item (0x434970) */
        in->command_known[command] = cmvs_scene_item(in->scene, arg(in, 1, 0), -1);
        return 0;
    case 0x050:   /* 0x0045f6d0: the same, for one part */
        in->command_known[command] =
            cmvs_scene_item(in->scene, arg(in, 2, 1), arg(in, 2, 0));
        return 0;
    case 0x044:   /* 0x0045f910 -> 0x41bd00: the source rectangle */
        cmvs_scene_source(in->scene, arg(in, 6, 5), arg(in, 6, 4),
                          arg(in, 6, 3), arg(in, 6, 2), arg(in, 6, 1), arg(in, 6, 0));
        in->command_known[command] = 1;
        return 0;
    case 0x045:   /* 0x0045f9a0 -> 0x41bd60: where it lands */
        cmvs_scene_at(in->scene, arg(in, 4, 3), arg(in, 4, 2),
                      arg(in, 4, 1), arg(in, 4, 0));
        in->command_known[command] = 1;
        return 0;
    case 0x046:   /* 0x0045faa0 -> 0x41bd40: an offset on top of that */
        cmvs_scene_offset(in->scene, arg(in, 4, 3), arg(in, 4, 2),
                          arg(in, 4, 1), arg(in, 4, 0));
        in->command_known[command] = 1;
        return 0;
    case 0x047:   /* 0x0045fb10 -> 0x41bda0: the draw order */
        cmvs_scene_depth(in->scene, arg(in, 3, 2), arg(in, 3, 1), arg(in, 3, 0));
        in->command_known[command] = 1;
        return 0;
    /* ------------------------------------------------------- layer sprites */
    /*
     * The eight display layers, and the family the played scene is made of.
     * Every one of these handlers does the same two things before anything
     * else: it refuses a layer index of 8 or more, and it turns (layer, id)
     * into an object through 0x00451d30 - the layer's own graphic object for
     * id -1, otherwise that object's part `id`. So each command below is the
     * object command of the same shape addressed at CMVS_LAYER_OBJECT(layer),
     * and the geometry setters are literally the same four routines that
     * commands 0x044 to 0x047 reach (0x41bd00, 0x41bd40, 0x41bd60, 0x41bda0).
     */
    case 0x170:   /* 0x00467510: the layer object's own bitmap (0x420840) */
        in->command_known[command] =
            cmvs_scene_bitmap(in->scene, CMVS_LAYER_OBJECT(arg(in, 2, 1)),
                              string_text(in, arg(in, 2, 0)));
        return 0;
    case 0x178: {   /* 0x004676e0 -> 0x00451d60: a fresh sprite, or the layer */
        int32_t layer = arg(in, 2, 1), id = arg(in, 2, 0);
        in->command_known[command] = id < 0
            ? cmvs_scene_object(in->scene, CMVS_LAYER_OBJECT(layer))
            : cmvs_scene_part(in->scene, CMVS_LAYER_OBJECT(layer), id);
        return 0;
    }
    case 0x179:   /* 0x00467720 -> 0x00451e30: and the sprite is gone */
        cmvs_scene_drop(in->scene, CMVS_LAYER_OBJECT(arg(in, 2, 1)),
                        arg(in, 2, 0));
        in->command_known[command] = 1;
        return 0;
    case 0x17A:   /* 0x00467760 -> 0x00432b70: shown or not */
        cmvs_scene_show(in->scene, CMVS_LAYER_OBJECT(arg(in, 3, 2)),
                        arg(in, 3, 1), arg(in, 3, 0) != 0);
        in->command_known[command] = 1;
        return 0;
    case 0x17C:   /* 0x00467810 -> 0x004337a0: the object's extent */
        cmvs_scene_extent(in->scene, CMVS_LAYER_OBJECT(arg(in, 4, 3)),
                          arg(in, 4, 2), arg(in, 4, 1), arg(in, 4, 0));
        in->command_known[command] = 1;
        return 0;
    case 0x180:   /* 0x00467910 -> 0x00434970: give the sprite a draw item */
        in->command_known[command] =
            cmvs_scene_item(in->scene, CMVS_LAYER_OBJECT(arg(in, 2, 1)),
                            arg(in, 2, 0));
        return 0;
    case 0x182:   /* 0x00467a40 -> 0x00433310 -> 0x41bd00: the source rectangle */
        cmvs_scene_source(in->scene, CMVS_LAYER_OBJECT(arg(in, 6, 5)),
                          arg(in, 6, 4), arg(in, 6, 3), arg(in, 6, 2),
                          arg(in, 6, 1), arg(in, 6, 0));
        in->command_known[command] = 1;
        return 0;
    case 0x183:   /* 0x00467ac0 -> 0x00433340 -> 0x41bd60: where it lands */
        cmvs_scene_at(in->scene, CMVS_LAYER_OBJECT(arg(in, 4, 3)),
                      arg(in, 4, 2), arg(in, 4, 1), arg(in, 4, 0));
        in->command_known[command] = 1;
        return 0;
    case 0x184:   /* 0x00467b20 -> 0x00433380 -> 0x41bda0: one size */
        cmvs_scene_depth(in->scene, CMVS_LAYER_OBJECT(arg(in, 3, 2)),
                        arg(in, 3, 1), arg(in, 3, 0));
        in->command_known[command] = 1;
        return 0;
    case 0x185:   /* 0x00467b80 -> 0x00433330 -> 0x41bd40: an offset on top */
        cmvs_scene_offset(in->scene, CMVS_LAYER_OBJECT(arg(in, 4, 3)),
                          arg(in, 4, 2), arg(in, 4, 1), arg(in, 4, 0));
        in->command_known[command] = 1;
        return 0;
    /* ------------------------------------------------------------ the text */
    /*
     * Every one of these reaches [machine + layer*4 + 0xb90] and then that
     * layer object's own text object at +0x9dc, so the layer is the LAST
     * argument exactly as it is for the sprite commands, and the setters below
     * are the ones at 0x00450400..0x004505c0.
     */
    case 0x141:   /* 0x004668d0: the layer's text object is made again */
    case 0x14A: { /* 0x00452620 -> 0x004506a0: the line is gone */
        cmvs_text *t = cmvs_scene_text(in->scene, arg(in, 1, 0));
        if (t) cmvs_text_clear(t);
        in->command_known[command] = t != NULL;
        return 0;
    }
    case 0x143: { /* 0x004669d0 -> 0x00450450: the size, on its own */
        cmvs_text *t = cmvs_scene_text(in->scene, arg(in, 2, 1));
        if (t) cmvs_text_size(t, arg(in, 2, 0), -1, -1);
        in->command_known[command] = t != NULL;
        return 0;
    }
    case 0x145: { /* 0x00466a60 -> 0x00450420: the box, and the pen with it */
        cmvs_text *t = cmvs_scene_text(in->scene, arg(in, 5, 4));
        if (t) cmvs_text_box(t, arg(in, 5, 3), arg(in, 5, 2),
                                arg(in, 5, 1), arg(in, 5, 0));
        in->command_known[command] = t != NULL;
        return 0;
    }
    case 0x146: { /* 0x00466ab0 -> 0x00450520: the two colours */
        cmvs_text *t = cmvs_scene_text(in->scene, arg(in, 3, 2));
        if (t) cmvs_text_colours(t, (uint32_t) arg(in, 3, 1),
                                    (uint32_t) arg(in, 3, 0));
        in->command_known[command] = t != NULL;
        return 0;
    }
    case 0x14F: { /* 0x00466da0 -> 0x00450550: put the pen here */
        cmvs_text *t = cmvs_scene_text(in->scene, arg(in, 3, 2));
        if (t) cmvs_text_pen(t, arg(in, 3, 1), arg(in, 3, 0));
        in->command_known[command] = t != NULL;
        return 0;
    }
    case 0x150:
        /*
         * 0x00466df0 -> 0x00452750, and it is NOT the text box: it moves the
         * whole LAYER. The routine writes the layer's own x and y at +0xc and
         * +0x10 and hands the same pair to the layer's graphic object
         * (0x00433340) and to its text object (0x004503e0, which scales the
         * pair by +0x144/+0x14c first). Mode 1 adds to what is there, mode 0
         * replaces it, and the box and the pen are never touched.
         *
         * What was here before moved the BOX and, because 0x00450420 puts the
         * pen back to the box's corner, took the pen with it: snky01.ps3's
         * second line asks for (-1174, -2853) and every glyph of it landed
         * two and a half screens above the window. Doing nothing is closer to
         * the engine than doing the wrong thing, and it leaves the line where
         * 0x145 and 0x14f put it - inside the message window.
         *
         * Acting on it properly means giving the layer a position of its own
         * and finding what computes the argument: the same call site asks for
         * (595, 612) the first time round and (295, -198) the second, and
         * neither is where this game's message window sits, so a command that
         * is still missing is feeding it. Left unimplemented, and counted as
         * such, until that is known.
         */
        return 0;
    case 0x152: { /* 0x00466e90 -> 0x004523e0 -> 0x00451120: write the line */
        cmvs_text *t = cmvs_scene_text(in->scene, arg(in, 3, 2));
        const char *text = string_text(in, arg(in, 3, 0));
        if (t && text) cmvs_text_draw(t, text);
        in->command_known[command] = t && text;
        return 0;
    }
    /* ---------------------------------------------------- the input poll */
    /*
     * The device at +0xcb8 keeps a released/held/pressed triple for each of
     * twenty-one virtual buttons, twelve bytes apart from +0x43c; 0x00448b20
     * and 0x00448f00 read two of them and their callers (0x00468a00 and
     * 0x00469020) answer the same way: sys[0] is the RELEASED edge and sys[4]
     * the HELD level, each as one or zero.
     *
     * snky01.ps3 waits on these once its line is written - it polls them every
     * frame instead of stopping in the 0x153 wait, which is the path a line
     * whose window was sized by the string commands takes - so with neither of
     * them here a tap never advanced the scene.
     */
    case 0x1A0:     /* 0x00468a00 -> 0x00448b20: the left button, +0x43c */
        in->sys[0] = in->input.left_released ? 1 : 0;
        in->sys[4] = in->input.left_held ? 1 : 0;
        in->command_known[command] = 1;
        return 0;
    case 0x34A:
        /*
         * 0x00469020 -> 0x00448f00: virtual button 20 (+0x52c), one of the
         * bound keys 0x0044afea maps out of cmvs.cfg's key table. This engine
         * takes a pointer and a pad and has nothing bound to it, so it answers
         * "not pressed" - definitely, rather than leaving whatever the last
         * measurement wrote in sys[0], which is the difference between a
         * scene that advances and one that does not.
         */
        in->sys[0] = 0;
        in->sys[4] = 0;
        in->command_known[command] = 1;
        return 0;
    case 0x153: {
        /*
         * 0x00466fc0, and it is THE WAIT: the command a line rests on until the
         * reader has had it. It is the one command here that answers two ways.
         * While the wait is on it returns 0xA000 - stop the script, do not step
         * the pc, do not pop - so the very next frame re-enters it with its
         * three arguments still under the stack top, which is the whole of the
         * per-frame poll loop. When the wait is over it returns 0x400C and the
         * script goes on to clear the line and write the next one.
         *
         * Its last argument is the layer, read the way every text command reads
         * one ([esi + a*4 + 0xb90] at 0x0046704c); the one before it is the
         * message id 0x136 logged the line under, and the first is the voice
         * the line was spoken with.
         *
         * What is modelled is the path ChronoClock's scene takes: a press
         * brings a still-typing line forward whole (0x00452cd0 consumes the
         * edge and calls the line finished) and a press on a finished line ends
         * the wait. The rest of 0x00452c50 is not here and is not pretended to
         * be: auto-play's own timer out of the settings block at +0x5a0, skip
         * mode at +0x5b8, the already-read test at 0x00473af0, and the message
         * window's own buttons through the hit test at 0x00452a80.
         */
        cmvs_text *t = cmvs_scene_text(in->scene, arg(in, 3, 2));
        int click = in->input.left_pressed;
        if (click) {
            /* 0x00448B50, which is what the original calls the moment it has
             * acted on the press. Leaving the edge latched would spend one
             * click on every wait between here and the next frame. */
            in->input.left_pressed = 0;
            in->input.left_released = 0;
        }
        in->command_known[command] = 1;
        if (t && cmvs_text_revealing(t)) {
            if (click) cmvs_text_reveal_all(t);
            return CMVS_CMD_OWN | CMVS_CMD_REPEAT | CMVS_CMD_STOP;
        }
        if (!click) return CMVS_CMD_OWN | CMVS_CMD_REPEAT | CMVS_CMD_STOP;
        return CMVS_CMD_OWN | CMVS_CMD_ADVANCE | 0x0C;
    }
    case 0x155: { /* 0x00466b00 -> 0x004505b0: the edge colour */
        cmvs_text *t = cmvs_scene_text(in->scene, arg(in, 2, 1));
        if (t) cmvs_text_edge(t, (uint32_t) arg(in, 2, 0));
        in->command_known[command] = t != NULL;
        return 0;
    }
    case 0x15D: {
        /* 0x00466670: the message window. Its last two arguments are the text
         * object's id and the layer that draws it - ChronoClock's line is
         * layer 0 registered under id 7 - and the six before them are the
         * geometry the layer's own commands set anyway. */
        cmvs_scene_text_register(in->scene, arg(in, 8, 6), arg(in, 8, 7));
        in->command_known[command] = 1;
        return 0;
    }
    /* ------------------------------------------------------ the strings */
    /*
     * The script's own string library. ChronoClock wraps its message window
     * around the line it is about to show, and this is what it does the
     * wrapping with: it asks for the length, then walks the line a character
     * at a time looking for a space, and it sizes the window from the answer.
     * With none of these here, every one of those answers was whatever the
     * last measurement had left in sys[0], and the window came out 330x450 -
     * a tall narrow box in the bottom left corner instead of the wide strip
     * the reader is meant to read across.
     *
     * Each writes through a handle its caller passes, and none of them
     * allocates: the destination is a global, a script variable or a stack
     * local, and the pool is refused because a script's constant text is not
     * writable. See string_buffer above.
     */
    case 0x0F0: {   /* 0x00465120: lstrcpyA(dst, src) */
        size_t room = 0;
        char *dst = string_buffer(in, arg(in, 2, 1), &room);
        const char *src = string_text(in, arg(in, 2, 0));
        if (dst && src) snprintf(dst, room, "%s", src);
        in->command_known[command] = dst != NULL && src != NULL;
        return 0;
    }
    case 0x0F1: {
        /*
         * 0x00465160: ZERO when the two are the same text. The handler calls
         * 0x00406cd0, which answers 1 for "same", and then runs it through
         * neg/sbb/inc - which turns 1 into 0 and 0 into 1. So the value a
         * script tests is "these differ", and reading it the other way round
         * inverts every branch built on it.
         */
        const char *a = string_text(in, arg(in, 2, 1));
        const char *b = string_text(in, arg(in, 2, 0));
        in->sys[0] = (a && b && same_text(a, b)) ? 0 : 1;
        in->command_known[command] = a != NULL && b != NULL;
        return 0;
    }
    case 0x0F2: {   /* 0x004651b0: lstrcatA(dst, src) */
        size_t room = 0, have;
        char *dst = string_buffer(in, arg(in, 2, 1), &room);
        const char *src = string_text(in, arg(in, 2, 0));
        if (dst && src) {
            have = strlen(dst);
            if (have < room) snprintf(dst + have, room - have, "%s", src);
        }
        in->command_known[command] = dst != NULL && src != NULL;
        return 0;
    }
    case 0x0F3: {   /* 0x004651f0: dst = the `count` characters at `start` */
        size_t room = 0;
        char *dst = string_buffer(in, arg(in, 4, 3), &room);
        const char *src = string_text(in, arg(in, 4, 2));
        int at = arg(in, 4, 1), end = at + arg(in, 4, 0), taken = 0;
        size_t put = 0;
        in->sys[0] = 0;
        if (!dst || !src || at < 0 || room == 0) return 0;
        /*
         * The engine's own loop (0x00465243): it steps BYTES from `start` to
         * `start + count` and copies a whole character each time, so a
         * two-byte one costs two of the count. sys[0] counts characters.
         */
        while (at < end && src[at]) {
            unsigned char c = (unsigned char) src[at];
            int len = cmvs_lead_byte(c) && src[at + 1] ? 2 : 1;
            if (put + (size_t) len + 1 > room) break;
            memcpy(dst + put, src + at, (size_t) len);
            put += (size_t) len;
            at += len;
            taken++;
        }
        dst[put] = 0;
        in->sys[0] = taken;
        in->command_known[command] = 1;
        return 0;
    }
    case 0x0F8:     /* 0x00465460: lstrlenA, in bytes */
        {
            const char *text = string_text(in, arg(in, 1, 0));
            in->sys[0] = text ? (int32_t) strlen(text) : 0;
            in->command_known[command] = text != NULL;
        }
        return 0;
    case 0x119: {
        /*
         * 0x00464250 -> 0x004508c0: how wide the line would be on one line,
         * in half-width units. The script asks this before 0x112 for a line
         * long enough to need wrapping and sizes the window from the answer;
         * without it, snky01.ps3's second line got a box one character wide
         * and wrapped itself into ribbons. It takes the +0xbb0 id like every
         * other measurement, and the id is only checked, never used - the
         * count is a property of the text, not of the box it will go in.
         */
        cmvs_text *t = cmvs_scene_text_by_id(in->scene, arg(in, 2, 1));
        const char *text = string_text(in, arg(in, 2, 0));
        if (t && text) {
            in->sys[0] = cmvs_text_span(text);
            in->command_known[command] = 1;
        }
        return 0;
    }
    case 0x112: {
        /* 0x004641c0 -> 0x004509a0: how the string would lay out. The script
         * centres its window on the answer, so a stale accumulator here put
         * the message box and every glyph in it somewhere else entirely.
         * The argument is the +0xbb0 ID, not a layer: 0x15d said which. */
        cmvs_text *t = cmvs_scene_text_by_id(in->scene, arg(in, 2, 1));
        const char *text = string_text(in, arg(in, 2, 0));
        int widest = 0, last = 0;
        if (t && text) {
            in->sys[0] = cmvs_text_measure(t, text, &widest, &last);
            in->sys[1] = widest;
            in->sys[4] = last;
            in->command_known[command] = 1;
        }
        return 0;
    }
    /* --------------------------------------------- registered procedures */
    case 0x088: {   /* 0x004634D0: proc[arg4] = this script at label arg3 */
        int32_t which = arg(in, 5, 4);
        if (which >= 0 && which < 64) {
            in->proc[which].used = 1;
            in->proc[which].slot = in->current;
            in->proc[which].pc = arg(in, 5, 3);
            in->proc[which].a = arg(in, 5, 2);
            in->proc[which].b = arg(in, 5, 1);
            in->proc[which].c = arg(in, 5, 0);
            in->command_known[command] = 1;
        }
        return 0;
    }
    case 0x08A: {
        /*
         * 0x00463560: a CALL, and the one command that moves the pc itself.
         * It steps past its own two bytes, pops the index it was given, and
         * puts the caller slot, the return pc and the current slot in its
         * place - which is exactly the three words statement 0x0414 pops on
         * the way back, so a registered procedure returns like any other.
         * Its ABI byte count is zero and it returns no advance, because by
         * then the pc belongs to the procedure.
         */
        int32_t which = arg(in, 1, 0) & 0x3F;
        if (!in->proc[which].used) {
            /* Nothing registered: step over the call rather than jumping to
             * an address the game never wrote. Without this the pc would sit
             * still and the script would spin for ever. */
            in->pc += 2;
            in->sp -= 4;
            if (in->sp < 0) in->sp = 0;
            return 0;
        }
        in->pc += 2;
        in->sp -= 4;
        if (in->sp < 0) in->sp = 0;
        push(in, in->repeat);
        push(in, in->pc);
        push(in, in->current);
        in->repeat = 0;
        in->pc = in->proc[which].pc;
        if (in->proc[which].slot >= 0 && in->proc[which].slot < MAX_SLOTS
            && in->slot[in->proc[which].slot].loaded)
            in->current = in->proc[which].slot;
        in->command_known[command] = 1;
        return 0;
    }
    case 0x08B: {   /* 0x00463610: forget the procedure again */
        int32_t which = arg(in, 1, 0);
        if (which >= 0 && which < 64) in->proc[which].used = 0;
        in->command_known[command] = 1;
        return 0;
    }
    /* -------------------------------------------------------------- menus */
    case 0x210:   /* 0x004690C0: bind menu arg1 to graphic object arg0 */
        in->command_known[command] = cmvs_menu_bind(in->menus, arg(in, 2, 1), arg(in, 2, 0));
        return 0;
    case 0x211:   /* 0x00469370: throw the menu away */
        cmvs_menu_drop(in->menus, arg(in, 1, 0));
        in->command_known[command] = 1;
        return 0;
    case 0x212:   /* 0x004693D0 -> 0x00453870: one more item, by id */
        in->sys[0] = cmvs_menu_add(in->menus, arg(in, 2, 1), arg(in, 2, 0));
        in->command_known[command] = 1;
        return 0;
    case 0x213:   /* 0x00469420 -> 0x00453930: its hit rectangle */
        cmvs_menu_rect(in->menus, arg(in, 6, 5), arg(in, 6, 4),
                       arg(in, 6, 3), arg(in, 6, 2), arg(in, 6, 1), arg(in, 6, 0));
        in->command_known[command] = 1;
        return 0;
    case 0x214:   /* 0x00469470 -> 0x00453970: one of its four sprite states */
        cmvs_menu_state(in->menus, arg(in, 10, 9), arg(in, 10, 8), arg(in, 10, 7),
                        arg(in, 10, 6), arg(in, 10, 5), arg(in, 10, 4),
                        arg(in, 10, 3), arg(in, 10, 2), arg(in, 10, 1), arg(in, 10, 0));
        in->command_known[command] = 1;
        return 0;
    case 0x215:   /* 0x004694D0 -> 0x00453B40: the menu is built; draw it */
        cmvs_menu_finish(in->menus, arg(in, 1, 0), in->scene);
        in->command_known[command] = 1;
        return 0;
    case 0x216:   /* 0x00469510 -> 0x00453BF0: which item is selected */
        in->sys[0] = cmvs_menu_current(in->menus, arg(in, 1, 0));
        in->command_known[command] = 1;
        return 0;
    case 0x217:   /* 0x00469560 -> 0x00453DC0: the frame's input, as an item */
        in->sys[0] = cmvs_menu_poll(in->menus, arg(in, 1, 0), &in->input, in->scene);
        if (in->sys[0] >= 0) {
            in->menu_events++;
            in->menu_last = in->sys[0];
        }
        in->command_known[command] = 1;
        return 0;
    case 0x081: {
        /*
         * 0x00463410 -> 0x0045b7d0, and it is not a load: it is a load AND A
         * CALL. The routine takes a slot number the script chooses (0, 1 or 2;
         * anything else is refused), loads the script one place higher because
         * slot 0 is the running script that command 0x080 replaces, and then
         * does exactly what 0x08a does - drop its own two arguments, step the
         * pc past itself, push the repeat counter, the return pc and the
         * caller's slot, and jump to the loaded script's header entry at 0x20.
         * The loaded script runs to its own 0x0414 and returns here.
         *
         * That is how ChronoClock's start.ps3 gets its procedures: it loads
         * intproc.ps3 and intcode.ps3, and those two run their own bodies,
         * whose whole purpose is a run of command 0x088 registrations. While
         * this command only loaded, procedures 33 and 34 were never registered,
         * so every 0x08a to them did nothing - which is why the played scene
         * built its message window and its text but never had a background:
         * procedure 33 is what fills the background object.
         *
         * It answers 0 on success (the pc belongs to the loaded script now, so
         * no advance and no pop) and 0x4008 when the load fails, which is why
         * it declares its own return instead of taking the table's.
         */
        int32_t which = arg(in, 2, 1);
        int slot;
        char why[256];
        name = string_text(in, arg(in, 2, 0));
        if (!name || which < 0 || which >= 3)
            return CMVS_CMD_OWN | CMVS_CMD_ADVANCE | 0x08;
        slot = (int) which + 1;
        if (!load_slot(in, slot, name, why, sizeof why)) {
            if (in->trace) fprintf(stderr, "  cannot load %s: %s\n", name, why);
            return CMVS_CMD_OWN | CMVS_CMD_ADVANCE | 0x08;
        }
        in->sp -= 8;
        if (in->sp < 0) in->sp = 0;
        in->pc += 2;
        push(in, in->repeat);
        push(in, in->pc);
        push(in, in->current);
        in->current = slot;
        in->pc = in->slot[slot].script.entry;
        in->command_known[command] = 1;
        return CMVS_CMD_OWN;
    }
    default:
        return 0;
    }
}

/* ------------------------------------------------------------- statements */

static int load_slot(cmvs_interp *in, int slot, const char *name, char *err, size_t errlen)
{
    if (slot < 0 || slot >= MAX_SLOTS) { fail(err, errlen, "script slot out of range"); return 0; }
    if (in->slot[slot].loaded) cmvs_script_close(&in->slot[slot].script);
    memset(&in->slot[slot], 0, sizeof in->slot[slot]);
    if (!cmvs_game_script(in->game, name, &in->slot[slot].script, err, errlen)) return 0;
    in->slot[slot].loaded = 1;
    snprintf(in->slot[slot].name, sizeof in->slot[slot].name, "%s", name);
    return 1;
}

/*
 * Enters a script the way 0x0046EF20 does: over slot 0, at the entry the
 * script's own header carries at 0x20, with an empty stack and no call depth.
 */
static int enter_script(cmvs_interp *in, const char *name, char *err, size_t errlen)
{
    if (!load_slot(in, 0, name, err, errlen)) return 0;
    in->current = 0;
    in->pc = in->slot[0].script.entry;
    in->sp = 0;
    in->depth = 0;
    in->frame[0] = 0;
    in->running = 1;
    in->alive = 1;
    return 1;
}

int cmvs_interp_boot(cmvs_interp *in, const char *script, char *err, size_t errlen)
{
    return enter_script(in, script, err, errlen);
}

const char *cmvs_interp_script(const cmvs_interp *in)
{
    return in->current >= 0 ? in->slot[in->current].name : "(none)";
}

void cmvs_interp_frame_ms(cmvs_interp *in, int ms) { in->frame_ms = ms; }

int cmvs_interp_frame(cmvs_interp *in, long budget, char *err, size_t errlen)
{
    int t;
    if (!in->alive) return 0;
    /* 0x0045A8E0 opens with exactly this: ten timers, each advanced by the
     * frame's elapsed time before a single statement runs. */
    for (t = 0; t < 10; t++) in->timer[t] += in->frame_ms;
    /* And the reveal runs off the same clock: 0x00452c50 adds the frame's own
     * elapsed milliseconds (0x00406a70) before it decides how much of the line
     * is due. */
    cmvs_scene_text_tick(in->scene, in->frame_ms);
    in->running = 1;
    while (in->running && budget-- > 0) {
        int op = word_at(in, in->pc);
        if (op < 0) { in->running = 0; in->alive = 0; continue; }
        in->statements++;
        if (in->trace > 1)
            fprintf(stderr, "%-12s %06x op %04x sp=%-6d depth=%-3d acc=%d\n",
                    in->slot[in->current].name, in->pc, op, in->sp, in->depth, in->acc);

        if (op == 0x0201) {
            int end = 0, set_acc = 0;
            int32_t v = 0;
            if (!run_string_expression(in, in->pc + 2, &end, &set_acc, &v)) {
                fail(err, errlen, "a string expression the interpreter could not evaluate");
                return -1;
            }
            if (set_acc) in->acc = v;   /* 0x00459a0e leaves it alone otherwise */
            in->pc = end;
            continue;
        }
        if (op == 0x0200 || op == 0x0202) {
            int grammar = op == 0x0200 ? CMVS_GRAMMAR_200 : CMVS_GRAMMAR_202;
            int32_t v = 0;
            int end = cmvs_expression_end(code(in), grammar, in->pc + 2);
            if (end < 0 || !eval_expression(in, grammar, in->pc + 2, 0, &v)) {
                fail(err, errlen, "an expression the interpreter could not evaluate");
                return -1;
            }
            in->acc = v;
            in->flag = v != 0;
            in->pc = end;
            continue;
        }
        if (op >= 0x2000 && op <= 0x27FF) {
            int flags = do_command(in, op & 0x7FF);
            /* Bit 0x4000 is the handler saying "step past me". A command that
             * does not set it has moved the pc itself - 0x080 jumps into
             * another script - so advancing here would skip its first word. */
            if (flags & CMVS_CMD_ADVANCE) in->pc += 2;
            continue;
        }
        switch (op) {
        case 0x0400: in->pc = dword_at(in, in->pc + 2); break;
        case 0x0401: in->pc = in->flag ? in->pc + 6 : dword_at(in, in->pc + 2); break;
        case 0x0402: in->pc = in->flag ? dword_at(in, in->pc + 2) : in->pc + 6; break;
        case 0x0403:
            in->pc = in->acc == dword_at(in, in->pc + 2) ? dword_at(in, in->pc + 6) : in->pc + 10;
            break;
        case 0x0405: {
            int idx = word_at(in, in->pc + 2);
            const cmvs_script *s = code(in);
            in->pc = (idx >= 0 && idx < s->index_count) ? (int) s->index[idx] : in->pc + 4;
            break;
        }
        case 0x0407: in->pc = dword_at(in, in->pc + 6); break;
        case 0x0410: {
            int idx = word_at(in, in->pc + 2);
            const cmvs_script *s = code(in);
            push(in, in->pc + 4);
            if (in->depth + 1 < MAX_DEPTH) in->frame[++in->depth] = in->sp;
            in->pc = (idx >= 0 && idx < s->index_count) ? (int) s->index[idx] : in->pc + 4;
            break;
        }
        case 0x0411: {
            int drop = word_at(in, in->pc + 2);
            in->pc = pop(in);
            in->sp -= drop;
            if (in->sp < 4) in->sp = 4;
            if (in->depth > 0) in->depth--;
            break;
        }
        case 0x0412:
            in->sp -= word_at(in, in->pc + 2);
            if (in->sp < 4) in->sp = 4;
            in->pc += 4;
            break;
        case 0x0413:
            in->pc = pop(in);
            in->sp -= 4 * (in->acc & 0xFFFF);
            if (in->sp < 4) in->sp = 4;
            if (in->depth > 0) in->depth--;
            break;
        case 0x0414: {
            /*
             * 0x0045ab2c: the return that pairs with 0x08a. It pops the three
             * words that call left - slot, pc, repeat - and that is ALL it
             * does: it never touches the call depth at +0x13d28, because 0x08a
             * (0x00463560) never raised it. Decrementing here cost a frame
             * level per registered-procedure call, so after the scene's first
             * one every local read came off the wrong frame base and the
             * script's own epilogue returned to a pc it had never pushed.
             */
            int slot = pop(in);
            int back = pop(in);
            in->repeat = pop(in);
            if (slot >= 0 && slot < MAX_SLOTS && in->slot[slot].loaded) in->current = slot;
            in->pc = back;
            break;
        }
        case 0x0416:
            push(in, in->pc + 2);
            if (in->depth + 1 < MAX_DEPTH) in->frame[++in->depth] = in->sp;
            in->pc = in->acc;
            break;
        case 0x0430: push(in, in->acc); in->pc += 2; break;
        case 0x0440: case 0x0442: {
            /*
             * 0x0045abdc and 0x0045ac1a: a local, and NOT a push. It writes the
             * dword at pc+4 into the slot the stack pointer is on and then
             * raises the pointer by the WORD AT pc+2, which is the local's size
             * in bytes - four for a number, but 0x18 for a string buffer, which
             * is what makes 0x127 (a string above the frame) possible. Treating
             * every one of them as four bytes left the frame short by the
             * difference, so the epilogue's own drop unwound past the return
             * address and the pc landed in the middle of an expression. The
             * float form differs only in storing through the FPU, which for a
             * value that is already four bytes is the same four bytes.
             */
            stack_set(in, in->sp, dword_at(in, in->pc + 4));
            in->sp += word_at(in, in->pc + 2);
            if (in->sp > STACK_BYTES - 4) in->sp = STACK_BYTES - 4;
            in->pc += 8;
            break;
        }
        default:
            in->pc += 2;   /* the engine skips any word its tables do not match */
            break;
        }
        if (in->pc < 0 || in->pc >= code(in)->code_size) { in->running = 0; in->alive = 0; }
    }
    return in->alive;
}

int cmvs_interp_unimplemented(const cmvs_interp *in, int *distinct)
{
    int i, total = 0, kinds = 0;
    for (i = 0; i < COMMANDS; i++) {
        if (!in->command_seen[i] || in->command_known[i]) continue;
        total += in->command_seen[i];
        kinds++;
    }
    if (distinct) *distinct = kinds;
    return total;
}

void cmvs_interp_report(const cmvs_interp *in, void *out)
{
    FILE *f = out;
    int i, shown = 0;
    fprintf(f, "%ld statements executed, stopped at %06x in %s\n", in->statements, in->pc,
            in->current >= 0 ? in->slot[in->current].name : "(none)");
    for (i = 0; i < COMMANDS; i++) {
        if (!in->command_seen[i]) continue;
        if (shown++ == 0) fprintf(f, "commands called:\n");
        if (shown <= 40)
            fprintf(f, "  0x%03x %6d  %s\n", i, in->command_seen[i],
                    in->command_known[i] ? "" : "not implemented");
    }
    if (shown > 40) fprintf(f, "  ... %d command slots in all\n", shown);
}
