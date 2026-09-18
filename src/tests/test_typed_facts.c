/* test_typed_facts.c: typed-fact store + write gate over the sqlite shim —
 * ontology gate, kind validation, contradiction-supersede, recall. */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "aimee.h"
#include "modules/db2/c/db2_test_shim.h"
#include "modules/db2/c/db2.h"
#include "modules/db2/c/db_postgres.h"
#include "../modules/db2/c/typed_facts.h"
#include "../modules/db2/c/fact_lifecycle.h"  /* FACT_AUTHORITY_MODEL */
#include "../modules/db2/c/rel_types_store.h" /* db2_fact_commit */
#include "modules/memory/memory_fact_gate.h"  /* FACT_GATE_NOVEL */
#include "modules/memory/memory_ontology.h"   /* NODE_PERSON, NODE_OTHER */
#include "support/memory_policy_stub.h"

int main(void)
{
   db2_test_shim_open();
   test_memory_policy_register();

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
    * free-form (NOVEL) relation as a provisional semantic edge. */
   assert(db2_fact_commit("user", NODE_PERSON, "works_as", "engineer", NODE_OTHER,
                          FACT_AUTHORITY_MODEL, 1) == FACT_GATE_NOVEL);
   /* Authenticated-user evidence promotes the exact candidate to persistent. */
   assert(db2_fact_commit("user", NODE_PERSON, "works_as", "engineer", NODE_OTHER,
                          FACT_AUTHORITY_USER, 1) == FACT_GATE_NOVEL);
   /* Unknown PII relations remain candidates until promoted by authenticated
    * user evidence. Recall policy itself is covered by the Go memory owner. */
   assert(db2_fact_commit("user", NODE_PERSON, "home_address", "12 Oak St", NODE_OTHER,
                          FACT_AUTHORITY_MODEL, 1) == FACT_GATE_NOVEL);
   assert(db2_fact_commit("user", NODE_PERSON, "home_address", "12 Oak St", NODE_OTHER,
                          FACT_AUTHORITY_USER, 1) == FACT_GATE_NOVEL);
   /* Personal-data boundary (Track A): a CREDENTIAL relation is withheld from the
    * shared KB entirely. */
   assert(db2_fact_commit("user", NODE_PERSON, "api_key", "sk-123", NODE_OTHER,
                          FACT_AUTHORITY_MODEL, 1) == FACT_GATE_REJECT_SENSITIVE);

   /* Temporal search, scoped evidence and versioned assertion indexing now
    * run through the Go owner in assertion_search_test.go with PostgreSQL. */

   db2_test_shim_close();
   printf("typed_facts: all tests passed\n");
   return 0;
}
