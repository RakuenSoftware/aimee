/* test_entity_renorm_pg.c: the alias re-key migration against a REAL Postgres.
 *
 * db2_entity_renormalize_aliases() is tested on the sqlite shim from a single
 * seeded row. That is enough to show the logic, and not enough to trust it: it
 * MERGES ENTITIES, which is the least reversible operation in this subsystem,
 * and it runs unattended at aimee-kb startup against a table it did not write.
 *
 * So it is exercised here on a production-shaped table: many rows, several
 * old-key spellings of the same name, two entities that turn out to be one, and
 * edges hanging off the loser that must keep resolving afterwards.
 *
 * OPT-IN via AIMEE_TEST_PG_URL, and not in TEST_TARGETS: a test that needs a
 * server it cannot start is a broken CI job rather than a test.
 */
#include "../db2/db2.h"
#include "../db2/lifecycle.h"
#include "../db2/db2_internal.h"
#include "../db2/db_postgres.h"
#include "../db2/entity_registry.h"
#include "../db2/entity_edges.h"
#include "../db2/fact_lifecycle.h"
#include "../db2/rel_types_store.h"
#include "memory_fact_gate.h"
#include "memory_ontology.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void wipe(void)
{
   char err[256] = "";
   const char *sql = "TRUNCATE entity_edges, entity_aliases, entity_registry, kb_meta"
                     " RESTART IDENTITY CASCADE";
   aimee_pg_stmt_t *st = aimee_pg_prepare(db2_conn(), sql, err, sizeof(err));
   if (st)
   {
      (void)aimee_pg_step(st, err, sizeof(err));
      aimee_pg_finalize(st);
   }
}

/* Bind a name under an OLD-STYLE key, the way a pre-fix release would have. */
static int64_t legacy_bind(const char *display, const char *old_norm, int kind)
{
   int64_t cid = db2_entity_register(kind, ENTITY_STATUS_ACTIVE);
   if (cid <= 0)
      return -1;
   char err[256] = "";
   aimee_pg_stmt_t *st =
       aimee_pg_prepare(db2_conn(),
                        "INSERT INTO entity_aliases (name, name_norm, canonical_id, is_preferred)"
                        " VALUES (?1, ?2, ?3, 1)",
                        err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_text(st, "?1", display);
   aimee_pg_bind_text(st, "?2", old_norm);
   aimee_pg_bind_int64(st, "?3", cid);
   int rc = aimee_pg_step(st, err, sizeof(err));
   aimee_pg_finalize(st);
   return rc == AIMEE_PG_ERR ? -1 : cid;
}

int main(void)
{
   const char *url = getenv("AIMEE_TEST_PG_URL");
   if (!url || !url[0])
   {
      printf("SKIP: set AIMEE_TEST_PG_URL to run the Postgres re-key test\n");
      return 0;
   }
   printf("test_entity_renorm_pg (live Postgres)\n");
   db2_set_embedding_dim_default(384);
   if (db2_init(url) != 0)
   {
      fprintf(stderr, "FAIL: db2_init could not connect\n");
      return 1;
   }
   assert(db2_rel_types_ensure_seed() == 0);
   wipe();

   /* A production-shaped table: old keys, several spellings, one pair that is
    * really one entity. */
   int64_t kb = legacy_bind("kb_server", "kb_server", NODE_DEVICE);
   int64_t sun = legacy_bind("Sunshine", "sunshine", NODE_ORG);
   int64_t sunteam = legacy_bind("Sunshine team", "sunshine team", NODE_ORG);
   int64_t okafor = legacy_bind("Dr. Okafor", "dr. okafor", NODE_PERSON);
   int64_t untouched = legacy_bind("Northwind Marine", "northwind marine", NODE_ORG);
   assert(kb > 0 && sun > 0 && sunteam > 0 && okafor > 0 && untouched > 0);
   assert(sun != sunteam); /* two nodes for one client, which is the bug */

   /* A LEGACY edge: written by a pre-fix release, so its text columns carry the
    * display name that was preferred back then. This is the case the shim test
    * could not produce, and the one that decides whether the migration joins
    * facts or strands them -- db2_fact_recall_block and db2_fact_current_count
    * match entity_edges.source/target as LITERAL TEXT, with no canonicalisation
    * at read time. */
   {
      char err[256] = "";
      aimee_pg_stmt_t *e = aimee_pg_prepare(
          db2_conn(),
          "INSERT INTO entity_edges (source, relation, target, weight, relation_id,"
          " subject_kind, object_kind, edge_class, confidence_class, confidence,"
          " superseded_at, suppressed, asserted_at)"
          " VALUES ('Sunshine team', 'customer_of', 'user', 1, 0, 3, 99, 'semantic',"
          " 'B', 0.6, '', 0, '2026-01-01 00:00:00')",
          err, sizeof(err));
      assert(e);
      assert(aimee_pg_step(e, err, sizeof(err)) != AIMEE_PG_ERR);
      aimee_pg_finalize(e);
   }
   assert(db2_fact_current_count("Sunshine team") == 1); /* reachable under the OLD name */

   int rc = db2_entity_renormalize_aliases();
   printf("  re-key reported %d change(s)\n", rc);
   assert(rc > 0);

   /* Every spelling of the client now reaches ONE node. */
   int64_t s1 = db2_entity_resolve("Sunshine");
   int64_t s2 = db2_entity_resolve("Sunshine team");
   int64_t s3 = db2_entity_resolve("sunshine_team");
   int64_t s4 = db2_entity_resolve("the Sunshine Team");
   assert(s1 > 0 && s1 == s2 && s2 == s3 && s3 == s4);
   printf("  PASS: four spellings of one client resolve to a single entity\n");

   /* The separator and honorific folds took effect on real rows. */
   assert(db2_entity_resolve("kb server") == kb);
   assert(db2_entity_resolve("kb-server") == kb);
   assert(db2_entity_resolve("KB_SERVER") == kb);
   assert(db2_entity_resolve("Okafor") == okafor);
   printf("  PASS: separator and honorific folds applied to stored rows\n");

   /* THE ASSERTION THAT MATTERS. After the merge, a caller asking about the
    * surviving canonical name must find the legacy edge. Registry merges alone
    * do not achieve this: entity_edges stores names as text and recall does not
    * canonicalise, so unless the migration rewrites the endpoints too, the fact
    * is still filed under a name nothing resolves to any more -- present in the
    * table, invisible to every query. */
   assert(db2_fact_current_count("Sunshine") >= 1);
   printf("  PASS: a legacy edge is reachable under the surviving name\n");

   /* A name needing no change is left alone. */
   assert(db2_entity_resolve("Northwind Marine") == untouched);

   /* Idempotent: the guard fires and a second pass is a no-op. */
   assert(db2_entity_renormalize_aliases() == 0);
   printf("  PASS: second pass is a no-op (kb_meta guard)\n");

   wipe();
   printf("all tests passed\n");
   return 0;
}
