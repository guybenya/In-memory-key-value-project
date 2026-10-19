# AUTHOR: Claude
# RUN THE WHOLE DEMO, start to finish, and check the result:
#   server -> producer (one job per batch) -> N worker processes -> SQLite -> checks -> anomaly report
#
#     python demo/protein/run_demo.py                      # 4 workers, full data set
#     python demo/protein/run_demo.py --kill-one           # CRASH DEMO: kill -9 one worker mid-job
#
# It starts its own server on a free port, so it never touches a server you are running.
# Output files go to build/demo/ (or --out): the results database and one log file per worker.
#
# CHECKS at the end (exit code 1 if any fails):
#   - STATS: every job acked, none dead, none left ready/reserved
#   - every sequence in the manifest has a result row, and rows == input sequences
# With --kill-one, a killed worker's job must come back after its visibility timeout and be
# finished by another worker; the "Reprocessed batches" query then shows the retry.

import argparse
import os
import signal
import socket
import sqlite3
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, os.path.join(REPO, "client"))

from jobqueue import Client      # noqa: E402  (import after the path change above)
import report                    # noqa: E402

DATA = os.path.join(HERE, "data")
OUT = os.path.join(REPO, "build", "demo")


# AUTHOR: Claude
def free_port():
    """Asks the OS for a free port: binding to port 0 makes the OS pick one."""
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


# AUTHOR: Claude
def start_server(path, port):
    """Starts the queue server on this port and waits until it accepts connections. Returns the process."""
    proc = subprocess.Popen([path, str(port)], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    for _ in range(100):
        try:
            socket.create_connection(("127.0.0.1", port), timeout=1).close()
            return proc
        except ConnectionRefusedError:
            time.sleep(0.05)
    proc.kill()
    raise RuntimeError("server did not start")


# AUTHOR: Claude
def start_worker(i, port, out_dir, db, timeout):
    """Starts demo worker number i as a separate process, logging to <out_dir>/worker-<i>.log."""
    log = open(os.path.join(out_dir, "worker-%d.log" % i), "w")
    cmd = [sys.executable, os.path.join(HERE, "worker.py"), "--port", str(port), "--db", db, "--timeout", str(timeout)]
    return subprocess.Popen(cmd, stdout=log, stderr=subprocess.STDOUT)


# AUTHOR: Claude
def wait_for_jobs(client, queue, workers, kill_one, deadline_sec):
    """Prints progress until no job is ready or reserved. With kill_one, SIGKILLs worker 1 mid-job.

    Worker 1 is killed the first time every worker holds a job (reserved == number of workers),
    so it is certainly in the middle of one. Returns the final STATS dict.
    """
    killed = not kill_one
    start = time.time()
    last_line = None
    while True:
        stats = client.stats(queue)
        line = "  ready %(ready)3d   reserved %(reserved)2d   acked %(acked)3d   dead %(dead)d" % stats
        if line != last_line:
            print("%5.1fs %s" % (time.time() - start, line))
            last_line = line
        if not killed and stats["reserved"] >= len(workers):
            workers[0].send_signal(signal.SIGKILL)
            killed = True
            print("%5.1fs   >>> kill -9 worker 1 (pid %d) while it holds a job <<<" % (time.time() - start, workers[0].pid))
        if stats["ready"] == 0 and stats["reserved"] == 0:
            return stats
        if time.time() - start > deadline_sec:
            raise RuntimeError("jobs did not finish within %d s" % deadline_sec)
        time.sleep(0.2)


# AUTHOR: Claude
def check_results(db, n_sequences, n_jobs, stats, killed_one):
    """Checks that nothing was lost or left over. Prints each check; returns True if all passed.

    With killed_one, also checks that the killed worker's batch was redone by another worker (attempt > 1).
    """
    conn = sqlite3.connect(db)
    rows = conn.execute("SELECT COUNT(*) FROM results").fetchone()[0]
    missing = conn.execute("SELECT COUNT(*) FROM manifest m LEFT JOIN results r USING (seq_index) "
                           "WHERE r.seq_index IS NULL").fetchone()[0]
    retried = conn.execute("SELECT COUNT(DISTINCT batch) FROM results WHERE attempt > 1").fetchone()[0]
    conn.close()
    checks = [
        ("all %d jobs acked" % n_jobs, stats["acked"] == n_jobs),
        ("no dead jobs", stats["dead"] == 0),
        ("result rows (%d) == input sequences (%d)" % (rows, n_sequences), rows == n_sequences),
        ("no sequence missing from the results", missing == 0),
    ]
    if killed_one:
        checks.append(("the killed worker's batch was redone by another worker (%d retried)" % retried, retried >= 1))
    print("\nChecks:")
    for text, ok in checks:
        print("  [%s] %s" % ("OK" if ok else "FAILED", text))
    return all(ok for _, ok in checks)


# AUTHOR: Claude
def main(argv=None):
    """Runs the full demo pipeline once and returns the process exit code (0 = every check passed)."""
    parser = argparse.ArgumentParser(description="Run the protein batch-analysis demo end to end.")
    parser.add_argument("--fasta", nargs="+",
                        default=[os.path.join(DATA, "swissprot_human_3000.fasta"), os.path.join(DATA, "bad_examples.fasta")])
    parser.add_argument("--workers", type=int, default=4)
    parser.add_argument("--batch-size", type=int, default=200)
    parser.add_argument("--timeout", type=int, default=None,
                        help="visibility timeout in seconds (default 60, or 10 with --kill-one so the lost job returns fast)")
    parser.add_argument("--kill-one", action="store_true", help="kill -9 one worker in the middle of a job")
    parser.add_argument("--server", default=os.path.join(REPO, "build", "server"))
    parser.add_argument("--out", default=OUT, help="directory for the results database and worker logs")
    parser.add_argument("--no-report", action="store_true", help="skip printing the anomaly queries")
    args = parser.parse_args(argv)
    timeout = args.timeout or (10 if args.kill_one else 60)

    os.makedirs(args.out, exist_ok=True)
    db = os.path.join(args.out, "results.db")
    for suffix in ("", "-wal", "-shm"):
        if os.path.exists(db + suffix):
            os.remove(db + suffix)

    port = free_port()
    server = start_server(args.server, port)
    workers = []
    try:
        print("server on port %d; %d workers; visibility timeout %d s" % (port, args.workers, timeout))
        out = subprocess.run([sys.executable, os.path.join(HERE, "producer.py"), *args.fasta, "--port", str(port),
                              "--batch-size", str(args.batch_size), "--db", db],
                             check=True, capture_output=True, text=True).stdout
        print(out.strip())
        n_sequences, n_jobs = int(out.split()[1]), int(out.split()[4])

        started = time.time()
        workers = [start_worker(i + 1, port, args.out, db, timeout) for i in range(args.workers)]
        with Client(port=port) as client:
            stats = wait_for_jobs(client, "proteins", workers, args.kill_one, deadline_sec=600)
        elapsed = time.time() - started
        print("\nfinished in %.1f s (%.0f sequences/s)" % (elapsed, n_sequences / elapsed))
    finally:
        for w in workers:
            if w.poll() is None:
                w.send_signal(signal.SIGTERM)       # clean stop: the worker is idle, so it exits at once
        for w in workers:
            try:
                w.wait(timeout=10)
            except subprocess.TimeoutExpired:
                w.kill()
        server.terminate()
        server.wait()

    ok = check_results(db, n_sequences, n_jobs, stats, args.kill_one)
    if not args.no_report:
        report.main([db])
    print("\nresults database: %s   worker logs: %s" % (os.path.relpath(db), os.path.relpath(args.out)))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
