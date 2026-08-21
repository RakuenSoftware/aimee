/* kb_curator_llm.c: curator stage -> LLM dispatch. See header. */

#include "kb_curator_llm.h"

#include "cJSON.h"
#include "kb_curator_sidecar.h"
#include "provider_client.h"

#include <stdio.h>
#include <stdlib.h>

#define KB_CURATOR_PROVIDER_TIMEOUT_MS 300000

/* Build [{role:system, content:system_prompt}?, {role:user, content:request_json}]. */
static cJSON *build_messages(const char *system_prompt, const char *request_json)
{
   cJSON *msgs = cJSON_CreateArray();
   if (!msgs)
      return NULL;
   if (system_prompt && system_prompt[0])
   {
      cJSON *sys = cJSON_CreateObject();
      if (!sys)
      {
         cJSON_Delete(msgs);
         return NULL;
      }
      cJSON_AddStringToObject(sys, "role", "system");
      cJSON_AddStringToObject(sys, "content", system_prompt);
      cJSON_AddItemToArray(msgs, sys);
   }
   cJSON *user = cJSON_CreateObject();
   if (!user)
   {
      cJSON_Delete(msgs);
      return NULL;
   }
   cJSON_AddStringToObject(user, "role", "user");
   cJSON_AddStringToObject(user, "content", request_json ? request_json : "");
   cJSON_AddItemToArray(msgs, user);
   return msgs;
}

char *kb_curator_llm_run(const config_t *cfg, kb_curator_stage_t stage, const char *system_prompt,
                         const char *request_json, cJSON *json_schema, const char *fallback_command,
                         int out_cap, char *errbuf, size_t errlen)
{
   (void)cfg; /* legacy resolver reads the process configuration atomically */
   if (errbuf && errlen)
      errbuf[0] = '\0';

   /* 1. Configured provider for this stage's tier (§2a). */
   provider_def_owned_t prov;
   if (kb_curator_provider_for_stage(stage, &prov))
   {
      provider_def_t *def = &prov.def;
      /* The durable curator contract already carries one operator-controlled
       * output budget (kb.curator.extract_max_tokens), and the legacy sidecar
       * receives it in each job payload.  Apply the same budget to the direct
       * provider path.  Without this, the managed gateway sends an unbounded
       * generation to its single CPU synth slot: the curator times out after
       * five minutes while llama-server keeps the slot occupied, starving
       * subsequent curator and interactive requests. */
      if (def->max_tokens <= 0 && config_kb_curator_extract_max_tokens() > 0)
         def->max_tokens = config_kb_curator_extract_max_tokens();
      /* Curator work is already retried by its durable job queue. Keep one
       * provider attempt bounded by the same five-minute ceiling as the legacy
       * sidecar instead of nesting the provider client's three retries. The old
       * 120s default was shorter than a normal constrained extraction on the
       * bundled CPU model and created overlapping abandoned generations. */
      if (def->timeout_ms <= 0)
         def->timeout_ms = KB_CURATOR_PROVIDER_TIMEOUT_MS;
      if (def->max_attempts <= 0)
         def->max_attempts = 1;
      cJSON *msgs = build_messages(system_prompt, request_json);
      if (!msgs)
      {
         if (errbuf && errlen)
            snprintf(errbuf, errlen, "out of memory building request");
         return NULL;
      }
      /* provider_client_complete zeroes out on entry, but zero-init here too so
       * provider_completion_free is unconditionally safe regardless of the callee. */
      provider_completion_t out = {0};
      int rc = provider_client_complete(def, msgs, json_schema, &out, errbuf, errlen);
      cJSON_Delete(msgs);
      if (rc != 0)
      {
         provider_completion_free(&out);
         if (kb_curator_error_is_provider_unavailable(errbuf))
            kb_curator_provider_backoff_note();
         else
            kb_curator_provider_backoff_recovered();
         return NULL; /* errbuf set by provider_client_complete */
      }
      char *content = out.content; /* transfer ownership to the caller */
      out.content = NULL;
      provider_completion_free(&out);
      if (!content && errbuf && errlen)
         snprintf(errbuf, errlen, "provider returned empty content");
      if (content)
         kb_curator_provider_backoff_recovered();
      return content;
   }

   /* 2. Legacy sidecar command. */
   if (fallback_command && fallback_command[0])
   {
      char *content =
          kb_curator_sidecar_run(fallback_command, request_json, out_cap, errbuf, errlen);
      if (content)
         kb_curator_provider_backoff_recovered();
      else if (kb_curator_error_is_provider_unavailable(errbuf))
         kb_curator_provider_backoff_note();
      return content;
   }

   /* 3. Neither configured — the stage stays idle. */
   if (errbuf && errlen)
      snprintf(errbuf, errlen, "no curator provider or command configured");
   return NULL;
}
