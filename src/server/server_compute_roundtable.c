/* server_compute_roundtable.c: split from server_compute.c into a real translation unit
 * (was server_compute_roundtable.inc, textually included only to stay under the
 * line-check ceiling). Cross-TU declarations live in the module header. */
#ifndef _GNU_SOURCE /* strcasestr/memmem are GNU extensions (container gcc) */
#define _GNU_SOURCE
#endif
#include "server_compute_internal.h"
#include "aimee.h"
#include "json_fluent.h" /* jo_ok */
#include "db1.h"
#include "server_delegate_monitor.h" /* delegate heartbeat begin/end (keep slow delegates alive) */
#include "server_compute_impl.h"
#include "agent_config.h"
#include "gateway_policy.h"
#include "presence.h"
#include "compute_pool.h"
#include "agent.h"
#include "agent_coord.h"
#include "cmd_agent_delegate_impl.h"
#include "config.h"
#include "token_tracker.h"
#include "delegate_credential_retry.h"
#include "delegate_launch.h"
#include "delegate_source_authority.h"
#include "agent_source_authority.h" /* TLS source-authority context (race-free in-process) */
#include "server_coord_dispatcher.h"
#include "delegate_credentials.h"
#include "vault_service.h" /* WP-C.1 vault-first credential resolution */
#include <openssl/crypto.h>
#include "delegate_economics.h"
#include "delegate_run_phases.h"
#include "db1/delegate_learning.h"
#include "kb_client.h"
#include "kb_bandit.h"
#include "db1/interaction_events.h"
#include "delegate_role.h"
#include "delegate_ensemble.h"
#include "evidence_replay.h"
#include "guardrails.h"
#include "liveness.h"
#include "log.h"
#include "model_registry.h"
#include "openai_runs_store.h"
#include "platform_process.h"
#include "prompts.h"
#include "persona.h"
#include "server_http.h"
#include "provider_catalog.h"
#include "role_templates.h"
#include "workspace.h"
#include "workspace_provider.h"
#include "workspace_turn.h"
#include "cJSON.h"
#include <ctype.h>
#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static int brief_append(char *buf, size_t cap, size_t *pos, const char *fmt, ...)
{
   if (!buf || !pos || *pos >= cap)
      return -1;
   va_list ap;
   va_start(ap, fmt);
   int n = vsnprintf(buf + *pos, cap - *pos, fmt, ap);
   va_end(ap);
   if (n < 0)
      return -1;
   if ((size_t)n >= cap - *pos)
   {
      *pos = cap - 1;
      buf[*pos] = '\0';
      return 1;
   }
   *pos += (size_t)n;
   return 0;
}

static int brief_array_append(const char *label, cJSON *arr, char *buf, size_t cap, size_t *pos,
                              normalized_roundtable_brief_t *out, char *err, size_t err_n)
{
   if (!arr)
      return 0;
   if (!cJSON_IsArray(arr))
   {
      snprintf(err, err_n, "brief.%s must be an array of strings", label);
      return -1;
   }
   if (cJSON_GetArraySize(arr) <= 0)
      return 0;
   int rc = brief_append(buf, cap, pos, "%s:\n", label);
   cJSON *it;
   cJSON_ArrayForEach(it, arr)
   {
      if (!cJSON_IsString(it))
      {
         snprintf(err, err_n, "brief.%s must be an array of strings", label);
         return -1;
      }
      if (strcmp(label, "questions") == 0)
      {
         if (out->question_count < ROUNDTABLE_MAX_QUESTIONS)
         {
            snprintf(out->questions[out->question_count], sizeof(out->questions[0]), "%s",
                     it->valuestring ? it->valuestring : "");
            out->question_ptrs[out->question_count] = out->questions[out->question_count];
            out->question_count++;
         }
         else
            out->truncated = 1; /* more questions asked than the engine can track */
      }
      if (rc == 0)
         rc = brief_append(buf, cap, pos, "- %s\n", it->valuestring ? it->valuestring : "");
   }
   if (rc == 0)
      rc = brief_append(buf, cap, pos, "\n");
   if (rc > 0)
      out->truncated = 1;
   return 0;
}

int normalize_roundtable_brief(cJSON *req, normalized_roundtable_brief_t *out, char *err,
                               size_t err_n)
{
   memset(out, 0, sizeof(*out));
   cJSON *brief = cJSON_GetObjectItemCaseSensitive(req, "brief");
   if (!brief)
      return 0;
   /* Heap, not stack: a 256 KB buffer must never sit on a thread stack. */
   const size_t cap = ROUNDTABLE_BRIEF_MAX_BYTES + 1;
   char *tmp = calloc(cap, 1);
   if (!tmp)
   {
      snprintf(err, err_n, "out of memory");
      return -1;
   }
   size_t pos = 0;
   if (cJSON_IsString(brief))
   {
      const char *s = brief->valuestring ? brief->valuestring : "";
      while (*s && isspace((unsigned char)*s))
         s++;
      if (!*s)
      {
         free(tmp);
         return 0;
      }
      int rc = brief_append(tmp, cap, &pos, "focus:\n- %s\n", s);
      if (rc > 0)
         out->truncated = 1;
   }
   else if (cJSON_IsObject(brief))
   {
      /* brief_array_append returns 0 on success OR truncation (it sets
       * out->truncated itself) and -1 only on a malformed (non-array/non-string)
       * field, so this `!= 0` short-circuit frees+fails only on real errors —
       * truncation keeps the partial render, matching the string branch above. */
      if (brief_array_append("focus", cJSON_GetObjectItemCaseSensitive(brief, "focus"), tmp, cap,
                             &pos, out, err, err_n) != 0 ||
          brief_array_append("fixes", cJSON_GetObjectItemCaseSensitive(brief, "fixes"), tmp, cap,
                             &pos, out, err, err_n) != 0 ||
          brief_array_append("invariants", cJSON_GetObjectItemCaseSensitive(brief, "invariants"),
                             tmp, cap, &pos, out, err, err_n) != 0 ||
          brief_array_append("questions", cJSON_GetObjectItemCaseSensitive(brief, "questions"), tmp,
                             cap, &pos, out, err, err_n) != 0)
      {
         free(tmp);
         return -1;
      }
      if (pos == 0)
      {
         free(tmp);
         return 0;
      }
   }
   else
   {
      free(tmp);
      snprintf(err, err_n, "brief must be a string or object");
      return -1;
   }
   out->rendered = tmp; /* transfer ownership; freed by the caller */
   return 0;
}

/* Cap the serialized items so the whole result stays under SHTTP_RESP_MAX
 * (256 KB). The fixed item array alone can serialize to ~190 KB; left unbounded
 * it would push a large consolidated `artifact` over the async op-run capture
 * buffer, which loopback_rpc rejects as "response too large" (a failed run with
 * an opaque error) instead of returning the findings. Bounding items to ~96 KB
 * leaves ample headroom for the artifact + answered_questions. */
#define ROUNDTABLE_ITEMS_JSON_BUDGET (96 * 1024)

void add_roundtable_arrays(cJSON *resp, const roundtable_result_t *result)
{
   cJSON *items = cJSON_CreateArray();
   size_t used = 0;
   int items_truncated = 0;
   for (int i = 0; i < result->item_count; i++)
   {
      const roundtable_review_item_t *it = &result->items[i];
      size_t est = strlen(it->severity) + strlen(it->category) + strlen(it->location) +
                   strlen(it->summary) + strlen(it->recommendation) + strlen(it->identity_key) +
                   strlen(it->sources) + 160; /* JSON keys, quotes, count */
      if (i > 0 && used + est > ROUNDTABLE_ITEMS_JSON_BUDGET)
      {
         items_truncated = 1;
         break;
      }
      used += est;
      cJSON *o = cJSON_CreateObject();
      cJSON_AddStringToObject(o, "severity", it->severity);
      cJSON_AddStringToObject(o, "category", it->category);
      cJSON_AddStringToObject(o, "location", it->location);
      cJSON_AddStringToObject(o, "summary", it->summary);
      cJSON_AddStringToObject(o, "recommendation", it->recommendation);
      cJSON_AddStringToObject(o, "identity_key", it->identity_key);
      cJSON_AddStringToObject(o, "sources", it->sources);
      cJSON_AddNumberToObject(o, "count", it->count);
      cJSON_AddItemToArray(items, o);
   }
   cJSON_AddItemToObject(resp, "items", items);
   if (items_truncated)
      cJSON_AddBoolToObject(resp, "items_truncated", 1);

   cJSON *answers = cJSON_CreateArray();
   for (int i = 0; i < result->answered_question_count; i++)
   {
      const roundtable_answered_question_t *a = &result->answered_questions[i];
      cJSON *o = cJSON_CreateObject();
      cJSON_AddStringToObject(o, "question", a->question);
      cJSON_AddStringToObject(o, "answer", a->answer);
      cJSON_AddStringToObject(o, "evidence", a->evidence);
      cJSON_AddBoolToObject(o, "answered", a->answered ? 1 : 0);
      cJSON_AddItemToArray(answers, o);
   }
   cJSON_AddItemToObject(resp, "answered_questions", answers);

   cJSON *gaps = cJSON_CreateArray();
   for (int i = 0; i < result->coverage_gap_count; i++)
      cJSON_AddItemToArray(gaps, cJSON_CreateString(result->coverage_gaps[i]));
   cJSON_AddItemToObject(resp, "coverage_gaps", gaps);
}
