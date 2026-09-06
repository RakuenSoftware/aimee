/* Legacy typed-fact recall ABI over a host-installed memory provider. */
#include "fact_recall.h"
#include <string.h>

#define FACT_RECALL_MAX_CAP (512u * 1024u)

static aimee_db2_fact_recall_fn g_fact_recall_provider;

void aimee_db2_register_fact_recall_provider(aimee_db2_fact_recall_fn provider)
{
   g_fact_recall_provider = provider;
}

static int fact_recall_call(const char *entity, const char *query, int turn_requests_sensitive,
                            char *out, size_t cap)
{
   if ((!entity && !query) || (entity && query) || !out || cap == 0 || cap > FACT_RECALL_MAX_CAP)
      return -1;
   out[0] = '\0';
   int count = -1;
   if (!g_fact_recall_provider ||
       g_fact_recall_provider(entity, query, turn_requests_sensitive != 0, out, cap, &count) != 0 ||
       count < 0 || !memchr(out, '\0', cap))
   {
      out[0] = '\0';
      return -1;
   }
   return count;
}

int db2_fact_recall_block(const char *entity, int turn_requests_sensitive, char *out, size_t cap)
{
   if (!entity || !entity[0])
      return -1;
   return fact_recall_call(entity, NULL, turn_requests_sensitive, out, cap);
}

int db2_fact_recall_in_query(const char *query, int turn_requests_sensitive, char *out, size_t cap)
{
   if (!query)
      return -1;
   return fact_recall_call(NULL, query, turn_requests_sensitive, out, cap);
}
