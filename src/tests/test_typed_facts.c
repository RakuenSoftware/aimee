/* test_typed_facts.c: typed-fact store + write gate over the sqlite shim —
 * ontology gate, kind validation, contradiction-supersede, recall. */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "aimee.h"
#include "modules/db2/c/db2_test_shim.h"
#include "../modules/db2/c/typed_facts.h"
#include "../modules/db2/c/fact_recall.h"     /* db2_fact_recall_block */
#include "../modules/db2/c/fact_lifecycle.h"  /* FACT_AUTHORITY_MODEL */
#include "../modules/db2/c/rel_types_store.h" /* db2_fact_commit */
#include "modules/memory/memory_fact_gate.h"  /* FACT_GATE_NOVEL */
#include "modules/memory/memory_ontology.h"   /* NODE_PERSON, NODE_OTHER */

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

   const char *T = "2026-01-01T00:00:00Z";

   /* ontology membership */
   assert(typed_fact_relation_known("naming_convention"));
   assert(!typed_fact_relation_known("totally_made_up"));

   /* assert a convention fact */
   assert(db2_typed_fact_assert("fizzy", "project", "naming_convention", "BEM", "scalar", 90,
                                "exemplar-scan", T) == TYPED_FACT_ASSERTED);

   typed_fact_t f[16];
   int n = db2_typed_fact_recall("fizzy", "naming_convention", f, 16);
   assert(n == 1);
   assert(strcmp(f[0].object, "BEM") == 0 && f[0].confidence == 90);
   assert(strcmp(f[0].source, "exemplar-scan") == 0);

   /* idempotent re-assert of the identical fact */
   assert(db2_typed_fact_assert("fizzy", "project", "naming_convention", "BEM", "scalar", 90,
                                "exemplar-scan", T) == TYPED_FACT_UNCHANGED);
   assert(db2_typed_fact_recall("fizzy", "naming_convention", f, 16) == 1);

   /* contradiction supersedes the prior value (history retained, only 1 active) */
   assert(db2_typed_fact_assert("fizzy", "project", "naming_convention", "utility", "scalar", 80,
                                "human-correction", T) == TYPED_FACT_ASSERTED);
   n = db2_typed_fact_recall("fizzy", "naming_convention", f, 16);
   assert(n == 1 && strcmp(f[0].object, "utility") == 0); /* new value wins; old superseded */

   /* unknown relation is rejected by the gate */
   assert(db2_typed_fact_assert("fizzy", "project", "vibes", "good", "scalar", 50, "x", T) ==
          TYPED_FACT_REJECTED_REL);

   /* kind mismatch rejected: should_match needs subject_kind=component */
   assert(db2_typed_fact_assert("fizzy", "project", "should_match", "conv", "convention", 50, "x",
                                T) == TYPED_FACT_REJECTED_KIND);

   /* A rejection is a SUCCESS in the project result convention: the gate was
    * asked whether this proposition may be stored and it answered. Only a
    * failure to run is negative. Pinned by sign rather than by value so this
    * keeps meaning what it says if the band codes are ever renumbered. */
   assert(AIMEE_SUCCEEDED(TYPED_FACT_REJECTED_REL));
   assert(AIMEE_SUCCEEDED(TYPED_FACT_REJECTED_KIND));
   assert(AIMEE_SUCCEEDED(TYPED_FACT_ASSERTED));
   assert(AIMEE_SUCCEEDED(TYPED_FACT_UNCHANGED));
   assert(AIMEE_FAILED_P(TYPED_FACT_INVALID));
   assert(AIMEE_FAILED_P(TYPED_FACT_UNAVAILABLE));
   assert(AIMEE_FAILED_P(TYPED_FACT_ERROR));

   /* And nothing the gate answers is zero, because the gate always finishes. */
   assert(!AIMEE_PENDING_P(TYPED_FACT_ASSERTED) && !AIMEE_PENDING_P(TYPED_FACT_UNCHANGED) &&
          !AIMEE_PENDING_P(TYPED_FACT_REJECTED_REL) && !AIMEE_PENDING_P(TYPED_FACT_REJECTED_KIND) &&
          !AIMEE_PENDING_P(TYPED_FACT_INVALID) && !AIMEE_PENDING_P(TYPED_FACT_UNAVAILABLE) &&
          !AIMEE_PENDING_P(TYPED_FACT_ERROR));

   /* An incoherent request is not a rejection: one is an answer about the
    * proposition, the other is that there was never a question. */
   assert(db2_typed_fact_assert(NULL, "project", "naming_convention", "BEM", "scalar", 50, "x",
                                T) == TYPED_FACT_INVALID);
   assert(TYPED_FACT_INVALID != TYPED_FACT_ERROR);

   /* per-component should_match facts + by_relation lookup */
   assert(db2_typed_fact_assert("Button.tsx", "component", "should_match", "btn_convention",
                                "convention", 70, "driver", T) == TYPED_FACT_ASSERTED);
   assert(db2_typed_fact_assert("Card.tsx", "component", "should_match", "card_convention",
                                "convention", 70, "driver", T) == TYPED_FACT_ASSERTED);
   n = db2_typed_fact_by_relation("should_match", f, 16);
   assert(n == 2);

   /* recall all relations for a subject */
   assert(db2_typed_fact_assert("fizzy", "project", "token_strategy", "css-vars", "scalar", 85, "x",
                                T) == TYPED_FACT_ASSERTED);
   n = db2_typed_fact_recall("fizzy", NULL, f, 16);
   assert(n == 2); /* naming_convention(utility) + token_strategy */

   /* db2_fact_commit path (entity_edges) — the one the memory-fact extractor and
    * auto-inject use. Unlike the strict CSS assert above, this gate ACCEPTs a
    * free-form (NOVEL) relation as a provisional semantic edge, and recall must
    * surface it. This is the assumption the LLM extractor relies on (it emits
    * arbitrary snake_case relations). */
   assert(db2_fact_commit("user", NODE_PERSON, "works_as", "engineer", NODE_OTHER,
                          FACT_AUTHORITY_MODEL, 1) == FACT_GATE_NOVEL);
   /* A free-form (unknown) relation now defaults OPEN, so a benign LLM fact like
    * works_as surfaces on an ORDINARY turn (turn_requests_sensitive=0). */
   char facts[1024] = "";
   int fn = db2_fact_recall_block("user", 0, facts, sizeof(facts));
   assert(fn >= 1);
   assert(strstr(facts, "works_as") != NULL && strstr(facts, "engineer") != NULL);

   /* But an unknown relation whose NAME plainly denotes PII is still gated:
    * withheld on an ordinary turn, surfaced only when the turn asks. (PII facts
    * remain in the typed-fact layer, recall-gated — only credentials are dropped;
    * see the api_key assertion below.) */
   assert(db2_fact_commit("user", NODE_PERSON, "home_address", "12 Oak St", NODE_OTHER,
                          FACT_AUTHORITY_MODEL, 1) == FACT_GATE_NOVEL);
   char ord[1024] = "";
   (void)db2_fact_recall_block("user", 0, ord, sizeof(ord));
   assert(strstr(ord, "home_address") == NULL); /* PII-looking: withheld by default */
   char sens[1024] = "";
   (void)db2_fact_recall_block("user", 1, sens, sizeof(sens));
   assert(strstr(sens, "home_address") != NULL); /* surfaced when asked */

   /* Personal-data boundary (Track A): a CREDENTIAL relation is withheld from the
    * shared KB entirely — never committed to DB2, so it never surfaces there. */
   assert(db2_fact_commit("user", NODE_PERSON, "api_key", "sk-123", NODE_OTHER,
                          FACT_AUTHORITY_MODEL, 1) == FACT_GATE_REJECT_SENSITIVE);
   char cred[1024] = "";
   (void)db2_fact_recall_block("user", 1, cred, sizeof(cred));
   assert(strstr(cred, "api_key") == NULL); /* not in the shared KB, even when asked */

   db2_test_shim_close();
   printf("typed_facts: all tests passed\n");
   return 0;
}
