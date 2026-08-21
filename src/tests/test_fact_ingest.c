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

_Static_assert(sizeof(((db2_fact_candidate_t *)0)->subject) ==
                   sizeof(((pattern_triple_t *)0)->subject),
               "test extractor subject capacity must match DB2");
_Static_assert(sizeof(((db2_fact_candidate_t *)0)->rel_type) ==
                   sizeof(((pattern_triple_t *)0)->rel_type),
               "test extractor relation capacity must match DB2");
_Static_assert(sizeof(((db2_fact_candidate_t *)0)->object) ==
                   sizeof(((pattern_triple_t *)0)->object),
               "test extractor object capacity must match DB2");

static int extract_facts(const char *text, db2_fact_candidate_t *out, int max, int *count)
{
   if (!text || !out || max <= 0 || max > 32 || !count)
      return -1;
   pattern_triple_t triples[32] = {0};
   int found = memory_extract_patterns(text, triples, max);
   if (found < 0)
      return -1;
   for (int i = 0; i < found; ++i)
   {
      memcpy(out[i].subject, triples[i].subject, sizeof(out[i].subject));
      memcpy(out[i].rel_type, triples[i].rel_type, sizeof(out[i].rel_type));
      memcpy(out[i].object, triples[i].object, sizeof(out[i].object));
      out[i].subject_kind = (int)triples[i].subject_kind;
      out[i].object_kind = (int)triples[i].object_kind;
   }
   *count = found;
   return 0;
}

static int failing_extract(const char *text, db2_fact_candidate_t *out, int max, int *count)
{
   (void)text;
   (void)out;
   (void)max;
   (void)count;
   return -1;
}

static int invalid_extract_count(const char *text, db2_fact_candidate_t *out, int max, int *count)
{
   (void)text;
   (void)out;
   *count = max + 1;
   return 0;
}

static int scan_fact_turn(const char *text, int *is_retraction, int *has_attr,
                          char attr[DB2_FACT_ATTR_MAX])
{
   memory_pattern_turn_t scan;
   if (memory_pattern_scan_turn(text, &scan) != 0)
      return -1;
   *is_retraction = scan.is_retraction;
   *has_attr = scan.has_attr;
   memcpy(attr, scan.attr, DB2_FACT_ATTR_MAX);
   return 0;
}

static int failing_scan(const char *text, int *is_retraction, int *has_attr,
                        char attr[DB2_FACT_ATTR_MAX])
{
   (void)text;
   (void)is_retraction;
   (void)has_attr;
   (void)attr;
   return -1;
}

static int invalid_scan(const char *text, int *is_retraction, int *has_attr,
                        char attr[DB2_FACT_ATTR_MAX])
{
   (void)text;
   *is_retraction = 1;
   *has_attr = 1;
   attr[0] = '\0';
   return 0;
}

/* Reads every turn as "retract works_for", so the authority the ingress was
 * called with is the only thing that decides whether the fact goes. */
static int scan_retract_works_for(const char *text, int *is_retraction, int *has_attr,
                                  char attr[DB2_FACT_ATTR_MAX])
{
   (void)text;
   *is_retraction = 1;
   *has_attr = 1;
   snprintf(attr, DB2_FACT_ATTR_MAX, "works_for");
   return 0;
}

int main(void)
{
   db2_test_shim_open();
   aimee_db2_register_fact_gate_provider(check_fact_gate);
   assert(db2_rel_types_ensure_seed() == 0);

   /* Extraction is authoritative: absence, failure, or an invalid count cannot
    * be mistaken for a turn with zero facts. */
   assert(db2_fact_ingest_text("my city is Berlin", FACT_AUTHORITY_USER, 1) == -1);
   aimee_db2_register_fact_extract_provider(failing_extract);
   assert(db2_fact_ingest_text("my city is Berlin", FACT_AUTHORITY_USER, 1) == -1);
   aimee_db2_register_fact_extract_provider(invalid_extract_count);
   assert(db2_fact_ingest_text("my city is Berlin", FACT_AUTHORITY_USER, 1) == -1);
   assert(semantic_count("user") == 0);
   aimee_db2_register_fact_extract_provider(extract_facts);

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

   /* §4 retraction flow: no scanner, a failed scanner, or an inconsistent answer
    * cannot delete. The host-installed scanner then retracts only the named fact. */
   assert(db2_fact_current_count("user") == 2); /* email + city currently believed */
   assert(db2_typed_fact_ingress("please forget my email", FACT_AUTHORITY_USER, NULL, 0) == 0);
   assert(db2_fact_current_count("user") == 2);
   aimee_db2_register_fact_scan_provider(failing_scan);
   assert(db2_typed_fact_ingress("please forget my email", FACT_AUTHORITY_USER, NULL, 0) == 0);
   assert(db2_fact_current_count("user") == 2);
   aimee_db2_register_fact_scan_provider(invalid_scan);
   assert(db2_typed_fact_ingress("please forget my email", FACT_AUTHORITY_USER, NULL, 0) == 0);
   assert(db2_fact_current_count("user") == 2);
   aimee_db2_register_fact_scan_provider(scan_fact_turn);
   assert(db2_typed_fact_ingress("please forget my email", FACT_AUTHORITY_USER, NULL, 0) == 0);
   assert(db2_fact_current_count("user") == 1); /* only city remains current */

   /* §4/§5: the ingress retracts at the authority it was CALLED with, not at the
    * user's. A turn asking to forget a user-stated (Class A) fact leaves it
    * standing when the caller could only prove model authority — which is what
    * every model-driven surface passes — and withdraws it at user authority. */
   assert(db2_fact_commit("user", NODE_PERSON, "works_for", "acme", NODE_ORG, FACT_AUTHORITY_USER,
                          1) == FACT_GATE_ACCEPT);
   assert(db2_fact_current_count("user") == 2); /* city + works_for */
   aimee_db2_register_fact_scan_provider(scan_retract_works_for);
   assert(db2_typed_fact_ingress("forget where I work", FACT_AUTHORITY_MODEL, NULL, 0) == 0);
   assert(db2_fact_current_count("user") == 2); /* model refused: Class A stands */
   assert(db2_typed_fact_ingress("forget where I work", FACT_AUTHORITY_USER, NULL, 0) == 0);
   assert(db2_fact_current_count("user") == 1); /* the user's own retraction lands */

   /* Bad args. */
   assert(db2_fact_ingest_text(NULL, FACT_AUTHORITY_USER, 1) == -1);

   db2_test_shim_close();
   printf("fact_ingest: all tests passed\n");
   return 0;
}
