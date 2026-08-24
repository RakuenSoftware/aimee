/* test_fact_recall.c: typed-fact recall into the envelope + §7 PII gating,
 * against the sqlite shim. P5. */
#include "../headers/aimee.h"
#include "../db2/fact_recall.h"
#include "../db2/fact_lifecycle.h"
#include "../db2/rel_types_store.h"
#include "../db2/entity_registry.h"
#include "../db2/db2_test_shim.h"
#include "../db2/db2_internal.h"
#include "../db2/db_postgres.h"
#include "memory_ontology.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
   db2_test_shim_open();
   assert(db2_rel_types_ensure_seed() == 0);

   /* user facts via the gate (USER authority -> Class A, conf 1.0, above floor):
    * works_for is SENS_NORMAL, age is SENS_PII (per the seed ontology). */
   assert(db2_fact_commit("user", NODE_PERSON, "works_for", "acme", NODE_ORG, FACT_AUTHORITY_USER,
                          1) == FACT_GATE_ACCEPT);
   assert(db2_fact_commit("user", NODE_PERSON, "age", "30", NODE_SCALAR, FACT_AUTHORITY_USER, 1) ==
          FACT_GATE_ACCEPT);

   char buf[2048];

   /* turn does NOT request sensitive info: NORMAL passes, PII withheld. */
   int n = db2_fact_recall_block("user", 0, buf, sizeof(buf));
   assert(n == 1);
   assert(strstr(buf, "works_for: acme") != NULL);
   assert(strstr(buf, "age: 30") == NULL);

   /* turn DOES request sensitive info: PII now passes too. */
   n = db2_fact_recall_block("user", 1, buf, sizeof(buf));
   assert(n == 2);
   assert(strstr(buf, "works_for: acme") != NULL);
   assert(strstr(buf, "age: 30") != NULL);

   /* defensive: a row whose formatted line would exceed the internal 256B buffer
    * is skipped (never truncated into the prompt, never over-read). */
   {
      char longt[400];
      memset(longt, 'z', sizeof(longt) - 1);
      longt[sizeof(longt) - 1] = '\0';
      fact_actor_t actor;
      assert(db2_fact_actor_internal(FACT_ACTOR_SYSTEM, &actor) == 0);
      fact_assertion_input_t input = {.source = "user",
                                      .relation = "bio",
                                      .target = longt,
                                      .confidence_class = "C",
                                      .confidence = 0.9,
                                      .assertion_kind = FACT_KIND_WORLD_FACT};
      assert(db2_fact_mutation_assert(&actor, &input, NULL) == 0);
   }
   n = db2_fact_recall_block("user", 1, buf, sizeof(buf));
   assert(n == 2); /* works_for + age; the over-long bio row skipped */
   assert(strstr(buf, "bio") == NULL);

   /* tight caller buffer: the first line doesn't fit -> no facts, NUL-terminated. */
   n = db2_fact_recall_block("user", 1, buf, 16);
   assert(n == 0 && buf[0] == '\0');

   /* unknown entity -> nothing. */
   n = db2_fact_recall_block("nobody-here", 0, buf, sizeof(buf));
   assert(n == 0 && buf[0] == '\0');

   /* superseded facts are excluded from current-state recall. */
   assert(db2_fact_retract("user", "works_for", NULL, FACT_AUTHORITY_USER) >= 1);
   n = db2_fact_recall_block("user", 1, buf, sizeof(buf));
   assert(n == 1);
   assert(strstr(buf, "works_for") == NULL); /* superseded -> not current */
   assert(strstr(buf, "age: 30") != NULL);

   /* query-scoped recall: the user's facts PLUS facts about an entity named in the
    * turn. Register DevBox (+alias) with a fact, then a query mentioning it. */
   int64_t dev = db2_entity_register_named("DevBox", NODE_DEVICE);
   assert(dev > 0);
   assert(db2_entity_alias_bind("the workstation", dev, 0) == 0);
   assert(db2_fact_commit("DevBox", NODE_DEVICE, "device_has_ip", "10.0.0.5", NODE_IP,
                          FACT_AUTHORITY_USER, 1) == FACT_GATE_ACCEPT);
   /* a query mentioning DevBox surfaces its fact (request sensitive so any PII
    * passes; we only assert the entity-scoping here). */
   n = db2_fact_recall_in_query("what is the ip of devbox", 1, buf, sizeof(buf));
   assert(strstr(buf, "device_has_ip: 10.0.0.5") != NULL); /* DevBox fact recalled */
   assert(strstr(buf, "age: 30") != NULL);                 /* user fact also present */
   /* the entity is reachable via its alias too (registry resolution). */
   n = db2_fact_recall_in_query("tell me about the workstation", 1, buf, sizeof(buf));
   assert(strstr(buf, "device_has_ip: 10.0.0.5") != NULL);

   /* A durable fact whose subject kind is OTHER has no entity_registry row. It
    * is still query-recallable by its direct assertion subject after promotion /
    * persistence; otherwise LLM facts with an unknown subject kind disappear
    * even after an operator approves them. */
   assert(db2_fact_commit("warehouse", NODE_OTHER, "located_in", "Rotterdam", NODE_PLACE,
                          FACT_AUTHORITY_USER, 1) == FACT_GATE_ACCEPT);
   n = db2_fact_recall_in_query("what is known about the warehouse", 1, buf, sizeof(buf));
   assert(strstr(buf, "located_in: Rotterdam") != NULL);

   /* a query naming no known entity -> just the user's facts. */
   n = db2_fact_recall_in_query("what's the weather", 1, buf, sizeof(buf));
   assert(strstr(buf, "device_has_ip") == NULL);
   assert(strstr(buf, "age: 30") != NULL);
   assert(db2_fact_recall_in_query(NULL, 0, buf, sizeof(buf)) == -1);

   /* multi-entity: a query naming two devices recalls both. */
   int64_t rt = db2_entity_register_named("RouterX", NODE_DEVICE);
   assert(rt > 0);
   assert(db2_fact_commit("RouterX", NODE_DEVICE, "device_has_ip", "10.0.0.9", NODE_IP,
                          FACT_AUTHORITY_USER, 1) == FACT_GATE_ACCEPT);
   n = db2_fact_recall_in_query("compare devbox and routerx", 1, buf, sizeof(buf));
   assert(strstr(buf, "10.0.0.5") != NULL); /* DevBox */
   assert(strstr(buf, "10.0.0.9") != NULL); /* RouterX */

   /* PII gating still applies in the query path: the user's PII age is withheld
    * when the turn does not request sensitive info (even while an entity matches). */
   n = db2_fact_recall_in_query("what about devbox", 0, buf, sizeof(buf));
   assert(strstr(buf, "device_has_ip: 10.0.0.5") != NULL); /* NORMAL device fact passes */
   assert(strstr(buf, "age: 30") == NULL);                 /* user PII withheld */

   /* tight caller buffer: bounded, NUL-terminated, no overflow. */
   n = db2_fact_recall_in_query("devbox", 1, buf, 12);
   assert(n >= 0 && strlen(buf) < 12);

   /* bad args. */
   assert(db2_fact_recall_block(NULL, 0, buf, sizeof(buf)) == -1);
   assert(db2_fact_recall_block("user", 0, NULL, 10) == -1);
   assert(db2_fact_recall_block("user", 0, buf, 0) == -1);

   db2_test_shim_close();
   printf("fact_recall: all tests passed\n");
   return 0;
}
