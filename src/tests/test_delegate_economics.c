/* test_delegate_economics.c: supervisor-centric delegate economics tests */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include <aimee/delegates/delegate_economics.h>
#include "cmd_agent_delegate_impl.h"
#include "cJSON.h"

static void add_agent(agent_config_t *cfg, int idx, const char *name, int tier)
{
   assert(idx >= 0 && idx < MAX_AGENTS);
   snprintf(cfg->agents[idx].name, sizeof(cfg->agents[idx].name), "%s", name);
   cfg->agents[idx].cost_tier = tier;
   if (idx >= cfg->agent_count)
      cfg->agent_count = idx + 1;
}

static void test_agent_result_json_metadata(void)
{
   agent_config_t cfg = {0};
   add_agent(&cfg, 0, "free-a", 0);

   agent_result_t result = {0};
   snprintf(result.agent_name, sizeof(result.agent_name), "%s", "free-a");
   result.prompt_tokens = 11;
   result.completion_tokens = 7;
   result.cache_read_tokens = 3;
   result.cache_write_tokens = 5;

   cJSON *obj = cJSON_CreateObject();
   delegate_economics_add_agent_result_json(obj, &cfg, "review", &result, NULL);
   assert(strcmp(cJSON_GetObjectItem(obj, "agent")->valuestring, "free-a") == 0);
   assert(cJSON_GetObjectItem(obj, "agent_cost_tier")->valueint == 0);
   assert(strcmp(cJSON_GetObjectItem(obj, "delegate_cost_model")->valuestring,
                 DELEGATE_ECONOMICS_COST_MODEL) == 0);
   assert(cJSON_GetObjectItem(obj, "delegate_tokens_estimated")->valueint == 18);
   assert(cJSON_GetObjectItem(obj, "delegate_cache_read_tokens")->valueint == 3);
   cJSON_Delete(obj);
   printf("  PASS: test_agent_result_json_metadata\n");
}

/* Judging a handoff is the delegates module's rule now
 * (server-go/modules/delegates/handoff.go) and this binary hosts no bus. The
 * subject of these tests is COORDINATION -- which packets are reviewable, which
 * conflict, which need a supervisor -- so the test supplies the verdicts it
 * wants to coordinate over.
 *
 * This is deliberately NOT the rule. It does not check schema_version, status
 * admission, summary presence or the done-without-verification downgrade; it
 * reads only the two numbers these fixtures vary, so it cannot drift into a
 * second copy of a rule that lives in exactly one place. */
static int econ_test_handoff_provider(const char *text, const char *owned_files_json,
                                      int require_verification, delegate_handoff_validation_t *out)
{
   (void)require_verification;
   memset(out, 0, sizeof(*out));
   snprintf(out->status, sizeof(out->status), "%s", "needs_supervisor_review");
   if (!text || !text[0])
      return -1;

   cJSON *root = cJSON_Parse(text);
   if (!cJSON_IsObject(root))
   {
      cJSON_Delete(root);
      snprintf(out->error, sizeof(out->error), "%s", "handoff is not valid JSON object");
      out->needs_supervisor_review = 1;
      return -1;
   }

   cJSON *changed = cJSON_GetObjectItemCaseSensitive(root, "changed_files");
   cJSON *tests = cJSON_GetObjectItemCaseSensitive(root, "tests");
   cJSON *owned = owned_files_json ? cJSON_Parse(owned_files_json) : NULL;

   cJSON *item = NULL;
   cJSON_ArrayForEach(item, tests)
   {
      cJSON *st = cJSON_GetObjectItemCaseSensitive(item, "status");
      if (cJSON_IsString(st) && strcmp(st->valuestring, "passed") == 0)
         out->passed_tests++;
   }
   cJSON_ArrayForEach(item, changed)
   {
      if (!cJSON_IsString(item))
         continue;
      out->changed_files_count++;
      int owned_here = 0;
      cJSON *o = NULL;
      cJSON_ArrayForEach(o, owned)
      {
         if (cJSON_IsString(o) && strcmp(o->valuestring, item->valuestring) == 0)
         {
            owned_here = 1;
            break;
         }
      }
      if (cJSON_IsArray(owned) && cJSON_GetArraySize(owned) > 0 && !owned_here)
         out->outside_ownership_count++;
   }
   cJSON_Delete(owned);

   cJSON *raw = cJSON_GetObjectItemCaseSensitive(root, "status");
   if (cJSON_IsString(raw))
   {
      snprintf(out->raw_status, sizeof(out->raw_status), "%s", raw->valuestring);
      snprintf(out->status, sizeof(out->status), "%s", raw->valuestring);
   }
   cJSON_Delete(root);

   out->valid = 1;
   if (out->outside_ownership_count > 0)
   {
      snprintf(out->status, sizeof(out->status), "%s", "needs_supervisor_review");
      snprintf(out->error, sizeof(out->error), "%s", "handoff touched files outside owned_files");
      out->needs_supervisor_review = 1;
   }
   return 0;
}

int main(void)
{
   delegate_register_handoff_provider(econ_test_handoff_provider);
   test_agent_result_json_metadata();
   printf("delegate_economics: all tests passed\n");
   return 0;
}
