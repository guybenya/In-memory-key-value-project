#ifndef QUEUE_H
#define QUEUE_H

#include <stddef.h>
#include <stdint.h>

// AUTHOR: Claude (this whole file)
//
// JOB QUEUE CORE - the queue logic, with NO network code: no sockets, no RESP.
// That separation is what makes it unit-testable (tests/test_queue.c), exactly like process_input.
//
// Life of a job:
//   push     -> READY     (waits in its queue, first in first out)
//   reserve  -> RESERVED  (a worker is processing it; it has a deadline = the visibility timeout)
//   ack      -> deleted   (the worker finished it)
//   deadline passes without ack -> back to READY (the worker probably crashed),
//                                  or DEAD after QUEUE_MAX_ATTEMPTS reservations (a "poison" job).
//
// TIME IS PASSED IN: every function that needs the time takes 'now_ms' as a parameter instead of
// reading a clock. The server passes the real clock; tests pass any time they like, so a test can
// "wait 30 seconds" instantly.

// A job reserved this many times without an ack goes to DEAD instead of back to READY.
#define QUEUE_MAX_ATTEMPTS 5

// Queue names are 1..QUEUE_MAX_NAME bytes (checked by the server).
#define QUEUE_MAX_NAME 64

// Size of the id -> job hash table. Ids are consecutive numbers, so 'id % JOB_BUCKETS' spreads them evenly.
#define JOB_BUCKETS 4096

typedef enum { JOB_READY, JOB_RESERVED, JOB_DEAD } JobState;

struct Queue;

// One job. It is always in exactly ONE list (its queue's ready, reserved or dead list)
// and in the id hash table.
typedef struct Job {
    uint64_t id;               // unique, starts at 1, never reused
    char *data;                // the payload - heap copy, any bytes (not null-terminated)
    size_t len;
    int attempts;              // how many times it was reserved
    long long deadline_ms;     // RESERVED only: when the reservation expires
    JobState state;
    struct Queue *queue;       // the queue it belongs to
    struct Job *prev, *next;   // neighbours in its list (doubly linked -> removal in O(1))
    struct Job *id_next;       // next job in the same hash-table bucket
} Job;

// A doubly linked list of jobs with O(1) add-at-tail, remove-from-head and remove-anywhere.
typedef struct {
    Job *head;
    Job *tail;
    size_t count;
} JobList;

// One named queue, created automatically by the first push to its name.
typedef struct Queue {
    char *name;                // heap copy, not null-terminated
    size_t name_len;
    JobList ready;             // waiting for a worker, oldest first
    JobList reserved;          // being processed
    JobList dead;              // gave up after QUEUE_MAX_ATTEMPTS
    unsigned long acked;       // how many jobs were finished (acked) so far
    struct Queue *next;        // next queue in the store's list
} Queue;

// All queues and all jobs. OWNER: whoever calls js_create() must call js_destroy().
typedef struct {
    Queue *queues;                 // linked list (a server has few queues, so a list is enough)
    Job *by_id[JOB_BUCKETS];       // hash table: id -> job, chained
    uint64_t next_id;
    size_t reserved_total;         // reserved jobs in ALL queues - the server checks deadlines only if > 0
} JobStore;

// Counters for one queue, returned by js_stats().
typedef struct {
    size_t ready;
    size_t reserved;
    size_t dead;
    unsigned long acked;
} QueueStats;

// Creates an empty store. Returns NULL if memory ran out.
JobStore *js_create(void);

// Frees the store with all its queues and jobs. Safe to call with NULL.
void js_destroy(JobStore *s);

// Adds a job to the END of queue 'name' (creating the queue if needed). 'data' is copied.
// Returns the new job's id (>= 1), or 0 if memory ran out.
uint64_t js_push(JobStore *s, const char *name, size_t name_len, const char *data, size_t len);

// Takes the OLDEST ready job of queue 'name', marks it RESERVED until now_ms + timeout_ms,
// and returns it. Returns NULL if the queue is empty or doesn't exist.
// POINTER: the job still belongs to the store - read it right away, before the next js_* call.
Job *js_reserve(JobStore *s, const char *name, size_t name_len, long long now_ms, long long timeout_ms);

// Finishes a job: deletes it, wherever it is (reserved, back in ready, or dead).
// Returns 1 if the job existed and was deleted, 0 if there is no job with that id.
int js_ack(JobStore *s, uint64_t id);

// Returns every reserved job whose deadline has passed to READY (or to DEAD after QUEUE_MAX_ATTEMPTS).
// Returns how many jobs it moved.
int js_expire(JobStore *s, long long now_ms);

// Fills 'out' with the counters of queue 'name' (all zeros if the queue doesn't exist).
void js_stats(const JobStore *s, const char *name, size_t name_len, QueueStats *out);

#endif
