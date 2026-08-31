#!/usr/bin/env python3
"""Refresh the reviewed signature hashes for the three declarations this change
re-typed. The dispositions are unchanged — each is still a private-db2 memory
declaration retained in DB2 — so only signature_sha256 moves. Run from the repo
root after the headers change, then re-run the ledger check."""
import hashlib
import json
import pathlib

# The ledger's normalized form: tokens separated by single spaces (see
# gen_db2_declaration_ledger.py). Kept verbatim rather than re-derived so this
# script is auditable against the headers by eye.
NEW = {
    "db2_kb_service_memory_context_block_json":
        "cJSON * db2_kb_service_memory_context_block_json ( const char * query , "
        "const char * block_type , int limit , fact_authority_t authority )",
    "db2_kb_service_memory_insert_ex_json":
        "cJSON * db2_kb_service_memory_insert_ex_json ( const char * tier , const char * kind , "
        "const char * key , const char * content , const char * use_cases , double confidence , "
        "const char * session_id , int authority )",
    "db2_memory_row_insert_ex":
        "int64_t db2_memory_row_insert_ex ( const char * tier , const char * kind , "
        "const char * key , const char * content , const char * use_cases , double confidence , "
        "const char * session_id , const char * ts , const char * sensitivity , "
        "double evidence_strength , double salience , double surprise , "
        "const char * provenance_category )",
}

path = pathlib.Path("src/modules/db2/eventcontract/declaration-review.json")
doc = json.loads(path.read_text())
rows = doc["reviews"] if isinstance(doc, dict) else doc
changed = 0
for row in rows:
    want = NEW.get(row["symbol"])
    if not want:
        continue
    digest = hashlib.sha256(want.encode("utf-8")).hexdigest()
    if row["signature_sha256"] != digest:
        row["signature_sha256"] = digest
        changed += 1
        print(f"{row['symbol']} -> {digest}")

path.write_text(json.dumps(doc, indent=2) + "\n")
print(f"updated {changed} review signature(s)")
