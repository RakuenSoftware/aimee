#!/usr/bin/env python3
"""Embed tier schema SQL as C string constants.

Emits one static const char * per schema file so each tier's db_schema.c
can apply its schema directly through a single idempotent
CREATE-IF-NOT-EXISTS pass. See
docs/STORAGE_TIERS.md.
"""
import json
import sys
from pathlib import Path

OUT = sys.argv[1] if len(sys.argv) > 1 else "schema_data.h"
ROOT = Path(__file__).resolve().parent

SCHEMAS = [
    ("db1/schema.sql", "AIMEE_DB1_SCHEMA_SQL"),
    ("db2/schema.sql", "AIMEE_DB2_SCHEMA_SQL"),
    ("db2/schema_sqlite.sql", "AIMEE_DB2_SCHEMA_SQLITE_SQL"),
]

lines = [
    "/* Auto-generated from tier schema SQL — do not edit directly. */",
]
for path, symbol in SCHEMAS:
    with open(ROOT / path, "r", encoding="utf-8") as fh:
        text = fh.read()
    lines.append(f"static const char *{symbol} __attribute__((unused)) = {json.dumps(text)};")

with open(OUT, "w", encoding="utf-8") as fh:
    fh.write("\n".join(lines) + "\n")
