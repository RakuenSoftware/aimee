/* test_memory_facts_retract_pg.c: the retraction path against a REAL Postgres.
 *
 * test_memory_facts_retract.c proves the C path -- a negated fact reaches
 * db2_fact_retract and the edge stops being current. It proves it over the
 * sqlite shim, which stands in for Postgres behind the aimee_pg_* wrappers.
 * What the shim cannot rule out is a dialect or semantics difference in the one
 * statement that matters:
 *
 *   UPDATE entity_edges SET suppressed = 1, superseded_at = ?3
 *    WHERE source = ?1 AND relation = ?2 AND edge_class = 'semantic'
 *      AND target = ?4
 *
 * That SQL is ANSI and binds its timestamp as text computed in C, so the risk is
 * low -- but "low risk" is an argument, not a result, and a retraction is
 * destructive. This runs the same assertions against a live server.
 *
 * OPT-IN: skips cleanly unless AIMEE_TEST_PG_URL names a database this test may
 * create tables in and truncate. It is not wired into `make unit-tests`, because
 * a test that needs a server it cannot start is a broken CI job, not a test.
 */
#include "../db2/db2.h"
#include "../db2/lifecycle.h"
#include "../db2/db2_internal.h"
#include "../db2/db_postgres.h"
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
   /* Each case starts from a known graph. TRUNCATE rather than DROP so the
    * schema application stays a one-time cost. */
   char err[256] = "";
   aimee_pg_stmt_t *st =
       aimee_pg_prepare(db2_conn(), "TRUNCATE entity_edges, entities RESTART IDENTITY CASCADE",
                        err, sizeof(err));
   if (st)
   {
      (void)aimee_pg_step(st, err, sizeof(err));
      aimee_pg_finalize(st);
   }
}

int main(void)
{
   const char *url = getenv("AIMEE_TEST_PG_URL");
   if (!url || !url[0])
   {
      printf("SKIP: set AIMEE_TEST_PG_URL to run the Postgres retraction test\n");
      return 0;
   }

   printf("test_memory_facts_retract_pg (live Postgres)\n");
   /* db2_init applies the schema, and the schema is parameterised by the
    * deployment's embedding width -- which normally arrives from config at
    * startup. Without it db2_init refuses rather than guessing, so the harness
    * has to supply it exactly as the server does. */
   db2_set_embedding_dim_default(384);
   if (db2_init(url) != 0)
   {
      fprintf(stderr, "FAIL: db2_init could not connect\n");
      return 1;
   }
   /* db2_init applied the schema on connect. */
   assert(db2_rel_types_ensure_seed() == 0);
   wipe();

   /* 1. a retraction deactivates the named edge */
   assert(db2_fact_commit("Ingrid Sandoval", NODE_PERSON, "member_of", "Kestrel Freight", NODE_ORG,
                          FACT_AUTHORITY_MODEL, 1) == FACT_GATE_ACCEPT);
   assert(db2_fact_current_count("Ingrid Sandoval") == 1);
   assert(db2_fact_retract("Ingrid Sandoval", "member_of", "Kestrel Freight",
                           FACT_AUTHORITY_MODEL) >= 1);
   assert(db2_fact_current_count("Ingrid Sandoval") == 0);
   printf("  PASS: retraction deactivates the edge on Postgres\n");

   /* 2. scoped to the named target, which is the whole reason polarity rides on
    * the original fact. If Postgres treated the target predicate differently
    * from sqlite, this is where it would show. */
   wipe();
   assert(db2_fact_commit("Tomas Bauer", NODE_PERSON, "member_of", "Aldridge Labs", NODE_ORG,
                          FACT_AUTHORITY_MODEL, 1) == FACT_GATE_ACCEPT);
   assert(db2_fact_commit("Tomas Bauer", NODE_PERSON, "member_of", "Corvo Surveying", NODE_ORG,
                          FACT_AUTHORITY_MODEL, 1) == FACT_GATE_ACCEPT);
   assert(db2_fact_current_count("Tomas Bauer") == 2);
   assert(db2_fact_retract("Tomas Bauer", "member_of", "Aldridge Labs", FACT_AUTHORITY_MODEL) == 1);
   assert(db2_fact_current_count("Tomas Bauer") == 1);
   printf("  PASS: scoped to the named target, sibling edge survives\n");

   /* 3. a NULL target takes every value of (source, relation) -- the behaviour
    * the empty-object guard in mf_commit_facts exists to prevent reaching. */
   wipe();
   assert(db2_fact_commit("Saskia Lindqvist", NODE_PERSON, "member_of", "Grimsby Systems",
                          NODE_ORG, FACT_AUTHORITY_MODEL, 1) == FACT_GATE_ACCEPT);
   assert(db2_fact_commit("Saskia Lindqvist", NODE_PERSON, "member_of", "Halden Instruments",
                          NODE_ORG, FACT_AUTHORITY_MODEL, 1) == FACT_GATE_ACCEPT);
   assert(db2_fact_current_count("Saskia Lindqvist") == 2);
   assert(db2_fact_retract("Saskia Lindqvist", "member_of", NULL, FACT_AUTHORITY_MODEL) == 2);
   assert(db2_fact_current_count("Saskia Lindqvist") == 0);
   printf("  PASS: a NULL target takes the whole relation (the guarded case)\n");

   /* 4. the §4/§5 authority guard: a MODEL authority must not retract a
    * user-stated Class-A edge. This is the safety property that makes it
    * acceptable for an LLM to emit retractions at all. */
   wipe();
   assert(db2_fact_commit("Orla Carrington", NODE_PERSON, "works_for", "Northwind Marine", NODE_ORG,
                          FACT_AUTHORITY_USER, 1) == FACT_GATE_ACCEPT);
   assert(db2_fact_current_count("Orla Carrington") == 1);
   assert(db2_fact_retract("Orla Carrington", "works_for", "Northwind Marine",
                           FACT_AUTHORITY_MODEL) == 0);
   assert(db2_fact_current_count("Orla Carrington") == 1); /* user fact stands */
   assert(db2_fact_retract("Orla Carrington", "works_for", "Northwind Marine",
                           FACT_AUTHORITY_USER) >= 1);
   assert(db2_fact_current_count("Orla Carrington") == 0);
   printf("  PASS: a model authority cannot retract a user-stated fact\n");

   wipe();
   printf("all tests passed\n");
   return 0;
}
