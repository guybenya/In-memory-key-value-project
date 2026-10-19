# AUTHOR: Claude
# PROTEIN WORKER: one worker process for the demo. It takes batch jobs from the queue and, for every
# sequence in the batch: QC -> properties -> closest reference by alignment -> one result row.
# The whole batch is then written to SQLite in one transaction, and only after that is the job acked.
#
#     python demo/protein/worker.py --port 8080 --db results.db
#
# Run several of these at once (separate processes, so they use separate CPU cores - Python threads
# would share one core for this CPU-bound work because of the GIL). Stop with Ctrl+C.
#
# ORDER MATTERS: write results, THEN ack. If the worker dies between the two, the job comes back and
# the batch is written again - harmless, because the writes are idempotent (see results_db.py).
# The other order (ack, then write) could lose a batch forever if the worker died in between.

import argparse
import hashlib
import json
import logging
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "..", "client"))   # the jobqueue library, without installing it

from jobqueue import Worker      # noqa: E402  (imports after the path change above)
import analysis                  # noqa: E402
import qc                        # noqa: E402
import results_db                # noqa: E402


# AUTHOR: Claude
def analyze_record(record, references, aligner):
    """Runs QC and (if QC allows) the analysis on one input record. Returns its result dict.

    The dict has the results table's columns that depend on the sequence, plus "flags".
    A record with a blocking flag still gets a row - with analyzed=0 and empty analysis columns.
    """
    seq, flags = qc.check_sequence(record["seq"])
    row = {
        "seq_index": record["index"],
        "seq_id": record["id"],
        "length": len(seq),
        "seq_sha1": hashlib.sha1(seq.encode("utf-8")).hexdigest(),
        "flags": flags,
        "analyzed": 0,
    }
    if qc.is_analyzable(flags):
        row.update(analysis.properties(seq))
        row.update(analysis.best_match(seq, references, aligner))
        row["analyzed"] = 1
    return row


# AUTHOR: Claude
def make_handler(conn, references, worker_name):
    """Builds the job handler: a function that analyzes one batch job and stores its rows.

    It is built here (a closure) so it can use this process's database connection, references
    and aligner, which are created once per process instead of once per job.
    """
    aligner = analysis.make_aligner()

    # AUTHOR: Claude
    def handle(job):
        """Analyzes every sequence of one batch job and writes all rows in one transaction."""
        payload = json.loads(job.payload)
        rows = []
        for record in payload["records"]:
            row = analyze_record(record, references, aligner)
            row.update(batch=payload["batch"], job_id=job.id, attempt=job.attempts, worker=worker_name)
            rows.append(row)
        results_db.write_batch(conn, rows)
        logging.info("%s: job %d (batch %d, attempt %d): %d sequences stored",
                     worker_name, job.id, payload["batch"], job.attempts, len(rows))

    return handle


# AUTHOR: Claude
def main(argv=None):
    """Parses the options, prepares this process's resources and runs the worker loop until stopped."""
    parser = argparse.ArgumentParser(description="Analyze protein batch jobs from the queue.")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=8080)
    parser.add_argument("--queue", default="proteins")
    parser.add_argument("--db", default="results.db")
    parser.add_argument("--refs", default=os.path.join(HERE, "data", "references.fasta"))
    parser.add_argument("--timeout", type=int, default=60,
                        help="visibility timeout in seconds; must be longer than one batch takes")
    parser.add_argument("--max-jobs", type=int, default=None, help="exit after this many jobs (default: run until stopped)")
    args = parser.parse_args(argv)

    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(message)s")
    name = "worker-%d" % os.getpid()
    conn = results_db.connect(args.db)
    handler = make_handler(conn, analysis.load_references(args.refs), name)
    try:
        Worker(args.queue, handler, host=args.host, port=args.port, visibility_timeout=args.timeout,
               max_jobs=args.max_jobs, name=name).run()
    finally:
        conn.close()


if __name__ == "__main__":
    main()
