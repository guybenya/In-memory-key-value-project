# Job Queue Server in C

[![CI](https://github.com/guybenya/In-memory-key-value-project/actions/workflows/ci.yml/badge.svg?branch=main)](https://github.com/guybenya/In-memory-key-value-project/actions/workflows/ci.yml)

A job-queue server written in C, in the style of Beanstalkd. Producers push jobs into named queues, workers
reserve and acknowledge them, and a job whose worker crashes goes back to the queue automatically. The server
also has a small key-value store (`SET` / `GET` / `DEL`), where the project started.

It speaks the Redis protocol, serves many clients at once from a single-threaded `poll()` event loop, and is
tested in CI on Linux and macOS. A Python client library, a worker framework and an example app (batch
analysis of protein sequences) are next. See the [Roadmap](#roadmap).

## Build and run

Requirements: `clang` and `make` (macOS or Linux).

```sh
make all              # builds build/server
./build/server        # listens on port 8080
./build/server 9000   # or on any port from 1 to 65535
```

The server speaks [RESP](https://redis.io/docs/latest/develop/reference/protocol-spec/), the Redis wire
protocol, so any Redis client can talk to it. Connect from another terminal with `redis-cli`:

```sh
redis-cli -p 8080
```

## Job queue

| Command                     | Reply                                                                      |
|-----------------------------|----------------------------------------------------------------------------|
| `PUSH <queue> <payload>`    | The new job's id. Adds the job to the end of the queue.                    |
| `RESERVE <queue> <seconds>` | `[id, payload, attempts]` for the oldest waiting job, or `(nil)` if none.  |
| `ACK <id>`                  | `1` if the job was deleted, `0` if there is no job with that id.           |
| `STATS <queue>`             | Counts: `ready`, `reserved`, `acked` and `dead`.                           |

A queue is created by its first `PUSH`. Queue names are 1 to 64 bytes, and payloads may contain any bytes.

**The life of a job:**

1. `PUSH` puts the job at the end of the queue (*ready*).
2. `RESERVE` hands the oldest ready job to a worker and hides it from all other workers for the given
   number of seconds, the *visibility timeout* (*reserved*). `attempts` counts how many times the job has
   been reserved, so a worker can tell a retry from a first try.
3. When the worker finishes, it sends `ACK`, and the job is deleted.
4. If the worker does not `ACK` in time, for example because it crashed or hung, the server puts the job
   back at the end of the queue and another worker gets it. The server checks deadlines ten times a second,
   even when no client sends anything.
5. A job that has been reserved 5 times without an `ACK` is moved to *dead* and not handed out again, so
   one job that crashes every worker cannot circulate forever.

**Delivery guarantee: at least once.** No job is lost when a worker crashes, but a job can be processed
twice: a worker that is slower than its timeout loses the job to another worker, and both finish it.
Workers should therefore write their results so that writing the same result twice does no harm (for
example, insert-or-replace by job id). A late `ACK` is still accepted and deletes the job, wherever it is.

Example session with a producer and a worker:

```
127.0.0.1:8080> PUSH proteins "batch-001.fasta"
(integer) 1
127.0.0.1:8080> RESERVE proteins 30
1) (integer) 1
2) "batch-001.fasta"
3) (integer) 1
127.0.0.1:8080> ACK 1
(integer) 1
127.0.0.1:8080> RESERVE proteins 30
(nil)
```

## Key-value commands

Command names are case-insensitive (`get` works like `GET`). Keys and values may contain any bytes,
including spaces and newlines, except the null byte.

| Command                 | Reply                                                              |
|-------------------------|--------------------------------------------------------------------|
| `PING`                  | `PONG`                                                             |
| `SET <key> <value>`     | `OK`. Replaces the value if the key already exists.                |
| `GET <key>`             | The value, or `(nil)` if the key does not exist.                   |
| `DEL <key> [<key> ...]` | The number of keys that existed and were deleted.                  |

Errors are standard RESP error replies: `ERR unknown command '<name>'`, `ERR wrong number of arguments for
'<command>' command`, and `ERR null bytes in arguments are not supported`. After an error the connection
stays open. The one exception is malformed RESP input, which gets `ERR Protocol error` and then the server
closes the connection, because it can no longer tell where the next command starts.

Example session:

```
127.0.0.1:8080> PING
PONG
127.0.0.1:8080> SET name guy
OK
127.0.0.1:8080> GET name
"guy"
127.0.0.1:8080> DEL name missing
(integer) 1
127.0.0.1:8080> GET name
(nil)
```

Several commands can be sent in one write (pipelining). They are executed in order and all replies are
sent back together.

## How it works

The server serves many clients at once with a single thread and a `poll()` event loop:

1. `poll()` sleeps until at least one socket is ready: a new connection is waiting, a client sent data,
   or a client that had replies waiting can receive more.
2. For each ready client, the server reads one chunk into that client's **input buffer**, executes every
   complete command in it, and appends the replies to the client's **output buffer**. A partial command
   stays in the input buffer until the rest arrives.
3. Replies are sent right away. If the client's socket is full, the rest stays in the output buffer and is
   sent when `poll()` reports the socket writable again.

All sockets are non-blocking, so a slow or stuck client never holds up the others. A client that sends
commands without reading the replies stops being read until it catches up (backpressure), so it cannot make
the server pile up replies in memory.

To watch this happen, start the server in trace mode. Every event is printed to stderr:

```sh
KV_TRACE=1 ./build/server
```

Output for `redis-cli -p 8080 SET name guy`:

```
[trace] poll: 1 socket(s) ready
[trace] fd 4: client connected (1 clients)
[trace] poll: 1 socket(s) ready
[trace] fd 4: read 32 bytes
[trace]     execute SET (3 args)
[trace] fd 4: sent 5 bytes
[trace] poll: 1 socket(s) ready
[trace] fd 4: closed (client disconnected)
```

## Tests

| Target          | What it does                                                                                  |
|-----------------|-----------------------------------------------------------------------------------------------|
| `make test`     | Builds and runs the unit tests for the hash table, the RESP parser and the job queue.         |
| `make asan`     | Rebuilds the tests with AddressSanitizer and UndefinedBehaviorSanitizer and runs them.        |
| `make leaks`    | Runs the tests under the macOS `leaks` tool and fails if any heap memory is left at exit.     |
| `make check`    | Runs `test` and `asan`, plus `leaks` on macOS. On Linux, `asan` already detects leaks.        |
| `make mutation` | Builds a deliberately broken copy of `database.c` and checks that the tests catch the bug.    |
| `make integration` | Starts the real server on a free port and tests it over TCP, like a real client (Python 3, standard library only). |
| `make integration-asan` | The same integration test against a server built with AddressSanitizer and UBSan.      |

A single test can be run by name, for example `./build/test_database deep_copy`.

The integration test (`tests/integration_test.py`) covers what unit tests cannot see: a command that
arrives one byte at a time, several commands in one write, a value large enough to need many reads, and
the error paths, and many clients at once: 50 clients connected together, a client stuck in the middle of a
command, and a client that does not read its replies, while the others must still be served. CI runs all of
the above on every pull request.

## Current limitations and known bugs

This section is kept honest and up to date. Everything here is planned to be fixed in a later stage.

- **At most 1024 clients at once.** Further connections are accepted and closed right away.
- **One thread.** Commands run one after another. Every command is fast (a hash-table lookup), so this is
  not a bottleneck today, but a slow command would delay all clients. It also means one CPU core.
- **No idle timeout.** A client that connects and sends nothing keeps its slot until it disconnects.
- **Size limits.** A command can have at most 16 arguments, and each argument at most 1 MB. A client that
  goes over a limit gets a protocol error and is disconnected, so it cannot make the server buffer
  unlimited memory.
- **No null bytes in keys and values.** The hash table stores C strings, which end at the first null byte,
  so `SET`, `GET` and `DEL` reject arguments that contain one (rather than silently cutting them). Queue
  payloads do not have this limit.
- **`RESERVE` does not wait.** On an empty queue it answers `(nil)` right away, and the worker has to ask
  again a little later. *(Planned: a blocking `RESERVE` that holds the request until a job arrives.)*
- **A disconnected worker's job waits for its timeout.** When a worker's connection closes, its reserved job
  is not returned at once, only when the visibility timeout ends.
- **Dead jobs can only be counted.** There is no command yet to list, retry or delete them, and the limit
  of 5 attempts is fixed.
- **Queues are never deleted**, even when they are empty.
- **RESP only.** Plain-text "inline" commands (what `nc` or `telnet` would send) are not supported and
  get a protocol error. Use `redis-cli` or any Redis client library.
- **No persistence.** All data is lost when the server stops. *(Fix: append-only file, stage 5.)*

## Roadmap

1. **Continuous integration** *(done)*: GitHub Actions runs the build and all checks on Linux and
   macOS for every push and pull request.
2. **Protocol and concurrency** *(done)*: RESP, the Redis wire protocol, and a `poll()` event loop that
   serves many clients at once.
3. **Queue core** *(done)*: commands to push, reserve and acknowledge jobs, with a visibility timeout that
   returns jobs of crashed workers to the queue, and dead jobs after repeated failures.
4. **Python client and worker framework**, plus an example app: batch analysis of protein sequences.
   A FASTA file is split into jobs of a few hundred sequences each. Workers validate the sequences
   (invalid characters, empty or duplicate entries), compute their properties (length, molecular weight,
   isoelectric point, composition, hydrophobicity), and align each one against a small set of known
   proteins to find its closest match. Results are stored in SQLite and queried with SQL. A benchmark
   shows how throughput changes with the number of workers and with the job size.
5. **Durability:** an append-only log and recovery from it, with a `kill -9` demo where no job is lost.
6. **Packaging:** Docker image published automatically on every release.

Later: job batches with a completion callback, priorities, a dead-letter queue, group commit and replication.
