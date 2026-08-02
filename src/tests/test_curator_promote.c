/* test_curator_promote.c: unit tests for the promote_entity pass — the pure
 * scope-lattice step plus a graceful no-data drain call under the sqlite shim
 * (gated off by default -> returns 0 without crashing). */
#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include <sqlite3.h>

#include <stdlib.h>
#include "aimee.h"
#include "db2_test_shim.h"
#include "kb_curator_promote.h"

void *(db2_conn)(void);

static void test_scope_lattice(void)
{
   assert(strcmp(kb_curator_scope_next_kind("project"), "workspace") == 0);
   assert(strcmp(kb_curator_scope_next_kind("workspace"), "global") == 0);
   assert(kb_curator_scope_next_kind("global") == NULL);
   assert(kb_curator_scope_next_kind("bogus") == NULL);
   assert(kb_curator_scope_next_kind(NULL) == NULL);
   printf("  scope lattice project->workspace->global OK\n");
}

static void test_drain_graceful(void)
{
   db2_test_shim_open();
   int rc = kb_curator_promote_entity_one(NULL);
   assert(rc == 0 || rc == 1);
   printf("  promote graceful on empty/disabled shim OK (rc=%d)\n", rc);
   db2_test_shim_close();
}

static void seed(sqlite3 *db, const char *sql)
{
   assert(sqlite3_exec(db, sql, NULL, NULL, NULL) == SQLITE_OK);
}

static void test_pick_seeded(void)
{
   /* Regression: promote_pick must SELECT the real `payload` column (not
    * `payload_json`). A project-scoped entity cited by >= min_sources distinct
    * `mentions` sources must be picked; under the old column name the SELECT
    * fails to prepare and nothing is picked. */
   db2_test_shim_open();
   sqlite3 *db = (sqlite3 *)db2_test_shim_handle();
   assert(db != NULL);
   seed(db, "INSERT INTO artifacts (id,kind,state,scope_kind,scope_id,payload) VALUES"
            " ('ent','entity','committed','project','projA','{\"name\":\"pgvector\"}'),"
            " ('s1','claim','committed','project','projA','{}'),"
            " ('s2','claim','committed','project','projA','{}'),"
            " ('s3','claim','committed','project','projA','{}')");
   seed(db, "INSERT INTO artifact_links (from_id,to_id,kind) VALUES"
            " ('s1','ent','mentions'),('s2','ent','mentions'),('s3','ent','mentions')");

   char id[64] = "", sk[64] = "", si[128] = "";
   char *payload = NULL;
   int found = kb_curator_promote_pick(db2_conn(), 3, id, sizeof(id), &payload, sk, sizeof(sk), si,
                                       sizeof(si));
   assert(found == 1);
   assert(strcmp(id, "ent") == 0);
   assert(strcmp(sk, "project") == 0 && strcmp(si, "projA") == 0);
   free(payload);

   /* threshold not met (min_sources=4) → no pick */
   char id2[64] = "";
   char *p2 = NULL;
   int f2 = kb_curator_promote_pick(db2_conn(), 4, id2, sizeof(id2), &p2, sk, sizeof(sk), si,
                                    sizeof(si));
   assert(f2 == 0);
   free(p2);

   db2_test_shim_close();
   printf("  promote_pick selects an entity with >= min_sources OK\n");
}

int main(void)
{
   test_scope_lattice();
   test_drain_graceful();
   test_pick_seeded();
   printf("curator_promote: all tests passed\n");
   return 0;
}
