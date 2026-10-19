-- AUTHOR: Claude
-- ANOMALY QUERIES over the demo's results database (tables: manifest, results, qc_flags - see results_db.py).
-- Run them all with:  python demo/protein/report.py results.db
-- or one by one in:   sqlite3 results.db
-- Each query starts with a "name" comment line; report.py uses it as the title.

-- name: Completeness - did every sequence the producer sent come back?
-- The manifest lists every sequence that was sent; a LEFT JOIN keeps manifest rows with no result,
-- so "missing" counts sequences that were lost. With at-least-once delivery it must be 0.
SELECT COUNT(*)                  AS sent,
       COUNT(r.seq_index)        AS stored,
       COUNT(*) - COUNT(r.seq_index) AS missing
FROM manifest m
LEFT JOIN results r ON r.seq_index = m.seq_index;

-- name: QC flags - how many sequences have each problem
SELECT flag, COUNT(*) AS sequences
FROM qc_flags
GROUP BY flag
ORDER BY sequences DESC, flag;

-- name: Flagged sequences - each one with all its flags
SELECT r.seq_index, r.seq_id, r.length, r.analyzed, GROUP_CONCAT(f.flag, ', ') AS flags
FROM results r
JOIN qc_flags f ON f.seq_index = r.seq_index
GROUP BY r.seq_index
ORDER BY r.seq_index;

-- name: Duplicate sequences - the same sequence under more than one record
-- Found here and not in the worker: the copies can sit in different batches, handled by different workers.
SELECT seq_sha1, COUNT(*) AS copies, GROUP_CONCAT(seq_id, ' | ') AS records
FROM results
WHERE length > 0
GROUP BY seq_sha1
HAVING COUNT(*) > 1;

-- name: Duplicate ids - the same FASTA id used for more than one record
SELECT seq_id, COUNT(*) AS records, COUNT(DISTINCT seq_sha1) AS different_sequences
FROM results
GROUP BY seq_id
HAVING COUNT(*) > 1;

-- name: Weight outliers - average residue mass more than 3 standard deviations from the mean
-- Molecular weight alone mostly follows length, so compare weight PER RESIDUE (mol_weight / length):
-- normal proteins are ~110 Da per residue; far from that means an unusual composition (e.g. glycine-rich
-- keratin-associated proteins, mucins) - or a data error worth a look.
-- SQLite has no STDEV(), so compare squares: (x - mean)^2 > 9 * variance, where
-- variance = mean(x^2) - mean(x)^2. Same test as |x - mean| > 3 * stdev, without a square root.
WITH x AS (
    SELECT seq_id, length, mol_weight, mol_weight / length AS per_residue
    FROM results WHERE analyzed = 1
),
s AS (
    SELECT AVG(per_residue) AS mean, AVG(per_residue * per_residue) - AVG(per_residue) * AVG(per_residue) AS var
    FROM x
)
SELECT x.seq_id, x.length, x.mol_weight, ROUND(x.per_residue, 1) AS da_per_residue, ROUND(s.mean, 1) AS mean
FROM x, s
WHERE (x.per_residue - s.mean) * (x.per_residue - s.mean) > 9 * s.var
ORDER BY x.per_residue;

-- name: Isoelectric point extremes - pI below 4.2 (very acidic) or above 11.5 (very basic)
SELECT seq_id, length, isoelectric_point, gravy
FROM results
WHERE analyzed = 1 AND (isoelectric_point < 4.2 OR isoelectric_point > 11.5)
ORDER BY isoelectric_point;

-- name: Identity to the closest reference - distribution
-- Most proteins are unrelated to the 20 references, so low identity is normal here; the strong
-- matches are the references themselves and their close relatives. A match covering less than half
-- of the sequence is counted apart: high identity over a short region does not make two proteins related.
SELECT CASE
         WHEN coverage < 0.5  THEN '5) covers < 50% of the sequence'
         WHEN identity >= 0.9 THEN '1) >= 90% (same protein / very close)'
         WHEN identity >= 0.5 THEN '2) 50-90% (clear homolog)'
         WHEN identity >= 0.3 THEN '3) 30-50% (possible homolog)'
         ELSE                      '4) < 30% (no meaningful match)'
       END AS identity_range,
       COUNT(*) AS sequences
FROM results
WHERE analyzed = 1
GROUP BY identity_range
ORDER BY identity_range;

-- name: Strong matches - at least 50% identical to a reference, over at least half of the sequence
-- align_score >= 50 drops matches too short to mean anything (e.g. a 3-residue sequence matching 2 residues).
SELECT seq_id, best_ref, ROUND(identity * 100, 1) AS identity_pct, ROUND(coverage * 100, 1) AS coverage_pct,
       align_score
FROM results
WHERE analyzed = 1 AND identity >= 0.5 AND coverage >= 0.5 AND align_score >= 50
ORDER BY identity DESC, align_score DESC;

-- name: Reprocessed batches - rows written by a retry (attempt > 1), e.g. after a worker was killed
SELECT batch, job_id, attempt, worker, COUNT(*) AS rows
FROM results
WHERE attempt > 1
GROUP BY batch, job_id, attempt, worker
ORDER BY batch;
