#include "resp.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Reads one header line - "*<number>\r\n" or "$<number>\r\n" - starting at *pos.
// 'static' = visible only inside this file (like 'private' in Java). It's a helper for the parser below.
// Returns  1 - success: the number is stored in *out, and *pos is moved to just after the '\n'.
// Returns  0 - the line is not complete yet (the buffer ends before "\r\n").
// Returns -1 - malformed line, or the number is bigger than 'max'.
static int parse_header_line(char **pos, const char *end, char prefix, long max, long *out) {
    char *p = *pos;
    // STEP 1: Make sure there is at least one byte, and that it is 'prefix'.
    if (p > end) {
        return -1;
    }
    if (p == end) {
        return 0;
    }
    if (*p != prefix) {
        return -1;
    }
    p++;
    // STEP 2: Read the digits and build the number. Check the limit after every digit.
    long value = 0;
    // Remember where the digits start - step 3 compares it with 'p' to see if any digits were read.
    char *digits_start = p;
    // The loop STOPS at the first non-digit (normally '\r') and leaves 'p' pointing at it.
    // It doesn't judge that byte - step 4 decides whether it is valid.
    while (p < end && isdigit((unsigned char) *p)) {
        value = value * 10 + (*p - '0');
        if (value > max) {
            return -1;
        }
        p++;
    }
    // STEP 3: Require at least one digit.
    if (p == digits_start) {
        // No digits were read
        // ran out of bytes  -> incomplete -> return 0
        if (p == end) {
            return 0;
        }
        //hit a non-digit -> malformed -> return -1
        else {
            return -1;
        }
    }

    // STEP 4: Require "\r\n" after the digits (incomplete -> 0, wrong byte -> -1).
    if (p < end) {
        if (*p == '\r') {
            p++;
            if (p < end) {
                if (!(*p == '\n')) {return -1;}
            }
            // p == end
            else { return 0;}
        }
        else {return -1;}
    }
    // p == end
    else {return 0;}
    // Continue to step 5
    p++;

    // STEP 5: Success - store the number, move the caller's position forward.
    *out = value;
    *pos = p;
    return 1;
}

// Parses one command: an array of bulk strings, e.g. "*2\r\n$3\r\nGET\r\n$4\r\nname\r\n".
// (Documented in resp.h. The argv pointers point INTO 'buf' - nothing is copied or allocated.)
int resp_parse_command(char *buf, size_t len, RespCommand *cmd, size_t *consumed) {
    // STEP 1: Set up 'p' (current position) and 'end' (one past the last byte).
    char *p = buf;
    char *end = buf + len;

    // STEP 2: Read the array header "*<argc>\r\n". Pass 0 / -1 up. Require argc >= 1.
    long count;
    int val = parse_header_line(&p, end, '*', RESP_MAX_ARGS, &count);
    if (val != 1) {
        return val;
    }
    if (count < 1) {
        return -1;
    }
    // STEP 3: For each argument (there are 'count' of them):
    for (long i = 0; i < count; i++) {
        // 3.1 Read its header "$<length>\r\n" into 'arg_len'. Pass 0 / -1 up.
        //     (Same call as step 2 - only the prefix, the max and the target variable change.)
        long arg_len;
        int arg_val = parse_header_line(&p, end, '$', RESP_MAX_BULK_LEN, &arg_len);
        if (arg_val != 1) {
            return arg_val;
        }
        // 3.2 Not enough bytes for the data + "\r\n" yet -> 0.
        if ((end - p) < arg_len + 2) {
            return 0;
        }
        // 3.3 Record the argument: cmd->argv[i] = where the data starts, cmd->argv_len[i] = its length.
        cmd->argv[i] = p;
        cmd->argv_len[i] = arg_len;
        // 3.4 Skip over the data.
        p = p + arg_len;
        // 3.5 Require "\r\n" after the data (-1 if not), then skip it.
        // No 'p < end' checks needed: step 3.2 already guaranteed these two bytes exist.
        if (p[0] != '\r' || p[1] != '\n') {
            return -1;
        }
        p += 2;
    }
    // STEP 4: Success - fill in argc and how many bytes were consumed.
    cmd->argc = (int) count;
    *consumed = p - buf;
    return 1;
}

// ---------------------------------------------------------------------------------------------
// REPLY BUFFER
// ---------------------------------------------------------------------------------------------

// (Documented in resp.h.)
int resp_buf_append(RespBuf *b, const char *data, size_t len) {
    // STEP 1: If the new bytes already fit (len + used <= capacity), skip to step 4.
    if (b->len + len > b->cap) {
    // STEP 2: Pick the new capacity: start from the current one (or 64 if it is 0),
    // and double it until the new bytes fit.
        size_t new_cap = b->cap;
        // Keep doubling while (bytes already used + new bytes) still don't fit in the candidate capacity.
        while (b->len + len > new_cap) {
            if (new_cap == 0) {
                new_cap = 64;
            }
            else {
                new_cap = new_cap * 2;
            }
        }
    // STEP 3: Grow with realloc. If it fails, return -1 WITHOUT losing the old data.
        char *new_data = realloc(b->data, new_cap);
        if (new_data == NULL) {
            return -1;
        }
        b->data = new_data;
        b->cap = new_cap;
    } 
    // STEP 4: Copy the new bytes to the end of the used part, and update the length.
    // The 'len > 0' guard matters for a brand-new buffer: appending 0 bytes skips the growth above,
    // so b->data is still NULL - and passing NULL to memcpy is undefined behavior even for 0 bytes.
    if (len > 0) {
        memcpy(b->data + b->len, data, len);
        b->len += len;
    }
    return 0;
}

void resp_buf_free(RespBuf *b) {
    free(b->data);      // free(NULL) does nothing, so an empty buffer is fine
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
}

// ---------------------------------------------------------------------------------------------
// REPLIES - each one is a type prefix, a payload and "\r\n", appended in pieces.
// If any piece fails (out of memory), stop and return -1; the caller will drop the connection.
// ---------------------------------------------------------------------------------------------

int resp_write_simple(RespBuf *b, const char *s) {
    if (resp_buf_append(b, "+", 1) < 0) return -1;
    if (resp_buf_append(b, s, strlen(s)) < 0) return -1;
    return resp_buf_append(b, "\r\n", 2);
}

int resp_write_error(RespBuf *b, const char *msg) {
    // "ERR" is the generic error code Redis clients expect at the start of an error message.
    if (resp_buf_append(b, "-ERR ", 5) < 0) return -1;
    if (resp_buf_append(b, msg, strlen(msg)) < 0) return -1;
    return resp_buf_append(b, "\r\n", 2);
}

int resp_write_bulk(RespBuf *b, const char *data, size_t len) {
    // The header "$<len>\r\n" is formatted into a small stack buffer first.
    // 32 bytes is plenty: the longest size_t has 20 digits.
    char header[32];
    int n = snprintf(header, sizeof(header), "$%zu\r\n", len);
    if (resp_buf_append(b, header, (size_t)n) < 0) return -1;
    if (resp_buf_append(b, data, len) < 0) return -1;
    return resp_buf_append(b, "\r\n", 2);
}

int resp_write_null(RespBuf *b) {
    return resp_buf_append(b, "$-1\r\n", 5);
}

int resp_write_integer(RespBuf *b, long n) {
    char line[32];
    int written = snprintf(line, sizeof(line), ":%ld\r\n", n);
    return resp_buf_append(b, line, (size_t)written);
}

// AUTHOR: Claude
// Writes the header of an array reply. An array is a count followed by that many ordinary replies,
// e.g. RESERVE answers *3 then :<id> $<payload> :<attempts>.
int resp_write_array_header(RespBuf *b, long count) {
    char line[32];
    int written = snprintf(line, sizeof(line), "*%ld\r\n", count);
    return resp_buf_append(b, line, (size_t)written);
}
