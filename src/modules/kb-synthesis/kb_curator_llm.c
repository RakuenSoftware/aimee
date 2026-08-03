/* kb_curator_llm.c: curator stage -> LLM dispatch. See header. */

#include "kb_curator_llm.h"

#include "cJSON.h"
#include "kb_curator_sidecar.h"
#include "provider_client.h"

#include <stdio.h>
#include <stdlib.h>

/* THE CEILING HAS TO CLEAR THE SLOWEST SIDECAR WE SHIP, and at 300000 it did not.
 *
 * The history below already records 120s being shorter than a normal extraction on
 * the bundled CPU model. 300s is the same mistake one size up, and E4B is where it
 * shows: measured on the deployed aimee-llm-e4b sidecar, one realistic document
 * paragraph costs 1768 completion tokens at 4.98 tokens/s -- 375 seconds, with
 * multi-token prediction already accepting 78% of drafts. Every non-trivial
 * extraction on E4B was cut off by this deadline, retried, and failed for good,
 * while E2B (1087 tokens at ~10 tokens/s, 118s) fit and looked fine.
 *
 * 600s, and the ceiling is set by the STALE-LEASE REAPER, not by the token budget.
 * CE_STALE_LEASE / CCU_STALE_LEASE reclaim a job whose lease is older than 15
 * minutes, so a deadline at or near 900s would let the reaper hand a still-running
 * document to a second worker and pay for the same extraction twice. 600s sits
 * clearly inside that window and clears the measured 375s by 60%.
 *
 * That leaves a gap worth naming: 4096 tokens at E4B's 4.98 tokens/s is ~825s, so a
 * document that reasons all the way to the cap still exceeds this. That direction is
 * survivable and the other is not -- a deadline overrun is a transport error, which
 * kb_curator_error_is_provider_unavailable retries with backoff, while truncating at
 * the token cap yields invalid JSON and fails permanently. Raising both the deadline
 * and the lease windows together is the way to close it, and it needs the reaper
 * thought about rather than a constant nudged.
 *
 * None of this lets a dead peer hold a worker for ten minutes. agent_http keeps
 * SO_RCVTIMEO at min(timeout, 30s) per recv, so a peer that stops answering fails in
 * 30s; this wall-clock ceiling only extends while tokens are still arriving -- that
 * is, while the model is genuinely working. */
#define KB_CURATOR_PROVIDER_TIMEOUT_MS 600000

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

char *kb_curator_llm_run(kb_curator_stage_t stage, const char *system_prompt,
                         const char *request_json, cJSON *json_schema, const char *fallback_command,
                         int out_cap, char *errbuf, size_t errlen)
{
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
       * generation to its single CPU synth slot: the curator times out at
       * KB_CURATOR_PROVIDER_TIMEOUT_MS while llama-server keeps the slot occupied, starving
       * subsequent curator and interactive requests. */
      if (def->max_tokens <= 0 && config_kb_curator_extract_max_tokens() > 0)
         def->max_tokens = config_kb_curator_extract_max_tokens();
      /* Curator work is already retried by its durable job queue. Keep one
       * provider attempt bounded by KB_CURATOR_PROVIDER_TIMEOUT_MS instead of
       * nesting the provider client's three retries. The old 120s default was
       * shorter than a normal constrained extraction on the bundled CPU model and
       * created overlapping abandoned generations; see that constant for why 300s
       * turned out to be the same mistake on E4B. */
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
