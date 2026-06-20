/* test_typed_facts.c: typed-fact store + write gate over the sqlite shim —
 * ontology gate, kind validation, contradiction-supersede, recall. */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "aimee.h"
#include "db2_test_shim.h"
#include "../db2/typed_facts.h"
#include "../db2/fact_recall.h"     /* db2_fact_recall_block */
#include "../db2/fact_lifecycle.h"  /* FACT_AUTHORITY_MODEL */
#include "../db2/rel_types_store.h" /* db2_fact_commit */
#include "memory_fact_gate.h"       /* FACT_GATE_NOVEL */
#include "memory_ontology.h"        /* NODE_PERSON, NODE_OTHER */

int main(void)
{
   db2_test_shim_open();

   const char *T = "2026-01-01T00:00:00Z";

   /* ontology membership */
   assert(typed_fact_relation_known("naming_convention"));
   assert(!typed_fact_relation_known("totally_made_up"));

   /* assert a convention fact */
   assert(db2_typed_fact_assert("fizzy", "project", "naming_convention", "BEM", "scalar", 90,
                                "exemplar-scan", T) == TYPED_FACT_OK);

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
                                "human-correction", T) == TYPED_FACT_OK);
   n = db2_typed_fact_recall("fizzy", "naming_convention", f, 16);
   assert(n == 1 && strcmp(f[0].object, "utility") == 0); /* new value wins; old superseded */

   /* unknown relation is rejected by the gate */
   assert(db2_typed_fact_assert("fizzy", "project", "vibes", "good", "scalar", 50, "x", T) ==
          TYPED_FACT_REJECTED_REL);

   /* kind mismatch rejected: should_match needs subject_kind=component */
   assert(db2_typed_fact_assert("fizzy", "project", "should_match", "conv", "convention", 50, "x",
                                T) == TYPED_FACT_REJECTED_KIND);

   /* per-component should_match facts + by_relation lookup */
   assert(db2_typed_fact_assert("Button.tsx", "component", "should_match", "btn_convention",
                                "convention", 70, "driver", T) == TYPED_FACT_OK);
   assert(db2_typed_fact_assert("Card.tsx", "component", "should_match", "card_convention",
                                "convention", 70, "driver", T) == TYPED_FACT_OK);
   n = db2_typed_fact_by_relation("should_match", f, 16);
   assert(n == 2);

   /* recall all relations for a subject */
   assert(db2_typed_fact_assert("fizzy", "project", "token_strategy", "css-vars", "scalar", 85, "x",
                                T) == TYPED_FACT_OK);
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
    * withheld on an ordinary turn, surfaced only when the turn asks. */
   assert(db2_fact_commit("user", NODE_PERSON, "home_address", "12 Oak St", NODE_OTHER,
                          FACT_AUTHORITY_MODEL, 1) == FACT_GATE_NOVEL);
   char ord[1024] = "";
   (void)db2_fact_recall_block("user", 0, ord, sizeof(ord));
   assert(strstr(ord, "home_address") == NULL); /* PII-looking: withheld by default */
   char sens[1024] = "";
   (void)db2_fact_recall_block("user", 1, sens, sizeof(sens));
   assert(strstr(sens, "home_address") != NULL); /* surfaced when asked */

   db2_test_shim_close();
   printf("typed_facts: all tests passed\n");
   return 0;
}
