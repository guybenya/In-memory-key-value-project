# Job Queue Server in C

[![CI](https://github.com/guybenya/In-memory-key-value-project/actions/workflows/ci.yml/badge.svg?branch=main)](https://github.com/guybenya/In-memory-key-value-project/actions/workflows/ci.yml)

A job-queue server written in C, in the style of Beanstalkd. Producers push jobs into named queues, workers
reserve and acknowledge them, and a job whose worker crashes goes back to the queue automatically. The server
also has a small key-value store (`SET` / `GET` / `DEL`), where the project started.

It speaks the Redis protocol, serves many clients at once from a single-threaded `poll()` event loop, and is
tested in CI on Linux and macOS. On top of it there is a [Python client library and worker
framework](#python-client-and-workers), and an [example app](#example-app-batch-analysis-of-protein-sequences)
that analyzes thousands of protein sequences with several workers, survives a worker killed with `kill -9`
in the middle of a job, and stores its results in SQLite.

```
                 PUSH (1 job = 1 batch)        RESERVE / ACK
  producer.py  ------------------------>  server  <------------>  worker.py  x N processes
       |                                 (C, queue)                    |
       +---- manifest ----> SQLite results.db <---- results (1 transaction per batch, then ACK)
                                  |
                           anomalies.sql (data-quality report)
```

## Build and run

Requirements: `clang` and `make` (macOS or Linux). The tests and the Python library also need Python 3.9 or newer.

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
example, insert-or-replace keyed by a stable id, as the [example app](#example-app-batch-analysis-of-protein-sequences) does). A late `ACK` is still accepted and deletes the job, wherever it is.

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

## Python client and workers

`client/jobqueue/` is a small Python library (standard library only, Python 3.9+). It is generic: the
payload is just bytes, and nothing in it knows about proteins.

```python
from jobqueue import Client, Worker

with Client(port=8080) as c:
    job_id = c.push("emails", b"hello")     # -> new job id
    c.stats("emails")                       # -> {"ready": 1, "reserved": 0, "acked": 0, "dead": 0}

def handle(job):                            # job.id, job.payload (bytes), job.attempts
    send_email(job.payload)                 # raise an exception to mark the job as failed

Worker("emails", handle, port=8080, visibility_timeout=60).run()   # Ctrl+C to stop
```

The `Worker` loop is `RESERVE` -> `handle(job)` -> `ACK`:

- **Empty queue:** `RESERVE` does not wait, so the worker sleeps before asking again, starting at 0.1 s
  and doubling up to 2 s while the queue stays empty.
- **Handler raises:** the job is not acked. The server gives it to a worker again when its visibility
  timeout ends, and after 5 attempts it becomes *dead*.
- **Retries:** a job with `attempts > 1` is logged as a retry. An `ACK` that returns `0` is logged as a
  *late ACK*: the job's timeout ran out, and another worker already handled it.
- **Stopping:** the first Ctrl+C (or `SIGTERM`) lets the current job finish and be acked, then the worker
  exits. A second Ctrl+C stops at once, and the unfinished job comes back after its timeout, as after a crash.
- **Lost connection:** the worker reconnects, waiting a little longer after each failed try, and gives up
  after 5 tries.

Because delivery is at least once, a handler must be **idempotent**: running it twice on the same job must
give the same result as running it once.

## Example app: batch analysis of protein sequences

`demo/protein/` uses the library to analyze 3,000 human proteins from
[UniProt Swiss-Prot](https://www.uniprot.org/), plus 10 deliberately bad records (`data/bad_examples.fasta`).

- **Producer** (`producer.py`): reads the FASTA files, writes a *manifest* (every sequence it sends) to
  SQLite, and pushes one job per batch of 200 sequences. A job per sequence would spend most of its time
  on network round trips, because one sequence is only about 10 ms of work.
- **Worker** (`worker.py`, run as several processes): for every sequence in the batch it runs:
  1. **QC** (`qc.py`): flags empty sequences, invalid characters (digits, `*`, `-`), non-standard residues
     (`X`, `B`, `U`, ...), DNA put in the protein file by mistake, very short and lowercase sequences.
     Bad records are **flagged, never dropped**: every input sequence gets a result row.
  2. **Properties** with Biopython: length, molecular weight, isoelectric point, GRAVY and composition.
  3. **Closest reference**: local alignment (BLOSUM62) against 20 well-known proteins, with identity and
     coverage. This is the heavy step, about 2 s of CPU per batch.

  The whole batch is written in **one transaction, before the `ACK`**. If the worker dies between the
  two, the batch is simply written again.
- **Results** (`results_db.py`): tables `manifest`, `results` and `qc_flags`. Writes are idempotent:
  `INSERT OR REPLACE` keyed by the sequence's position in the input, so a batch processed twice leaves
  exactly the same rows. Several worker processes share the file in SQLite's WAL mode.
- **Anomaly queries** (`anomalies.sql`, printed by `report.py`): completeness (sent vs. stored), QC flag
  counts, duplicate sequences and duplicate ids (found in SQL because copies can sit in different batches),
  weight-per-residue and pI outliers, identity and coverage of the closest matches, and batches that were
  written by a retry.

Run it (Biopython is needed only for the demo):

```sh
python3 -m venv .venv && .venv/bin/pip install -r demo/requirements.txt
make demo          # server + producer + 4 workers + checks + report
make crash-demo    # the same, but one worker is killed with kill -9 in the middle of a job
```

`run_demo.py` starts its own server on a free port, waits for the queue to drain, and fails unless every
job is acked, no job is dead, and every sequence in the manifest has a result row. Output of
`make crash-demo` (shortened):

```
pushed 3010 sequences in 16 jobs (batch size 200) to queue 'proteins'
  0.2s   ready  12   reserved  4   acked   0   dead 0
  0.2s   >>> kill -9 worker 1 (pid 4770) while it holds a job <<<
  ...
  8.7s   ready   0   reserved  3   acked  13   dead 0
 10.2s   ready   1   reserved  2   acked  13   dead 0     <- the killed worker's job is back after its 10 s timeout
 12.2s   ready   0   reserved  0   acked  16   dead 0

Checks:
  [OK] all 16 jobs acked
  [OK] no dead jobs
  [OK] result rows (3010) == input sequences (3010)
  [OK] no sequence missing from the results
  [OK] the killed worker's batch was redone by another worker (1 retried)
```

With 4 workers on a 10-core laptop the full run takes about 8.5 s (about 350 sequences per second).

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
| `make python-test` | Python tests: client library, worker framework and protein demo, against the real server.  |
| `make python-test-asan` | The same Python tests against the AddressSanitizer + UBSan server.                     |
| `make demo-ci`  | A small crash demo (110 sequences, 2 workers, one killed); fails if anything is lost.         |

A single test can be run by name, for example `./build/test_database deep_copy`.

The integration test (`tests/integration_test.py`) covers what unit tests cannot see: a command that
arrives one byte at a time, several commands in one write, a value large enough to need many reads, and
the error paths, and many clients at once: 50 clients connected together, a client stuck in the middle of a
command, and a client that does not read its replies, while the others must still be served.

The Python tests (`tests/test_client.py`, `test_worker.py`, `test_demo.py`) include a worker killed with
`SIGKILL` while it holds a job (the job comes back as attempt 2 and is finished by another worker), a clean
stop on `SIGINT`, a job that always fails ending up *dead*, a batch written twice leaving the same rows,
and a failed batch writing nothing. Tests that need Biopython are skipped without it. CI runs all of the
above on every pull request.

## Current limitations and known bugs

This section is kept honest and up to date. Most of it is planned to be fixed in a later stage.

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

Python library and demo:

- **The worker waits for the server.** If the server is down, a worker retries 5 times, waiting longer each
  time, and then exits with an error. Something outside it (a process supervisor) has to restart it.
- **One connection per `Client`, not thread-safe.** Use one `Client` per thread or process. The demo runs
  its workers as separate processes, which also lets them use several CPU cores.
- **Not packaged.** The library is imported from `client/` by a path, not installed with `pip`.
- **The visibility timeout must be longer than the slowest job**, or a slow job is handed out twice. The
  work is not lost, but it is done twice (the result is the same, thanks to idempotent writes). There is
  no way yet for a worker to extend its timeout while it works.
- **SQLite allows one writer at a time.** Workers wait for each other's commit (one short transaction per
  batch). That is fine for a handful of workers; many more would need a database server.
- **The demo's science is simple.** 20 reference proteins instead of a real database search, fixed QC
  rules, and duplicates are reported, not resolved.

## Roadmap

1. **Continuous integration** *(done)*: GitHub Actions runs the build and all checks on Linux and
   macOS for every push and pull request.
2. **Protocol and concurrency** *(done)*: RESP, the Redis wire protocol, and a `poll()` event loop that
   serves many clients at once.
3. **Queue core** *(done)*: commands to push, reserve and acknowledge jobs, with a visibility timeout that
   returns jobs of crashed workers to the queue, and dead jobs after repeated failures.
4. **Python client and worker framework** *(done)*, plus an example app: batch analysis of protein
   sequences with QC flags, Biopython properties, alignment to reference proteins, idempotent SQLite
   results, anomaly queries, and a crash demo. *(Not done: a benchmark graph of throughput against the
   number of workers.)*
5. **Durability:** an append-only log and recovery from it, with a `kill -9` demo where no job is lost.
6. **Packaging:** Docker image published automatically on every release.

Later: job batches with a completion callback, priorities, a dead-letter queue, group commit and replication.
