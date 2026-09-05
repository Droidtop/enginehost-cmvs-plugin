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
 * How far the expression parser advances over one token. Read off the handler
 * table at 0x459658: the groups that take a 32-bit operand advance 6, the two
 * that take a word plus a dword advance 8, and the bare operator tokens 2. The
 * fallback handler at 0x4595a3 - which also serves every token above 0x200,
 * that is the 0x04xx variable family - advances 6.
 */
static int token_length(int t)
{
    if (t == 0x101 || t == 0x102 || t == 0x12B) return 2;    /* g0  */
    if (t == 0x107 || t == 0x10B || t == 0x111 || t == 0x113) return 8;  /* g2 g4 g6 g8 */
    if (t == 0x105 || t == 0x109 || t == 0x110 || t == 0x112
        || t == 0x12A || t == 0x12D || t == 0x12F) return 6; /* g1 g3 g5 g7 g9 g10 g11 */
    if (t >= 0x160 && t <= 0x17E) return 2;                  /* g12..g15 operators */
    return 6;                                                /* g16 and the 0x04xx family */
}

/* Parses one expression, which the caller has already stepped past the opening
 * word of. Returns the offset just past its 0x020F terminator, or -1. */
static int parse_expression(const cmvs_script *s, int at, int depth, int *strings)
{
    if (depth > 32) return -1;
    for (;;) {
        int t = word_at(s, at);
        if (t < 0) return -1;
        if (t == 0x020F) return at + 2;
        if (t == 0x0200 || t == 0x0201 || t == 0x0202) {
            at = parse_expression(s, at + 2, depth + 1, strings);
            if (at < 0) return -1;
            continue;
        }
        if (t == 0x0120 && strings) (*strings)++;
        at += token_length(t);
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
    case 0x405: case 0x407: case 0x410: case 0x411: case 0x412: return 4;
    case 0x413: case 0x414: case 0x416: case 0x430: return 2;
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
        int end = parse_expression(s, pc + 2, 0, &out->strings);
        if (end < 0) return 0;
        out->kind = CMVS_ST_EXPRESSION;
        out->length = end - pc;
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
            /* Show the strings the expression names; that is the readable part. */
            int at = pc + 2, shown = 0;
            fprintf(f, "%06x  expr 0x%03x (%d bytes)", pc, st.op, st.length);
            while (at < pc + st.length - 2) {
                int t = word_at(s, at);
                if (t < 0) break;
                if (t == 0x0120 && at + 6 <= s->code_size) {
                    const char *text = cmvs_script_string(s, dword_at(s, at + 2));
                    if (text && *text && shown < 3) {
                        fprintf(f, "%s \"%s\"", shown ? "," : "  ", text);
                        shown++;
                    }
                }
                if (t == 0x0200 || t == 0x0201 || t == 0x0202) at += 2;
                else at += token_length(t);
            }
            fputc('\n', f);
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
