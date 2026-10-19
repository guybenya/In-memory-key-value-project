# AUTHOR: Claude
# RESULTS DATABASE (SQLite): where the producer records what it sent and the workers store results.
#
# Tables:
#   manifest  - one row per input sequence, written by the producer: the list of what MUST come back.
#   results   - one row per analyzed sequence, written by the workers.
#   qc_flags  - one row per (sequence, flag); a sequence with no problems has no rows here.
#
# IDEMPOTENT WRITES: the queue delivers at least once, so the same batch can be processed twice
# (a worker is killed after writing but before its ACK, or its timeout runs out while it works).
# Every write is keyed by seq_index - the sequence's position in the input, chosen by the producer -
# and uses INSERT OR REPLACE, so writing the same batch twice leaves exactly the same rows.
# seq_index is used instead of the server's job id because a job id is only stable for one push:
# running the producer again gives new job ids, but the same seq_index for the same sequence.
#
# CONCURRENCY: several worker PROCESSES write to one file. WAL mode lets readers work while one
# writer writes; writers still take turns, and busy_timeout makes a writer wait for its turn
# instead of failing at once with "database is locked". Each batch is written in ONE transaction,
# so a batch is either fully stored or not at all - a crash never leaves half a batch.

import sqlite3

SCHEMA = """
CREATE TABLE IF NOT EXISTS manifest (
    seq_index   INTEGER PRIMARY KEY,   -- position in the input (0, 1, 2, ...), set by the producer
    seq_id      TEXT NOT NULL,         -- FASTA id, e.g. sp|P69905|HBA_HUMAN
    batch       INTEGER NOT NULL       -- which job (batch) the sequence was sent in
);
CREATE TABLE IF NOT EXISTS results (
    seq_index         INTEGER PRIMARY KEY,
    seq_id            TEXT NOT NULL,
    length            INTEGER NOT NULL,
    seq_sha1          TEXT NOT NULL,   -- fingerprint of the sequence, for finding duplicates in SQL
    analyzed          INTEGER NOT NULL,-- 1 = properties + alignment computed, 0 = skipped by a blocking QC flag
    mol_weight        REAL,
    isoelectric_point REAL,
    gravy             REAL,
    composition       TEXT,            -- JSON {"A": 8.1, "C": 1.2, ...} percent per amino acid
    best_ref          TEXT,            -- closest reference protein
    align_score       REAL,
    identity          REAL,            -- 0..1, identical positions / aligned columns
    coverage          REAL,            -- 0..1, share of the sequence inside the aligned region
    batch             INTEGER NOT NULL,
    job_id            INTEGER NOT NULL,-- server job id of the delivery that wrote this row
    attempt           INTEGER NOT NULL,-- which delivery of that job wrote it (> 1 = a retry)
    worker            TEXT NOT NULL,
    processed_at      TEXT NOT NULL DEFAULT (datetime('now'))
);
CREATE TABLE IF NOT EXISTS qc_flags (
    seq_index   INTEGER NOT NULL,
    flag        TEXT NOT NULL,
    PRIMARY KEY (seq_index, flag)
);
"""

RESULT_COLUMNS = ["seq_index", "seq_id", "length", "seq_sha1", "analyzed", "mol_weight", "isoelectric_point",
                  "gravy", "composition", "best_ref", "align_score", "identity", "coverage", "batch", "job_id", "attempt",
                  "worker"]


# AUTHOR: Claude
def connect(path):
    """Opens (and creates if needed) the results database, in WAL mode, with the tables in place."""
    # timeout=30: wait up to 30 s for another process's write transaction instead of failing.
    conn = sqlite3.connect(path, timeout=30)
    conn.execute("PRAGMA journal_mode=WAL")
    conn.executescript(SCHEMA)
    return conn


# AUTHOR: Claude
def write_manifest(conn, rows):
    """Records the sequences the producer is sending: rows of (seq_index, seq_id, batch). Idempotent."""
    with conn:
        conn.executemany("INSERT OR REPLACE INTO manifest (seq_index, seq_id, batch) VALUES (?, ?, ?)", rows)


# AUTHOR: Claude
def write_batch(conn, results):
    """Stores one batch of results in a single transaction. Idempotent: rewriting a batch changes nothing.

    results - list of dicts with the RESULT_COLUMNS keys plus "flags" (list of flag names).
    A sequence's old flags are deleted before its new ones are inserted, so a rewrite never
    leaves stale flags behind.
    """
    placeholders = ", ".join("?" for _ in RESULT_COLUMNS)
    sql = "INSERT OR REPLACE INTO results (%s) VALUES (%s)" % (", ".join(RESULT_COLUMNS), placeholders)
    # 'with conn' = one transaction: COMMIT if the block finishes, ROLLBACK if anything raises.
    with conn:
        for r in results:
            conn.execute(sql, [r.get(col) for col in RESULT_COLUMNS])
            conn.execute("DELETE FROM qc_flags WHERE seq_index = ?", (r["seq_index"],))
            conn.executemany("INSERT INTO qc_flags (seq_index, flag) VALUES (?, ?)",
                             [(r["seq_index"], flag) for flag in r["flags"]])
