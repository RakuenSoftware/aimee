/* test_fact_recall.c: DB2's bounded host-provider recall ABI. */
#include "modules/db2/c/fact_recall.h"

#include "modules/db2/include/aimee/db2/host_contracts.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static int g_fail;
static int g_unterminated;
static int g_sensitive;

static int recall_provider(const char *entity, const char *query, int turn_requests_sensitive,
                           char *out, size_t cap, int *count)
{
   assert((entity != NULL) != (query != NULL));
   g_sensitive = turn_requests_sensitive;
   if (g_fail)
      return -1;
   if (g_unterminated)
   {
      memset(out, 'x', cap);
      *count = 1;
      return 0;
   }
   const char *block = entity ? "- works_for: acme\n" : "- device_has_ip: 10.0.0.5\n";
   if (strlen(block) >= cap)
      return -1;
   memcpy(out, block, strlen(block) + 1);
   *count = 1;
   return 0;
}

int main(void)
{
   char out[128] = "dirty";

   /* An absent memory owner fails closed and never leaks stale caller bytes. */
   aimee_db2_register_fact_recall_provider(NULL);
   assert(db2_fact_recall_block("user", 0, out, sizeof(out)) == -1);
   assert(out[0] == '\0');

   aimee_db2_register_fact_recall_provider(recall_provider);
   assert(db2_fact_recall_block("user", 1, out, sizeof(out)) == 1);
   assert(strcmp(out, "- works_for: acme\n") == 0);
   assert(g_sensitive == 1);
   assert(db2_fact_recall_in_query("what about devbox", 0, out, sizeof(out)) == 1);
   assert(strcmp(out, "- device_has_ip: 10.0.0.5\n") == 0);
   assert(g_sensitive == 0);

   g_fail = 1;
   assert(db2_fact_recall_block("user", 0, out, sizeof(out)) == -1);
   assert(out[0] == '\0');
   g_fail = 0;

   /* A provider may not hand an unterminated block across the ABI. */
   g_unterminated = 1;
   assert(db2_fact_recall_block("user", 0, out, sizeof(out)) == -1);
   assert(out[0] == '\0');
   g_unterminated = 0;

   assert(db2_fact_recall_block(NULL, 0, out, sizeof(out)) == -1);
   assert(db2_fact_recall_block("", 0, out, sizeof(out)) == -1);
   assert(db2_fact_recall_block("user", 0, NULL, sizeof(out)) == -1);
   assert(db2_fact_recall_block("user", 0, out, 0) == -1);
   assert(db2_fact_recall_in_query(NULL, 0, out, sizeof(out)) == -1);

   puts("fact_recall: all tests passed");
   return 0;
}
