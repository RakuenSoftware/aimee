/* cmd_memory_lint.c: `aimee memory lint` subcommand. */

#include "aimee.h"
#include "cmd_memory_internal.h"
#include "kb_client.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void mem_lint(app_ctx_t *ctx, int argc, char **argv)
{
   (void)argc;
   (void)argv;
   char *envelope = kb_v1_action_request("memory.lint", cJSON_CreateObject());
   cJSON *resp = envelope ? cJSON_Parse(envelope) : NULL;
   free(envelope);

   if (!resp)
      fatal("memory lint failed: no response from knowledge service");

   cJSON *issues = cJSON_GetObjectItemCaseSensitive(resp, "issues");
   cJSON *count_j = cJSON_GetObjectItemCaseSensitive(resp, "issue_count");
   int issue_count = cJSON_IsNumber(count_j) ? (int)count_j->valuedouble : 0;

   if (ctx->json_output)
   {
      emit_json_ctx(resp, ctx->json_fields, ctx->response_profile);
      return;
   }

   if (issue_count == 0)
   {
      printf("memory lint: ok (no issues)\n");
      cJSON_Delete(resp);
      return;
   }

   printf("memory lint: %d issue%s\n\n", issue_count, issue_count == 1 ? "" : "s");
   cJSON *iss = NULL;
   cJSON_ArrayForEach(iss, issues)
   {
      cJSON *type_j = cJSON_GetObjectItemCaseSensitive(iss, "type");
      cJSON *key_j = cJSON_GetObjectItemCaseSensitive(iss, "key");
      cJSON *msg_j = cJSON_GetObjectItemCaseSensitive(iss, "message");
      cJSON *id_j = cJSON_GetObjectItemCaseSensitive(iss, "memory_id");
      const char *type = cJSON_IsString(type_j) ? type_j->valuestring : "?";
      const char *key = cJSON_IsString(key_j) ? key_j->valuestring : "";
      const char *msg = cJSON_IsString(msg_j) ? msg_j->valuestring : "";
      if (cJSON_IsNumber(id_j))
         printf("[%s] #%lld %s \xe2\x80\x94 %s\n", type, (long long)(int64_t)id_j->valuedouble, key,
                msg);
      else
         printf("[%s] %s \xe2\x80\x94 %s\n", type, key, msg);
   }
   cJSON_Delete(resp);
}
