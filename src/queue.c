#include "queue.h"
#include <stdlib.h>
#include <string.h>

// AUTHOR: Claude (this whole file)

// ---------------------------------------------------------------------------------------------
// JOB LISTS - a doubly linked list. Each job knows its neighbours (prev/next), so it can be
// unlinked from the middle in O(1), without walking the list. ACK of a job that sits anywhere in
// a list needs exactly that.
// ---------------------------------------------------------------------------------------------

// Adds a job at the end of a list.
static void list_push_tail(JobList *list, Job *job) {
    job->prev = list->tail;
    job->next = NULL;
    if (list->tail != NULL) {
        list->tail->next = job;
    } else {
        list->head = job;           // the list was empty
    }
    list->tail = job;
    list->count++;
}

// Unlinks a job from a list, wherever it is (head, middle or tail). The job itself is not freed.
static void list_remove(JobList *list, Job *job) {
    if (job->prev != NULL) {
        job->prev->next = job->next;
    } else {
        list->head = job->next;     // it was the head
    }
    if (job->next != NULL) {
        job->next->prev = job->prev;
    } else {
        list->tail = job->prev;     // it was the tail
    }
    job->prev = NULL;
    job->next = NULL;
    list->count--;
}

// Returns the list that a job in state 'state' lives in.
static JobList *list_for_state(Queue *q, JobState state) {
    switch (state) {
        case JOB_READY:    return &q->ready;
        case JOB_RESERVED: return &q->reserved;
        default:           return &q->dead;
    }
}

// ---------------------------------------------------------------------------------------------
// ID HASH TABLE - finds a job by its id in O(1) on average, so ACK doesn't scan all jobs.
// ---------------------------------------------------------------------------------------------

// Returns the bucket (chain head) where a job with this id is stored.
static Job **bucket_for(JobStore *s, uint64_t id) {
    return &s->by_id[id % JOB_BUCKETS];
}

// Adds a job to the front of its bucket's chain.
static void id_insert(JobStore *s, Job *job) {
    Job **bucket = bucket_for(s, job->id);
    job->id_next = *bucket;
    *bucket = job;
}

// Finds a job by id and unlinks it from the hash table. Returns the job, or NULL if there is no such id.
// POINTER TO POINTER: 'link' points at the pointer that points to the current job (the bucket head, or the
// previous job's id_next), so unlinking is one assignment - the same trick as db_delete.
static Job *id_take(JobStore *s, uint64_t id) {
    Job **link = bucket_for(s, id);
    while (*link != NULL) {
        Job *job = *link;
        if (job->id == id) {
            *link = job->id_next;
            job->id_next = NULL;
            return job;
        }
        link = &job->id_next;
    }
    return NULL;
}

// ---------------------------------------------------------------------------------------------
// QUEUES
// ---------------------------------------------------------------------------------------------

// Finds a queue by name. Returns NULL if no queue has that name.
// memcmp with a length check, not strcmp: names are not null-terminated.
static Queue *find_queue(const JobStore *s, const char *name, size_t name_len) {
    for (Queue *q = s->queues; q != NULL; q = q->next) {
        if (q->name_len == name_len && memcmp(q->name, name, name_len) == 0) {
            return q;
        }
    }
    return NULL;
}

// Finds a queue by name, or creates it if it doesn't exist yet. Returns NULL if memory ran out.
static Queue *get_or_create_queue(JobStore *s, const char *name, size_t name_len) {
    Queue *q = find_queue(s, name, name_len);
    if (q != NULL) {
        return q;
    }
    // calloc = malloc + set every byte to 0: all lists start empty and all counters at 0.
    q = calloc(1, sizeof(Queue));
    if (q == NULL) {
        return NULL;
    }
    q->name = malloc(name_len > 0 ? name_len : 1);
    if (q->name == NULL) {
        free(q);
        return NULL;
    }
    memcpy(q->name, name, name_len);
    q->name_len = name_len;
    q->next = s->queues;            // add at the front of the store's list
    s->queues = q;
    return q;
}

// Frees one job and its payload.
static void free_job(Job *job) {
    free(job->data);
    free(job);
}

// Frees every job in a list (used only by js_destroy).
static void free_list(JobList *list) {
    Job *job = list->head;
    while (job != NULL) {
        Job *next = job->next;      // save it BEFORE freeing the job that holds it
        free_job(job);
        job = next;
    }
}

// ---------------------------------------------------------------------------------------------
// PUBLIC API (documented in queue.h)
// ---------------------------------------------------------------------------------------------

// Creates an empty store: no queues, no jobs, and the first id will be 1.
JobStore *js_create(void) {
    JobStore *s = calloc(1, sizeof(JobStore));   // zeroed: empty queue list, every bucket NULL
    if (s == NULL) {
        return NULL;
    }
    s->next_id = 1;                 // 0 is kept free to mean "failed" in js_push
    return s;
}

// Frees everything the store owns: every job in every list of every queue, the queues, and the store.
void js_destroy(JobStore *s) {
    if (s == NULL) {
        return;
    }
    Queue *q = s->queues;
    while (q != NULL) {
        Queue *next = q->next;
        free_list(&q->ready);
        free_list(&q->reserved);
        free_list(&q->dead);
        free(q->name);
        free(q);
        q = next;
    }
    free(s);
}

// Creates a job with a copy of the payload and adds it to the end of the queue's ready list.
uint64_t js_push(JobStore *s, const char *name, size_t name_len, const char *data, size_t len) {
    Queue *q = get_or_create_queue(s, name, name_len);
    if (q == NULL) {
        return 0;
    }
    Job *job = calloc(1, sizeof(Job));
    if (job == NULL) {
        return 0;
    }
    job->data = malloc(len > 0 ? len : 1);     // malloc(0) may return NULL - always ask for at least 1 byte
    if (job->data == NULL) {
        free(job);
        return 0;
    }
    memcpy(job->data, data, len);
    job->len = len;
    job->id = s->next_id++;
    job->state = JOB_READY;
    job->queue = q;

    list_push_tail(&q->ready, job);
    id_insert(s, job);
    return job->id;
}

// Moves the oldest ready job to the reserved list, counts the attempt, and sets its deadline.
Job *js_reserve(JobStore *s, const char *name, size_t name_len, long long now_ms, long long timeout_ms) {
    Queue *q = find_queue(s, name, name_len);
    if (q == NULL || q->ready.head == NULL) {
        return NULL;
    }
    Job *job = q->ready.head;
    list_remove(&q->ready, job);
    job->state = JOB_RESERVED;
    job->attempts++;
    job->deadline_ms = now_ms + timeout_ms;
    list_push_tail(&q->reserved, job);
    s->reserved_total++;
    return job;
}

// Looks the job up by id, unlinks it from its list and from the hash table, and frees it.
int js_ack(JobStore *s, uint64_t id) {
    Job *job = id_take(s, id);
    if (job == NULL) {
        return 0;
    }
    Queue *q = job->queue;
    list_remove(list_for_state(q, job->state), job);
    if (job->state == JOB_RESERVED) {
        s->reserved_total--;
    }
    q->acked++;
    free_job(job);
    return 1;
}

// Walks the reserved list of every queue and moves each job whose deadline has passed:
// back to the END of the ready list, or to the dead list if it already used all its attempts.
//
// COST: O(number of reserved jobs). That is about the number of workers (each holds one job at a time),
// so it stays small. With huge numbers, a min-heap ordered by deadline would find expired jobs in O(log n).
int js_expire(JobStore *s, long long now_ms) {
    int moved = 0;
    for (Queue *q = s->queues; q != NULL; q = q->next) {
        Job *job = q->reserved.head;
        while (job != NULL) {
            Job *next = job->next;          // save it - moving the job changes job->next
            if (job->deadline_ms <= now_ms) {
                list_remove(&q->reserved, job);
                s->reserved_total--;
                if (job->attempts >= QUEUE_MAX_ATTEMPTS) {
                    job->state = JOB_DEAD;
                    list_push_tail(&q->dead, job);
                } else {
                    job->state = JOB_READY;
                    list_push_tail(&q->ready, job);
                }
                moved++;
            }
            job = next;
        }
    }
    return moved;
}

// Copies the counters of one queue into 'out'.
void js_stats(const JobStore *s, const char *name, size_t name_len, QueueStats *out) {
    memset(out, 0, sizeof(*out));
    Queue *q = find_queue(s, name, name_len);
    if (q == NULL) {
        return;
    }
    out->ready = q->ready.count;
    out->reserved = q->reserved.count;
    out->dead = q->dead.count;
    out->acked = q->acked;
}
