/* test_context_reduce.c: unit tests for the context economizer orchestrator
 * (Slice 1 — measure-only: baseline + foldable opportunity + cost forecast, no
 * mutation, hard-bypass contract). */
#include "context_reduce.h"
#include <assert.h>
#include <stdio.h>
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

int main(void)
{
   printf("context_reduce: unit tests\n");
   test_hard_bypass_null_out();
   test_null_messages();
   test_measure_only_no_mutation();
   test_short_history_no_foldable();
   test_provenance_already_reduced();
   test_unpriced_model_no_cost();
   test_history_fold_reduces();
   test_history_fold_skip_no_gain();
   printf("All context_reduce tests passed.\n");
   return 0;
}
