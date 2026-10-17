#include "resp.h"
#include <ctype.h>

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
