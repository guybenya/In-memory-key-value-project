# AUTHOR: Claude
# REPORT: runs every query in anomalies.sql against a results database and prints each as a table.
#
#     python demo/protein/report.py results.db
#
# Standard library only (sqlite3), so it works without the sqlite3 command-line tool.

import os
import sqlite3
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
MAX_ROWS = 15     # long result lists are cut, with a note saying how many rows were hidden


# AUTHOR: Claude
def load_queries(path):
    """Splits the .sql file into (title, sql) pairs, using the '-- name:' line that starts each query."""
    queries = []
    for chunk in open(path).read().split("-- name:")[1:]:
        title, _, sql = chunk.partition("\n")
        queries.append((title.strip(), sql.strip()))
    return queries


# AUTHOR: Claude
def format_table(columns, rows):
    """Formats rows as a plain-text table with a header line, each column as wide as its widest value."""
    cells = [[("" if v is None else str(v)) for v in row] for row in rows]
    widths = [max([len(c)] + [len(row[i]) for row in cells]) for i, c in enumerate(columns)]
    lines = ["  ".join(c.ljust(w) for c, w in zip(columns, widths)),
             "  ".join("-" * w for w in widths)]
    lines += ["  ".join(v.ljust(w) for v, w in zip(row, widths)) for row in cells]
    return "\n".join(lines)


# AUTHOR: Claude
def main(argv=None):
    """Opens the database given on the command line and prints the result of every anomaly query."""
    argv = sys.argv[1:] if argv is None else argv
    db = argv[0] if argv else "results.db"
    conn = sqlite3.connect(db)
    for title, sql in load_queries(os.path.join(HERE, "anomalies.sql")):
        cur = conn.execute(sql)
        rows = cur.fetchall()
        print("\n== %s" % title)
        if not rows:
            print("(no rows)")
            continue
        print(format_table([d[0] for d in cur.description], rows[:MAX_ROWS]))
        if len(rows) > MAX_ROWS:
            print("... %d more rows" % (len(rows) - MAX_ROWS))
    conn.close()


if __name__ == "__main__":
    main()
