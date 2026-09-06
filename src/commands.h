/*
 * The ABI of the engine built-in commands, one entry per slot. See commands.c
 * for where the table comes from and what the bits mean.
 */
#ifndef CMVS_COMMANDS_H
#define CMVS_COMMANDS_H

#define CMVS_COMMANDS 976

#define CMVS_CMD_STOP    0x8000   /* the script stops here */
#define CMVS_CMD_ADVANCE 0x4000   /* step the pc past the two-byte call */
/*
 * 0x2000: this command is REPEATING. 0x0045AC97 reads it and bumps the
 * counter at +0x3398; any other return resets that counter to zero, and
 * 0x00467037 asks it whether this is the first frame of the wait it is in.
 * A handler that answers with it is declaring its whole return, because
 * the table below holds one constant per slot and a command with two
 * returns (0x153 answers 0xA000 while it waits and 0x400C when the wait
 * is over) could only ever have had one of them extracted.
 */
#define CMVS_CMD_REPEAT  0x2000
#define CMVS_CMD_ARGS(v) ((v) & 0xFF)   /* bytes of argument to pop */

extern const int cmvs_command_abi[CMVS_COMMANDS];

#endif
