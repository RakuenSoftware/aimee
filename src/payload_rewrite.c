/* payload_rewrite.c: prompt-cache-aware deferred payload rewrite.
 *
 * Feature gate: transport.cache_aware_rewrite.enabled (default false).
 */
#include "headers/payload_rewrite.h"
#include "config.h"
#include "headers/log.h"
#include <cJSON.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define PAYLOAD_REWRITE_FNV_OFFSET              14695981039346656037ULL
#define PAYLOAD_REWRITE_FNV_PRIME               1099511628211ULL
#define PAYLOAD_REWRITE_BYTES_PER_TOKEN         4
#define PAYLOAD_REWRITE_DEFAULT_CONTEXT         200000
#define PAYLOAD_REWRITE_DEFAULT_MIN_SAVINGS     500
#define PAYLOAD_REWRITE_DEFAULT_HARD_FRAC       0.85
#define PAYLOAD_REWRITE_DEFAULT_MAX_DEFER_TURNS 20
#define PAYLOAD_REWRITE_DEFAULT_SEGMENT_CHECK   5

static int cache_aware_rewrite_on(void)
{
   config_t cfg;
   if (config_load(&cfg) != 0)
      return 0;
   return cfg.cache_aware_rewrite_enabled;
}

static int payload_rewrite_pending_bytes_for_tokens(int tokens)
{
   if (tokens <= 0)
      return 0;
   if (tokens > 0x1fffffff)
      return 0x7fffffff;
   return tokens * PAYLOAD_REWRITE_BYTES_PER_TOKEN;
}

static void fnv1a_update(uint64_t *h, const void *data, size_t len)
{
   const unsigned char *p = (const unsigned char *)data;
   for (size_t i = 0; i < len; i++)
   {
      *h ^= (uint64_t)p[i];
      *h *= PAYLOAD_REWRITE_FNV_PRIME;
   }
}

static void decision_set(payload_rewrite_decision_t *out, int defer, const char *reason)
{
   if (!out)
      return;
   out->defer = defer;
   snprintf(out->reason, sizeof(out->reason), "%s", reason ? reason : "");
}

void payload_rewrite_prefix_hash(const char *system_prompt, const char *static_context, char *out,
                                 size_t out_len)
{
   if (!out || out_len == 0)
      return;

   uint64_t h = PAYLOAD_REWRITE_FNV_OFFSET;
   static const unsigned char sep1[] = {0xff, 0x00};
   static const unsigned char sep2[] = {0xfe, 0x00};

   if (system_prompt)
      fnv1a_update(&h, system_prompt, strlen(system_prompt));
   fnv1a_update(&h, sep1, sizeof(sep1));
   if (static_context)
      fnv1a_update(&h, static_context, strlen(static_context));
   fnv1a_update(&h, sep2, sizeof(sep2));

   snprintf(out, out_len, "%016llx", (unsigned long long)h);
}

int payload_rewrite_should_defer(const char *sid, const char *new_prefix_hash, int current_tokens,
                                 int model_context_limit, payload_rewrite_decision_t *out)
{
   if (!out)
      return -1;
   decision_set(out, 0, "invalid");

   config_t cfg;
   if (config_load(&cfg) != 0 || !cfg.cache_aware_rewrite_enabled)
   {
      decision_set(out, 0, "disabled");
      return 0;
   }

   if (!sid || !sid[0] || !new_prefix_hash || !new_prefix_hash[0])
   {
      decision_set(out, 0, "invalid_input");
      return -1;
   }

   payload_rewrite_state_t state;
   if (db1_payload_rewrite_state_get(sid, &state) != 0 || !state.last_prefix_hash[0])
   {
      decision_set(out, 0, "no_prior_state");
      return 0;
   }

   if (strcmp(state.last_prefix_hash, new_prefix_hash) != 0)
   {
      decision_set(out, 0, "prefix_changed");
      return 0;
   }

   int limit = model_context_limit > 0 ? model_context_limit : PAYLOAD_REWRITE_DEFAULT_CONTEXT;
   double hard_frac = cfg.cache_aware_rewrite_hard_context_threshold;
   if (hard_frac <= 0.0 || hard_frac > 1.0)
      hard_frac = PAYLOAD_REWRITE_DEFAULT_HARD_FRAC;
   if (current_tokens > 0 && limit > 0 && (double)current_tokens > (double)limit * hard_frac)
   {
      decision_set(out, 0, "context_pressure");
      return 0;
   }

   int max_defer = cfg.cache_aware_rewrite_max_defer_turns;
   if (max_defer < 0)
      max_defer = PAYLOAD_REWRITE_DEFAULT_MAX_DEFER_TURNS;
   if (max_defer > 0 && state.consecutive_deferred_count >= max_defer)
   {
      decision_set(out, 0, "cache_horizon");
      return 0;
   }

   int seg_check = cfg.cache_aware_rewrite_segment_check_turns;
   if (seg_check < 0)
      seg_check = PAYLOAD_REWRITE_DEFAULT_SEGMENT_CHECK;
   if (seg_check > 0 && state.consecutive_deferred_count > 0 &&
       (state.consecutive_deferred_count % seg_check) == 0)
   {
      decision_set(out, 0, "required_segment_miss");
      return 0;
   }

   int min_tokens = cfg.cache_aware_rewrite_min_savings_tokens;
   if (min_tokens < 0)
      min_tokens = PAYLOAD_REWRITE_DEFAULT_MIN_SAVINGS;
   int min_bytes = payload_rewrite_pending_bytes_for_tokens(min_tokens);
   if (min_bytes > 0 && state.bytes_saved_pending >= min_bytes)
   {
      decision_set(out, 0, "savings_threshold");
      return 0;
   }

   decision_set(out, 1, "cache_warm");
   return 0;
}

int payload_rewrite_track_request(const char *system_prompt, const char *static_context,
                                  int current_tokens, int model_context_limit)
{
   if (!cache_aware_rewrite_on())
      return 0;

   const char *sid = session_id();
   if (!sid || !sid[0])
      return -1;

   char prefix_hash[17];
   payload_rewrite_prefix_hash(system_prompt, static_context, prefix_hash, sizeof(prefix_hash));

   payload_rewrite_decision_t decision;
   int rc = payload_rewrite_should_defer(sid, prefix_hash, current_tokens, model_context_limit,
                                         &decision);
   if (rc != 0)
      return rc;
   if (strcmp(decision.reason, "disabled") == 0)
      return 0;

   if (decision.defer)
   {
      return payload_rewrite_record_deferred(
          payload_rewrite_pending_bytes_for_tokens(current_tokens), current_tokens, decision.reason,
          prefix_hash);
   }

   return payload_rewrite_record_forced(current_tokens, decision.reason, prefix_hash);
}

int payload_rewrite_record_deferred(int bytes_saved, int payload_tokens, const char *reason,
                                    const char *prefix_hash)
{
   if (!cache_aware_rewrite_on())
      return 0;
   const char *sid = session_id();
   if (!sid)
      return -1;
   LOG_DEBUG("payload_rewrite", "deferred: bytes_saved=%d tokens=%d reason=%s", bytes_saved,
             payload_tokens, reason ? reason : "");
   return db1_payload_rewrite_record(sid, 1, bytes_saved, payload_tokens, reason, prefix_hash);
}

int payload_rewrite_record_forced(int payload_tokens, const char *reason, const char *prefix_hash)
{
   if (!cache_aware_rewrite_on())
      return 0;
   const char *sid = session_id();
   if (!sid)
      return -1;
   LOG_DEBUG("payload_rewrite", "forced rewrite: tokens=%d reason=%s", payload_tokens,
             reason ? reason : "");
   return db1_payload_rewrite_record(sid, 0, 0, payload_tokens, reason, prefix_hash);
}

cJSON *tool_payload_rewrite_status(cJSON *args)
{
   (void)args;

   config_t cfg;
   config_load(&cfg);

   cJSON *r = cJSON_CreateObject();
   cJSON_AddBoolToObject(r, "enabled", cfg.cache_aware_rewrite_enabled ? 1 : 0);

   if (!cfg.cache_aware_rewrite_enabled)
   {
      cJSON_AddStringToObject(r, "note",
                              "Set transport.cache_aware_rewrite.enabled=true to activate");
      return r;
   }

   const char *sid = session_id();
   cJSON_AddStringToObject(r, "session_id", sid ? sid : "");

   payload_rewrite_state_t state;
   if (db1_payload_rewrite_state_get(sid ? sid : "", &state) == 0)
   {
      cJSON_AddNumberToObject(r, "payload_epoch", (double)state.payload_epoch);
      cJSON_AddNumberToObject(r, "compaction_epoch", (double)state.compaction_epoch);
      cJSON_AddStringToObject(r, "last_prefix_hash", state.last_prefix_hash);
      cJSON_AddNumberToObject(r, "last_payload_tokens", state.last_payload_tokens);
      cJSON_AddStringToObject(r, "last_rewrite_at", state.last_rewrite_at);
      cJSON_AddNumberToObject(r, "deferred_rewrite_count", state.deferred_rewrite_count);
      cJSON_AddNumberToObject(r, "consecutive_deferred_count", state.consecutive_deferred_count);
      cJSON_AddNumberToObject(r, "forced_rewrite_count", (double)state.payload_epoch);
      cJSON_AddNumberToObject(r, "bytes_saved_pending", state.bytes_saved_pending);
      cJSON_AddStringToObject(r, "rewrite_reason", state.rewrite_reason);
      long long total_decisions = state.deferred_rewrite_count + state.payload_epoch;
      double cache_hit_estimate =
          total_decisions > 0 ? (double)state.deferred_rewrite_count / (double)total_decisions
                              : 0.0;
      cJSON_AddNumberToObject(r, "cache_hit_estimate", cache_hit_estimate);
   }
   else
   {
      cJSON_AddStringToObject(r, "note", "No rewrite events recorded yet for this session");
      cJSON_AddNumberToObject(r, "forced_rewrite_count", 0);
      cJSON_AddNumberToObject(r, "cache_hit_estimate", 0.0);
   }
   return r;
}
