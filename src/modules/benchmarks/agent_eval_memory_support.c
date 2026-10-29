#include "json_fluent.h"
#include "module_commands.h"
/* _GNU_SOURCE: strcasestr/memmem are GNU extensions; declare them before any
 * libc header so gcc-12 (the container toolchain) does not implicit-decl + -Werror. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
/* agent_eval_memory_support.c: live-corpus host adapters and benchmark transport.
 * Split from agent_eval.c to keep the core eval harness under the lint cap. */
#include "aimee.h"
#include "agent_eval.h"
#include "agent_eval_internal.h"
#include "agent_config.h"
#include "agent_exec.h"
#include "config.h"
#include "config_database.h"
#include "memory.h"
#include <math.h>
#include "lifecycle.h"
#include "cJSON.h"
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* --- Corpus-based memory retrieval eval --- */

/* Temporary host transport while the remaining benchmark runner migrates. */
int mem_eval_dispatch_diagnostic(cJSON *args, cJSON **reply)
{
   *reply = NULL;
   return aimee_module_commands_dispatch_internal("memory.runtime", args, reply);
}

int mem_eval_write_hard_negative(FILE *fp, const char *suite, const eval_task_t *task,
                                 const agent_result_t *result)
{
   if (!fp || !task || !result)
      return -1;
   if (aimee_ir_response_complete_text_valid(&result->canonical_response))
      return -1;
   char *authored = aimee_ir_response_text_alloc(&result->canonical_response);
   if (!authored)
      return -1;
   cJSON *args = cJSON_CreateObject(), *reply = NULL;
   if (!args || !cJSON_AddStringToObject(args, "operation", "benchmark-hard-negative") ||
       !cJSON_AddStringToObject(args, "suite", suite ? suite : "") ||
       !cJSON_AddStringToObject(args, "task", task->name) ||
       !cJSON_AddStringToObject(args, "query", task->prompt) ||
       !cJSON_AddStringToObject(args, "expected", task->success_check_value) ||
       !cJSON_AddStringToObject(args, "got", authored) ||
       !cJSON_AddStringToObject(args, "error", result->error))
   {
      free(authored);
      cJSON_Delete(args);
      return -1;
   }
   free(authored);
   int rc = mem_eval_dispatch_diagnostic(args, &reply);
   cJSON_Delete(args);
   const cJSON *line = cJSON_GetObjectItemCaseSensitive(reply, "line");
   if (rc <= 0 || strcmp(jo_cstr(reply, "status"), "ok") || !cJSON_IsString(line) ||
       !line->valuestring[0] || strlen(line->valuestring) > 1048576 ||
       strchr(line->valuestring, '\n') || strchr(line->valuestring, '\r'))
   {
      cJSON_Delete(reply);
      return -1;
   }
   rc = fprintf(fp, "%s\n", line->valuestring) < 0 || fflush(fp) != 0 ? -1 : 0;
   cJSON_Delete(reply);
   return rc;
}

int mem_eval_build_retrieval_context(const char *query, int top_k, int token_budget,
                                     char *context_out, size_t context_len,
                                     int *retrieved_tokens_out)
{
   if (retrieved_tokens_out)
      *retrieved_tokens_out = 0;
   if (!context_out || context_len == 0)
      return -1;
   context_out[0] = '\0';
   if (!query)
      return -1;
   cJSON *args = cJSON_CreateObject(), *reply = NULL;
   if (!args || !cJSON_AddStringToObject(args, "operation", "benchmark-context") ||
       !cJSON_AddStringToObject(args, "query", query) ||
       !cJSON_AddNumberToObject(args, "top_k", top_k) ||
       !cJSON_AddNumberToObject(args, "token_budget", token_budget) ||
       !cJSON_AddNumberToObject(args, "capacity", context_len))
   {
      cJSON_Delete(args);
      return -1;
   }
   int rc = aimee_module_commands_dispatch_internal("memory.runtime", args, &reply);
   cJSON_Delete(args);
   const cJSON *text = cJSON_GetObjectItemCaseSensitive(reply, "context");
   const cJSON *tokens = cJSON_GetObjectItemCaseSensitive(reply, "tokens");
   const cJSON *kept = cJSON_GetObjectItemCaseSensitive(reply, "kept");
   if (rc <= 0 || strcmp(jo_cstr(reply, "status"), "ok") || !cJSON_IsString(text) ||
       strlen(text->valuestring) >= context_len || !cJSON_IsNumber(tokens) ||
       !isfinite(tokens->valuedouble) || tokens->valuedouble < 0 || tokens->valuedouble > 131072 ||
       floor(tokens->valuedouble) != tokens->valuedouble ||
       (token_budget > 0 && tokens->valuedouble > token_budget) || !cJSON_IsNumber(kept) ||
       !isfinite(kept->valuedouble) || kept->valuedouble < 0 || kept->valuedouble > 32 ||
       floor(kept->valuedouble) != kept->valuedouble || (top_k > 0 && kept->valuedouble > top_k) ||
       ((kept->valuedouble == 0) != (text->valuestring[0] == '\0')) ||
       ((kept->valuedouble == 0) != (tokens->valuedouble == 0)))
   {
      cJSON_Delete(reply);
      return -1;
   }
   memcpy(context_out, text->valuestring, strlen(text->valuestring) + 1);
   if (retrieved_tokens_out)
      *retrieved_tokens_out = (int)tokens->valuedouble;
   int count = (int)kept->valuedouble;
   cJSON_Delete(reply);
   return count;
}
