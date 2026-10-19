# AUTHOR: Claude
# TESTS for the protein demo (demo/protein/): QC flags, FASTA reading, batching, the analysis,
# idempotent database writes, and one small end-to-end run with a worker killed mid-job.
# Skipped when Biopython is not installed (e.g. plain system Python); CI installs it.

import os
import sqlite3
import sys
import tempfile
import unittest

from server_helper import REPO_ROOT, SERVER_PATH

DEMO = os.path.join(REPO_ROOT, "demo", "protein")
sys.path.insert(0, DEMO)

try:
    import Bio  # noqa: F401
    HAVE_BIOPYTHON = True
except ImportError:
    HAVE_BIOPYTHON = False

import qc            # noqa: E402  (qc, producer and results_db need no Biopython)
import producer      # noqa: E402
import results_db    # noqa: E402

HBA_HUMAN = ("MVLSPADKTNVKAAWGKVGAHAGEYGAEALERMFLSFPTTKTYFPHFDLSHGSAQVKGHGKKVADALTNAVAHVDDMPNALSALSDLHAH"
             "KLRVDPVNFKLLSHCLLVTLAAHLPAEFTPAVHASLDKFLASVSTVLTSKYR")


# AUTHOR: Claude
class QcTest(unittest.TestCase):

    # AUTHOR: Claude
    def test_clean_sequence_has_no_flags(self):
        """Checks a normal protein passes QC untouched."""
        self.assertEqual(qc.check_sequence(HBA_HUMAN), (HBA_HUMAN, []))
        self.assertTrue(qc.is_analyzable([]))

    # AUTHOR: Claude
    def test_each_problem_gets_its_flag(self):
        """Checks every kind of bad input is flagged with the right name."""
        cases = {
            "": ["empty"],
            "MKTAYIAK11QRQISF": ["invalid_chars"],
            "MVLSPADK*TNVKAAWGK": ["invalid_chars"],
            "MVHLTPEE--KSAVTALW": ["invalid_chars"],
            "MGLSDGEWQXVLNVWGKV": ["nonstandard_residues"],
            "MKV": ["too_short"],
            "ATGGTGCTGTCTCCTGCCGACAAG": ["looks_like_dna"],
        }
        for seq, flags in cases.items():
            self.assertEqual(qc.check_sequence(seq)[1], flags, seq)

    # AUTHOR: Claude
    def test_lowercase_is_normalized_and_flagged(self):
        """Checks lowercase is upper-cased, flagged, and still analyzable."""
        seq, flags = qc.check_sequence(HBA_HUMAN.lower())
        self.assertEqual((seq, flags), (HBA_HUMAN, ["lowercase_normalized"]))
        self.assertTrue(qc.is_analyzable(flags))

    # AUTHOR: Claude
    def test_blocking_flags_stop_the_analysis(self):
        """Checks which flags block the analysis: data problems do, a short length does not."""
        self.assertFalse(qc.is_analyzable(["invalid_chars"]))
        self.assertFalse(qc.is_analyzable(["looks_like_dna"]))
        self.assertTrue(qc.is_analyzable(["too_short"]))


# AUTHOR: Claude
class ProducerTest(unittest.TestCase):

    # AUTHOR: Claude
    def test_read_fasta_keeps_bad_records(self):
        """Checks multi-line sequences are joined, spaces dropped, and an empty record is kept, not skipped."""
        with tempfile.NamedTemporaryFile("w", suffix=".fasta", delete=False) as f:
            f.write(">a first record\nMKV\nLLA\n>empty\n>b\nMK 11 V*\n>empty_at_end\n")
        self.addCleanup(os.remove, f.name)
        self.assertEqual(list(producer.read_fasta(f.name)),
                         [("a", "MKVLLA"), ("empty", ""), ("b", "MK11V*"), ("empty_at_end", "")])

    # AUTHOR: Claude
    def test_make_batches_keeps_every_record_in_order(self):
        """Checks batching splits into full batches plus one smaller last batch, losing nothing."""
        batches = producer.make_batches(list(range(7)), 3)
        self.assertEqual(batches, [[0, 1, 2], [3, 4, 5], [6]])


# AUTHOR: Claude
class ResultsDbTest(unittest.TestCase):

    # AUTHOR: Claude
    def setUp(self):
        """Opens a fresh database in a temporary directory."""
        self.dir = tempfile.TemporaryDirectory()
        self.conn = results_db.connect(os.path.join(self.dir.name, "r.db"))

    # AUTHOR: Claude
    def tearDown(self):
        """Closes the database and deletes the directory."""
        self.conn.close()
        self.dir.cleanup()

    # AUTHOR: Claude
    def row(self, index, flags, attempt=1):
        """Builds one minimal result dict for the tests."""
        return {"seq_index": index, "seq_id": "s%d" % index, "length": 5, "seq_sha1": "x", "analyzed": 0,
                "batch": 0, "job_id": 1, "attempt": attempt, "worker": "w", "flags": flags}

    # AUTHOR: Claude
    def counts(self):
        """Returns (number of result rows, number of flag rows)."""
        return (self.conn.execute("SELECT COUNT(*) FROM results").fetchone()[0],
                self.conn.execute("SELECT COUNT(*) FROM qc_flags").fetchone()[0])

    # AUTHOR: Claude
    def test_writing_a_batch_twice_changes_nothing(self):
        """Checks the at-least-once case: the same batch written twice leaves the same rows, no duplicates."""
        batch = [self.row(0, ["too_short"]), self.row(1, [])]
        results_db.write_batch(self.conn, batch)
        results_db.write_batch(self.conn, [self.row(0, ["too_short"], attempt=2), self.row(1, [], attempt=2)])
        self.assertEqual(self.counts(), (2, 1))
        self.assertEqual(self.conn.execute("SELECT MAX(attempt) FROM results").fetchone()[0], 2)

    # AUTHOR: Claude
    def test_rewrite_replaces_old_flags(self):
        """Checks a rewrite removes flags that no longer apply instead of keeping stale ones."""
        results_db.write_batch(self.conn, [self.row(0, ["too_short", "lowercase_normalized"])])
        results_db.write_batch(self.conn, [self.row(0, ["too_short"])])
        flags = [f for (f,) in self.conn.execute("SELECT flag FROM qc_flags")]
        self.assertEqual(flags, ["too_short"])

    # AUTHOR: Claude
    def test_failed_batch_writes_nothing(self):
        """Checks a batch is all-or-nothing: an error in the 2nd row rolls back the 1st row too."""
        bad = self.row(1, [])
        del bad["seq_id"]                                  # NOT NULL column missing -> the insert fails
        with self.assertRaises(sqlite3.IntegrityError):
            results_db.write_batch(self.conn, [self.row(0, []), bad])
        self.assertEqual(self.counts(), (0, 0))


# AUTHOR: Claude
@unittest.skipUnless(HAVE_BIOPYTHON, "Biopython is not installed")
class AnalysisTest(unittest.TestCase):

    # AUTHOR: Claude
    @classmethod
    def setUpClass(cls):
        """Loads the reference set and builds the aligner once."""
        import analysis
        cls.analysis = analysis
        cls.refs = analysis.load_references(os.path.join(DEMO, "data", "references.fasta"))
        cls.aligner = analysis.make_aligner()

    # AUTHOR: Claude
    def test_properties_of_hemoglobin_alpha(self):
        """Checks the properties against the known values for human hemoglobin alpha (UniProt P69905)."""
        p = self.analysis.properties(HBA_HUMAN)
        self.assertAlmostEqual(p["mol_weight"], 15257.5, delta=1)
        self.assertAlmostEqual(p["isoelectric_point"], 8.7, delta=0.1)

    # AUTHOR: Claude
    def test_reference_matches_itself(self):
        """Checks a reference protein finds itself as best match, 100% identical over its full length."""
        m = self.analysis.best_match(HBA_HUMAN, self.refs, self.aligner)
        self.assertEqual((m["best_ref"], m["identity"], m["coverage"]), ("sp|P69905|HBA_HUMAN", 1.0, 1.0))

    # AUTHOR: Claude
    def test_partial_match_has_partial_coverage(self):
        """Checks coverage: hemoglobin glued to an unrelated tail of equal length matches only about half."""
        m = self.analysis.best_match(HBA_HUMAN + "P" * len(HBA_HUMAN), self.refs, self.aligner)
        self.assertEqual(m["best_ref"], "sp|P69905|HBA_HUMAN")
        self.assertTrue(0.45 < m["coverage"] < 0.6, m["coverage"])

    # AUTHOR: Claude
    def test_mutated_copy_still_finds_its_source(self):
        """Checks a copy with every 10th residue changed still matches its source, at about 90% identity."""
        mutated = "".join("W" if i % 10 == 0 else c for i, c in enumerate(HBA_HUMAN))
        m = self.analysis.best_match(mutated, self.refs, self.aligner)
        self.assertEqual(m["best_ref"], "sp|P69905|HBA_HUMAN")
        self.assertTrue(0.85 < m["identity"] < 0.95, m["identity"])


# AUTHOR: Claude
@unittest.skipUnless(HAVE_BIOPYTHON, "Biopython is not installed")
class EndToEndTest(unittest.TestCase):

    # AUTHOR: Claude
    def test_small_run_with_a_killed_worker_loses_nothing(self):
        """Runs the full pipeline on 110 sequences with 2 workers, kills one mid-job, and checks every row arrives."""
        import run_demo
        data = os.path.join(DEMO, "data")
        out = tempfile.TemporaryDirectory()
        self.addCleanup(out.cleanup)
        code = run_demo.main(["--fasta", os.path.join(data, "ci_sample_100.fasta"), os.path.join(data, "bad_examples.fasta"),
                              "--workers", "2", "--batch-size", "20", "--kill-one", "--timeout", "3",
                              "--server", SERVER_PATH, "--out", out.name, "--no-report"])
        self.assertEqual(code, 0)


if __name__ == "__main__":
    unittest.main()
