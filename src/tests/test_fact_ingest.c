/* test_fact_ingest.c: pattern-first typed-fact ingest pipeline (§6 -> §1),
 * against the sqlite shim. P5. */
#include "../headers/aimee.h"
#include "../headers/memory.h" /* edge_t */
#include "../db2/fact_ingest.h"
#include "../db2/rel_types_store.h"
#include "../db2/entity_edges.h"
#include "../db2/ontology_evolution.h"
#include "../db2/db2_test_shim.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static int semantic_count(const char *entity)
{
   edge_t e[64];
   return db2_entity_edges_semantic_by_entity(entity, e, 64);
}

int main(void)
{
   db2_test_shim_open();
   assert(db2_rel_types_ensure_seed() == 0);

   /* Disabled: the gate is observe-only — nothing is written. */
   assert(db2_fact_ingest_text("my email is theo@example.com", FACT_AUTHORITY_USER, 0) == 0);
   assert(semantic_count("user") == 0);

   /* Enabled: two personal-fact templates -> two semantic edges. Both rel_types
    * ("email", "city") are novel, so they stage provisional + are written Class C,
    * and the §2 occurrence tracker sees them. */
   int w = db2_fact_ingest_text("my email is theo@example.com. my city is Berlin.",
                                FACT_AUTHORITY_USER, 1);
   assert(w == 2);
   assert(semantic_count("user") == 2);
   assert(db2_ontology_eval_count("email") == 1);
   assert(db2_ontology_eval_count("city") == 1);

   /* Re-ingest the same turn: triples already exist -> weight bumps, no new rows;
    * still reported as written (the gate returns NOVEL/ACCEPT). */
   assert(db2_fact_ingest_text("my email is theo@example.com", FACT_AUTHORITY_USER, 1) == 1);
   assert(semantic_count("user") == 2);           /* no duplicate row */
   assert(db2_ontology_eval_count("email") == 2); /* observed again */

   /* No template in the text -> nothing committed. */
   assert(db2_fact_ingest_text("the server crashed last night", FACT_AUTHORITY_USER, 1) == 0);

   /* Bad args. */
   assert(db2_fact_ingest_text(NULL, FACT_AUTHORITY_USER, 1) == -1);

   db2_test_shim_close();
   printf("fact_ingest: all tests passed\n");
   return 0;
}
