#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "queue.h"

// AUTHOR: Claude (this whole file)
// Unit tests for the job queue core. The queue never reads a clock - each test passes the time itself,
// so "30 seconds later" is just a bigger number, and the tests run instantly.

// LIT: Expands a string literal into two arguments - the text and its length.
#define LIT(s) (s), (sizeof(s) - 1)

// Checks that a job's payload is exactly 'expected' (same length, same bytes).
static void assert_payload(const Job *job, const char *expected, size_t expected_len) {
    assert(job != NULL);
    assert(job->len == expected_len);
    assert(memcmp(job->data, expected, expected_len) == 0);
}

// TEST: Jobs come out in the order they went in (FIFO), with increasing ids starting at 1.
static void test_fifo_order(void) {
    JobStore *s = js_create();
    assert(js_push(s, LIT("q"), LIT("first")) == 1);
    assert(js_push(s, LIT("q"), LIT("second")) == 2);
    assert(js_push(s, LIT("q"), LIT("third")) == 3);

    assert_payload(js_reserve(s, LIT("q"), 0, 1000), LIT("first"));
    assert_payload(js_reserve(s, LIT("q"), 0, 1000), LIT("second"));
    assert_payload(js_reserve(s, LIT("q"), 0, 1000), LIT("third"));
    assert(js_reserve(s, LIT("q"), 0, 1000) == NULL);       // empty now
    js_destroy(s);
}

// TEST: Reserving from a queue that was never pushed to returns NULL (and doesn't create it).
static void test_unknown_queue(void) {
    JobStore *s = js_create();
    assert(js_reserve(s, LIT("nothing-here"), 0, 1000) == NULL);
    QueueStats st;
    js_stats(s, LIT("nothing-here"), &st);
    assert(st.ready == 0 && st.reserved == 0 && st.dead == 0 && st.acked == 0);
    js_destroy(s);
}

// TEST: Queues are independent - a job pushed to one is never reserved from another.
static void test_queues_are_separate(void) {
    JobStore *s = js_create();
    js_push(s, LIT("proteins"), LIT("P1"));
    js_push(s, LIT("images"), LIT("I1"));

    assert_payload(js_reserve(s, LIT("images"), 0, 1000), LIT("I1"));
    assert(js_reserve(s, LIT("images"), 0, 1000) == NULL);
    assert_payload(js_reserve(s, LIT("proteins"), 0, 1000), LIT("P1"));
    js_destroy(s);
}

// TEST: Payloads are binary-safe - a null byte in the middle is kept.
static void test_binary_payload(void) {
    JobStore *s = js_create();
    js_push(s, LIT("q"), LIT("a\0b"));
    assert_payload(js_reserve(s, LIT("q"), 0, 1000), LIT("a\0b"));
    js_destroy(s);
}

// TEST: ACK deletes a reserved job; a second ACK of the same id finds nothing.
static void test_ack(void) {
    JobStore *s = js_create();
    uint64_t id = js_push(s, LIT("q"), LIT("job"));
    Job *job = js_reserve(s, LIT("q"), 0, 1000);
    assert(job->id == id);

    assert(js_ack(s, id) == 1);
    assert(js_ack(s, id) == 0);         // already gone
    assert(js_ack(s, 999) == 0);        // never existed

    QueueStats st;
    js_stats(s, LIT("q"), &st);
    assert(st.ready == 0 && st.reserved == 0 && st.acked == 1);
    assert(s->reserved_total == 0);
    js_destroy(s);
}

// TEST: A reserved job is NOT handed to a second worker before its deadline, and comes back after it.
static void test_visibility_timeout(void) {
    JobStore *s = js_create();
    js_push(s, LIT("q"), LIT("job"));
    Job *job = js_reserve(s, LIT("q"), 0, 30000);           // reserved at t=0 for 30 s
    uint64_t id = job->id;
    assert(job->attempts == 1);

    assert(js_expire(s, 29999) == 0);                       // 1 ms before the deadline: still reserved
    assert(js_reserve(s, LIT("q"), 29999, 30000) == NULL);  // a second worker gets nothing

    assert(js_expire(s, 30000) == 1);                       // deadline reached: back to ready
    job = js_reserve(s, LIT("q"), 30000, 30000);            // another worker gets the SAME job
    assert(job != NULL && job->id == id);
    assert(job->attempts == 2);
    js_destroy(s);
}

// TEST: An expired job goes to the END of the ready list, behind jobs that were already waiting.
static void test_expired_job_goes_to_the_back(void) {
    JobStore *s = js_create();
    js_push(s, LIT("q"), LIT("A"));
    js_push(s, LIT("q"), LIT("B"));
    js_reserve(s, LIT("q"), 0, 1000);                       // A reserved, B waiting
    js_expire(s, 1000);                                     // A comes back

    assert_payload(js_reserve(s, LIT("q"), 1000, 1000), LIT("B"));
    assert_payload(js_reserve(s, LIT("q"), 1000, 1000), LIT("A"));
    js_destroy(s);
}

// TEST: A job that times out QUEUE_MAX_ATTEMPTS times goes to DEAD and is not handed out again.
static void test_poison_job_goes_dead(void) {
    JobStore *s = js_create();
    js_push(s, LIT("q"), LIT("poison"));
    long long now = 0;
    for (int attempt = 1; attempt <= QUEUE_MAX_ATTEMPTS; attempt++) {
        Job *job = js_reserve(s, LIT("q"), now, 1000);
        assert(job != NULL && job->attempts == attempt);
        now += 1000;
        assert(js_expire(s, now) == 1);                     // the worker "crashed" again
    }
    assert(js_reserve(s, LIT("q"), now, 1000) == NULL);     // never handed out again

    QueueStats st;
    js_stats(s, LIT("q"), &st);
    assert(st.ready == 0 && st.reserved == 0 && st.dead == 1);
    js_destroy(s);
}

// TEST: A slow worker that ACKs AFTER its job expired and went back to ready still finishes it -
// the job is deleted from the ready list, so nobody processes it again.
static void test_late_ack(void) {
    JobStore *s = js_create();
    uint64_t id = js_push(s, LIT("q"), LIT("slow"));
    js_reserve(s, LIT("q"), 0, 1000);
    js_expire(s, 1000);                                     // back in ready

    assert(js_ack(s, id) == 1);
    assert(js_reserve(s, LIT("q"), 1000, 1000) == NULL);
    js_destroy(s);
}

// TEST: ACK from the MIDDLE of the reserved list keeps the list intact (the doubly linked removal).
static void test_ack_from_middle(void) {
    JobStore *s = js_create();
    js_push(s, LIT("q"), LIT("A"));
    uint64_t b = js_push(s, LIT("q"), LIT("B"));
    js_push(s, LIT("q"), LIT("C"));
    js_reserve(s, LIT("q"), 0, 1000);
    js_reserve(s, LIT("q"), 0, 1000);
    js_reserve(s, LIT("q"), 0, 1000);                       // reserved list: A B C

    assert(js_ack(s, b) == 1);                              // remove B from the middle
    assert(js_expire(s, 1000) == 2);                        // A and C still there, both come back
    assert_payload(js_reserve(s, LIT("q"), 1000, 1000), LIT("A"));
    assert_payload(js_reserve(s, LIT("q"), 1000, 1000), LIT("C"));
    js_destroy(s);
}

// TEST: Many jobs whose ids share hash buckets (more jobs than buckets) are all found by ACK.
static void test_many_jobs(void) {
    JobStore *s = js_create();
    int n = JOB_BUCKETS * 3;
    for (int i = 0; i < n; i++) {
        js_push(s, LIT("q"), LIT("x"));
    }
    for (int i = 0; i < n; i++) {
        assert(js_reserve(s, LIT("q"), 0, 1000) != NULL);
    }
    for (uint64_t id = 1; id <= (uint64_t)n; id++) {
        assert(js_ack(s, id) == 1);
    }
    QueueStats st;
    js_stats(s, LIT("q"), &st);
    assert(st.acked == (unsigned long)n && st.reserved == 0);
    js_destroy(s);
}

typedef struct {
    const char *name;
    void (*fn)(void);
} TestCase;

static const TestCase tests[] = {
    {"fifo_order",                 test_fifo_order},
    {"unknown_queue",              test_unknown_queue},
    {"queues_are_separate",        test_queues_are_separate},
    {"binary_payload",             test_binary_payload},
    {"ack",                        test_ack},
    {"visibility_timeout",         test_visibility_timeout},
    {"expired_job_goes_to_the_back", test_expired_job_goes_to_the_back},
    {"poison_job_goes_dead",       test_poison_job_goes_dead},
    {"late_ack",                   test_late_ack},
    {"ack_from_middle",            test_ack_from_middle},
    {"many_jobs",                  test_many_jobs},
};

// Runs all tests, or only the one named on the command line: ./build/test_queue visibility_timeout
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

    printf("All %d queue tests passed\n", ran);
    return 0;
}
