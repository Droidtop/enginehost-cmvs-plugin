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
    int caller_slot;             /* +0x3398 */

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
     * The four string operands evaluate to NOTHING. Each appends its text to
     * the accumulator and falls into `mov eax, edi` with edi zeroed at the top
     * of the resolver, which is how concatenation is expressed without a value
     * ever carrying the text.
     */
    case 0x120: case 0x121: case 0x125: case 0x127:
        string_append(in, string_text(in, string_handle(in, token, operand)));
        return 0;
    case 0x122: return stack_get(in, stack_get(in, base - operand));
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
        flags |= abi;
        /* The argument pop is the engine's own bookkeeping at 0x45AC55, done
         * whether or not the command itself is implemented. */
        in->sp -= CMVS_CMD_ARGS(flags);
        if (in->sp < 0) in->sp = 0;
        if (flags & CMVS_CMD_STOP) in->running = 0;
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
    case 0x022:   /* 0x00433cb0: a fresh part, an object of the same class */
        in->command_known[command] =
            cmvs_scene_part(in->scene, arg(in, 2, 1), arg(in, 2, 0));
        return 0;
    case 0x030:   /* 0x0045ee10: decode a PB3 into the object (0x420840) */
        in->sys[0] = cmvs_scene_bitmap(in->scene, arg(in, 2, 1),
                                       string_text(in, arg(in, 2, 0)));
        in->command_known[command] = 1;
        return 0;
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
    case 0x047:   /* 0x0045fb10 -> 0x41bda0: one size */
        cmvs_scene_size(in->scene, arg(in, 3, 2), arg(in, 3, 1), arg(in, 3, 0));
        in->command_known[command] = 1;
        return 0;
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
        push(in, in->caller_slot);
        push(in, in->pc);
        push(in, in->current);
        in->caller_slot = 0;
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
        int slot;
        char why[256];
        name = string_text(in, arg(in, 2, 0));
        if (!name) return 0;
        for (slot = 1; slot < MAX_SLOTS; slot++) if (!in->slot[slot].loaded) break;
        if (slot < MAX_SLOTS && load_slot(in, slot, name, why, sizeof why)) {
            in->command_known[command] = 1;
            in->acc = slot;
        }
        return 0;
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
            int slot = pop(in);
            int back = pop(in);
            in->caller_slot = pop(in);
            if (slot >= 0 && slot < MAX_SLOTS && in->slot[slot].loaded) in->current = slot;
            in->pc = back;
            if (in->depth > 0) in->depth--;
            break;
        }
        case 0x0416:
            push(in, in->pc + 2);
            if (in->depth + 1 < MAX_DEPTH) in->frame[++in->depth] = in->sp;
            in->pc = in->acc;
            break;
        case 0x0430: push(in, in->acc); in->pc += 2; break;
        case 0x0440: case 0x0442: push(in, dword_at(in, in->pc + 4)); in->pc += 8; break;
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
