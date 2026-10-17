# In-Memory Key-Value Server (on its way to a job queue)

A TCP server written in C that stores key-value pairs in memory, using a hash table with separate chaining.

This is the first stage of a larger project: the KV store is going to become the internal storage of a
**durable job-queue server** (in the style of Beanstalkd), with a Python client library and worker framework
on top of it. See the [Roadmap](#roadmap).

## Build and run

Requirements: `clang` and `make` (macOS or Linux).

```sh
make all            # builds build/server
./build/server      # listens on port 8080
```

Connect from another terminal with `nc`:

```sh
nc localhost 8080
```

## Commands

Commands are plain text, one per line. Keys and values are single words.

| Command             | Reply on success          | Reply on failure                     |
|---------------------|---------------------------|--------------------------------------|
| `SET <key> <value>` | `OK - Saved to database`  | `ERROR - Usage: SET <key> <value>`   |
| `GET <key>`         | `VALUE: <value>`          | `ERROR - Key not found`              |
| `DEL <key>`         | `OK - Deleted`            | `ERROR - Key not found`              |

Any other command gets `ERROR - Unknown command`. `SET` on an existing key replaces its value.

Example session:

```
SET name guy
OK - Saved to database
GET name
VALUE: guy
DEL name
OK - Deleted
GET name
ERROR - Key not found
```

## Tests

| Target          | What it does                                                                                  |
|-----------------|-----------------------------------------------------------------------------------------------|
| `make test`     | Builds and runs the unit tests for the hash table (`tests/test_database.c`).                  |
| `make asan`     | Rebuilds the tests with AddressSanitizer and UndefinedBehaviorSanitizer and runs them.        |
| `make leaks`    | Runs the tests under the macOS `leaks` tool and fails if any heap memory is left at exit.     |
| `make check`    | Runs `test`, `asan` and `leaks` together. Run it before every commit.                         |
| `make mutation` | Builds a deliberately broken copy of `database.c` and checks that the tests catch the bug.    |

A single test can be run by name, for example `./build/test_database deep_copy`.

## Current limitations and known bugs

This section is kept honest and up to date. Everything here is planned to be fixed in a later stage.

- **One client at a time.** The server handles a client until it disconnects. A second client waits
  without any reply until then. *(Fix: event loop with `poll()`, stage 2.)*
- **One `read()` is treated as one command.** TCP is a byte stream, so two commands that arrive together
  are read as one, and only the first is executed. For example, sending `SET a 1\nSET b 2\n` in one write
  stores only `a`. A command split across two reads is also broken. *(Fix: RESP parser with proper
  buffering, stage 2.)*
- **Values cannot contain spaces.** Words after the value are silently ignored: `SET k hello world`
  stores `hello`. *(Fix: RESP bulk strings, stage 2.)*
- **Size limits.** A command longer than 1023 bytes is cut, and a `GET` reply longer than 1023 bytes is
  truncated.
- **No persistence.** All data is lost when the server stops. *(Fix: append-only file, stage 5.)*
- **Fixed port.** The port (8080) is hard-coded in `src/main.c`.
- **`make check` works only on macOS** because it uses the `leaks` tool. *(Fix: in progress, stage 1.)*

## Roadmap

1. **Continuous integration** *(in progress)*: GitHub Actions runs the build and all checks on Linux and
   macOS for every push and pull request.
2. **Protocol and concurrency:** RESP, the Redis wire protocol, and a `poll()` event loop that serves many
   clients at once.
3. **Queue core:** commands to push, reserve and acknowledge jobs, with a visibility timeout that returns
   jobs of crashed workers to the queue.
4. **Python client and worker framework**, plus an example batch-processing app that shows throughput
   growing as workers are added.
5. **Durability:** an append-only log and recovery from it, with a `kill -9` demo where no job is lost.
6. **Packaging:** Docker image published automatically on every release.

Later: job batches with a completion callback, priorities, a dead-letter queue, group commit and replication.
