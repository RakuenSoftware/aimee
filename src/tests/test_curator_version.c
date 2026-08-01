/* test_curator_version.c: curator charter version-bump replay over the shim. */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include <sqlite3.h>

#include "aimee.h"
#include "db2_test_shim.h"
#include "kb_curator_version.h"

static sqlite3 *open_db(void)
{
   db2_test_shim_open();
   sqlite3 *db = (sqlite3 *)db2_test_shim_handle();
   assert(db != NULL);
   return db;
}

static void seed(sqlite3 *db, const char *sql)
{
   assert(sqlite3_exec(db, sql, NULL, NULL, NULL) == SQLITE_OK);
}

static int count(sqlite3 *db, const char *sql)
{
   sqlite3_stmt *st;
   assert(sqlite3_prepare_v2(db, sql, -1, &st, NULL) == SQLITE_OK);
   assert(sqlite3_step(st) == SQLITE_ROW);
   int n = sqlite3_column_int(st, 0);
   sqlite3_finalize(st);
   return n;
}

static void seed_corpus(sqlite3 *db)
{
   seed(db,
        "INSERT INTO projects (name,root,workspace,scanned_at,lifecycle_state,current_generation)"
        " VALUES ('p','/repo/p','/repo','now','current',2)");
   seed(db, "INSERT INTO kb_documents (project,generation,file_path,file_hash,chunk_index,content)"
            " VALUES ('p',2,'f.md','h',0,'text')");
   /* Retained history must not be replayed when the current prompt changes. */
   seed(db, "INSERT INTO kb_documents (project,generation,file_path,file_hash,chunk_index,content)"
            " VALUES ('p',1,'old.md','old-h',0,'old text')");
   seed(db, "INSERT INTO artifacts (id,kind,state,payload)"
            " VALUES ('ds','doc_summary','committed','{\"summary\":\"x\"}')");
}

int main(void)
{
   /* 1. first observation records baselines, no replay. */
   sqlite3 *db = open_db();
   seed_corpus(db);
   kb_curator_version_replay_t r;
   assert(kb_curator_version_replay("p1", "m1", &r) == 0);
   assert(r.prompt_bumped == 0 && r.model_bumped == 0);
   assert(kb_curator_version_replay("p1", "m1", &r) == 0); /* same again: no-op */
   assert(r.prompt_bumped == 0 && r.model_bumped == 0);
   assert(count(db, "SELECT COUNT(*) FROM artifacts WHERE id='ds' AND state='committed'") == 1);
   printf("  baseline + no-op OK\n");

   /* 2. prompt bump re-extracts (re-arms extract_doc), leaves vectors/state. */
   assert(kb_curator_version_replay("p2", "m1", &r) == 0);
   assert(r.prompt_bumped == 1 && r.model_bumped == 0);
   assert(r.docs_reextracted >= 1);
   assert(r.docs_reextracted == 1);
   assert(count(db, "SELECT COUNT(*) FROM kb_async_jobs"
                    " WHERE kind='extract_doc' AND status='pending'") >= 1);
   assert(count(db, "SELECT COUNT(*) FROM artifacts WHERE id='ds' AND state='committed'") == 1);
   printf("  prompt bump re-extracts only OK\n");

   /* 3. model bump re-embeds (committed -> proposed), no re-extraction. */
   assert(kb_curator_version_replay("p2", "m2", &r) == 0);
   assert(r.model_bumped == 1 && r.prompt_bumped == 0);
   assert(r.artifacts_reembedded >= 1);
   assert(count(db, "SELECT COUNT(*) FROM artifacts WHERE id='ds' AND state='proposed'") == 1);
   printf("  model bump re-embeds only OK\n");

   db2_test_shim_close();
   printf("curator_version: all tests passed\n");
   return 0;
}
