#ifndef SERVER_H
#define SERVER_H
#include "database.h"

int start_server(int port);

// Serves all clients at once with a poll() event loop: accepts new connections, reads commands,
// sends replies. Never returns. Run with KV_TRACE=1 in the environment to print every event to stderr.
void run_event_loop(int server_fd, HashTable *db);

#endif
