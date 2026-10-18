#ifndef SERVER_H
#define SERVER_H
#include "database.h"
#include "queue.h"

// Everything the commands work on. One instance lives in main() for the whole run.
typedef struct {
    HashTable *db;      // key-value store: SET / GET / DEL
    JobStore *jobs;     // job queues: PUSH / RESERVE / ACK / STATS
} ServerState;

int start_server(int port);

// Serves all clients at once with a poll() event loop: accepts new connections, reads commands,
// sends replies, and returns expired jobs to their queues. Never returns.
// Run with KV_TRACE=1 in the environment to print every event to stderr.
void run_event_loop(int server_fd, ServerState *state);

#endif
