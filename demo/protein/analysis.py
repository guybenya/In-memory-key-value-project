# AUTHOR: Claude
# ANALYSIS of one clean protein sequence, with Biopython:
#   1. physical/chemical properties (length, molecular weight, isoelectric point, GRAVY, composition)
#   2. the closest protein in a small reference set, by local pairwise alignment (a "mini-BLAST")
#
# Step 2 is the heavy part on purpose: about 10 ms per sequence against 20 references, so a batch of
# 200 sequences is ~2 s of CPU. That makes the queue's per-job overhead (a few network round trips)
# tiny next to the work, which is when spreading jobs over many workers pays off.

import json

from Bio import Align, SeqIO
from Bio.Align import substitution_matrices
from Bio.SeqUtils.ProtParam import ProteinAnalysis


# AUTHOR: Claude
def make_aligner():
    """Builds the aligner used for every comparison: local alignment, BLOSUM62 scores, affine gaps.

    Local (Smith-Waterman) finds the best-matching REGION, so a protein that shares one domain
    with a reference still scores well. Gap open -10 / extend -0.5 are the usual protein defaults.
    """
    aligner = Align.PairwiseAligner()
    aligner.mode = "local"
    aligner.substitution_matrix = substitution_matrices.load("BLOSUM62")
    aligner.open_gap_score = -10
    aligner.extend_gap_score = -0.5
    return aligner


# AUTHOR: Claude
def load_references(path):
    """Reads the reference FASTA file into a list of (id, sequence string) pairs."""
    return [(rec.id, str(rec.seq)) for rec in SeqIO.parse(path, "fasta")]


# AUTHOR: Claude
def properties(seq):
    """Computes the physical/chemical properties of a clean sequence (only the 20 standard letters).

    Returns a dict: molecular weight (Da), isoelectric point (pI), GRAVY (average
    hydropathy: > 0 hydrophobic, < 0 hydrophilic) and amino-acid composition (JSON text, percent per letter).
    """
    pa = ProteinAnalysis(seq)
    composition = {aa: round(pct, 2) for aa, pct in sorted(pa.amino_acids_percent.items())}
    return {
        "mol_weight": round(pa.molecular_weight(), 2),
        "isoelectric_point": round(pa.isoelectric_point(), 3),
        "gravy": round(pa.gravy(), 4),
        "composition": json.dumps(composition),
    }


# AUTHOR: Claude
def best_match(seq, references, aligner):
    """Finds the reference most similar to seq and how similar it is.

    Scores seq against every reference (score only - fast, no alignment is built), then builds
    the full alignment just for the winner to count identical positions.
    Returns a dict: best_ref (id), align_score, identity (identical / aligned columns incl. gaps, 0..1),
    and coverage (the share of seq inside the aligned region, 0..1). Coverage matters because a local
    alignment can be a tiny region: 8 of 12 residues identical is "67% identity" but means nothing
    for a 300-residue protein.
    """
    best_id, best_seq, best_score = None, None, float("-inf")
    for ref_id, ref_seq in references:
        score = aligner.score(seq, ref_seq)
        if score > best_score:
            best_id, best_seq, best_score = ref_id, ref_seq, score
    alignment = aligner.align(seq, best_seq)[0]
    counts = alignment.counts()
    columns = counts.identities + counts.mismatches + counts.gaps
    identity = counts.identities / columns if columns else 0.0
    # coordinates[0] holds the alignment's positions in seq: first and last = where the region starts and ends.
    start, end = alignment.coordinates[0][0], alignment.coordinates[0][-1]
    coverage = (end - start) / len(seq)
    return {"best_ref": best_id, "align_score": round(best_score, 1), "identity": round(identity, 4),
            "coverage": round(float(coverage), 4)}
