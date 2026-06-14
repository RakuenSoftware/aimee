/* test_fact_recall.c: typed-fact recall into the envelope + §7 PII gating,
 * against the sqlite shim. P5. */
#include "../headers/aimee.h"
#include "../db2/fact_recall.h"
#include "../db2/fact_lifecycle.h"
#include "../db2/rel_types_store.h"
#include "../db2/db2_test_shim.h"
#include "../db2/db2_internal.h"
#include "../db2/db_postgres.h"
#include "../headers/memory_ontology.h"
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

   /* tight caller buffer: the first line doesn't fit -> no facts, NUL-terminated. */
   n = db2_fact_recall_block("user", 1, buf, 16);
   assert(n == 0 && buf[0] == '\0');

   /* unknown entity -> nothing. */
   n = db2_fact_recall_block("nobody-here", 0, buf, sizeof(buf));
   assert(n == 0 && buf[0] == '\0');

   /* superseded facts are excluded from current-state recall. */
   assert(db2_fact_retract("user", "works_for", FACT_AUTHORITY_USER) >= 1);
   n = db2_fact_recall_block("user", 1, buf, sizeof(buf));
   assert(n == 1);
   assert(strstr(buf, "works_for") == NULL); /* superseded -> not current */
   assert(strstr(buf, "age: 30") != NULL);

   /* bad args. */
   assert(db2_fact_recall_block(NULL, 0, buf, sizeof(buf)) == -1);
   assert(db2_fact_recall_block("user", 0, NULL, 10) == -1);
   assert(db2_fact_recall_block("user", 0, buf, 0) == -1);

   db2_test_shim_close();
   printf("fact_recall: all tests passed\n");
   return 0;
}
