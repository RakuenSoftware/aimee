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
#include "../headers/kb_identity.h"
#include "modules/memory/memory_extract_patterns.h"
#include "support/memory_policy_stub.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

/* AN AUTHENTICATED REQUEST CONTEXT, because without one this file could not
 * reach the bug it already had an assertion for.
 *
 * The "retracts at the authority it was CALLED with" case below has been here
 * all along and passed all along -- while the shipped code was doing the
 * opposite. db2_fact_actor_from_request() consults kb_reqctx_actor(), a WEAK
 * symbol that no test binary links, so it always returned -1 here and the code
 * fell through to the authority-derived branch, which is correct. In the KB the
 * symbol IS defined, the request context always exists for a tool call inside a
 * user's turn, and the wrong branch ran every time.
 *
 * So the assertion was right and blind at once: it could not reach the
 * condition that breaks it. Defining these makes the failing path reachable, so
 * the test now fails if the declared authority is ever again treated as a
 * fallback for the request identity rather than a cap on it. */
static int g_reqctx_authenticated = 0;

const kb_principal_t *kb_reqctx_actor(void)
{
   static kb_principal_t p;
   if (!g_reqctx_authenticated)
      return NULL;
   memset(&p, 0, sizeof(p));
   p.authenticated = 1;
   p.kind = KB_PRIN_OIDC;
   snprintf(p.subject, sizeof(p.subject), "%s", "alice@example.com");
   return &p;
}

int kb_identity_key(const kb_principal_t *p, char *out, size_t cap)
{
   if (!p || !out || !cap)
      return -1;
   snprintf(out, cap, "oidc:%s", p->subject);
   return 0;
}

static int semantic_count(const char *entity)
{
   edge_t e[64];
   return db2_entity_edges_semantic_by_entity(entity, e, 64);
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

static int module_extract_patterns(const char *text, pattern_triple_t *out, int max, int *count)
{
   if (!text || !out || max <= 0 || !count)
      return -1;
   int n = 0;
   const char *cursor = text;
   while (n < max)
   {
      const char *prefix = strstr(cursor, "my ");
      if (!prefix)
         break;
      const char *separator = strstr(prefix + 3, " is ");
      if (!separator)
         break;
      const char *end = strchr(separator + 4, '.');
      if (!end)
         end = text + strlen(text);
      snprintf(out[n].subject, sizeof(out[n].subject), "user");
      snprintf(out[n].rel_type, sizeof(out[n].rel_type), "%.*s", (int)(separator - prefix - 3),
               prefix + 3);
      snprintf(out[n].object, sizeof(out[n].object), "%.*s", (int)(end - separator - 4),
               separator + 4);
      out[n].subject_kind = NODE_PERSON;
      out[n].object_kind = NODE_OTHER;
      ++n;
      cursor = *end ? end + 1 : end;
   }
   *count = n;
   return 0;
}

static int module_scan_turn(const char *text, memory_pattern_turn_t *out)
{
   if (!text || !out)
      return -1;
   memset(out, 0, sizeof(*out));
   out->is_retraction = strstr(text, "forget") != NULL;
   const char *attribute = strstr(text, "my ");
   if (attribute)
   {
      attribute += 3;
      out->has_attr = 1;
      snprintf(out->attr, sizeof(out->attr), "%s", attribute);
   }
   return 0;
}

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
   test_memory_policy_register();
   memory_extract_register_extractor(module_extract_patterns);
   memory_extract_register_turn_scanner(module_scan_turn);
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

   /* THE SAME CALL, WITH SOMEONE AUTHENTICATED. This is the case the KB always
    * runs and this file never could: get_context_block is a tool the model
    * calls inside a human's authenticated turn, so a request context exists and
    * names a real person. The text is still the MODEL's, so MODEL authority must
    * still refuse -- an authenticated human in the session does not make the
    * agent's words the human's.
    *
    * Before the fix this deleted the row: db2_fact_actor_from_request() was
    * tried first and returns FACT_ACTOR_USER for any authenticated principal, so
    * the declared MODEL was discarded whenever this context existed. */
   g_reqctx_authenticated = 1;
   assert(db2_typed_fact_ingress("forget where I work", FACT_AUTHORITY_MODEL, NULL, 0) == 0);
   assert(db2_fact_current_count("user") == 2); /* still refused: the caller's
                                                 * identity must not raise the
                                                 * authority the text was
                                                 * composed at */
   g_reqctx_authenticated = 0;

   assert(db2_typed_fact_ingress("forget where I work", FACT_AUTHORITY_USER, NULL, 0) == 0);
   assert(db2_fact_current_count("user") == 1); /* the user's own retraction lands */

   /* A SEED relation whose declared kinds disagree with the extractor's guess
    * must still commit. The extractor infers kinds from the value's spelling --
    * a bare number is NODE_OTHER -- while `age` declares tail NODE_SCALAR, so
    * the gate rejected it and the fact vanished with no error anywhere. Most
    * seed relations were unreachable from this path for that reason. */
   assert(db2_fact_ingest_text("my age is 41", FACT_AUTHORITY_USER, 1) == 1);
   {
      edge_t e[16];
      int n = db2_entity_edges_semantic_by_entity("user", e, 16);
      int found = 0;
      for (int i = 0; i < n; i++)
         if (strcmp(e[i].relation, "age") == 0 && strcmp(e[i].target, "41") == 0)
            found = 1;
      assert(found); /* rejected on kind before the fixup */
   }

   /* Bad args. */
   assert(db2_fact_ingest_text(NULL, FACT_AUTHORITY_USER, 1) == -1);

   db2_test_shim_close();
   printf("fact_ingest: all tests passed\n");
   return 0;
}
