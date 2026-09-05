/*
 * Decoder for the PS3 bytecode.
 *
 * The instruction set was read out of the game's own cmvs32.exe rather than
 * guessed from the stream, because it cannot be guessed: instruction length is
 * per-opcode, and three of the statement opcodes open a nested expression whose
 * tokens live in a different opcode space. A linear sweep that does not know
 * that reads expression operands as instructions and never resynchronises.
 *
 *   statements    the interpreter at 0x0045a8e0
 *     0x0200/0x0201/0x0202   open an expression, closed by 0x020F
 *     0x0400                 unconditional jump
 *     0x0401..0x0442         control flow, 15 handler groups
 *     0x2000..0x27FF         call built-in command (op & 0x7FF), 976 of them
 *   expression tokens   the parser at 0x00458eb0, dispatch at 0x00458f2d
 *     0x0101..0x017E         126 tokens, 17 handler groups
 *     0x0200                 nested expression
 *     anything above 0x0200  the 0x04xx variable family, 6 bytes
 */
#ifndef CMVS_VM_H
#define CMVS_VM_H

#include <stddef.h>
#include <stdint.h>

#include "script.h"

typedef enum {
    CMVS_ST_EXPRESSION,   /* 0x200 / 0x201 / 0x202 */
    CMVS_ST_JUMP,         /* 0x400 */
    CMVS_ST_CONTROL,      /* 0x401..0x442 */
    CMVS_ST_COMMAND,      /* 0x2000..0x27FF */
    CMVS_ST_UNKNOWN
} cmvs_statement_kind;

typedef struct {
    cmvs_statement_kind kind;
    int op;         /* the opcode word */
    int command;    /* for CMVS_ST_COMMAND, op & 0x7FF */
    int length;     /* bytes this statement occupies */
    int strings;    /* string references seen inside its expression */
    int string_shown;       /* how many of the offsets below were recorded */
    uint32_t string_at[8];  /* their string-pool offsets, for a listing */
} cmvs_statement;

/*
 * The three statement opcodes that open an expression each parse it with their
 * own token grammar. The interpreter needs the same tables the decoder uses, so
 * they are shared rather than written twice.
 */
typedef enum { CMVS_GRAMMAR_200, CMVS_GRAMMAR_201, CMVS_GRAMMAR_202 } cmvs_grammar;

/* Length in bytes of one expression token, and whether a nested expression
 * follows it. */
void cmvs_token_shape(cmvs_grammar g, int token, int *len, int *nested);

/* The offset just past the 0x020F that closes the expression whose first token
 * is at `at`, or -1 if the stream runs out. */
int cmvs_expression_end(const cmvs_script *s, cmvs_grammar g, int at);

/* Decodes the statement at `pc`. Returns 0 if it runs off the end. */
int cmvs_decode(const cmvs_script *s, int pc, cmvs_statement *out);

/* Walks a whole script, counting what it finds. Returns 1 if it decoded to the
 * exact end of the bytecode with no unknown opcode. */
typedef struct {
    int statements;
    int commands;
    int expressions;
    int unknown;
    int strings;
    int first_unknown_pc;
    int first_unknown_op;
} cmvs_walk_stats;

int cmvs_walk(const cmvs_script *s, cmvs_walk_stats *stats);

/* Prints a listing of the script to `out`, at most `limit` statements. */
void cmvs_disassemble(const cmvs_script *s, int limit, void *out);

#endif
