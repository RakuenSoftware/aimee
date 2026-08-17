/* test_fact_ingest.c: pattern-first typed-fact ingest pipeline (§6 -> §1),
 * against the sqlite shim. P5. */
#include "../headers/aimee.h"
#include "../headers/memory.h" /* edge_t */
#include "../modules/db2/c/fact_ingest.h"
#include "../modules/db2/c/fact_lifecycle.h"
#include "../modules/db2/c/rel_types_store.h"
#include "../modules/db2/c/entity_edges.h"
#include "../modules/db2/c/ontology_evolution.h"
#include "../modules/db2/c/db2_test_shim.h"
#include "modules/memory/memory_extract_patterns.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static int semantic_count(const char *entity)
{
   edge_t e[64];
   return db2_entity_edges_semantic_by_entity(entity, e, 64);
}

static int check_fact_gate(int head_kind, const char *rel_type, int tail_kind, int *verdict)
{
   if (!verdict)
      return -1;
   *verdict = (int)memory_fact_gate_check((memory_node_kind_t)head_kind, rel_type,
                                          (memory_node_kind_t)tail_kind, NULL);
   return 0;
}

int main(void)
{
   db2_test_shim_open();
   aimee_db2_register_fact_gate_provider(check_fact_gate);
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

   /* §4 retraction flow (the handler's glue): a retraction turn extracts the named
    * attribute and retracts it. After ingesting email+city, "forget my email"
    * supersedes only the email fact. */
   assert(db2_fact_current_count("user") == 2); /* email + city currently believed */
   char attr[128];
   assert(memory_pattern_is_retraction("please forget my email"));
   assert(memory_pattern_possessive_attr("please forget my email", attr, sizeof(attr)) == 1);
   assert(db2_fact_retract("user", attr, NULL, FACT_AUTHORITY_USER) == 1);
   assert(db2_fact_current_count("user") == 1); /* only city remains current */

   /* Bad args. */
   assert(db2_fact_ingest_text(NULL, FACT_AUTHORITY_USER, 1) == -1);

   db2_test_shim_close();
   printf("fact_ingest: all tests passed\n");
   return 0;
}
