/* test_curator_invalidate.c: a changed/removed source doc marks its derived
 * curator artifacts stale (db2_curator_invalidate_doc). */
#include <assert.h>
#include <stdio.h>

#include <sqlite3.h>

#include "aimee.h"
#include "db2_test_shim.h"
#include "db2/kb_payload.h"

static int scalar(sqlite3 *db, const char *sql)
{
   sqlite3_stmt *st;
   assert(sqlite3_prepare_v2(db, sql, -1, &st, NULL) == SQLITE_OK);
   assert(sqlite3_step(st) == SQLITE_ROW);
   int n = sqlite3_column_int(st, 0);
   sqlite3_finalize(st);
   return n;
}

int main(void)
{
   db2_test_shim_open();
   sqlite3 *db = (sqlite3 *)db2_test_shim_handle();
   assert(db != NULL);

   assert(sqlite3_exec(db,
                       "INSERT INTO projects "
                       "(name,root,workspace,scanned_at,lifecycle_state,current_generation)"
                       " VALUES ('p','/repo/p','/repo','now','current',2)",
                       NULL, NULL, NULL) == SQLITE_OK);
   assert(sqlite3_exec(db,
                       "INSERT INTO kb_documents"
                       " (project,generation,file_path,file_hash,chunk_index,content)"
                       " VALUES ('p',2,'f.md','h',0,'text')",
                       NULL, NULL, NULL) == SQLITE_OK);
   int doc_id = scalar(db, "SELECT id FROM kb_documents WHERE project='p' AND file_path='f.md'");

   /* A committed doc_summary citing that document chunk; and an unrelated one. */
   assert(sqlite3_exec(db,
                       "INSERT INTO artifacts (id,kind,state) VALUES"
                       " ('a1','doc_summary','committed'),('a2','doc_summary','committed')",
                       NULL, NULL, NULL) == SQLITE_OK);
   char ins[256];
   snprintf(ins, sizeof(ins),
            "INSERT INTO artifact_citations (artifact_id,source_kind,source_id)"
            " VALUES ('a1','kb_document','%d')",
            doc_id);
   assert(sqlite3_exec(db, ins, NULL, NULL, NULL) == SQLITE_OK);

   int n = db2_curator_invalidate_doc("p", "f.md");
   assert(n >= 1);
   assert(scalar(db, "SELECT COUNT(*) FROM artifacts WHERE id='a1' AND state='stale'") == 1);
   assert(scalar(db, "SELECT COUNT(*) FROM artifacts WHERE id='a2' AND state='committed'") == 1);

   /* Unknown file invalidates nothing. */
   assert(db2_curator_invalidate_doc("p", "other.md") == 0);

   /* The invalidation was recorded as a pollable event. */
   db2_curator_invalidation_t evs[16];
   int ne = db2_curator_invalidations_since(0, evs, 16);
   assert(ne == 1);
   assert(strcmp(evs[0].source_kind, "kb_file") == 0);
   assert(strcmp(evs[0].source_id, "f.md") == 0);
   assert(evs[0].artifacts_stale >= 1);
   /* since=last id returns nothing new. */
   assert(db2_curator_invalidations_since(evs[0].id, evs, 16) == 0);

   db2_test_shim_close();
   printf("  invalidate_doc marks citing artifacts stale OK\n");
   printf("curator_invalidate: all tests passed\n");
   return 0;
}
