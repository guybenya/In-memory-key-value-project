#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "resp.h"

// LIT: Expands a string literal into two arguments - the text and its length (without the hidden '\0').
// Needed because RESP data can contain '\0' and "\r\n", so strlen() can't be trusted for lengths.
#define LIT(s) (s), (sizeof(s) - 1)

// EXACT-SIZE BUFFER: Copies 'len' bytes into a heap block of exactly 'len' bytes.
// With ASan, reading even ONE byte past the end crashes the test - a string literal would hide
// that bug, because its hidden '\0' makes a one-byte overread look harmless.
// OWNER: the caller must free() the result.
static char *copy_exact(const char *data, size_t len) {
    char *buf = malloc(len > 0 ? len : 1);  // malloc(0) may return NULL - always ask for at least 1 byte
    assert(buf != NULL);
    memcpy(buf, data, len);
    return buf;
}

// Parses 'len' bytes of 'data' from an exact-size buffer. The buffer is returned through 'buf_out'
// so the test can still read cmd->argv (which points INTO it) - the test must free() it afterwards.
static int parse(const char *data, size_t len, RespCommand *cmd, size_t *consumed, char **buf_out) {
    *buf_out = copy_exact(data, len);
    return resp_parse_command(*buf_out, len, cmd, consumed);
}

// Checks that argument 'i' is exactly 'expected' - same length and same bytes.
// memcmp, not strcmp: the arguments are NOT null-terminated.
static void assert_arg(const RespCommand *cmd, int i, const char *expected, size_t expected_len) {
    assert(cmd->argv_len[i] == expected_len);
    assert(memcmp(cmd->argv[i], expected, expected_len) == 0);
}

// TEST: A normal command is split into its arguments, and the whole input is consumed.
static void test_parse_set(void) {
    const char input[] = "*3\r\n$3\r\nSET\r\n$4\r\nname\r\n$3\r\nguy\r\n";
    RespCommand cmd;
    size_t consumed;
    char *buf;

    assert(parse(LIT(input), &cmd, &consumed, &buf) == 1);
    assert(cmd.argc == 3);
    assert_arg(&cmd, 0, LIT("SET"));
    assert_arg(&cmd, 1, LIT("name"));
    assert_arg(&cmd, 2, LIT("guy"));
    assert(consumed == sizeof(input) - 1);

    free(buf);
}

// TEST: "$0" is a valid, empty argument (e.g. SET key "").
static void test_empty_argument(void) {
    RespCommand cmd;
    size_t consumed;
    char *buf;

    assert(parse(LIT("*1\r\n$0\r\n\r\n"), &cmd, &consumed, &buf) == 1);
    assert(cmd.argc == 1);
    assert(cmd.argv_len[0] == 0);
    assert(consumed == 10);

    free(buf);
}

// TEST: Arguments are found by their LENGTH, not by searching for separators -
// so a value may contain spaces and even "\r\n" (the old text protocol cut "hello world" to "hello").
static void test_binary_safe(void) {
    RespCommand cmd;
    size_t consumed;
    char *buf;

    // "hello world\r\n!" is 14 bytes, and contains a space and a "\r\n".
    assert(parse(LIT("*2\r\n$3\r\nSET\r\n$14\r\nhello world\r\n!\r\n"), &cmd, &consumed, &buf) == 1);
    assert(cmd.argc == 2);
    assert_arg(&cmd, 1, LIT("hello world\r\n!"));

    free(buf);
}

// TEST: TCP may deliver a command in pieces. Cut a valid command after EVERY possible byte:
// every prefix must return 0 ("incomplete") and must not touch 'consumed'. Only the full input returns 1.
static void test_every_split_point(void) {
    const char input[] = "*3\r\n$3\r\nSET\r\n$4\r\nname\r\n$3\r\nguy\r\n";
    size_t total = sizeof(input) - 1;
    RespCommand cmd;
    char *buf;

    for (size_t cut = 0; cut < total; cut++) {
        size_t consumed = 12345;  // sentinel: must survive unchanged
        assert(parse(input, cut, &cmd, &consumed, &buf) == 0);
        assert(consumed == 12345);
        free(buf);
    }

    size_t consumed;
    assert(parse(input, total, &cmd, &consumed, &buf) == 1);
    assert(consumed == total);
    free(buf);
}

// TEST: Two commands in one buffer (pipelining). The first parse must consume EXACTLY the first
// command, and parsing again from that point must find the second one.
static void test_pipelined(void) {
    const char first[] = "*1\r\n$4\r\nPING\r\n";
    const char both[]  = "*1\r\n$4\r\nPING\r\n*2\r\n$3\r\nGET\r\n$1\r\na\r\n";
    size_t total = sizeof(both) - 1;
    RespCommand cmd;
    size_t consumed;
    char *buf;

    assert(parse(LIT(both), &cmd, &consumed, &buf) == 1);
    assert(cmd.argc == 1);
    assert_arg(&cmd, 0, LIT("PING"));
    assert(consumed == sizeof(first) - 1);

    size_t offset = consumed;
    assert(resp_parse_command(buf + offset, total - offset, &cmd, &consumed) == 1);
    assert(cmd.argc == 2);
    assert_arg(&cmd, 0, LIT("GET"));
    assert_arg(&cmd, 1, LIT("a"));
    assert(offset + consumed == total);

    free(buf);
}

// TEST: Malformed input returns -1 (and NOT 0 - waiting for more bytes would never fix it).
static void test_malformed(void) {
    const char *bad[] = {
        "+OK\r\n",                  // wrong prefix for a command (must start with '*')
        "*\r\n",                    // array header with no number
        "*0\r\n",                   // empty command
        "*-1\r\n",                  // negative count
        "*1\n",                     // '\n' without '\r'
        "*1\r\n:3\r\nabc\r\n",      // argument header must start with '$'
        "*1\r\n$-1\r\n",            // negative length (Redis "null" - not valid in a command)
        "*1\r\n$3x\r\nabc\r\n",     // non-digit inside a length
        "*1\r\n$3\r\nabcXY",        // data not followed by "\r\n"
        "*1\r\n$3\r\nabc\rX",       // '\r' followed by the wrong byte
    };
    RespCommand cmd;
    size_t consumed;
    char *buf;

    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        int rc = parse(bad[i], strlen(bad[i]), &cmd, &consumed, &buf);
        if (rc != -1) {
            fprintf(stderr, "  malformed input #%zu returned %d, expected -1\n", i, rc);
        }
        assert(rc == -1);
        free(buf);
    }
}

// TEST: The limits are exact - the maximum is accepted, one more is rejected.
static void test_limits(void) {
    RespCommand cmd;
    size_t consumed;
    char *buf;
    char input[256];

    // RESP_MAX_ARGS arguments -> OK.
    int len = snprintf(input, sizeof(input), "*%d\r\n", RESP_MAX_ARGS);
    for (int i = 0; i < RESP_MAX_ARGS; i++) {
        len += snprintf(input + len, sizeof(input) - len, "$1\r\nx\r\n");
    }
    assert(parse(input, (size_t)len, &cmd, &consumed, &buf) == 1);
    assert(cmd.argc == RESP_MAX_ARGS);
    free(buf);

    // RESP_MAX_ARGS + 1 -> rejected as soon as the header is read.
    len = snprintf(input, sizeof(input), "*%d\r\n", RESP_MAX_ARGS + 1);
    assert(parse(input, (size_t)len, &cmd, &consumed, &buf) == -1);
    free(buf);

    // A length of exactly RESP_MAX_BULK_LEN is accepted: the header is fine, the data hasn't arrived -> 0.
    len = snprintf(input, sizeof(input), "*1\r\n$%d\r\n", RESP_MAX_BULK_LEN);
    assert(parse(input, (size_t)len, &cmd, &consumed, &buf) == 0);
    free(buf);

    // One byte more -> rejected immediately, without waiting for a megabyte of data.
    len = snprintf(input, sizeof(input), "*1\r\n$%d\r\n", RESP_MAX_BULK_LEN + 1);
    assert(parse(input, (size_t)len, &cmd, &consumed, &buf) == -1);
    free(buf);
}

typedef struct {
    const char *name;
    void (*fn)(void);
} TestCase;

static const TestCase tests[] = {
    {"parse_set",          test_parse_set},
    {"empty_argument",     test_empty_argument},
    {"binary_safe",        test_binary_safe},
    {"every_split_point",  test_every_split_point},
    {"pipelined",          test_pipelined},
    {"malformed",          test_malformed},
    {"limits",             test_limits},
};

// USAGE: ./build/test_resp             -> runs all tests
//        ./build/test_resp malformed   -> runs only the test with that name
int main(int argc, char *argv[]) {
    size_t count = sizeof(tests) / sizeof(tests[0]);
    const char *only = (argc > 1) ? argv[1] : NULL;
    int ran = 0;

    for (size_t i = 0; i < count; i++) {
        if (only != NULL && strcmp(only, tests[i].name) != 0) {
            continue;
        }
        printf("RUN  %s\n", tests[i].name);
        tests[i].fn();
        ran++;
    }

    if (ran == 0) {
        fprintf(stderr, "No test named '%s'\n", only);
        return 1;
    }

    printf("All %d RESP tests passed\n", ran);
    return 0;
}
