# AUTHOR: Claude
# QC (quality control) for one protein sequence: finds problems and FLAGS them, never drops a record.
# Every input sequence gets a result row; bad ones carry flags explaining what is wrong,
# so a data problem is visible in the database instead of silently disappearing.
#
# Per-record flags (decided from one sequence alone):
#   empty                 no residues at all
#   invalid_chars         characters that are not amino-acid letters (digits, '*', '-', ...)
#   nonstandard_residues  letters outside the 20 standard amino acids (X, B, Z, J, U, O)
#   looks_like_dna        90%+ of the letters are A/C/G/T/N - probably a DNA sequence in the wrong file
#   too_short             fewer than MIN_LENGTH residues
#   lowercase_normalized  had lowercase letters; upper-cased before analysis
#
# Duplicates (same sequence, or same id, twice) are NOT checked here: a worker only sees its own
# batch, so a duplicate in another batch is invisible to it. They are found in SQL, where all
# results meet (see anomalies.sql).

STANDARD = frozenset("ACDEFGHIKLMNPQRSTVWY")
NONSTANDARD = frozenset("BJOUXZ")      # IUPAC ambiguity codes and rare residues (selenocysteine U, pyrrolysine O)
NUCLEOTIDES = frozenset("ACGTN")
MIN_LENGTH = 10
DNA_FRACTION = 0.9

# Flags that make the analysis meaningless or impossible: properties and alignment are skipped.
# (Biopython cannot compute the molecular weight of X, and the properties of a DNA string mean nothing.)
BLOCKING_FLAGS = frozenset(["empty", "invalid_chars", "nonstandard_residues", "looks_like_dna"])


# AUTHOR: Claude
def check_sequence(raw):
    """Checks one raw sequence string and returns (clean_sequence, flags).

    clean_sequence is the upper-cased sequence (unchanged otherwise - bad characters are kept,
    so the stored row shows exactly what came in). flags is a sorted list of flag names; empty if all is well.
    """
    flags = set()
    seq = raw.upper()
    if seq != raw:
        flags.add("lowercase_normalized")
    if not seq:
        return seq, ["empty"]
    letters = set(seq)
    # The 20 standard + 6 non-standard letters are exactly A-Z; anything else is invalid.
    if letters - STANDARD - NONSTANDARD:
        flags.add("invalid_chars")
    if letters & NONSTANDARD:
        flags.add("nonstandard_residues")
    if sum(seq.count(n) for n in NUCLEOTIDES) >= DNA_FRACTION * len(seq):
        flags.add("looks_like_dna")
    if len(seq) < MIN_LENGTH:
        flags.add("too_short")
    return seq, sorted(flags)


# AUTHOR: Claude
def is_analyzable(flags):
    """Returns True if none of the flags blocks the analysis (properties + alignment)."""
    return not (set(flags) & BLOCKING_FLAGS)
