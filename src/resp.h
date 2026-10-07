#ifndef RESP_H
#define RESP_H

#include <stddef.h>

// Maximum number of arguments in a single command (e.g. "SET key value" = 3).
#define RESP_MAX_ARGS 16

// A parsed command. The argument pointers point INTO the caller's buffer - no memory is allocated.
// NOTE: The arguments are NOT null-terminated; use argv_len for their length.
typedef struct {
    int argc;
    char *argv[RESP_MAX_ARGS];
    size_t argv_len[RESP_MAX_ARGS];
} RespCommand;

// Parses one command in RESP format (an array of bulk strings) from the start of 'buf'.
// Returns  1 - a complete command was parsed; '*consumed' is set to the number of bytes it used.
// Returns  0 - the buffer holds only part of a command; call again after more data arrives.
// Returns -1 - protocol error (malformed input).
int resp_parse_command(char *buf, size_t len, RespCommand *cmd, size_t *consumed);

#endif
