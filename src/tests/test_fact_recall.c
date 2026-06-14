/* test_fact_recall.c: typed-fact recall into the envelope + §7 PII gating,
 * against the sqlite shim. P5. */
#include "../headers/aimee.h"
#include "../db2/fact_recall.h"
#include "../db2/fact_lifecycle.h"
#include "../db2/rel_types_store.h"
#include "../db2/db2_test_shim.h"
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
