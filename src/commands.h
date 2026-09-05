/*
 * The ABI of the engine built-in commands, one entry per slot. See commands.c
 * for where the table comes from and what the bits mean.
 */
#ifndef CMVS_COMMANDS_H
#define CMVS_COMMANDS_H

#define CMVS_COMMANDS 976

#define CMVS_CMD_STOP    0x8000   /* the script stops here */
#define CMVS_CMD_ADVANCE 0x4000   /* step the pc past the two-byte call */
#define CMVS_CMD_ARGS(v) ((v) & 0xFF)   /* bytes of argument to pop */

extern const int cmvs_command_abi[CMVS_COMMANDS];

#endif
