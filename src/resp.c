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
