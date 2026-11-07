/* test_curator_synthesize.c: synthesize_topic pass.
 * The full _one path is config-gated and provider-driven; here we drive the topic
 * picker seam directly so the artifacts.payload SELECT is actually exercised
 * (empty-path-only tests masked a subsystem-wide column bug — see #2488). */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sqlite3.h>

#include "aimee.h"
#include "modules/kb/c/kb_store_test_shim.h"
#include "kb_curator_synthesize.h"

void *(kb_store_conn)(void);

static void seed(sqlite3 *db, const char *sql)
{
   assert(sqlite3_exec(db, sql, NULL, NULL, NULL) == SQLITE_OK);
}

static void test_gated_empty(void)
{
   kb_store_test_shim_open();
   /* Disabled synthesis returns 0 before any DB work. */
   assert(kb_curator_synthesize_one(NULL) == 0);
   kb_store_test_shim_close();
   printf("  synthesize gated/empty OK\n");
}

static void test_pick_seeded(void)
{
   kb_store_test_shim_open();
   sqlite3 *db = (sqlite3 *)kb_store_test_shim_handle();
   assert(db != NULL);
   /* A committed entity with one inbound `mentions` and no synthesis about it. */
   seed(db, "INSERT INTO artifacts (id,kind,state,scope_kind,scope_id,payload) VALUES"
            " ('ent','entity','committed','project','projA','{\"name\":\"pgvector\"}'),"
            " ('cu','code_unit','committed','project','projA','{}')");
   seed(db, "INSERT INTO artifact_links (from_id,to_id,kind) VALUES ('cu','ent','mentions')");

   char id[64] = "", sk[64] = "", si[128] = "";
   char *payload = NULL;
   int found = kb_curator_synth_pick_topic(kb_store_conn(), id, sizeof(id), &payload, sk,
                                           sizeof(sk), si, sizeof(si));
   assert(found == 1);
   assert(strcmp(id, "ent") == 0);
   assert(payload && strstr(payload, "pgvector") != NULL);
   free(payload);

   /* Once a synthesis is linked `about` the entity, it is no longer eligible. */
   seed(db, "INSERT INTO artifacts (id,kind,state) VALUES ('syn','synthesis','committed')");
   seed(db, "INSERT INTO artifact_links (from_id,to_id,kind) VALUES ('syn','ent','about')");
   char id2[64] = "";
   char *p2 = NULL;
   int f2 = kb_curator_synth_pick_topic(kb_store_conn(), id2, sizeof(id2), &p2, sk, sizeof(sk), si,
                                        sizeof(si));
   assert(f2 == 0);
   free(p2);

   kb_store_test_shim_close();
   printf("  synth_pick_topic selects an un-synthesised entity OK\n");
}

int main(void)
{
   if (kb_store_test_shim_skip_on_postgres("curator_synthesize"))
      return 0;

   test_gated_empty();
   test_pick_seeded();
   printf("curator_synthesize: all tests passed\n");
   return 0;
}
