# AUTHOR: Claude
# PRODUCER: reads FASTA files, splits the sequences into batches and pushes one job per batch.
#
#     python demo/protein/producer.py data/swissprot_human_3000.fasta data/bad_examples.fasta \
#         --port 8080 --batch-size 200 --db results.db
#
# Why batches and not one job per sequence: one sequence is ~10 ms of work but every job costs
# network round trips (PUSH, RESERVE, ACK) and a database transaction. A batch of a few hundred
# makes that overhead negligible, while still giving enough jobs to spread over many workers.
#
# Each job's payload is JSON:  {"batch": 3, "records": [{"index": 600, "id": "sp|...", "seq": "MKV..."}, ...]}
# "index" is the sequence's position across all input files; it is the key of its result row.
# The producer also writes the MANIFEST (every index it sent) to the database first, so missing
# results can be found later with a simple SQL join (see anomalies.sql).

import argparse
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "..", "client"))   # the jobqueue library, without installing it

from jobqueue import Client      # noqa: E402  (import after the path change above)
import results_db                # noqa: E402


# AUTHOR: Claude
def read_fasta(path):
    """Reads a FASTA file and yields (id, sequence) pairs, keeping bad sequences exactly as written.

    The id is the first word of the '>' header line. Line breaks and spaces inside a sequence are
    removed (they are only formatting); every other character is kept, so QC can flag it.
    A header with no sequence lines yields an empty sequence - it is not skipped.
    """
    seq_id, parts = None, []
    with open(path) as f:
        for line in f:
            line = line.rstrip("\r\n")
            if line.startswith(">"):
                if seq_id is not None:
                    yield seq_id, "".join(parts)
                header = line[1:].split()
                seq_id, parts = (header[0] if header else ""), []
            elif seq_id is not None:
                parts.append("".join(line.split()))
    if seq_id is not None:
        yield seq_id, "".join(parts)


# AUTHOR: Claude
def make_batches(records, batch_size):
    """Splits a list of records into consecutive lists of at most batch_size records."""
    return [records[i:i + batch_size] for i in range(0, len(records), batch_size)]


# AUTHOR: Claude
def main(argv=None):
    """Reads the input files, writes the manifest, pushes one job per batch and prints a summary."""
    parser = argparse.ArgumentParser(description="Push batches of protein sequences as jobs.")
    parser.add_argument("fasta", nargs="+", help="input FASTA file(s)")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=8080)
    parser.add_argument("--queue", default="proteins")
    parser.add_argument("--batch-size", type=int, default=200)
    parser.add_argument("--db", default="results.db", help="SQLite file for the manifest and the results")
    args = parser.parse_args(argv)

    records = []
    for path in args.fasta:
        for seq_id, seq in read_fasta(path):
            records.append({"index": len(records), "id": seq_id, "seq": seq})
    batches = make_batches(records, args.batch_size)

    # Manifest first: if the producer dies halfway through pushing, the manifest still lists
    # everything that should exist, and the missing-results query shows exactly what is absent.
    conn = results_db.connect(args.db)
    results_db.write_manifest(conn, [(r["index"], r["id"], b) for b, batch in enumerate(batches) for r in batch])
    conn.close()

    with Client(args.host, args.port) as client:
        for b, batch in enumerate(batches):
            client.push(args.queue, json.dumps({"batch": b, "records": batch}))
    print("pushed %d sequences in %d jobs (batch size %d) to queue %r"
          % (len(records), len(batches), args.batch_size, args.queue))
    return len(records), len(batches)


if __name__ == "__main__":
    main()
