/* test_curator_synthesize.c: synthesize_topic pass.
 * The full _one path is config-gated + sidecar-driven; here we drive the topic
 * picker seam directly so the artifacts.payload SELECT is actually exercised
 * (empty-path-only tests masked a subsystem-wide column bug — see #2488). */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sqlite3.h>

#include "aimee.h"
#include "db2/artifacts.h"
#include "db2_test_shim.h"
#include "../kb/kb_curator_synthesize.h"

void *db2_conn(void);

static void seed(sqlite3 *db, const char *sql)
{
   assert(sqlite3_exec(db, sql, NULL, NULL, NULL) == SQLITE_OK);
}

static void test_gated_empty(void)
{
   db2_test_shim_open();
   /* Gated off (no synthesize_command) → returns 0 before any DB work. */
   assert(kb_curator_synthesize_one(NULL) == 0);
   db2_test_shim_close();
   printf("  synthesize gated/empty OK\n");
}

static void test_pick_seeded(void)
{
   db2_test_shim_open();
   sqlite3 *db = (sqlite3 *)db2_test_shim_handle();
   assert(db != NULL);
   /* A committed entity with one inbound `mentions` and no synthesis about it. */
   seed(db, "INSERT INTO artifacts (id,kind,state,scope_kind,scope_id,payload) VALUES"
            " ('ent','entity','committed','project','projA','{\"name\":\"pgvector\"}'),"
            " ('cu','code_unit','committed','project','projA','{}')");
   seed(db, "INSERT INTO artifact_links (from_id,to_id,kind) VALUES ('cu','ent','mentions')");

   char id[64] = "", sk[64] = "", si[128] = "";
   char *payload = NULL;
   int found = kb_curator_synth_pick_topic(db2_conn(), id, sizeof(id), &payload, sk, sizeof(sk), si,
                                           sizeof(si));
   assert(found == 1);
   assert(strcmp(id, "ent") == 0);
   assert(payload && strstr(payload, "pgvector") != NULL);
   free(payload);

   /* Once a synthesis is linked `about` the entity, it is no longer eligible. */
   seed(db, "INSERT INTO artifacts (id,kind,state) VALUES ('syn','synthesis','committed')");
   seed(db, "INSERT INTO artifact_links (from_id,to_id,kind) VALUES ('syn','ent','about')");
   char id2[64] = "";
   char *p2 = NULL;
   int f2 = kb_curator_synth_pick_topic(db2_conn(), id2, sizeof(id2), &p2, sk, sizeof(sk), si,
                                        sizeof(si));
   assert(f2 == 0);
   free(p2);

   db2_test_shim_close();
   printf("  synth_pick_topic selects an un-synthesised entity OK\n");
}

static int count_sql(sqlite3 *db, const char *sql)
{
   sqlite3_stmt *st = NULL;
   assert(sqlite3_prepare_v2(db, sql, -1, &st, NULL) == SQLITE_OK);
   assert(sqlite3_step(st) == SQLITE_ROW);
   int n = sqlite3_column_int(st, 0);
   sqlite3_finalize(st);
   return n;
}

static void test_restore_fragment_record(void)
{
   db2_test_shim_open();
   sqlite3 *db = (sqlite3 *)db2_test_shim_handle();
   assert(db != NULL);
   seed(db, "INSERT INTO artifacts (id,kind,state,scope_kind,scope_id,payload) VALUES"
            " ('base','summary','committed','doc','base-doc','{\"text\":\"base\"}')");

   char out_id[64] = "";
   assert(kb_curator_restore_fragment_record(42, "base",
                                             "Restored title: [unknown]. Preserved number 17.",
                                             0.82, "restore-test-v1", out_id, sizeof(out_id)) == 0);
   assert(out_id[0] != '\0');
   assert(count_sql(db, "SELECT COUNT(*) FROM artifacts WHERE id != 'base' AND kind='restoration'"
                        " AND state='committed' AND payload LIKE '%unknown_sentinel_present%'"
                        " AND payload LIKE '%synthesised%'") == 1);
   assert(count_sql(db, "SELECT COUNT(*) FROM artifact_citations WHERE artifact_id != 'base'"
                        " AND source_kind='doc' AND source_id='42'") == 1);
   assert(count_sql(db, "SELECT COUNT(*) FROM artifact_links WHERE from_id != 'base'"
                        " AND to_id='base' AND kind='restored_from'") == 1);
   assert(count_sql(db, "SELECT COUNT(*) FROM artifact_links WHERE from_id='base'"
                        " AND kind='restores'") == 1);
   assert(count_sql(
              db, "SELECT COUNT(*) FROM audit_events WHERE target_surface='corpus.restore'") == 1);

   db2_test_shim_close();
   printf("  restore_fragment_record writes provenance OK\n");
}

int main(void)
{
   test_gated_empty();
   test_pick_seeded();
   test_restore_fragment_record();
   printf("curator_synthesize: all tests passed\n");
   return 0;
}
