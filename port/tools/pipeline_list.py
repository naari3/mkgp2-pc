#!/usr/bin/env python3
"""Merge Aurora pipeline lists into one file the game can be shipped with.

Aurora records every pipeline configuration (the GX state a draw was made
with) it meets in pipeline_cache.db of the user directory and compiles the
recorded ones on its pipeline thread at the next start. At every start it
also merges `initial_pipeline_cache.db` from the executable's folder into
that list. This script builds such a file from any number of recorded lists,
so that a machine that has never run the game compiles everything known
before the first race instead of at first sight (docs/port/census.md
section 36).

    python3 port/tools/pipeline_list.py -o initial_pipeline_cache.db LIST...
    python3 port/tools/pipeline_list.py --count LIST...

LIST is a pipeline_cache.db (its -wal next to it is read too) or an earlier
output of this script. Rows are keyed by (type, hash); the hash is of the
configuration bytes, the same on Windows and Linux (checked 2026-11-12: 145
of 163 rows of a Linux replay were in a Windows list, byte for byte). Rows of
another configuration version than the newest one seen are left out: Aurora
would skip them. The output is a plain (not WAL) database, as Aurora's
read-only loader needs.
"""
import argparse
import os
import sqlite3
import sys

SCHEMA = """
CREATE TABLE aurora_schema(value INTEGER);
CREATE TABLE pipeline_cache (
  type INTEGER NOT NULL,
  hash INTEGER NOT NULL,
  config_version INTEGER NOT NULL,
  config_size INTEGER NOT NULL,
  config BLOB NOT NULL,
  first_frame_used INTEGER NOT NULL,
  PRIMARY KEY (type, hash)
);
"""


def read(path):
    db = sqlite3.connect(f"file:{path}?mode=ro", uri=True)
    try:
        schema = [r[0] for r in db.execute("SELECT value FROM aurora_schema")]
        rows = db.execute(
            "SELECT type, hash, config_version, config_size, config, first_frame_used FROM pipeline_cache"
        ).fetchall()
    finally:
        db.close()
    return schema, rows


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("lists", nargs="+")
    ap.add_argument("-o", "--output")
    ap.add_argument("--count", action="store_true", help="only print what each list holds")
    args = ap.parse_args()

    merged = {}
    schemas = set()
    for path in args.lists:
        try:
            schema, rows = read(path)
        except sqlite3.Error as e:
            print(f"{path}: not readable ({e})", file=sys.stderr)
            continue
        schemas.update(schema)
        new = 0
        for type_, hash_, version, size, config, first in rows:
            key = (type_, hash_)
            old = merged.get(key)
            if old is None:
                new += 1
                merged[key] = (version, size, config, first)
            elif version > old[0] or (version == old[0] and first < old[3]):
                merged[key] = (version, size, config, min(first, old[3]))
        print(f"{path}: {len(rows)} rows, {new} new")

    newest = {}
    for (type_, _), (version, *_rest) in merged.items():
        newest[type_] = max(newest.get(type_, 0), version)
    keep = {k: v for k, v in merged.items() if v[0] == newest[k[0]]}
    print(f"total {len(keep)} rows (types and versions: "
          + ", ".join(f"type {t} v{v}: {sum(1 for k in keep if k[0] == t)}" for t, v in sorted(newest.items()))
          + f"; {len(merged) - len(keep)} of older versions left out)")
    if args.count or not args.output:
        return 0
    if len(schemas) != 1:
        print(f"the lists do not agree on Aurora's schema number: {sorted(schemas)}", file=sys.stderr)
        return 1
    if os.path.exists(args.output):
        os.remove(args.output)
    out = sqlite3.connect(args.output)
    out.executescript(SCHEMA)
    out.execute("INSERT INTO aurora_schema VALUES (?)", (schemas.pop(),))
    out.executemany(
        "INSERT INTO pipeline_cache VALUES (?, ?, ?, ?, ?, ?)",
        [(t, h, v, s, c, f) for (t, h), (v, s, c, f) in sorted(keep.items(), key=lambda kv: kv[1][3])],
    )
    out.commit()
    out.execute("VACUUM")
    out.close()
    print(f"wrote {args.output} ({os.path.getsize(args.output)} bytes)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
