#include "vm.h"

#include <stdio.h>
#include <string.h>

static int word_at(const cmvs_script *s, int at)
{
    if (at < 0 || at + 2 > s->code_size) return -1;
    return s->code[at] | (s->code[at + 1] << 8);
}

static uint32_t dword_at(const cmvs_script *s, int at)
{
    return (uint32_t) s->code[at] | ((uint32_t) s->code[at + 1] << 8)
         | ((uint32_t) s->code[at + 2] << 16) | ((uint32_t) s->code[at + 3] << 24);
}

/*
 * The three statement opcodes that open an expression each parse it with their
 * OWN token grammar, and the grammars differ in both length and shape. Reading
 * one grammar for all three is what desynchronised the earlier decoder.
 *
 *   0x0200  parser 0x458eb0, group table 0x45969C -> handlers 0x459658
 *   0x0201  parser 0x459790, group table 0x459A34 -> handlers 0x459A24
 *   0x0202  parser 0x459AB0, group table 0x45A7E4 -> handlers 0x45A784
 *
 * Each handler advances the pc by a fixed amount, and some of them then call
 * the 0x0200 parser on what follows - those tokens are the assignment targets,
 * so a token can be followed by a whole nested expression. The nested call is
 * made with the pc already past the token and the parser itself skips one more
 * word, so a sub-expression costs 2 bytes of opener plus its tokens plus its
 * own 0x020F.
 */
typedef enum { GRAMMAR_200, GRAMMAR_201, GRAMMAR_202 } cmvs_grammar;

static int in_range(int t, int lo, int hi) { return t >= lo && t <= hi; }

/* Fills the length in bytes and whether a nested expression follows. */
static void token_shape(cmvs_grammar g, int t, int *len, int *nested)
{
    *len = 6;       /* the default handler of all three grammars advances 6 */
    *nested = 0;

    if (g == GRAMMAR_201) {
        /* 0x459844: only 0x121..0x172 are in the table at all. */
        if (t == 0x121) { *len = 2; *nested = 1; return; }   /* 0x4598fb: add eax, 2 */
        if (in_range(t, 0x160, 0x172)) { *len = 2; return; }
        return;
    }

    /* 0x458f55 / 0x459b68: t - 0x101 bounds checked against 0x7D. */
    if (!in_range(t, 0x101, 0x17E)) return;

    if (t == 0x101 || t == 0x102 || t == 0x12B) { *len = 2; *nested = 1; return; }
    if (t == 0x107 || t == 0x10B || t == 0x111 || t == 0x113) { *len = 8; *nested = 1; return; }
    if (t == 0x105 || t == 0x109 || t == 0x110 || t == 0x112
        || t == 0x12D || t == 0x12F) { *nested = 1; return; }
    if (in_range(t, 0x160, 0x17E)) { *len = 2; return; }

    if (g == GRAMMAR_202) {
        /* The float grammar keeps six more assignment targets of its own. */
        if (t == 0x131 || t == 0x133 || t == 0x135 || t == 0x137) { *len = 8; *nested = 1; return; }
        if (t == 0x134 || t == 0x136) { *nested = 1; return; }
    }
}

/*
 * Parses one expression in grammar `g`, from the first token (the caller has
 * already stepped past the opening word). Returns the offset just past the
 * 0x020F terminator, or -1.
 */
typedef struct {
    int count;              /* string operands seen */
    uint32_t at[8];         /* the first few, for a listing */
} cmvs_expr_strings;

static int parse_expression(const cmvs_script *s, cmvs_grammar g, int at, int depth,
                            cmvs_expr_strings *strings)
{
    if (depth > 64) return -1;
    for (;;) {
        int t = word_at(s, at), len, nested;
        if (t < 0) return -1;
        if (t == 0x020F) return at + 2;
        /* 0x458f4f: only the 0x0200 grammar and its float twin open a nested
         * expression on a bare 0x0200; in the 0x0201 grammar it is out of table
         * range and takes the default six bytes. */
        if (t == 0x0200 && g != GRAMMAR_201) {
            at = parse_expression(s, GRAMMAR_200, at + 2, depth + 1, strings);
            if (at < 0) return -1;
            continue;
        }
        if (t == 0x0120 && strings && at + 6 <= s->code_size) {
            if (strings->count < (int) (sizeof strings->at / sizeof strings->at[0]))
                strings->at[strings->count] = dword_at(s, at + 2);
            strings->count++;
        }
        token_shape(g, t, &len, &nested);
        at += len;
        if (nested) {
            at = parse_expression(s, GRAMMAR_200, at + 2, depth + 1, strings);
            if (at < 0) return -1;
        }
        if (at > s->code_size) return -1;
    }
}

/*
 * Fall-through length of each control-flow statement, from the handler table at
 * 0x45acc8. Where a handler sets the pc outright the statement is still this
 * long in the stream, which is what a listing needs.
 */
static int control_length(int op)
{
    switch (op) {
    case 0x401: case 0x402: return 6;
    case 0x403: return 10;
    case 0x405: case 0x410: case 0x411: case 0x412: return 4;
    /* 0x413 and 0x414 are returns: the pc comes off the call stack, so the
     * handler never reveals a fall-through length. The stream always pairs them
     * with an unused word (0x0414 0x0000), which is what the compiler emits. */
    case 0x413: case 0x414: return 4;
    case 0x407: return 10;   /* 0x45aa2c reads a dword at +6 */
    case 0x416: case 0x430: return 2;
    case 0x440: case 0x442: return 8;
    default: return 2;   /* falls through to the command path */
    }
}

int cmvs_decode(const cmvs_script *s, int pc, cmvs_statement *out)
{
    int op = word_at(s, pc);
    if (op < 0) return 0;
    memset(out, 0, sizeof *out);
    out->op = op;

    if (op == 0x0200 || op == 0x0201 || op == 0x0202) {
        cmvs_grammar g = op == 0x0200 ? GRAMMAR_200
                       : op == 0x0201 ? GRAMMAR_201 : GRAMMAR_202;
        cmvs_expr_strings found;
        int end;
        memset(&found, 0, sizeof found);
        end = parse_expression(s, g, pc + 2, 0, &found);
        if (end < 0) return 0;
        out->kind = CMVS_ST_EXPRESSION;
        out->length = end - pc;
        out->strings = found.count;
        memcpy(out->string_at, found.at, sizeof out->string_at);
        out->string_shown = found.count < (int) (sizeof found.at / sizeof found.at[0])
                          ? found.count : (int) (sizeof found.at / sizeof found.at[0]);
        return 1;
    }
    if (op == 0x0400) {
        if (pc + 6 > s->code_size) return 0;
        out->kind = CMVS_ST_JUMP;
        out->length = 6;
        return 1;
    }
    if (op >= 0x0401 && op <= 0x0442) {
        out->kind = CMVS_ST_CONTROL;
        out->length = control_length(op);
        if (pc + out->length > s->code_size) return 0;
        return 1;
    }
    if (op >= 0x2000 && op <= 0x27FF) {
        out->kind = CMVS_ST_COMMAND;
        out->command = op & 0x7FF;
        out->length = 2;
        return 1;
    }
    out->kind = CMVS_ST_UNKNOWN;
    out->length = 2;
    return 1;
}

int cmvs_walk(const cmvs_script *s, cmvs_walk_stats *stats)
{
    int pc = 0;
    memset(stats, 0, sizeof *stats);
    stats->first_unknown_pc = -1;
    while (pc + 2 <= s->code_size) {
        cmvs_statement st;
        if (!cmvs_decode(s, pc, &st)) break;
        stats->statements++;
        stats->strings += st.strings;
        if (st.kind == CMVS_ST_COMMAND) stats->commands++;
        else if (st.kind == CMVS_ST_EXPRESSION) stats->expressions++;
        else if (st.kind == CMVS_ST_UNKNOWN) {
            stats->unknown++;
            if (stats->first_unknown_pc < 0) {
                stats->first_unknown_pc = pc;
                stats->first_unknown_op = st.op;
            }
        }
        if (st.length <= 0) break;
        pc += st.length;
    }
    return pc == s->code_size && stats->unknown == 0;
}

void cmvs_disassemble(const cmvs_script *s, int limit, void *out)
{
    FILE *f = out;
    int pc = 0, n = 0;
    while (pc + 2 <= s->code_size && (limit <= 0 || n < limit)) {
        cmvs_statement st;
        if (!cmvs_decode(s, pc, &st)) break;
        switch (st.kind) {
        case CMVS_ST_COMMAND:
            fprintf(f, "%06x  cmd  0x%03x\n", pc, st.command);
            break;
        case CMVS_ST_JUMP:
            fprintf(f, "%06x  jmp  %06x\n", pc, (int) dword_at(s, pc + 2));
            break;
        case CMVS_ST_CONTROL:
            fprintf(f, "%06x  ctl  0x%03x (%d bytes)\n", pc, st.op, st.length);
            break;
        case CMVS_ST_EXPRESSION: {
            int i;
            fprintf(f, "%06x  expr 0x%03x (%d bytes)", pc, st.op, st.length);
            for (i = 0; i < st.string_shown && i < 3; i++) {
                const char *text = cmvs_script_string(s, st.string_at[i]);
                if (text && *text) fprintf(f, "%s \"%s\"", i ? "," : "  ", text);
            }
            fputc(0x0A, f);
            break;
        }
        default:
            fprintf(f, "%06x  ???  0x%04x\n", pc, st.op);
            break;
        }
        pc += st.length > 0 ? st.length : 2;
        n++;
    }
}
