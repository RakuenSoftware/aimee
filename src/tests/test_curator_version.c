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

/* Restore the seeded artifact to committed so the next case starts from the same state a
 * real boot would: a re-embed leaves it proposed, and asserting "went to proposed"
 * against a row that was already proposed proves nothing. */
static void recommit(sqlite3 *db)
{
   seed(db, "UPDATE artifacts SET state='committed' WHERE id='ds'");
}

static void clear_extract_jobs(sqlite3 *db)
{
   seed(db, "DELETE FROM kb_async_jobs WHERE kind='extract_doc'");
}

int main(void)
{
   /* 1. first observation records baselines, no replay. */
   sqlite3 *db = open_db();
   seed_corpus(db);
   kb_curator_version_replay_t r;
   assert(kb_curator_version_replay("p1", "m1", "synth-a", "embed-a/aaaa", &r) == 0);
   assert(r.prompt_bumped == 0 && r.model_bumped == 0);
   /* same again: no-op */
   assert(kb_curator_version_replay("p1", "m1", "synth-a", "embed-a/aaaa", &r) == 0);
   assert(r.prompt_bumped == 0 && r.model_bumped == 0);
   assert(count(db, "SELECT COUNT(*) FROM artifacts WHERE id='ds' AND state='committed'") == 1);
   printf("  baseline + no-op OK\n");

   /* 2. prompt bump re-extracts (re-arms extract_doc), leaves vectors/state. */
   assert(kb_curator_version_replay("p2", "m1", "synth-a", "embed-a/aaaa", &r) == 0);
   assert(r.prompt_bumped == 1 && r.model_bumped == 0);
   assert(r.docs_reextracted >= 1);
   assert(r.docs_reextracted == 1);
   assert(count(db, "SELECT COUNT(*) FROM kb_async_jobs"
                    " WHERE kind='extract_doc' AND status='pending'") >= 1);
   assert(count(db, "SELECT COUNT(*) FROM artifacts WHERE id='ds' AND state='committed'") == 1);
   printf("  prompt bump re-extracts only OK\n");

   /* 3. model bump re-embeds (committed -> proposed), no re-extraction. */
   assert(kb_curator_version_replay("p2", "m2", "synth-a", "embed-a/aaaa", &r) == 0);
   assert(r.model_bumped == 1 && r.prompt_bumped == 0);
   assert(r.artifacts_reembedded >= 1);
   assert(count(db, "SELECT COUNT(*) FROM artifacts WHERE id='ds' AND state='proposed'") == 1);
   printf("  model bump re-embeds only OK\n");

   /* 4. A NEW SYNTHESIS MODEL re-extracts, with every declared version unchanged.
    *
    * This is the case that did not work. Extraction output is the synthesis model's
    * output, so swapping the model leaves every summary, claim and open question as the
    * previous model's work -- still committed, still cited as current. Nobody bumps a
    * prompt version when they pick a different model in the wizard, so keying on the
    * label alone meant the old model's artifacts simply stayed. */
   recommit(db);
   clear_extract_jobs(db);
   assert(kb_curator_version_replay("p2", "m2", "synth-B", "embed-a/aaaa", &r) == 0);
   assert(r.prompt_bumped == 1);
   assert(r.docs_reextracted == 1);
   assert(count(db, "SELECT COUNT(*) FROM kb_async_jobs"
                    " WHERE kind='extract_doc' AND status='pending'") >= 1);
   /* Extraction only. A different synthesis model does not move the vector space. */
   assert(r.model_bumped == 0);
   assert(count(db, "SELECT COUNT(*) FROM artifacts WHERE id='ds' AND state='committed'") == 1);
   printf("  new synthesis model re-extracts OK\n");

   /* 5. A NEW EMBEDDER SERVING IDENTITY re-embeds AND re-extracts, with every declared
    * version unchanged. This is the embedder-switch contract: drop, re-index, re-embed
    * and re-synthesise.
    *
    * Re-extraction is the part worth stating, since extraction is an LLM pass and a
    * vector space looks unrelated to it. What the pass reads is retrieved, and retrieval
    * IS that vector space, so artifacts synthesised over the old embedder's neighbours
    * are conclusions drawn from different inputs than the same query returns now.
    * Re-embedding alone would preserve the old conclusions at new coordinates.
    *
    * The identity rather than the embedder's name or width, because a pooling or prefix
    * change keeps both and still produces a different space. */
   recommit(db);
   clear_extract_jobs(db);
   assert(kb_curator_version_replay("p2", "m2", "synth-B", "embed-a/bbbb", &r) == 0);
   assert(r.model_bumped == 1);
   assert(r.artifacts_reembedded == 1);
   assert(count(db, "SELECT COUNT(*) FROM artifacts WHERE id='ds' AND state='proposed'") == 1);
   assert(r.prompt_bumped == 1);
   assert(r.docs_reextracted == 1);
   assert(count(db, "SELECT COUNT(*) FROM kb_async_jobs"
                    " WHERE kind='extract_doc' AND status='pending'") >= 1);
   printf("  new embedder identity re-embeds AND re-extracts OK\n");

   /* 6. A synthesis-model change re-extracts WITHOUT re-embedding, so the two triggers
    * stay distinguishable. Re-extraction leaves its artifacts proposed for the index
    * passes anyway; firing the re-embed too would walk every artifact twice for one
    * change. */
   recommit(db);
   clear_extract_jobs(db);
   assert(kb_curator_version_replay("p2", "m2", "synth-D", "embed-a/bbbb", &r) == 0);
   assert(r.prompt_bumped == 1 && r.docs_reextracted == 1);
   assert(r.model_bumped == 0 && r.artifacts_reembedded == 0);
   assert(count(db, "SELECT COUNT(*) FROM artifacts WHERE id='ds' AND state='committed'") == 1);
   printf("  synthesis change does not double-walk the artifacts OK\n");

   /* 7. Idempotence after all of that: a boot with nothing changed replays nothing. A
    * replay that fired every boot would re-extract the whole corpus on every restart. */
   recommit(db);
   clear_extract_jobs(db);
   assert(kb_curator_version_replay("p2", "m2", "synth-D", "embed-a/bbbb", &r) == 0);
   assert(r.prompt_bumped == 0 && r.model_bumped == 0);
   assert(r.docs_reextracted == 0 && r.artifacts_reembedded == 0);
   printf("  unchanged identities replay nothing OK\n");

   /* 8. Empty identities: a deployment that reports neither must be stable, not replay
    * forever. This is the external-embedder and no-declared-version shape. */
   assert(kb_curator_version_replay("", "", "", "", &r) == 0);
   assert(r.prompt_bumped == 1 && r.model_bumped == 1); /* a real change from case 7 */
   recommit(db);
   clear_extract_jobs(db);
   assert(kb_curator_version_replay("", "", "", "", &r) == 0);
   assert(r.prompt_bumped == 0 && r.model_bumped == 0);
   assert(kb_curator_version_replay(NULL, NULL, NULL, NULL, &r) == 0);
   assert(r.prompt_bumped == 0 && r.model_bumped == 0); /* NULL == empty, not a change */
   printf("  empty and NULL identities are stable OK\n");

   db2_test_shim_close();
   printf("curator_version: all tests passed\n");
   return 0;
}
