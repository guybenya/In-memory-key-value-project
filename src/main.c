#include "server.h"
#include "database.h"
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>


// AUTHOR: Claude
// Reads the port from the command line: "./build/server [port]". Without an argument -> 8080.
// Returns the port, or -1 if the argument is not a whole number between 1 and 65535.
static int parse_port(int argc, char *argv[]) {
    if (argc < 2) {
        return 8080;
    }
    // strtol (string to long) - like Java's Long.parseLong, but it doesn't throw.
    // It sets 'end' to the first character it could NOT convert, so "80x" or "" is detected:
    // a valid number means at least one digit was read AND the whole string was used.
    char *end;
    errno = 0;
    long value = strtol(argv[1], &end, 10);
    if (end == argv[1] || *end != '\0' || errno != 0 || value < 1 || value > 65535) {
        return -1;
    }
    return (int)value;
}

int main(int argc, char *argv[]) {
    int port = parse_port(argc, argv);
    if (port < 0) {
        fprintf(stderr, "Usage: %s [port]   (port: 1-65535, default 8080)\n", argv[0]);
        return 1;
    }

    // SIGNAL: Writing to a socket whose client already disconnected makes the OS send SIGPIPE,
    // and its default action KILLS the whole process (exit code 141 = 128 + signal 13).
    // SIG_IGN tells the OS to ignore it - write() then just returns -1 (errno = EPIPE),
    // an ordinary error we can handle, and the server keeps serving the other clients.
    // Set once, before the server starts, because it applies to the entire process.
    signal(SIGPIPE, SIG_IGN);

    // call our custom function to setup the server. This function handles the socket, bind and listen SYSTEM CALLS internally. It returns the main server file descriptor. 
    int server_fd = start_server(port);

    // HEAP ALLOCATION: Create the global database instance. 
    // POINTER: 'db' holds the memory address of the hash table. 
    // MEMORY OWNER: main() function. It owns this memory because the database must persist for the entire lifespan of the server. 
    HashTable *db = create_table();

    if (db == NULL) {
        perror("Failed to create databse");
        return 1;
    }

    // HEAP ALLOCATION: the job queues. OWNER: main(), for the whole run - like the database.
    JobStore *jobs = js_create();
    if (jobs == NULL) {
        perror("Failed to create job store");
        return 1;
    }

    // Everything the commands work on, handed to the event loop in one struct.
    ServerState state = { .db = db, .jobs = jobs };

    // EVENT LOOP: accepts clients and serves all of them at once. It never returns -
    // the server runs until the process is stopped (e.g. Ctrl+C).
    run_event_loop(server_fd, &state);

    return 0;
}