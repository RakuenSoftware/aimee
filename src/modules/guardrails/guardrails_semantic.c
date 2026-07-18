/* guardrails_semantic.c: semantic risk scoring for guardrails (Phase 0/1).
 *
 * Implements the sidecar invocation, response parsing, score-band mapping,
 * and DB1 event recording for the neural-assisted guardrails advisory layer.
 *
 * Feature gate: guardrails.semantic.enabled (default false).
 * Shadow mode:  guardrails.semantic.dry_run  (default true).
 *
 * Failure contract: any sidecar failure (spawn error, timeout, malformed
 * JSON) degrades to parse_ok=0 and recommendation="allow". The deterministic
 * guardrail result is unaffected. One warning is logged on failure. */
#include "guardrails_semantic.h"
#include "db1/guardrail_events.h"
#if !defined(AIMEE_DB2_DISABLED)
#include "db2/bandit.h"
#endif
#include "headers/log.h"
#include "platform_process.h"
#include <cJSON.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ── sidecar failure backoff ──────────────────────────────────────────────── */

static long long gsem_backoff_until_ms = 0; /* monotonic deadline; 0 = not in backoff */

static long long gsem_mono_ms(void)
{
   struct timespec ts;
   clock_gettime(CLOCK_MONOTONIC, &ts);
   return (long long)ts.tv_sec * 1000LL + (long long)ts.tv_nsec / 1000000LL;
}

int gsem_sidecar_in_backoff(void)
{
   return gsem_mono_ms() < gsem_backoff_until_ms;
}

void gsem_sidecar_reset_backoff(void)
{
   gsem_backoff_until_ms = 0;
}

/* Extract up to cap-1 bytes from |src| into |dst|, NUL-terminated. */
static void bounded_copy(char *dst, size_t cap, const char *src)
{
   if (!src || !dst || cap == 0)
      return;
   size_t n = strlen(src);
   if (n >= cap)
      n = cap - 1;
   memcpy(dst, src, n);
   dst[n] = '\0';
}

void gsem_build_input(const char *tool_name, cJSON *input_json, const char *cwd,
                      const char *session_mode, gsem_input_t *out)
{
   memset(out, 0, sizeof(*out));
   if (tool_name)
      bounded_copy(out->tool_name, sizeof(out->tool_name), tool_name);
   if (cwd)
      bounded_copy(out->cwd, sizeof(out->cwd), cwd);
   if (session_mode)
      bounded_copy(out->session_mode, sizeof(out->session_mode), session_mode);

   if (!input_json)
      return;

   /* Extract file_path(s) for edit tools */
   cJSON *fp = cJSON_GetObjectItemCaseSensitive(input_json, "file_path");
   if (fp && cJSON_IsString(fp) && fp->valuestring)
      bounded_copy(out->paths, sizeof(out->paths), fp->valuestring);

   /* Extract bounded shell command. execute_script uses "body"; keep
    * "command" first so existing shell-tool payloads remain unchanged. */
   cJSON *cmd = cJSON_GetObjectItemCaseSensitive(input_json, "command");
   if (cmd && cJSON_IsString(cmd) && cmd->valuestring)
      bounded_copy(out->shell_cmd, sizeof(out->shell_cmd), cmd->valuestring);
   else
   {
      cJSON *body = cJSON_GetObjectItemCaseSensitive(input_json, "body");
      if (body && cJSON_IsString(body) && body->valuestring)
         bounded_copy(out->shell_cmd, sizeof(out->shell_cmd), body->valuestring);
   }

   /* Extract diff excerpts: old_string / new_string */
   cJSON *old_s = cJSON_GetObjectItemCaseSensitive(input_json, "old_string");
   if (old_s && cJSON_IsString(old_s) && old_s->valuestring)
      bounded_copy(out->old_excerpt, sizeof(out->old_excerpt), old_s->valuestring);

   cJSON *new_s = cJSON_GetObjectItemCaseSensitive(input_json, "new_string");
   if (new_s && cJSON_IsString(new_s) && new_s->valuestring)
      bounded_copy(out->new_excerpt, sizeof(out->new_excerpt), new_s->valuestring);
}

int gsem_assess(const gsem_input_t *in, const char *command, gsem_output_t *out)
{
   memset(out, 0, sizeof(*out));
   bounded_copy(out->recommendation, sizeof(out->recommendation), "allow");

   if (!in || !command || !command[0])
      return -1;

   /* Honour failure backoff: if the sidecar failed recently, skip invocation
    * silently and degrade to deterministic-only until the cooldown expires. */
   if (gsem_sidecar_in_backoff())
      return -1;

   /* Build sidecar request JSON */
   cJSON *req = cJSON_CreateObject();
   cJSON_AddNumberToObject(req, "version", 1);
   cJSON_AddStringToObject(req, "role", "score");
   cJSON *inputs = cJSON_CreateObject();
   cJSON_AddStringToObject(inputs, "tool", in->tool_name);
   cJSON_AddStringToObject(inputs, "cwd", in->cwd);
   cJSON_AddStringToObject(inputs, "session_mode", in->session_mode);
   cJSON_AddStringToObject(inputs, "paths", in->paths);
   cJSON_AddStringToObject(inputs, "shell_cmd", in->shell_cmd);
   cJSON_AddStringToObject(inputs, "old_excerpt", in->old_excerpt);
   cJSON_AddStringToObject(inputs, "new_excerpt", in->new_excerpt);
   cJSON_AddStringToObject(inputs, "active_task", in->active_task);
   cJSON_AddItemToObject(req, "inputs", inputs);

   char *req_str = cJSON_PrintUnformatted(req);
   cJSON_Delete(req);
   if (!req_str)
      return -1;

   char *resp = NULL;
   size_t resp_len = 0;
   int ec = platform_exec_pipe(command, req_str, strlen(req_str), &resp, &resp_len);
   free(req_str);

   if (ec != 0 || !resp || resp_len == 0)
   {
      free(resp);
      /* Arm backoff so we don't spam on every subsequent call. */
      gsem_backoff_until_ms = gsem_mono_ms() + GSEM_BACKOFF_MS;
      LOG_WARN("guardrails.semantic",
               "sidecar failed (exit %d); disabling for %ds, degrading to deterministic", ec,
               GSEM_BACKOFF_MS / 1000);
      return -1;
   }

   cJSON *root = cJSON_Parse(resp);
   free(resp);
   if (!root)
   {
      gsem_backoff_until_ms = gsem_mono_ms() + GSEM_BACKOFF_MS;
      LOG_WARN("guardrails.semantic",
               "sidecar returned invalid JSON; disabling for %ds, degrading to deterministic",
               GSEM_BACKOFF_MS / 1000);
      return -1;
   }

   cJSON *outputs = cJSON_GetObjectItemCaseSensitive(root, "outputs");
   if (!outputs)
   {
      cJSON_Delete(root);
      return -1;
   }

   /* Parse risk sub-object */
   cJSON *risk = cJSON_GetObjectItemCaseSensitive(outputs, "risk");
   if (cJSON_IsObject(risk))
   {
      cJSON *v;
      v = cJSON_GetObjectItemCaseSensitive(risk, "overall");
      if (cJSON_IsNumber(v))
         out->overall = v->valuedouble;
      v = cJSON_GetObjectItemCaseSensitive(risk, "action_risk");
      if (cJSON_IsNumber(v))
         out->action_risk = v->valuedouble;
      v = cJSON_GetObjectItemCaseSensitive(risk, "diff_risk");
      if (cJSON_IsNumber(v))
         out->diff_risk = v->valuedouble;
      v = cJSON_GetObjectItemCaseSensitive(risk, "drift_risk");
      if (cJSON_IsNumber(v))
         out->drift_risk = v->valuedouble;
      v = cJSON_GetObjectItemCaseSensitive(risk, "antipattern_similarity");
      if (cJSON_IsNumber(v))
         out->antipattern_similarity = v->valuedouble;
   }

   /* Parse recommendation and labels */
   cJSON *rec = cJSON_GetObjectItemCaseSensitive(outputs, "recommendation");
   if (cJSON_IsString(rec) && rec->valuestring)
      bounded_copy(out->recommendation, sizeof(out->recommendation), rec->valuestring);

   cJSON *labels = cJSON_GetObjectItemCaseSensitive(outputs, "labels");
   if (cJSON_IsArray(labels))
   {
      int first = 1;
      cJSON *lbl;
      cJSON_ArrayForEach(lbl, labels)
      {
         if (!cJSON_IsString(lbl))
            continue;
         if (!first)
         {
            size_t cur = strlen(out->labels);
            if (cur + 2 < sizeof(out->labels))
            {
               out->labels[cur] = ',';
               out->labels[cur + 1] = '\0';
            }
         }
         size_t cur = strlen(out->labels);
         size_t rem = sizeof(out->labels) - cur - 1;
         if (rem > 0)
            strncat(out->labels, lbl->valuestring, rem);
         first = 0;
      }
   }

   /* Parse explanation from evidence sub-object */
   cJSON *evidence = cJSON_GetObjectItemCaseSensitive(root, "evidence");
   if (cJSON_IsObject(evidence))
   {
      cJSON *expl = cJSON_GetObjectItemCaseSensitive(evidence, "explanation");
      if (cJSON_IsString(expl) && expl->valuestring)
         bounded_copy(out->explanation, sizeof(out->explanation), expl->valuestring);
   }

   cJSON_Delete(root);
   out->parse_ok = 1;
   gsem_sidecar_reset_backoff(); /* successful call clears any prior backoff */
   return 0;
}

const char *gsem_policy(const gsem_output_t *out, double warn_t, double prompt_t, double block_t)
{
   if (!out || !out->parse_ok)
      return "allow";
   if (out->overall >= block_t)
      return "block";
   if (out->overall >= prompt_t)
      return "prompt";
   if (out->overall >= warn_t)
      return "warn";
   return "allow";
}

void gsem_apply_strictness_arm(config_t *cfg)
{
#if defined(AIMEE_DB2_DISABLED)
   (void)cfg;
#else
   char arm[64] = "";
   if (!cfg || db2_bandit_promotion_get("guardrail_strictness", arm, sizeof(arm)) != 0 ||
       strcmp(arm, "strict") != 0)
      return;
   if (cfg->guardrails_semantic_warn_threshold > 0.30)
      cfg->guardrails_semantic_warn_threshold = 0.30;
   if (cfg->guardrails_semantic_prompt_threshold > 0.55)
      cfg->guardrails_semantic_prompt_threshold = 0.55;
   if (cfg->guardrails_semantic_block_threshold > 0.80)
      cfg->guardrails_semantic_block_threshold = 0.80;
#endif
}

int gsem_format_advisory_message(char *buf, size_t buf_len, const char *action,
                                 const gsem_output_t *out)
{
   if (!buf || buf_len == 0 || !action || !out)
      return 0;

   if (strcmp(action, "warn") == 0)
   {
      snprintf(buf, buf_len,
               "semantic guardrail: elevated risk (score=%.2f, labels=[%s]) - proceeding",
               out->overall, out->labels);
      return 1;
   }
   if (strcmp(action, "prompt") == 0)
   {
      snprintf(buf, buf_len,
               "semantic guardrail: high risk (score=%.2f, labels=[%s]) - proceeding "
               "(advisory mode)",
               out->overall, out->labels);
      return 1;
   }
   return 0;
}

void gsem_record(const char *session_id, const gsem_input_t *in, const gsem_output_t *out,
                 const char *final_action, int dry_run)
{
   if (!session_id || !session_id[0] || !in || !out)
      return;

   guardrail_event_t e;
   memset(&e, 0, sizeof(e));
   bounded_copy(e.session_id, sizeof(e.session_id), session_id);
   bounded_copy(e.tool_name, sizeof(e.tool_name), in->tool_name);
   e.overall_risk = out->overall;
   e.action_risk = out->action_risk;
   e.diff_risk = out->diff_risk;
   e.drift_risk = out->drift_risk;
   e.antipattern_similarity = out->antipattern_similarity;
   bounded_copy(e.recommendation, sizeof(e.recommendation), out->recommendation);
   bounded_copy(e.labels, sizeof(e.labels), out->labels);
   bounded_copy(e.final_action, sizeof(e.final_action), final_action ? final_action : "allow");
   bounded_copy(e.explanation, sizeof(e.explanation), out->explanation);
   e.dry_run = dry_run;

   db1_guardrail_event_insert(&e);
}
