# In-Memory Key-Value Server (on its way to a job queue)

[![CI](https://github.com/guybenya/In-memory-key-value-project/actions/workflows/ci.yml/badge.svg?branch=main)](https://github.com/guybenya/In-memory-key-value-project/actions/workflows/ci.yml)

A TCP server written in C that stores key-value pairs in memory, using a hash table with separate chaining.

This is the first stage of a larger project: the KV store is going to become the internal storage of a
**durable job-queue server** (in the style of Beanstalkd), with a Python client library and worker framework
on top of it. See the [Roadmap](#roadmap).

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

## Commands

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

## Tests

| Target          | What it does                                                                                  |
|-----------------|-----------------------------------------------------------------------------------------------|
| `make test`     | Builds and runs the unit tests for the hash table and for the RESP parser.                    |
| `make asan`     | Rebuilds the tests with AddressSanitizer and UndefinedBehaviorSanitizer and runs them.        |
| `make leaks`    | Runs the tests under the macOS `leaks` tool and fails if any heap memory is left at exit.     |
| `make check`    | Runs `test` and `asan`, plus `leaks` on macOS. On Linux, `asan` already detects leaks.        |
| `make mutation` | Builds a deliberately broken copy of `database.c` and checks that the tests catch the bug.    |
| `make integration` | Starts the real server on a free port and tests it over TCP, like a real client (Python 3, standard library only). |

A single test can be run by name, for example `./build/test_database deep_copy`.

The integration test (`tests/integration_test.py`) covers what unit tests cannot see: a command that
arrives one byte at a time, several commands in one write, a value large enough to need many reads, and
the error paths. CI runs all of the above on every pull request.

## Current limitations and known bugs

This section is kept honest and up to date. Everything here is planned to be fixed in a later stage.

- **One client at a time.** The server handles a client until it disconnects. A second client waits
  without any reply until then. *(Fix: event loop with `poll()`, stage 2.)*
- **Size limits.** A command can have at most 16 arguments, and each argument at most 1 MB. A client that
  goes over a limit gets a protocol error and is disconnected, so it cannot make the server buffer
  unlimited memory.
- **No null bytes.** The hash table stores C strings, which end at the first null byte, so arguments that
  contain one are rejected with an error (rather than silently cut).
- **RESP only.** Plain-text "inline" commands (what `nc` or `telnet` would send) are not supported and
  get a protocol error. Use `redis-cli` or any Redis client library.
- **No persistence.** All data is lost when the server stops. *(Fix: append-only file, stage 5.)*

## Roadmap

1. **Continuous integration** *(done)*: GitHub Actions runs the build and all checks on Linux and
   macOS for every push and pull request.
2. **Protocol and concurrency** *(in progress)*: RESP, the Redis wire protocol *(done)*, and a `poll()`
   event loop that serves many clients at once.
3. **Queue core:** commands to push, reserve and acknowledge jobs, with a visibility timeout that returns
   jobs of crashed workers to the queue.
4. **Python client and worker framework**, plus an example app: batch analysis of protein sequences.
   A FASTA file is split into jobs of a few hundred sequences each. Workers validate the sequences
   (invalid characters, empty or duplicate entries), compute their properties (length, molecular weight,
   isoelectric point, composition, hydrophobicity), and align each one against a small set of known
   proteins to find its closest match. Results are stored in SQLite and queried with SQL. A benchmark
   shows how throughput changes with the number of workers and with the job size.
5. **Durability:** an append-only log and recovery from it, with a `kill -9` demo where no job is lost.
6. **Packaging:** Docker image published automatically on every release.

Later: job batches with a completion callback, priorities, a dead-letter queue, group commit and replication.
