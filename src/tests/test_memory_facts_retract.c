/* test_memory_facts_retract.c: a negated fact from the extractor actually
 * deactivates the edge it names. End-to-end over the sqlite shim.
 *
 * The unit tests either side of this seam already passed while the seam did not
 * exist. db2_fact_retract() has been complete and covered by
 * test_fact_lifecycle.c the whole time; kb_memory_facts.c simply never called
 * it, because the prompt told the model a retraction had nothing durable to
 * record. Testing the API and testing the prompt separately is exactly how a
 * missing connection between them stays invisible, so this drives the real
 * extractor entry point -- mf_commit_facts on a raw LLM response -- and then
 * asserts against the stored graph rather than against a return value.
 *
 * The TU is included rather than linked because mf_commit_facts is static, the
 * same pattern test_memory_facts_grounding.c uses for fact_grounding.c.
 */
#include "../db2/db2_test_shim.h"
#include "kb/kb_memory_facts.c"

#include <assert.h>
#include <stdio.h>
#include <string.h>

/* Currently-believed edges for an entity: active means not superseded and not
 * suppressed, which is what recall filters on. */
static int current(const char *entity)
{
   return db2_fact_current_count(entity);
}

static void test_negated_fact_retracts(void)
{
   /* A fact the model asserted earlier, exactly as an ordinary note would. */
   assert(db2_fact_commit("Ingrid Sandoval", NODE_PERSON, "member_of", "Kestrel Freight", NODE_ORG,
                          FACT_AUTHORITY_MODEL, 1) == FACT_GATE_ACCEPT);
   assert(current("Ingrid Sandoval") == 1);

   /* The note that ends it, extracted as polarity on the original fact. Fed as
    * the raw model response so the JSON parsing, grounding gate and relation
    * canonicalisation all run — the note text must contain both endpoints or
    * fact_grounded() drops it before any of this matters. */
   const char *note = "Ingrid Sandoval is no longer at Kestrel Freight.";
   const char *resp = "{\"facts\":[{\"subject\":\"Ingrid Sandoval\",\"relation\":\"member_of\","
                      "\"object\":\"Kestrel Freight\",\"confidence\":1.0,\"negated\":true}]}";
   int committed = mf_commit_facts(resp, note);

   /* Nothing is COMMITTED by a retraction: the count returned is assertions, and
    * a note that only deactivates edges legitimately reports zero. The effect is
    * in the graph, not the return value. */
   assert(committed == 0);
   assert(current("Ingrid Sandoval") == 0);
   printf("  PASS: a negated fact deactivates the edge it names\n");
}

static void test_retraction_is_scoped_to_the_named_edge(void)
{
   /* Two values of a multi-valued relation. member_of accumulates, so both are
    * live at once — this is the case with no supersede path, and the reason
    * polarity has to ride on the original fact: db2_fact_retract scopes on
    * `target`, and an empty one would take both. */
   assert(db2_fact_commit("Tomas Bauer", NODE_PERSON, "member_of", "Aldridge Labs", NODE_ORG,
                          FACT_AUTHORITY_MODEL, 1) == FACT_GATE_ACCEPT);
   assert(db2_fact_commit("Tomas Bauer", NODE_PERSON, "member_of", "Corvo Surveying", NODE_ORG,
                          FACT_AUTHORITY_MODEL, 1) == FACT_GATE_ACCEPT);
   assert(current("Tomas Bauer") == 2);

   const char *note = "Tomas Bauer has left Aldridge Labs, though he is still at Corvo Surveying.";
   const char *resp = "{\"facts\":[{\"subject\":\"Tomas Bauer\",\"relation\":\"member_of\","
                      "\"object\":\"Aldridge Labs\",\"confidence\":1.0,\"negated\":true}]}";
   (void)mf_commit_facts(resp, note);

   /* The partner edge must survive. If `object` were dropped on the way to
    * db2_fact_retract this would be 0, and the KB would have forgotten a fact
    * the note never mentioned. */
   assert(current("Tomas Bauer") == 1);
   printf("  PASS: retraction hits only the named edge, not the whole relation\n");
}

static void test_move_retracts_old_and_asserts_new(void)
{
   /* The change-of-state case: one note carrying both halves, which is what the
    * models actually emit on a move (85% of the time, measured at 1k). */
   assert(db2_fact_commit("fl512.c", NODE_OTHER, "located_in", "drivers/staging", NODE_PLACE,
                          FACT_AUTHORITY_MODEL, 1) == FACT_GATE_ACCEPT);
   assert(current("fl512.c") == 1);

   const char *note = "fl512.c used to be in drivers/staging; it is under drivers/comedi now.";
   const char *resp = "{\"facts\":["
                      "{\"subject\":\"fl512.c\",\"relation\":\"located_in\","
                      "\"object\":\"drivers/comedi\",\"confidence\":1.0},"
                      "{\"subject\":\"fl512.c\",\"relation\":\"located_in\","
                      "\"object\":\"drivers/staging\",\"confidence\":1.0,\"negated\":true}]}";
   int committed = mf_commit_facts(resp, note);

   assert(committed == 1); /* the new location */
   /* located_in is functional, so the assertion supersedes anyway; what this
    * pins is that the pair does not cancel out and leave the entity with
    * nothing believed. */
   assert(current("fl512.c") == 1);
   printf("  PASS: a move commits the new value and retracts the old\n");
}

static void test_absent_flag_still_commits(void)
{
   /* Backward compatibility: a model that never emits the field behaves exactly
    * as before, because cJSON_IsTrue(NULL) is false. Every pre-v6 result file
    * depends on this reading. */
   const char *note = "Vera Duarte joined the retrieval team last quarter.";
   const char *resp = "{\"facts\":[{\"subject\":\"Vera Duarte\",\"relation\":\"member_of\","
                      "\"object\":\"retrieval team\",\"confidence\":0.9}]}";
   assert(mf_commit_facts(resp, note) == 1);
   assert(current("Vera Duarte") == 1);

   /* negated:false is the same as absent. */
   const char *note2 = "Rafael Okonkwo works for Halden Instruments.";
   const char *resp2 = "{\"facts\":[{\"subject\":\"Rafael Okonkwo\",\"relation\":\"works_for\","
                       "\"object\":\"Halden Instruments\",\"confidence\":0.9,\"negated\":false}]}";
   assert(mf_commit_facts(resp2, note2) == 1);
   assert(current("Rafael Okonkwo") == 1);
   printf("  PASS: absent or false polarity commits as before\n");
}

static void test_ungrounded_retraction_is_refused(void)
{
   /* The grounding gate runs before the polarity branch, so a retraction naming
    * an entity absent from the note cannot deactivate anything. That ordering is
    * load-bearing: a retraction is destructive, and "delete an edge for someone
    * the note never mentions" is the worst thing this path could do. */
   assert(db2_fact_commit("Orla Carrington", NODE_PERSON, "member_of", "Northwind Marine", NODE_ORG,
                          FACT_AUTHORITY_MODEL, 1) == FACT_GATE_ACCEPT);
   assert(current("Orla Carrington") == 1);

   const char *note = "Kestrel Freight sent the renewal paperwork.";
   const char *resp = "{\"facts\":[{\"subject\":\"Orla Carrington\",\"relation\":\"member_of\","
                      "\"object\":\"Northwind Marine\",\"confidence\":1.0,\"negated\":true}]}";
   (void)mf_commit_facts(resp, note);
   assert(current("Orla Carrington") == 1); /* untouched */
   printf("  PASS: an ungrounded retraction is refused\n");
}

static void test_empty_object_cannot_blank_a_relation(void)
{
   /* Measured at 1k: the model sometimes emits {"object":""} on a retraction.
    * db2_fact_retract treats an empty target as "every current value of
    * (source, relation)", so this must never reach it. The malformed check
    * rejects it first — asserted here rather than trusted to line order. */
   assert(db2_fact_commit("Saskia Lindqvist", NODE_PERSON, "member_of", "Grimsby Systems", NODE_ORG,
                          FACT_AUTHORITY_MODEL, 1) == FACT_GATE_ACCEPT);
   assert(db2_fact_commit("Saskia Lindqvist", NODE_PERSON, "member_of", "Halden Instruments", NODE_ORG,
                          FACT_AUTHORITY_MODEL, 1) == FACT_GATE_ACCEPT);
   assert(current("Saskia Lindqvist") == 2);

   const char *note = "Saskia Lindqvist did not renew with Grimsby Systems or Halden Instruments.";
   const char *resp = "{\"facts\":[{\"subject\":\"Saskia Lindqvist\",\"relation\":\"member_of\","
                      "\"object\":\"\",\"confidence\":1.0,\"negated\":true}]}";
   (void)mf_commit_facts(resp, note);
   assert(current("Saskia Lindqvist") == 2); /* both survive */
   printf("  PASS: an empty object cannot blank a whole relation\n");
}

int main(void)
{
   db2_test_shim_open();
   assert(db2_rel_types_ensure_seed() == 0);

   printf("test_memory_facts_retract\n");
   test_negated_fact_retracts();
   test_retraction_is_scoped_to_the_named_edge();
   test_move_retracts_old_and_asserts_new();
   test_absent_flag_still_commits();
   test_ungrounded_retraction_is_refused();
   test_empty_object_cannot_blank_a_relation();
   printf("all tests passed\n");
   return 0;
}
