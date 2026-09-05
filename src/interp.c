#include "interp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "commands.h"
#include "vm.h"

#define MAX_SLOTS      8
#define STACK_BYTES    (256 * 1024)
#define MAX_DEPTH      256
#define GLOBALS        0x4000     /* the int array at 0x596d78 */
#define FLAGS          0x10000    /* the bit array at 0x5bcd80 */
#define SCRIPT_VARS    0x4000     /* the per-script area at +0x3aec */
#define COMMANDS       0x400

typedef struct {
    cmvs_script script;
    int loaded;
    char name[128];
    int32_t vars[SCRIPT_VARS / 4];
} cmvs_slot;

struct cmvs_interp {
    cmvs_game *game;

    cmvs_slot slot[MAX_SLOTS];
    int current;                 /* +0x3390 */
    int pc;                      /* +0x3394 */
    int caller_slot;             /* +0x3398 */

    uint8_t stack[STACK_BYTES];  /* +0x13c20 */
    int sp;                      /* +0x13c24, a byte offset */
    int frame[MAX_DEPTH];        /* +0x13c28 */
    int depth;                   /* +0x13d28 */

    int32_t acc;                 /* +0x13d30 */
    int flag;                    /* +0x13d2c bit 0 */
    int32_t sys[16];             /* +0x13d34 onwards, what token 0x10F reads */

    int32_t globals[GLOBALS];
    uint8_t flags[FLAGS / 8];

    int running;
    int trace;
    char main_script[128];
    int main_started;
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
    return in;
}

void cmvs_interp_free(cmvs_interp *in)
{
    int i;
    if (!in) return;
    for (i = 0; i < MAX_SLOTS; i++)
        if (in->slot[i].loaded) cmvs_script_close(&in->slot[i].script);
    free(in);
}

void cmvs_interp_trace(cmvs_interp *in, int on) { in->trace = on; }
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
    case 0x100: return operand;
    case 0x101: return global_get(in, operand);
    case 0x102: return flag_get(in, operand);
    case 0x103: return stack_get(in, base - operand);
    case 0x104: case 0x105: case 0x106: case 0x107: return script_var(in, operand);
    case 0x108: case 0x109: case 0x10A: case 0x10B: return stack_get(in, base + operand);
    case 0x10E: {
        const cmvs_script *s = code(in);
        if (operand >= 0 && operand < s->index_count) return (int32_t) s->index[operand];
        return 0;
    }
    case 0x10F:
        return (operand >= 0 && operand < (int32_t) (sizeof in->sys / sizeof in->sys[0]))
             ? in->sys[operand] : 0;
    case 0x120: return (int32_t) (CMVS_STRING_TAG | (uint32_t) operand);
    case 0x121: return operand;
    case 0x122: return stack_get(in, stack_get(in, base - operand));
    case 0x129: return stack_get(in, base - operand);
    case 0x12A: return operand;
    case 0x12B: return global_get(in, operand);
    case 0x12C: return stack_get(in, base + operand);
    case 0x12E: return script_var(in, operand);
    default: return operand;
    }
}

/* The store side, from 0x4583b0: the same storage classes, written instead. */
static void assign(cmvs_interp *in, int token, int32_t operand, int32_t value)
{
    int base = in->frame[in->depth];
    switch (token) {
    case 0x101: case 0x12B: global_set(in, operand, value); break;
    case 0x102: flag_set(in, operand, value); break;
    case 0x103: case 0x129: stack_set(in, base - operand, value); break;
    case 0x104: case 0x105: case 0x106: case 0x107:
    case 0x12E: set_script_var(in, operand, value); break;
    case 0x108: case 0x109: case 0x10A: case 0x10B:
    case 0x12C: stack_set(in, base + operand, value); break;
    case 0x122: stack_set(in, stack_get(in, base - operand), value); break;
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

static int run_tokens(cmvs_interp *in, int grammar, int at, int depth, nodes *st, int *end)
{
    for (;;) {
        int token = word_at(in, at), len, nested, next;
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
        cmvs_token_shape(grammar, token, &len, &nested);
        if (len >= 6) operand = dword_at(in, at + 2);
        if (nested) {
            int32_t v = 0;
            if (!eval_expression(in, CMVS_GRAMMAR_200, at + len + 2, depth + 1, &v)) return 0;
            next = cmvs_expression_end(code(in), CMVS_GRAMMAR_200, at + len + 2);
            if (next < 0) return 0;
            operand = v;      /* the nested value IS this node operand */
            at = next;
        } else {
            at += len;
        }
        if (!apply(in, st, token, operand)) return 0;
    }
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

static const char *as_string(cmvs_interp *in, int32_t v)
{
    if (((uint32_t) v & CMVS_STRING_TAG) == 0) return NULL;
    return cmvs_script_string(code(in), (uint32_t) v & ~CMVS_STRING_TAG);
}

/*
 * A command handler returns bit flags, the two that matter being 0x8000 "the
 * script stops here" and 0x4000 "step the pc past me". Command 0x000 returns
 * 0xC000: it is how a script yields back to the engine, which is why the boot
 * script ends on it rather than running off its own end.
 */
static int command_boot(cmvs_interp *in, int command);
static int load_slot(cmvs_interp *in, int slot, const char *name, char *err, size_t errlen);

static void do_command(cmvs_interp *in, int command)
{
    if (command >= 0 && command < COMMANDS) in->command_seen[command]++;
    if (in->trace) {
        const char *s0 = as_string(in, arg(in, 1, 0));
        const char *s1 = as_string(in, arg(in, 2, 0));
        fprintf(stderr, "  %-12s cmd 0x%03x  sp=%-6d acc=%-10d", in->slot[in->current].name,
                command, in->sp, in->acc);
        if (s1) fprintf(stderr, "  [%s] [%s]", s1, s0 ? s0 : "");
        else if (s0) fprintf(stderr, "  [%s]", s0);
        fputc(0x0A, stderr);
    }
    {
        int abi = (command >= 0 && command < CMVS_COMMANDS) ? cmvs_command_abi[command] : -1;
        int flags = command_boot(in, command);
        if (abi < 0) abi = CMVS_CMD_ADVANCE;   /* extractor gap; see commands.c */
        flags |= abi;
        /* The pc step and the argument pop are the engine own bookkeeping at
         * 0x45AC55, done whether or not the command itself is implemented. */
        in->sp -= CMVS_CMD_ARGS(flags);
        if (in->sp < 4) in->sp = 4;
        if (flags & CMVS_CMD_STOP) in->running = 0;
    }
}


/*
 * The commands the boot script uses. Their meaning is not guessed: start.ps3
 * calls each with the path it applies to, so the trace names them - 0x010 to
 * 0x01A mount the archives and folders, 0x080 names the script the engine runs
 * after boot, 0x081 registers a resident script, and 0x000 yields.
 *
 * The mounts are recorded rather than acted on, because cmvs_game already opens
 * every archive under the pack folder; keeping a second mount table would be
 * two mechanisms for one job.
 */
static int command_boot(cmvs_interp *in, int command)
{
    const char *name = as_string(in, arg(in, 1, 0));

    switch (command) {
    case 0x000:
        return CMVS_CMD_STOP;
    case 0x080:
        if (name) snprintf(in->main_script, sizeof in->main_script, "%s", name);
        in->command_known[command] = 1;
        return 0;
    case 0x081: {
        int slot;
        char why[256];
        name = as_string(in, arg(in, 2, 0));
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

int cmvs_interp_boot(cmvs_interp *in, const char *script, char *err, size_t errlen)
{
    if (!load_slot(in, 0, script, err, errlen)) return 0;
    in->current = 0;
    in->pc = in->slot[0].script.index_count > 0 ? (int) in->slot[0].script.index[0] : 0;
    in->sp = 4;
    in->depth = 0;
    in->frame[0] = in->sp;
    in->running = 1;
    return 1;
}

/*
 * start.ps3 ends by yielding, having named the script the engine runs next.
 * That hand-over is the engine outer loop, not something the desktop runner
 * should have to know, so the interpreter does it here.
 */
static int start_main_script(cmvs_interp *in, char *err, size_t errlen)
{
    int slot;
    for (slot = 1; slot < MAX_SLOTS; slot++) if (!in->slot[slot].loaded) break;
    if (slot >= MAX_SLOTS || !load_slot(in, slot, in->main_script, err, errlen)) return 0;
    in->main_started = 1;
    in->current = slot;
    in->pc = in->slot[slot].script.index_count > 0 ? (int) in->slot[slot].script.index[0] : 0;
    in->sp = 4;
    in->depth = 0;
    in->frame[0] = in->sp;
    in->running = 1;
    return 1;
}

int cmvs_interp_step(cmvs_interp *in, long budget, char *err, size_t errlen)
{
    while (budget-- > 0) {
        int op;
        if (!in->running) {
            if (in->main_started || !in->main_script[0]) break;
            if (!start_main_script(in, err, errlen)) return -1;
        }
        op = word_at(in, in->pc);
        if (op < 0) { in->running = 0; continue; }
        in->statements++;
        if (in->trace > 1)
            fprintf(stderr, "%-12s %06x op %04x sp=%-6d depth=%-3d acc=%d\n",
                    in->slot[in->current].name, in->pc, op, in->sp, in->depth, in->acc);

        if (op == 0x0200 || op == 0x0201 || op == 0x0202) {
            int grammar = op == 0x0200 ? CMVS_GRAMMAR_200
                        : op == 0x0201 ? CMVS_GRAMMAR_201 : CMVS_GRAMMAR_202;
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
            do_command(in, op & 0x7FF);
            in->pc += 2;
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
        if (in->pc < 0 || in->pc >= code(in)->code_size) in->running = 0;
    }
    return in->running;
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
