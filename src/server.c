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
    // The number 3 defines the backlog queue length: the maximum number of pending connections the OS will queue before rejecting new ones.
    if (listen(server_fd, 3) < 0) {
        perror("Listen failed");
        exit(EXIT_FAILURE);
    }
    // Prints a confirmation to the terminal so we know the server is successfully running.
    printf("Server listening on port %d... \n" , port);

    // Returns the file descriptor (the integer ID of the socket) so the rest of the program can use it to accept connections.
    return server_fd;
}

// Sends exactly 'len' bytes from 'buf'. Returns 0 on success, -1 on failure.
static int send_all(int fd, const char *buf, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = write(fd, buf + sent, len - sent);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;  
        }
        else {
            sent = sent + n;
        }
    }
    return 0;
}


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

// AUTHOR: Claude
// Runs one parsed command against the database and appends its RESP reply to 'out'.
// Returns 0 on success, -1 if memory ran out (the caller drops the connection).
static int execute_command(HashTable *db, const RespCommand *cmd, RespBuf *out) {
    // NULL BYTES: the database stores C strings, which end at the first '\0'.
    // A value containing '\0' would be cut silently - so refuse it with a clear error instead.
    for (int i = 0; i < cmd->argc; i++) {
        if (memchr(cmd->argv[i], '\0', cmd->argv_len[i]) != NULL) {
            return resp_write_error(out, "null bytes in arguments are not supported");
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
static int process_input(HashTable *db, RespBuf *in, RespBuf *out) {
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
            if (execute_command(db, &cmd, out) == -1) {return -1;}
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

// AUTHOR: Claude (READ_CHUNK, MAX_INPUT_BUFFER, handle_client)
// Bytes read from the socket per read() call.
#define READ_CHUNK 4096

// LIMIT: the biggest legal command is RESP_MAX_ARGS arguments of RESP_MAX_BULK_LEN bytes each, plus headers.
// A client whose unfinished input grows beyond that is broken or malicious - disconnect it,
// otherwise it could make the server buffer memory forever.
#define MAX_INPUT_BUFFER (RESP_MAX_ARGS * (RESP_MAX_BULK_LEN + 32) + 32)

void handle_client(int client_fd, HashTable *db) {
    // BUFFERS: 'in' collects bytes until they form complete commands, 'out' collects replies.
    // Both start empty and grow as needed. OWNER: handle_client - freed at the end.
    RespBuf in = {0};
    RespBuf out = {0};

    // STACK ALLOCATION: temporary landing area for each read().
    char chunk[READ_CHUNK];

    while (1) {
        // SYSTEM CALL: read() - blocks until the client sends something.
        ssize_t n = read(client_fd, chunk, sizeof(chunk));
        if (n < 0 && errno == EINTR) {
            continue;   // interrupted by a signal before any data arrived - just try again
        }
        if (n <= 0) {
            break;      // 0 = the client closed the connection, < 0 = error
        }

        if (resp_buf_append(&in, chunk, (size_t)n) < 0) {
            break;      // out of memory
        }

        // Run all complete commands. Replies pile up in 'out' - one per command, in order.
        int rc = process_input(db, &in, &out);

        // SEND: all replies at once - one send for many pipelined commands.
        // Sent even when rc == -1, so the client gets the "Protocol error" reply before we hang up.
        if (out.len > 0) {
            if (send_all(client_fd, out.data, out.len) < 0) {
                break;  // the client is gone
            }
            out.len = 0;    // keep the memory, reuse it for the next replies
        }

        if (rc < 0 || in.len > MAX_INPUT_BUFFER) {
            break;
        }
    }

    resp_buf_free(&in);
    resp_buf_free(&out);
}
