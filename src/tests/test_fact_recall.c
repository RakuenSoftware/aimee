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
#include "modules/memory/memory_ontology.h"
#include "modules/memory/memory_pii_gate.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

/* Stand-ins for the memory module on the recall path. */
static int failing_batch(const char *const *rel_types, int count, rel_sensitivity_t *out)
{
   (void)rel_types;
   (void)count;
   (void)out;
   return -1;
}

static int g_batch_calls;

static int counting_batch(const char *const *rel_types, int count, rel_sensitivity_t *out)
{
   g_batch_calls++;
   for (int i = 0; i < count; i++)
      out[i] = memory_pii_rel_sensitivity(rel_types[i]);
   return 0;
}

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
    * is skipped (never truncated into the prompt, never over-read). db2_fact_commit
    * caps endpoints below this, so insert the long-target row directly. */
   {
      void *conn = db2_conn();
      assert(conn);
      char longt[400];
      memset(longt, 'z', sizeof(longt) - 1);
      longt[sizeof(longt) - 1] = '\0';
      char err[256] = "";
      aimee_pg_stmt_t *ins = aimee_pg_prepare(
          conn,
          "INSERT INTO entity_edges (source, relation, target, weight, edge_class,"
          " confidence_class, confidence) VALUES ('user', 'bio', ?1, 1, 'semantic', 'C', 0.9)",
          err, sizeof(err));
      assert(ins);
      aimee_pg_bind_text(ins, "?1", longt);
      assert(aimee_pg_step(ins, err, sizeof(err)) == AIMEE_PG_DONE);
      aimee_pg_finalize(ins);
   }
   n = db2_fact_recall_block("user", 1, buf, sizeof(buf));
   assert(n == 2); /* works_for + age; the over-long bio row skipped */
   assert(strstr(buf, "bio") == NULL);

   /* The floor is applied PER FACT. Every fact above is Class A at confidence
    * 1.0, so a gate that read one row's confidence for all of them would agree
    * with all of the above. Insert a below-floor row of an otherwise-injectable
    * (SENS_NORMAL) relation: it must be withheld while its higher-confidence
    * neighbours pass, which only holds if each row is judged on its own. */
   {
      void *conn = db2_conn();
      assert(conn);
      char err[256] = "";
      aimee_pg_stmt_t *ins = aimee_pg_prepare(
          conn,
          "INSERT INTO entity_edges (source, relation, target, weight, edge_class,"
          " confidence_class, confidence) VALUES ('user', 'has_role', 'guesswork', 1,"
          " 'semantic', 'C', 0.2)",
          err, sizeof(err));
      assert(ins);
      assert(aimee_pg_step(ins, err, sizeof(err)) == AIMEE_PG_DONE);
      aimee_pg_finalize(ins);
   }
   n = db2_fact_recall_block("user", 1, buf, sizeof(buf));
   assert(n == 2); /* unchanged: the 0.2 row is below the 0.4 floor */
   assert(strstr(buf, "has_role: guesswork") == NULL);
   assert(strstr(buf, "works_for: acme") != NULL); /* its 1.0 neighbours still pass */
   assert(strstr(buf, "age: 30") != NULL);

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

   /* The block's relations are classified in one call, and a classifier that
    * cannot answer must withhold the whole block rather than let it through as
    * "nothing sensitive here". -1, not 0: the caller has to be able to tell a
    * failed gate from an empty one. */
   {
      memory_pii_register_sensitivity_batch(failing_batch);
      n = db2_fact_recall_block("user", 1, buf, sizeof(buf));
      assert(n == -1);
      assert(buf[0] == '\0'); /* nothing written on the way to failing */
      assert(db2_fact_recall_in_query("what about devbox", 1, buf, sizeof(buf)) == -1);

      /* One call for the whole block, not one per fact: the recall path is on the
       * turn, and per-fact round trips are what this batching exists to avoid. */
      g_batch_calls = 0;
      memory_pii_register_sensitivity_batch(counting_batch);
      n = db2_fact_recall_block("user", 1, buf, sizeof(buf));
      assert(n >= 1);
      assert(g_batch_calls == 1);

      memory_pii_register_sensitivity_batch(NULL);
      assert(db2_fact_recall_block("user", 1, buf, sizeof(buf)) >= 1);
   }

   /* bad args. */
   assert(db2_fact_recall_block(NULL, 0, buf, sizeof(buf)) == -1);
   assert(db2_fact_recall_block("user", 0, NULL, 10) == -1);
   assert(db2_fact_recall_block("user", 0, buf, 0) == -1);

   db2_test_shim_close();
   printf("fact_recall: all tests passed\n");
   return 0;
}
