#include "server.h"
#include "database.h"
#include "resp.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <errno.h>
#include <poll.h>
#include <fcntl.h>
#include <time.h>



// Establishing the structure
int start_server(int port) {
    // System call to create a new network endpoint (socket).
    // AF_INET specifies the IPv4 address family.
    // SOCK_STREAM specifies that we want a reliable, connection-oriented TCP stream.
    // 0 tells the operating system to choose the default protocol (TCP) for this setup.
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    
    // The socket system call returns -1 if the operating system could not allocate the resource.
    if (server_fd == -1) {
        // perror prints our custom message followed by the actual error reason from the OS (e.g., "Permission denied").
        perror("Socket creation failed");
        // exit immediately terminates the entire process and returns a failure code to the operating system.
        exit(EXIT_FAILURE);
    }

    // SYSTEM CALL: setsockopt(fd, level, option, value_ptr, value_len) - changes a setting of the socket.
    // SO_REUSEADDR: After the server stops, the OS keeps its old connections in TIME_WAIT for ~30-60 seconds,
    // and bind() would fail with "Address already in use". This option lets us bind to the port immediately.
    // MUST be set BEFORE bind(), because it changes what bind() is allowed to do.
    // 'opt' = 1 means "enable". The value is passed by pointer (void *) because different options use
    // different types - sizeof(opt) tells the OS how many bytes to read from that pointer.
    int opt = 1;
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("setsockopt failed");
        exit(EXIT_FAILURE);
    }

    // declares a specifically designed to hold IPv4 address and port information. 
    struct sockaddr_in address;

    // Sets the address family of the structure to IPv4, so the OS knows how to read the data inside. 
    address.sin_family = AF_INET;

    // "INADDR_ANY" configures the socket to listen on all available network interfaces on this machine (both local - host and external). 
    address.sin_addr.s_addr = INADDR_ANY;

    // htons (Host To Network Short) converts the port number from the local CPU's byte order to the standart network byteorder (Big Endian). 
    address.sin_port = htons(port);

    // System call that formally links (binds) the socket file descriptor to the specific IP and port we configured.
    // We must cast our specific 'sockaddr_in' pointer to the generic 'sockaddr' pointer that the bind function expects.
    // sizeof(address) tells the OS exactly how many bytes of memory to read from our structure.
    if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        perror("Bind failed");
        exit(EXIT_FAILURE);
    }

    // System call that transitions the socket from an active state to a passive listening state.
    // The second argument is the backlog: how many new connections the OS queues until we accept() them.
    // SOMAXCONN = the biggest value the OS allows. A small backlog (it used to be 3) makes the OS refuse
    // connections when many workers connect at the same moment.
    if (listen(server_fd, SOMAXCONN) < 0) {
        perror("Listen failed");
        exit(EXIT_FAILURE);
    }
    // Prints a confirmation to the terminal so we know the server is successfully running.
    printf("Server listening on port %d... \n" , port);

    // Returns the file descriptor (the integer ID of the socket) so the rest of the program can use it to accept connections.
    return server_fd;
}

// ---------------------------------------------------------------------------------------------
// TRACE MODE
// ---------------------------------------------------------------------------------------------

// AUTHOR: Claude (trace_on, TRACE)
// Run with  KV_TRACE=1 ./build/server  to print every event (connect, read, execute, send, close)
// to stderr - a way to watch the event loop work step by step. Off by default.
static int trace_on = 0;

// MACRO with a variable number of arguments: __VA_ARGS__ = "everything passed to TRACE",
// handed to fprintf like printf's "...". "[trace] " "fmt" are two string literals - C glues them into one.
// do { ... } while (0) makes the macro act like ONE statement, so it is safe inside an if without braces.
#define TRACE(...) do {                               \
        if (trace_on) {                               \
            fprintf(stderr, "[trace] " __VA_ARGS__);  \
            fputc('\n', stderr);                      \
        }                                             \
    } while (0)


// ---------------------------------------------------------------------------------------------
// COMMANDS
// ---------------------------------------------------------------------------------------------

// AUTHOR: Claude
// Checks whether argument 'i' is the word 'name', ignoring upper/lower case (Redis accepts "get" and "GET").
// strncasecmp compares at most n chars - needed because the arguments are NOT null-terminated.
static int arg_is(const RespCommand *cmd, int i, const char *name) {
    size_t n = strlen(name);
    return cmd->argv_len[i] == n && strncasecmp(cmd->argv[i], name, n) == 0;
}

// AUTHOR: Claude
// Copies argument 'i' into a new null-terminated C string, because db_set/db_get expect C strings.
// HEAP ALLOCATION - OWNER: the caller must free() it. Returns NULL if memory ran out.
static char *arg_to_cstr(const RespCommand *cmd, int i) {
    char *s = malloc(cmd->argv_len[i] + 1);
    if (s == NULL) {
        return NULL;
    }
    memcpy(s, cmd->argv[i], cmd->argv_len[i]);
    s[cmd->argv_len[i]] = '\0';
    return s;
}

// AUTHOR: Claude
// Writes the standard Redis error for a command with the wrong number of arguments.
static int write_arity_error(RespBuf *out, const char *command) {
    char msg[64];
    snprintf(msg, sizeof(msg), "wrong number of arguments for '%s' command", command);
    return resp_write_error(out, msg);
}

// ---------------------------------------------------------------------------------------------
// QUEUE COMMANDS - thin wrappers: check the arguments, call the queue core (queue.c), write the reply.
// ---------------------------------------------------------------------------------------------

// AUTHOR: Claude (everything from here down to execute_command)

// Longest visibility timeout a worker may ask for (one day).
#define MAX_RESERVE_TIMEOUT_SEC 86400

// Returns the current time in milliseconds, from a MONOTONIC clock.
// Monotonic = it only moves forward and is not the time of day. The normal clock can jump (e.g. when the
// computer syncs its time), and a jump forward would make every reserved job expire at once.
static long long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

// Reads argument 'i' as a whole number made only of digits (e.g. "30" or "17").
// Returns 0 on success, -1 if it is empty, contains anything but digits, or is too long.
// Written by hand (not strtol) because the argument is NOT null-terminated.
static int arg_to_u64(const RespCommand *cmd, int i, uint64_t *out) {
    size_t len = cmd->argv_len[i];
    // At most 19 digits: every 19-digit number fits in 64 bits, so the loop below can't overflow.
    if (len == 0 || len > 19) {
        return -1;
    }
    uint64_t value = 0;
    for (size_t k = 0; k < len; k++) {
        char ch = cmd->argv[i][k];
        if (ch < '0' || ch > '9') {
            return -1;
        }
        value = value * 10 + (uint64_t)(ch - '0');
    }
    *out = value;
    return 0;
}

// Checks that argument 'i' is a valid queue name (1 to QUEUE_MAX_NAME bytes). Returns 1 if it is.
// If it isn't, writes the error reply, puts that write's result (0 or -1) in *rc, and returns 0 -
// the caller then returns *rc right away.
static int check_queue_name(const RespCommand *cmd, int i, RespBuf *out, int *rc) {
    if (cmd->argv_len[i] >= 1 && cmd->argv_len[i] <= QUEUE_MAX_NAME) {
        return 1;
    }
    char msg[64];
    snprintf(msg, sizeof(msg), "queue name must be 1 to %d bytes", QUEUE_MAX_NAME);
    *rc = resp_write_error(out, msg);
    return 0;
}

// PUSH queue payload -> :<job id>
// Adds a job to the end of the queue (the queue is created by its first PUSH).
static int cmd_push(JobStore *jobs, const RespCommand *cmd, RespBuf *out) {
    int rc;
    if (cmd->argc != 3) {
        return write_arity_error(out, "push");
    }
    if (!check_queue_name(cmd, 1, out, &rc)) {
        return rc;
    }
    uint64_t id = js_push(jobs, cmd->argv[1], cmd->argv_len[1], cmd->argv[2], cmd->argv_len[2]);
    if (id == 0) {
        return -1;      // out of memory
    }
    TRACE("    job %llu pushed to '%.*s'", (unsigned long long)id, (int)cmd->argv_len[1], cmd->argv[1]);
    return resp_write_integer(out, (long)id);
}

// RESERVE queue timeout-seconds -> *3 :<id> $<payload> :<attempts>, or (nil) if the queue is empty.
// Hands the oldest waiting job to this worker and hides it from other workers for 'timeout' seconds.
// If the worker doesn't ACK it in time (e.g. it crashed), the job goes back to the queue.
static int cmd_reserve(JobStore *jobs, const RespCommand *cmd, RespBuf *out) {
    int rc;
    if (cmd->argc != 3) {
        return write_arity_error(out, "reserve");
    }
    if (!check_queue_name(cmd, 1, out, &rc)) {
        return rc;
    }
    uint64_t seconds;
    if (arg_to_u64(cmd, 2, &seconds) < 0 || seconds < 1 || seconds > MAX_RESERVE_TIMEOUT_SEC) {
        return resp_write_error(out, "timeout must be a whole number of seconds from 1 to 86400");
    }

    Job *job = js_reserve(jobs, cmd->argv[1], cmd->argv_len[1], now_ms(), (long long)seconds * 1000);
    if (job == NULL) {
        return resp_write_null(out);    // empty queue - the worker waits a little and asks again
    }
    TRACE("    job %llu reserved from '%.*s' (attempt %d, timeout %llu s)", (unsigned long long)job->id,
          (int)cmd->argv_len[1], cmd->argv[1], job->attempts, (unsigned long long)seconds);

    // POINTER: 'job' belongs to the store - copy what we need into the reply now.
    if (resp_write_array_header(out, 3) < 0) return -1;
    if (resp_write_integer(out, (long)job->id) < 0) return -1;
    if (resp_write_bulk(out, job->data, job->len) < 0) return -1;
    return resp_write_integer(out, job->attempts);
}

// ACK id -> :1 if the job was deleted, :0 if there is no such job.
// The worker calls it after finishing a job, so the job is never handed out again.
static int cmd_ack(JobStore *jobs, const RespCommand *cmd, RespBuf *out) {
    if (cmd->argc != 2) {
        return write_arity_error(out, "ack");
    }
    uint64_t id;
    if (arg_to_u64(cmd, 1, &id) < 0) {
        return resp_write_error(out, "invalid job id");
    }
    int deleted = js_ack(jobs, id);
    TRACE("    job %llu %s", (unsigned long long)id, deleted ? "acked" : "not found");
    return resp_write_integer(out, deleted);
}

// Appends one "name, number" pair to a STATS reply.
static int write_stat(RespBuf *out, const char *name, long value) {
    if (resp_write_bulk(out, name, strlen(name)) < 0) return -1;
    return resp_write_integer(out, value);
}

// STATS queue -> *8 "ready" :n "reserved" :n "acked" :n "dead" :n
// Reports how many jobs of the queue are waiting, being processed, finished, and given up.
static int cmd_stats(JobStore *jobs, const RespCommand *cmd, RespBuf *out) {
    int rc;
    if (cmd->argc != 2) {
        return write_arity_error(out, "stats");
    }
    if (!check_queue_name(cmd, 1, out, &rc)) {
        return rc;
    }
    QueueStats st;
    js_stats(jobs, cmd->argv[1], cmd->argv_len[1], &st);
    if (resp_write_array_header(out, 8) < 0) return -1;
    if (write_stat(out, "ready", (long)st.ready) < 0) return -1;
    if (write_stat(out, "reserved", (long)st.reserved) < 0) return -1;
    if (write_stat(out, "acked", (long)st.acked) < 0) return -1;
    return write_stat(out, "dead", (long)st.dead);
}

// AUTHOR: Claude
// Runs one parsed command and appends its RESP reply to 'out': picks the command by its name
// and passes it to the key-value store or to the job queues.
// Returns 0 on success, -1 if memory ran out (the caller drops the connection).
static int execute_command(ServerState *state, const RespCommand *cmd, RespBuf *out) {
    HashTable *db = state->db;

    TRACE("    execute %.*s (%d args)", cmd->argv_len[0] > 32 ? 32 : (int)cmd->argv_len[0], cmd->argv[0], cmd->argc);

    // NULL BYTES: the key-value database stores C strings, which end at the first '\0'.
    // A value containing '\0' would be cut silently - so refuse it with a clear error instead.
    // Only SET/GET/DEL have this limit: queue payloads are stored together with their length.
    if (arg_is(cmd, 0, "SET") || arg_is(cmd, 0, "GET") || arg_is(cmd, 0, "DEL")) {
        for (int i = 0; i < cmd->argc; i++) {
            if (memchr(cmd->argv[i], '\0', cmd->argv_len[i]) != NULL) {
                return resp_write_error(out, "null bytes in arguments are not supported");
            }
        }
    }

    // PING -> +PONG. Clients use it to check that the server is alive.
    if (arg_is(cmd, 0, "PING")) {
        if (cmd->argc != 1) {
            return write_arity_error(out, "ping");
        }
        return resp_write_simple(out, "PONG");
    }

    // SET key value -> +OK
    if (arg_is(cmd, 0, "SET")) {
        if (cmd->argc != 3) {
            return write_arity_error(out, "set");
        }
        char *key = arg_to_cstr(cmd, 1);
        char *value = arg_to_cstr(cmd, 2);
        if (key == NULL || value == NULL) {
            free(key);      // free(NULL) is safe, so no need to check which one failed
            free(value);
            return -1;
        }
        // db_set makes its own copies (strdup), so our temporary strings can be freed right away.
        int rc = db_set(db, key, value);
        free(key);
        free(value);
        if (rc != 0) {
            return resp_write_error(out, "failed to save");
        }
        return resp_write_simple(out, "OK");
    }

    // GET key -> $<len>\r\n<value>\r\n, or $-1\r\n (null) if the key doesn't exist
    if (arg_is(cmd, 0, "GET")) {
        if (cmd->argc != 2) {
            return write_arity_error(out, "get");
        }
        char *key = arg_to_cstr(cmd, 1);
        if (key == NULL) {
            return -1;
        }
        // POINTER: 'value' points INTO the database's memory - read it, don't free it.
        char *value = db_get(db, key);
        free(key);
        if (value == NULL) {
            return resp_write_null(out);
        }
        return resp_write_bulk(out, value, strlen(value));
    }

    // DEL key [key ...] -> :<number of keys that were actually deleted>
    if (arg_is(cmd, 0, "DEL")) {
        if (cmd->argc < 2) {
            return write_arity_error(out, "del");
        }
        long deleted = 0;
        for (int i = 1; i < cmd->argc; i++) {
            char *key = arg_to_cstr(cmd, i);
            if (key == NULL) {
                return -1;
            }
            if (db_delete(db, key) == 1) {
                deleted++;
            }
            free(key);
        }
        return resp_write_integer(out, deleted);
    }

    // Job queue commands.
    if (arg_is(cmd, 0, "PUSH")) {
        return cmd_push(state->jobs, cmd, out);
    }
    if (arg_is(cmd, 0, "RESERVE")) {
        return cmd_reserve(state->jobs, cmd, out);
    }
    if (arg_is(cmd, 0, "ACK")) {
        return cmd_ack(state->jobs, cmd, out);
    }
    if (arg_is(cmd, 0, "STATS")) {
        return cmd_stats(state->jobs, cmd, out);
    }

    // Anything else. "%.*s" prints at most N chars of a NOT null-terminated string (N comes first).
    // The name is capped at 32 chars so a huge garbage command can't make a huge error message.
    char msg[96];
    int name_len = cmd->argv_len[0] > 32 ? 32 : (int)cmd->argv_len[0];
    snprintf(msg, sizeof(msg), "unknown command '%.*s'", name_len, cmd->argv[0]);
    return resp_write_error(out, msg);
}

// ---------------------------------------------------------------------------------------------
// CLIENT CONNECTION
// ---------------------------------------------------------------------------------------------

// AUTHOR: Guy (skeleton by Claude)
// Runs every COMPLETE command in 'in', appends their replies to 'out',
// and removes the handled bytes from the front of 'in'. A partial command stays in 'in' for next time.
// Returns  0 - fine (the connection stays open).
// Returns -1 - protocol error (an error reply was already added to 'out') or out of memory -> close the connection.
// (Stage 3, by Claude: the first parameter changed from HashTable* to ServerState*, nothing else.)
static int process_input(ServerState *state, RespBuf *in, RespBuf *out) {
    // STEP 1: 'start' = offset of the first byte that hasn't been handled yet.
    size_t start = 0;
    // STEP 2: Loop - parse one command starting at in->data + start (there are in->len - start bytes left):
    //   incomplete (0)  -> stop the loop, wait for more bytes
    //   malformed  (-1) -> append the error reply "Protocol error" and return -1
    //   complete   (1)  -> execute it (if that returns -1, return -1), then move 'start' past it
    while (start < in->len) 
    {
        RespCommand cmd;
        size_t consumed;
        int rc = resp_parse_command(in->data + start, in->len - start, &cmd, &consumed);
        if (rc == 0) {break;}
        if (rc == -1)
        {
            resp_write_error(out, "Protocol error");
            return -1;
        }
        if (rc == 1)
        {
            if (execute_command(state, &cmd, out) == -1) {return -1;}
            start += consumed;
        }
    }
    // STEP 3: Remove the handled bytes: move what's left (the partial command, if any) to the front of
    //         in->data, and update in->len. Do this AFTER the loop - the commands point INTO this buffer.
    if (start > 0) {
        size_t remaining = in->len - start;
        memmove(in->data, in->data + start, remaining);
        in->len = remaining;
    }

    // STEP 4: Success.
    return 0;
}

// ---------------------------------------------------------------------------------------------
// EVENT LOOP
// ---------------------------------------------------------------------------------------------
//
// ONE thread serves ALL clients. Instead of waiting on one client (the old blocking read()),
// the server asks the OS with poll(): "wake me up when ANY of these sockets is ready", then handles
// only the sockets that are ready, and goes back to sleep. Every socket is NON-BLOCKING, so no single
// call can freeze the server - a slow or stuck client only affects itself.

// AUTHOR: Claude (everything from here to the end of the file)

// Bytes read from a socket per read() call.
#define READ_CHUNK 4096

// LIMIT: the biggest legal command is RESP_MAX_ARGS arguments of RESP_MAX_BULK_LEN bytes each, plus headers.
// A client whose unfinished input grows beyond that is broken or malicious - disconnect it,
// otherwise it could make the server buffer memory forever.
#define MAX_INPUT_BUFFER (RESP_MAX_ARGS * (RESP_MAX_BULK_LEN + 32) + 32)

// Maximum number of clients connected at the same time.
#define MAX_CLIENTS 1024

// Everything the server remembers about one client BETWEEN poll() calls.
// The old handle_client kept this in local variables while it waited for its one client.
// A single thread serving many clients can't wait inside a function per client, so each client's
// state lives here instead, and the loop picks it up again when that client's socket is ready.
typedef struct {
    RespBuf in;        // bytes received but not handled yet - a partial command waits here
    RespBuf out;       // replies that were not sent yet
    size_t out_sent;   // how many bytes at the front of 'out' were already sent
    int closing;       // 1 = protocol error: send what's left in 'out', then close
} Client;

// Two arrays kept in sync: fds[i] is what poll() watches, clients[i] is the state behind it.
// Slot 0 is the listening socket (clients[0] is unused). Slots 1 .. nfds-1 are clients.
// 'static' at file level = private to this file (like a private static field in Java).
static struct pollfd fds[MAX_CLIENTS + 1];
static Client clients[MAX_CLIENTS + 1];
static int nfds = 0;

// Switches a socket to NON-BLOCKING mode: read(), write() and accept() return -1 with errno EAGAIN
// instead of waiting when there is nothing to do right now.
// fcntl = "file control": F_GETFL reads the socket's flags, F_SETFL writes them back with O_NONBLOCK added.
static int set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        return -1;
    }
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

// "Nothing to do right now" on a non-blocking socket. Some systems use EWOULDBLOCK, some EAGAIN.
static int would_block(void) {
    return errno == EAGAIN || errno == EWOULDBLOCK;
}

// Closes client i and frees its buffers. To keep the arrays without holes, the LAST client is moved
// into slot i (O(1), and the order of clients doesn't matter).
// OWNERSHIP: the struct copy moves the last client's buffer pointers into slot i - nothing is freed twice,
// because slot nfds is no longer in use after the move.
static void close_client(int i, const char *reason) {
    TRACE("fd %d: closed (%s)", fds[i].fd, reason);
    close(fds[i].fd);
    resp_buf_free(&clients[i].in);
    resp_buf_free(&clients[i].out);
    nfds--;
    if (i != nfds) {
        fds[i] = fds[nfds];
        clients[i] = clients[nfds];
    }
}

// Accepts ALL connections that are waiting (there can be several per wake-up).
static void accept_clients(int listen_fd) {
    while (1) {
        // NULL, NULL = we don't need the client's address.
        int fd = accept(listen_fd, NULL, NULL);
        if (fd < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (!would_block()) {
                perror("accept failed");
            }
            return;     // EAGAIN = no more waiting connections
        }
        // Full: accept and close right away. Leaving it un-accepted would make poll() wake us up forever.
        if (nfds == MAX_CLIENTS + 1) {
            TRACE("fd %d: rejected (too many clients)", fd);
            close(fd);
            continue;
        }
        // Set explicitly: on Linux an accepted socket does NOT inherit O_NONBLOCK from the listening socket.
        if (set_nonblocking(fd) < 0) {
            perror("fcntl failed");
            close(fd);
            continue;
        }
        fds[nfds].fd = fd;
        fds[nfds].events = POLLIN;
        fds[nfds].revents = 0;              // not ready in this round - it hasn't been polled yet
        clients[nfds] = (Client){0};        // empty buffers, nothing sent, not closing
        nfds++;
        TRACE("fd %d: client connected (%d clients)", fd, nfds - 1);
    }
}

// Sends as much of client i's pending replies as the OS accepts right now.
// Returns 0 if the client is still open, -1 if it was closed.
static int flush_output(int i) {
    Client *c = &clients[i];
    while (c->out_sent < c->out.len) {
        ssize_t n = write(fds[i].fd, c->out.data + c->out_sent, c->out.len - c->out_sent);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (would_block()) {
                // The OS buffer for this socket is full: the client isn't reading as fast as we write.
                // Keep the rest - poll() reports POLLOUT when there is room again.
                TRACE("fd %d: socket full, %zu bytes wait for POLLOUT", fds[i].fd, c->out.len - c->out_sent);
                return 0;
            }
            close_client(i, "write failed");
            return -1;
        }
        c->out_sent += (size_t)n;
        TRACE("fd %d: sent %zd bytes", fds[i].fd, n);
    }
    // Everything was sent - reuse the buffer from the start.
    c->out.len = 0;
    c->out_sent = 0;
    if (c->closing) {
        close_client(i, "protocol error");
        return -1;
    }
    return 0;
}

// Client i's socket has data: read ONE chunk, run every complete command, and try to send the replies.
// One read per wake-up keeps it fair - a client sending a lot can't starve the others.
// Returns 0 if the client is still open, -1 if it was closed.
static int handle_readable(int i, ServerState *state) {
    Client *c = &clients[i];
    // STACK ALLOCATION: temporary landing area for this read().
    char chunk[READ_CHUNK];

    ssize_t n = read(fds[i].fd, chunk, sizeof(chunk));
    if (n < 0) {
        if (errno == EINTR || would_block()) {
            return 0;   // nothing to read after all - try again next round
        }
        close_client(i, "read failed");
        return -1;
    }
    if (n == 0) {
        close_client(i, "client disconnected");
        return -1;
    }
    TRACE("fd %d: read %zd bytes", fds[i].fd, n);

    if (resp_buf_append(&c->in, chunk, (size_t)n) < 0) {
        close_client(i, "out of memory");
        return -1;
    }

    // Guy's process_input - the same logic as in the one-client version. It never touches the socket, only
    // buffers, which is exactly why it works as-is inside an event loop.
    if (process_input(state, &c->in, &c->out) < 0) {
        c->closing = 1;     // stop reading; flush_output closes the connection after the error reply is out
    } else if (c->in.len > MAX_INPUT_BUFFER) {
        close_client(i, "input too large");
        return -1;
    }
    if (c->in.len > 0) {
        TRACE("fd %d: %zu bytes wait for the rest of a command", fds[i].fd, c->in.len);
    }

    // Try to send the replies right away. Usually the socket has room, which saves a whole poll() round.
    return flush_output(i);
}

// How often reserved jobs are checked for an expired deadline. Deadlines are whole seconds,
// so being up to 0.1 s late is fine.
#define EXPIRY_CHECK_MS 100

// Runs the server forever: waits for events with poll(), hands each ready socket to its handler,
// and regularly returns expired jobs to their queues.
void run_event_loop(int listen_fd, ServerState *state) {
    trace_on = getenv("KV_TRACE") != NULL;

    if (set_nonblocking(listen_fd) < 0) {
        perror("fcntl failed");
        exit(EXIT_FAILURE);
    }
    fds[0].fd = listen_fd;
    fds[0].events = POLLIN;     // on a listening socket, "readable" = a new connection is waiting
    nfds = 1;

    printf("Waiting for connections... \n");

    // When the next check for expired jobs is due (see STEP 5).
    long long next_expiry_check = 0;

    while (1) {
        // STEP 1: Tell poll() what to wait for, per client.
        // BACKPRESSURE: a client with unsent replies is not reading them, so we stop reading ITS commands
        // until it catches up (POLLOUT only). Otherwise a client that pipelines without reading would make
        // the server pile up replies in memory forever. Its unread commands wait in the OS buffer instead.
        for (int i = 1; i < nfds; i++) {
            if (clients[i].out_sent < clients[i].out.len) {
                fds[i].events = POLLOUT;
            } else {
                fds[i].events = POLLIN;
            }
        }

        // STEP 2: SYSTEM CALL poll() - sleep until at least one socket is ready.
        // The OS fills fds[i].revents with what happened to each socket.
        // TIMEOUT: if jobs are reserved, wake up at least every EXPIRY_CHECK_MS even when no client does
        // anything - a crashed worker sends nothing, yet its job must come back. Otherwise -1 = no timeout.
        int timeout = state->jobs->reserved_total > 0 ? EXPIRY_CHECK_MS : -1;
        int ready = poll(fds, (nfds_t)nfds, timeout);
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("poll failed");
            exit(EXIT_FAILURE);
        }
        if (ready > 0) {
            TRACE("poll: %d socket(s) ready", ready);
        }

        // STEP 3: New connections.
        if (fds[0].revents & POLLIN) {
            accept_clients(listen_fd);
        }

        // STEP 4: Clients. When client i is closed, the last client moves into slot i, so we look at
        // slot i again instead of moving on - the moved client hasn't been handled in this round yet.
        int i = 1;
        while (i < nfds) {
            short ev = fds[i].revents;
            // POLLERR = socket error, POLLNVAL = not an open socket.
            if (ev & (POLLERR | POLLNVAL)) {
                close_client(i, "socket error");
                continue;
            }
            // POLLHUP = the client hung up. read() then returns 0 (or the last bytes it sent), so read handles it.
            if ((ev & (POLLIN | POLLHUP)) && !clients[i].closing) {
                if (handle_readable(i, state) < 0) {
                    continue;
                }
            }
            if (ev & POLLOUT) {
                if (flush_output(i) < 0) {
                    continue;
                }
            }
            if ((ev & POLLHUP) && clients[i].closing) {
                close_client(i, "client disconnected");
                continue;
            }
            i++;
        }

        // STEP 5: Visibility timeout. Return jobs whose worker didn't ACK in time to their queue.
        // Done at most every EXPIRY_CHECK_MS, so a busy server doesn't scan the reserved jobs on every event.
        if (state->jobs->reserved_total > 0) {
            long long now = now_ms();
            if (now >= next_expiry_check) {
                int moved = js_expire(state->jobs, now);
                if (moved > 0) {
                    TRACE("expired: %d job(s) returned to their queue (or marked dead)", moved);
                }
                next_expiry_check = now + EXPIRY_CHECK_MS;
            }
        }
    }
}
