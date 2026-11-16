/* Actual audit producer, with the storage boundary captured synchronously. */
#include "agent_exec.h"
#include "db1_client/db1.h"
#include "request_context.h"
#include <assert.h>
#include <stdio.h>

static char kind[32];
static int inserts;
const request_context_t *request_context_get(void)
{
   return NULL;
}
const char *delegation_active_id(void)
{
   return "usage-delegate";
}
const char *session_id(void)
{
   return "usage-session";
}
int config_ingress_audit_async(void)
{
   return 0;
}
int db1_token_audit_insert(const db1_token_audit_row_t *row)
{
   assert(!strcmp(row->delegation_id, "usage-delegate"));
   snprintf(kind, sizeof kind, "%s", row->usage_kind);
   inserts++;
   return 0;
}
int main(void)
{
   agent_result_t result = {0};
   strcpy(result.model, "gpt-4o");
   agent_record_token_audit(&result, "draft", "agent");
   assert(inserts == 1 && !strcmp(kind, "realized")); /* Observed zero is valid. */
   result.canonical_response.usage_unobserved = 1;
   agent_record_token_audit(&result, "draft", "agent");
   assert(inserts == 2 && !strcmp(kind, "unobserved"));
   agent_result_clear_response(&result);
   assert(result.usage_unobserved);
   result.prompt_tokens = 10; /* Known partial attempt cannot repair an unknown total. */
   agent_record_token_audit(&result, "draft", "agent");
   assert(inserts == 3 && !strcmp(kind, "unobserved"));
   result.cache_hit = 1;
   agent_record_token_audit(&result, "draft", "agent");
   assert(inserts == 3);
   puts("agent usage audit: observed-zero and unknown aggregate distinct");
}
