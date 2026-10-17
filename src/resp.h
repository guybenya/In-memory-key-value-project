#ifndef RESP_H
#define RESP_H

#include <stddef.h>

// Maximum number of arguments in a single command (e.g. "SET key value" = 3).
#define RESP_MAX_ARGS 16

// Maximum length in bytes of a single argument (1 MB). Protects the server from a client that
// announces a huge length (e.g. "$999999999") and makes us wait for gigabytes that never arrive.
#define RESP_MAX_BULK_LEN (1024 * 1024)

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

// A growing byte buffer that replies are written into (like Java's StringBuilder).
// Start with an all-zero buffer:  RespBuf out = {0};   (data = NULL, len = 0, cap = 0)
// OWNER: whoever declares it must call resp_buf_free() when done.
typedef struct {
    char *data;   // heap memory, or NULL before the first append
    size_t len;   // bytes in use
    size_t cap;   // bytes allocated
} RespBuf;

// Appends 'len' bytes to the buffer, growing it if needed.
// Returns 0 on success, -1 if memory ran out (the buffer and its existing contents stay valid).
int resp_buf_append(RespBuf *b, const char *data, size_t len);

// Frees the buffer's memory and resets it to empty. Safe to call on an empty buffer.
void resp_buf_free(RespBuf *b);

// REPLIES: each function appends one RESP reply to 'b'. All return 0 on success, -1 if memory ran out.
int resp_write_simple(RespBuf *b, const char *s);                // +OK\r\n
int resp_write_error(RespBuf *b, const char *msg);               // -ERR <msg>\r\n
int resp_write_bulk(RespBuf *b, const char *data, size_t len);   // $<len>\r\n<data>\r\n
int resp_write_null(RespBuf *b);                                 // $-1\r\n
int resp_write_integer(RespBuf *b, long n);                      // :<n>\r\n

#endif
