/* test_context_reduce.c: unit tests for the context economizer orchestrator
 * (Slice 1 — measure-only: baseline + foldable opportunity + cost forecast, no
 * mutation, hard-bypass contract). */
#include "context_reduce.h"
#include "token_tracker.h" /* token_usage_t, token_estimate_cost_ex — table-tethered guard test */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PASS(name) printf("  %s: ok\n", name)

/* Build a messages array of `rounds` user/assistant pairs with bulky content so
 * the foldable prefix is non-trivial. */
static cJSON *make_messages(int rounds)
{
   cJSON *arr = cJSON_CreateArray();
   for (int i = 0; i < rounds; i++)
   {
      cJSON *u = cJSON_CreateObject();
      cJSON_AddStringToObject(u, "role", "user");
      cJSON_AddStringToObject(u, "content",
                              "please read the file and summarize the relevant section in detail");
      cJSON_AddItemToArray(arr, u);
      cJSON *a = cJSON_CreateObject();
      cJSON_AddStringToObject(a, "role", "assistant");
      cJSON_AddStringToObject(
          a, "content",
          "here is a fairly long assistant turn that adds bytes to the transcript so the "
          "fold-eligible prefix carries real token volume across many turns of the session");
      cJSON_AddItemToArray(arr, a);
   }
   return arr;
}

static void test_hard_bypass_null_out(void)
{
   cJSON *m = make_messages(2);
   int rc = context_reduce(m, "sys", "gpt-4o", "s1", REDUCE_SEAM_DELEGATE, NULL, NULL, NULL);
   assert(rc == 1); /* NULL out -> non-zero so caller forwards original */
   cJSON_Delete(m);
   PASS("hard bypass on NULL out");
}

static void test_null_messages(void)
{
   reduce_result_t out;
   int rc = context_reduce(NULL, "sys", "gpt-4o", "s1", REDUCE_SEAM_DELEGATE, NULL, NULL, &out);
   assert(rc == 0 && out.reason == REDUCE_REASON_NONE && out.messages == NULL);
   context_reduce_result_free(&out); /* safe on no-op result */
   PASS("null messages -> no-op");
}

static void test_measure_only_no_mutation(void)
{
   cJSON *m = make_messages(20); /* 40 messages */
   reduce_config_t cfg = {0};
   cfg.delegate_seam = 1;
   cfg.measure_only = 1;
   reduce_result_t out;
   int rc = context_reduce(m, "system prompt here", "gpt-4o", "s1", REDUCE_SEAM_DELEGATE, &cfg,
                           NULL, &out);
   assert(rc == 0);
   assert(out.reason == REDUCE_REASON_MEASURED);
   assert(out.baseline_tokens > 0);
   assert(out.reduced_tokens == out.baseline_tokens); /* measure-only: nothing removed */
   assert(out.removed_tokens == 0);
   assert(out.mutated == 0 && out.messages == NULL); /* original untouched */
   /* 40 messages, retained default 8 -> foldable region is the first 32 -> > 0 */
   assert(out.foldable_tokens > 0);
   /* gpt-4o is priced -> opportunity bracket populated, ceiling (fresh) >= floor (cache-read) */
   assert(out.est_saved_cost_ceiling >= out.est_saved_cost_floor);
   assert(out.est_saved_cost_floor > 0.0);
   context_reduce_result_free(&out);
   cJSON_Delete(m);
   PASS("measure-only: baseline + foldable opportunity, no mutation");
}

/* Slice 3: the GATEWAY seam in shadow-mode (gateway_seam=1, measure_only=1, st=NULL)
 * — the exact config the inbound /v1 gateway wiring uses. Asserts the baseline is
 * computed, nothing is mutated, and the original array is byte-stable, so the live
 * client request is safe to forward unchanged. */
static void test_gateway_seam_measure_only(void)
{
   cJSON *m = make_messages(20); /* 40 messages */
   int n_before = cJSON_GetArraySize(m);
   char *before = cJSON_PrintUnformatted(m);
   reduce_config_t cfg = {0};
   cfg.gateway_seam = 1;
   cfg.measure_only = 1; /* shadow mode: never mutate the live request */
   cfg.fold.retained_msgs = 0;
   reduce_result_t out;
   int rc = context_reduce(m, "system prompt here", "claude-sonnet-4-5", "s1", REDUCE_SEAM_GATEWAY,
                           &cfg, NULL /* st */, &out);
   assert(rc == 0);
   assert(out.reason == REDUCE_REASON_MEASURED);
   assert(out.baseline_tokens > 0);
   assert(out.reduced_tokens == out.baseline_tokens); /* nothing removed */
   assert(out.removed_tokens == 0);
   assert(out.mutated == 0 && out.messages == NULL); /* request untouched */
   assert(out.foldable_tokens > 0);                  /* opportunity still measured */
   /* original array is byte-identical -> the gateway forwards it unchanged */
   char *after = cJSON_PrintUnformatted(m);
   assert(cJSON_GetArraySize(m) == n_before);
   assert(before && after && strcmp(before, after) == 0);
   free(before);
   free(after);
   context_reduce_result_free(&out);
   cJSON_Delete(m);
   PASS("gateway seam measure-only: baseline computed, request byte-identical");
}

static void test_gateway_seam_null_system_and_model(void)
{
   /* Live-gateway edge (roundtable): anthropic_system_to_text can return NULL and
    * a request may carry no model — both reach context_reduce as NULL. It must not
    * crash, must still measure (messages-only baseline, no $ forecast), and must
    * leave the request byte-identical. */
   cJSON *m = make_messages(20);
   char *before = cJSON_PrintUnformatted(m);
   reduce_config_t cfg = {0};
   cfg.gateway_seam = 1;
   cfg.measure_only = 1;
   reduce_result_t out;
   int rc = context_reduce(m, NULL /* system */, NULL /* model */, NULL, REDUCE_SEAM_GATEWAY, &cfg,
                           NULL, &out);
   assert(rc == 0);
   assert(out.reason == REDUCE_REASON_MEASURED);
   assert(out.baseline_tokens > 0);         /* messages-only baseline */
   assert(out.est_saved_cost_floor == 0.0); /* NULL model -> no $ forecast */
   assert(out.mutated == 0 && out.messages == NULL);
   char *after = cJSON_PrintUnformatted(m);
   assert(before && after && strcmp(before, after) == 0);
   free(before);
   free(after);
   context_reduce_result_free(&out);
   cJSON_Delete(m);
   PASS("gateway seam tolerates NULL system + NULL model, request byte-identical");
}

static void test_short_history_no_foldable(void)
{
   cJSON *m = make_messages(2); /* 4 messages, < retained(8) */
   reduce_config_t cfg = {0};
   cfg.delegate_seam = 1;
   reduce_result_t out;
   context_reduce(m, NULL, "gpt-4o", "s1", REDUCE_SEAM_DELEGATE, &cfg, NULL, &out);
   assert(out.foldable_tokens == 0);        /* nothing ahead of the retained tail */
   assert(out.est_saved_cost_floor == 0.0); /* no opportunity -> no forecast */
   context_reduce_result_free(&out);
   cJSON_Delete(m);
   PASS("short history -> zero foldable opportunity");
}

static void test_provenance_already_reduced(void)
{
   cJSON *m = make_messages(20);
   reduce_config_t cfg = {0};
   cfg.delegate_seam = 1;
   reduce_state_t st = {0};
   st.reduced = 1; /* a prior seam already reduced this request */
   reduce_result_t out;
   context_reduce(m, "sys", "gpt-4o", "s1", REDUCE_SEAM_DELEGATE, &cfg, &st, &out);
   assert(out.reason == REDUCE_REASON_ALREADY);
   assert(out.baseline_tokens > 0);  /* still re-measured */
   assert(out.foldable_tokens == 0); /* but no re-accounting of the opportunity */
   assert(out.mutated == 0);
   context_reduce_result_free(&out);
   cJSON_Delete(m);
   PASS("provenance: second seam re-measures, does not re-reduce");
}

static void test_unpriced_model_no_cost(void)
{
   cJSON *m = make_messages(20);
   reduce_config_t cfg = {0};
   cfg.delegate_seam = 1;
   reduce_result_t out;
   context_reduce(m, "sys", "some-unknown-model-xyz", "s1", REDUCE_SEAM_DELEGATE, &cfg, NULL, &out);
   assert(out.foldable_tokens > 0);         /* opportunity still measured (token-only) */
   assert(out.est_saved_cost_floor == 0.0); /* but no $ for an unpriced model */
   assert(out.est_saved_cost_ceiling == 0.0);
   context_reduce_result_free(&out);
   cJSON_Delete(m);
   PASS("unpriced model: token opportunity without a $ forecast");
}

/* Slice 2b: history_fold on (not measure_only) actually folds the prefix. */
static void test_history_fold_reduces(void)
{
   cJSON *m = make_messages(20); /* 40 messages */
   reduce_config_t cfg = {0};
   cfg.delegate_seam = 1;
   cfg.history_fold = 1; /* measure_only stays 0 -> real reduction */
   cfg.fold.closet.enabled = 1;
   reduce_state_t st = {0};
   reduce_result_t out;
   int rc = context_reduce(m, "system prompt here", "gpt-4o", "s1", REDUCE_SEAM_DELEGATE, &cfg, &st,
                           &out);
   assert(rc == 0);
   assert(out.reason == REDUCE_REASON_REDUCED);
   assert(out.mutated == 1 && out.messages != NULL); /* a NEW folded array */
   assert(out.messages != m);                        /* not the original */
   assert(out.folded_msgs > 0 && out.retained_msgs > 0);
   assert(out.reduced_tokens < out.baseline_tokens); /* genuinely smaller */
   assert(out.removed_tokens == out.baseline_tokens - out.reduced_tokens);
   assert(st.reduced == 1); /* provenance stamped */
   /* cost bracket priced on the realized saving (removed_tokens) */
   assert(out.est_saved_cost_ceiling >= out.est_saved_cost_floor);
   /* original array untouched (immutable prefix zone honored) */
   assert(cJSON_GetArraySize(m) == 40);
   context_reduce_result_free(&out); /* frees the new array */
   cJSON_Delete(m);
   PASS("history_fold reduces: new folded array, original untouched");
}

/* Net-gain pre-check: foldable below min_gain_tokens -> skip, no mutation. */
static void test_history_fold_skip_no_gain(void)
{
   cJSON *m = make_messages(20);
   reduce_config_t cfg = {0};
   cfg.delegate_seam = 1;
   cfg.history_fold = 1;
   cfg.min_gain_tokens = 1000000; /* unreachable -> always skip */
   reduce_result_t out;
   int rc = context_reduce(m, "sys", "gpt-4o", "s1", REDUCE_SEAM_DELEGATE, &cfg, NULL, &out);
   assert(rc == 0);
   assert(out.reason == REDUCE_REASON_SKIP_NO_GAIN);
   assert(out.mutated == 0 && out.messages == NULL); /* original forwarded */
   context_reduce_result_free(&out);
   cJSON_Delete(m);
   PASS("history_fold skip_no_gain: below min_gain, no mutation");
}

/* Build a single-user-turn OpenAI tool-loop: 1 user msg, then `pairs` of
 * (assistant tool_calls + role:"tool" result with a big body). There is NO user
 * turn mid-loop, so context_fold_view can never find a clean boundary — the proof
 * surface for the boundary-free compress lever. */
static cJSON *make_tool_loop(int pairs)
{
   cJSON *arr = cJSON_CreateArray();
   cJSON *u = cJSON_CreateObject();
   cJSON_AddStringToObject(u, "role", "user");
   cJSON_AddStringToObject(u, "content", "Do the autonomous task.");
   cJSON_AddItemToArray(arr, u);
   for (int k = 0; k < pairs; k++)
   {
      char id[32];
      snprintf(id, sizeof(id), "call_%02d", k);
      cJSON *a = cJSON_CreateObject();
      cJSON_AddStringToObject(a, "role", "assistant");
      cJSON *tcs = cJSON_AddArrayToObject(a, "tool_calls");
      cJSON *tc = cJSON_CreateObject();
      cJSON_AddStringToObject(tc, "id", id);
      cJSON_AddStringToObject(tc, "type", "function");
      cJSON *fn = cJSON_AddObjectToObject(tc, "function");
      cJSON_AddStringToObject(fn, "name", "read_file");
      cJSON_AddStringToObject(fn, "arguments", "{}");
      cJSON_AddItemToArray(tcs, tc);
      cJSON_AddItemToArray(arr, a);

      cJSON *t = cJSON_CreateObject();
      cJSON_AddStringToObject(t, "role", "tool");
      cJSON_AddStringToObject(t, "tool_call_id", id);
      char body[700];
      size_t pos = 0;
      while (pos + 48 < sizeof(body) - 96)
         pos +=
             (size_t)snprintf(body + pos, sizeof(body) - pos, "filler output bytes here and on; ");
      snprintf(body + pos, sizeof(body) - pos, "tail at /work/src/module_%d.c done", k);
      cJSON_AddStringToObject(t, "content", body);
      cJSON_AddItemToArray(arr, t);
   }
   return arr;
}

/* Compress engages on a tool-loop where fold cannot: fold-only no-ops (no clean
 * boundary), but compress mutates and shrinks the old tool-result bodies. */
static void test_compress_engages_where_fold_cannot(void)
{
   /* fold-only: must NOT mutate (no clean user-turn boundary in the loop) */
   {
      cJSON *m = make_tool_loop(12);
      reduce_config_t cfg = {0};
      cfg.delegate_seam = 1;
      cfg.history_fold = 1;
      cfg.fold.closet.enabled = 1;
      reduce_result_t out;
      int rc = context_reduce(m, "sys", "gpt-4o", "s1", REDUCE_SEAM_DELEGATE, &cfg, NULL, &out);
      assert(rc == 0);
      assert(out.mutated == 0 && out.messages == NULL); /* fold found no boundary */
      context_reduce_result_free(&out);
      cJSON_Delete(m);
   }
   /* compress-only: mutates, shrinks, preserves every message + tool_call_id */
   {
      cJSON *m = make_tool_loop(12);
      int orig_count = cJSON_GetArraySize(m);
      reduce_config_t cfg = {0};
      cfg.delegate_seam = 1;
      cfg.compress = 1;
      cfg.fold.closet.enabled = 1;
      /* small head so the merged head+tail core net-shrinks these modest (~0.6 KB)
       * tool bodies; also exercises the compact.* knob threading into the lever. */
      cfg.fold.compact_head_bytes = 40;
      reduce_state_t st = {0};
      reduce_result_t out;
      int rc = context_reduce(m, "sys", "gpt-4o", "s1", REDUCE_SEAM_DELEGATE, &cfg, &st, &out);
      assert(rc == 0);
      assert(out.reason == REDUCE_REASON_REDUCED);
      assert(out.mutated == 1 && out.messages != NULL && out.messages != m);
      assert(out.folded_msgs > 0);
      assert(out.reduced_tokens < out.baseline_tokens); /* genuinely smaller */
      assert(out.removed_tokens == out.baseline_tokens - out.reduced_tokens);
      assert(st.reduced == 1);
      assert(cJSON_GetArraySize(m) == orig_count); /* original untouched */
      /* a buried path is conserved (Coordinate Closet rode along) */
      char *flat = cJSON_PrintUnformatted(out.messages);
      assert(flat && strstr(flat, "/work/src/module_2.c"));
      free(flat);
      context_reduce_result_free(&out);
      cJSON_Delete(m);
   }
}

/* measure_only must suppress the compress mutation (shadow mode). */
static void test_compress_measure_only_no_mutation(void)
{
   cJSON *m = make_tool_loop(12);
   reduce_config_t cfg = {0};
   cfg.delegate_seam = 1;
   cfg.compress = 1;
   cfg.measure_only = 1; /* shadow */
   cfg.fold.closet.enabled = 1;
   reduce_result_t out;
   int rc = context_reduce(m, "sys", "gpt-4o", "s1", REDUCE_SEAM_DELEGATE, &cfg, NULL, &out);
   assert(rc == 0);
   assert(out.reason == REDUCE_REASON_MEASURED);
   assert(out.mutated == 0 && out.messages == NULL);
   context_reduce_result_free(&out);
   cJSON_Delete(m);
   PASS("compress measure_only: shadow, no mutation");
}

/* Slice 5 freeze cost guardrail. The guard prices ONE cache-write PREMIUM (over
 * sending the prefix fresh once) against `horizon` reuses at the cache-read rate.
 * Key regression: it must NOT disable freeze for real providers — both OpenAI (free
 * cache writes) and Anthropic (small write premium, large read discount) pay off,
 * even at the conservative default horizon of 1. The disable branch fires only under
 * adverse pricing (write premium > horizon x per-reuse saving) that no model aimee
 * currently prices satisfies. */
static void test_freeze_guard(void)
{
   /* zero prefix -> no churn possible -> always favorable */
   assert(reduce_freeze_cost_favorable("claude-3-5-sonnet", 0, 1) == 1);
   /* NULL / empty / unpriced model -> fail-open (preserve prior always-on freeze) */
   assert(reduce_freeze_cost_favorable(NULL, 100000, 1) == 1);
   assert(reduce_freeze_cost_favorable("", 100000, 1) == 1);
   assert(reduce_freeze_cost_favorable("totally-unknown-model-xyz", 100000, 1) == 1);
   /* OpenAI: cache_write == 0 -> non-positive write premium -> always freeze */
   assert(reduce_freeze_cost_favorable("gpt-4o", 100000, 1) == 1);
   /* Anthropic at the default horizon=1: premium 0.75x vs per-reuse saving 2.70x ->
    * favorable. This is the regression guard against the naive full-write-cost
    * formula, which would wrongly disable Anthropic freeze here. */
   assert(reduce_freeze_cost_favorable("claude-3-5-sonnet", 100000, 1) == 1);
   assert(reduce_freeze_cost_favorable("claude-3-opus", 100000, 1) == 1);
   /* horizon monotonic + clamp: more reuse never makes a favorable case unfavorable,
    * and an out-of-range horizon is clamped (no crash, still favorable). */
   assert(reduce_freeze_cost_favorable("claude-3-5-sonnet", 100000, 9999) == 1);
   assert(reduce_freeze_cost_favorable("claude-3-5-sonnet", 100000, -5) == 1);

   /* Table-tethered invariant: recompute the Anthropic premium/saving from the LIVE
    * pricing table (not prose) so a future price change that flips the verdict fails
    * HERE, not silently. write premium must be positive and a single reuse must cover
    * it (this is exactly what the naive full-write-cost formula got wrong). */
   {
      const char *m = "claude-3-5-sonnet";
      int n = 100000;
      token_usage_t w = {0}, in = {0}, rd = {0};
      w.cache_write_tokens = n;
      in.input_tokens = n;
      rd.cache_read_tokens = n;
      double write_cost = token_estimate_cost_ex(m, &w, NULL);
      double input_cost = token_estimate_cost_ex(m, &in, NULL);
      double read_cost = token_estimate_cost_ex(m, &rd, NULL);
      assert(write_cost > input_cost); /* a real write premium exists */
      assert((input_cost - read_cost) >= (write_cost - input_cost)); /* 1 reuse covers it */
   }

   /* Disable branch — unreachable with currently-priced models, exercised here via
    * the scale-invariant arithmetic core with SYNTHETIC per-tier costs. */
   /* favorable (Anthropic-like: input 3, write 3.75, read 0.30) at H=1 */
   assert(reduce_freeze_favorable_rates(3.0, 3.75, 0.30, 1) == 1);
   /* free write but NO read discount (read == input) -> skip regardless of write
    * (pins B1: per-reuse saving is checked BEFORE the write premium) */
   assert(reduce_freeze_favorable_rates(1.0, 0.0, 1.0, 1) == 0);
   /* free write WITH a read discount -> always favorable */
   assert(reduce_freeze_favorable_rates(1.0, 0.0, 0.10, 1) == 1);
   /* adverse: write premium (9) far exceeds per-reuse saving (0.9) at H=1 -> disable */
   assert(reduce_freeze_favorable_rates(1.0, 10.0, 0.10, 1) == 0);
   /* same adverse rates, but the horizon clamp (max 5) still cannot cover it -> disable
    * (break-even needs H=10) */
   assert(reduce_freeze_favorable_rates(1.0, 10.0, 0.10, 9999) == 0);
   /* a milder premium that one reuse misses but several cover: premium 2.0, saving
    * 0.9 -> H=1 disable, H=3 (2.7 >= 2.0) enable */
   assert(reduce_freeze_favorable_rates(1.0, 3.0, 0.10, 1) == 0);
   assert(reduce_freeze_favorable_rates(1.0, 3.0, 0.10, 3) == 1);
   PASS("freeze cost guardrail: fail-open, real providers freeze, synthetic disable branch");
}

int main(void)
{
   printf("context_reduce: unit tests\n");
   test_freeze_guard();
   test_hard_bypass_null_out();
   test_null_messages();
   test_measure_only_no_mutation();
   test_gateway_seam_measure_only();
   test_gateway_seam_null_system_and_model();
   test_short_history_no_foldable();
   test_provenance_already_reduced();
   test_unpriced_model_no_cost();
   test_history_fold_reduces();
   test_history_fold_skip_no_gain();
   test_compress_engages_where_fold_cannot();
   test_compress_measure_only_no_mutation();
   printf("All context_reduce tests passed.\n");
   return 0;
}
