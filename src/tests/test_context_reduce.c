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

/* §4 page table, end to end through the orchestrator: a coordinate that is FOLDED AWAY
 * and then referenced again by the newest turn produces a recall hint, so the agent
 * learns the content is pageable rather than gone. This is the property that makes
 * eviction reversible; without it the fold is destructive. */
static void test_recall_hint_on_retouch(void)
{
   cJSON *m = make_messages(20); /* 40 messages; the prefix folds away */

   /* Put an address deep in the prefix (evicted), then re-touch it in the newest turn
    * (retained). Anything in the retained tail was never evicted, so a hint proves the
    * key came from the folded region rather than from text still in view. */
   cJSON *early = cJSON_GetArrayItem(m, 1);
   cJSON_ReplaceItemInObject(early, "content",
                             cJSON_CreateString("inspect src/modules/git/retry.c closely, it is "
                                                "the file that matters for this task"));
   cJSON *last = cJSON_GetArrayItem(m, cJSON_GetArraySize(m) - 1);
   cJSON_ReplaceItemInObject(last, "content",
                             cJSON_CreateString("what did we conclude about "
                                                "src/modules/git/retry.c ?"));

   reduce_config_t cfg = {0};
   cfg.delegate_seam = 1;
   cfg.history_fold = 1;
   cfg.fold.closet.enabled = 1;
   cfg.recall_enabled = 1;

   reduce_state_t st = {0};
   st.turn = 7;
   reduce_result_t out;
   assert(context_reduce(m, "system prompt here", "gpt-4o", "s1", REDUCE_SEAM_DELEGATE, &cfg, &st,
                         &out) == 0);
   assert(out.mutated == 1);
   assert(out.folded_msgs > 0);

   assert(out.recall_surfaced >= 1);
   assert(out.recall_hint != NULL);
   assert(strstr(out.recall_hint, "src/modules/git/retry.c") != NULL);
   /* The hint names the resolvers, so the agent knows how to act on it. */
   assert(strstr(out.recall_hint, "code_span_get") != NULL);

   /* The page table persists in the caller-owned state, not in the result. */
   assert(st.recall.count > 0);

   context_reduce_result_free(&out);
   assert(out.recall_hint == NULL); /* freed with the result */
   fold_recall_index_free(&st.recall);
   cJSON_Delete(m);
   PASS("recall hint fires when the newest turn re-touches a folded coordinate");
}

/* Default-off stays off: with the lever disabled nothing is tracked and no hint is
 * produced, so enabling it is an explicit choice and the reducer is unchanged without. */
static void test_recall_disabled_is_inert(void)
{
   cJSON *m = make_messages(20);
   cJSON *early = cJSON_GetArrayItem(m, 1);
   cJSON_ReplaceItemInObject(early, "content",
                             cJSON_CreateString("inspect src/modules/git/retry.c closely here"));
   cJSON *last = cJSON_GetArrayItem(m, cJSON_GetArraySize(m) - 1);
   cJSON_ReplaceItemInObject(last, "content",
                             cJSON_CreateString("what about src/modules/git/retry.c ?"));

   reduce_config_t cfg = {0};
   cfg.delegate_seam = 1;
   cfg.history_fold = 1;
   cfg.fold.closet.enabled = 1;
   cfg.recall_enabled = 0; /* the lever under test */

   reduce_state_t st = {0};
   reduce_result_t out;
   assert(context_reduce(m, "system prompt here", "gpt-4o", "s1", REDUCE_SEAM_DELEGATE, &cfg, &st,
                         &out) == 0);
   assert(out.mutated == 1);
   assert(out.recall_hint == NULL);
   assert(out.recall_surfaced == 0);
   assert(st.recall.count == 0);

   context_reduce_result_free(&out);
   fold_recall_index_free(&st.recall);
   cJSON_Delete(m);
   PASS("recall disabled: nothing tracked, no hint");
}

/* Injection puts the hint in front of the model. Default-off and separate from tracking,
 * because reporting is inert while injecting changes what the model does. */
static void test_recall_inject_appends_notice(void)
{
   cJSON *m = make_messages(20);
   cJSON *early = cJSON_GetArrayItem(m, 1);
   cJSON_ReplaceItemInObject(early, "content",
                             cJSON_CreateString("inspect src/modules/git/retry.c closely here"));
   cJSON *last = cJSON_GetArrayItem(m, cJSON_GetArraySize(m) - 1);
   cJSON_ReplaceItemInObject(last, "content",
                             cJSON_CreateString("what about src/modules/git/retry.c ?"));

   reduce_config_t cfg = {0};
   cfg.delegate_seam = 1;
   cfg.history_fold = 1;
   cfg.fold.closet.enabled = 1;
   cfg.recall_enabled = 1;
   cfg.recall_inject = 1;

   reduce_state_t st = {0};
   reduce_result_t out;
   assert(context_reduce(m, "sys", "gpt-4o", "s1", REDUCE_SEAM_DELEGATE, &cfg, &st, &out) == 0);
   assert(out.mutated == 1 && out.recall_surfaced >= 1);

   int n = cJSON_GetArraySize(out.messages);
   cJSON *tail = cJSON_GetArrayItem(out.messages, n - 1);
   const char *txt = cJSON_GetStringValue(cJSON_GetObjectItem(tail, "content"));
   assert(txt != NULL);
   assert(strstr(txt, "src/modules/git/retry.c") != NULL);
   /* Labelled as a notice: an unlabelled line appended after the user's turn reads as
    * something the USER said, which is a way for evicted text to put words in their
    * mouth. */
   assert(strstr(txt, "not from the user") != NULL);

   /* Appended at the very END — never spliced between an assistant tool_use and its
    * matching tool_result, which would make the request structurally invalid. */
   assert(tail != cJSON_GetArrayItem(out.messages, 0));

   context_reduce_result_free(&out);
   fold_recall_index_free(&st.recall);
   cJSON_Delete(m);
   PASS("recall injection appends a labelled notice at the tail");
}

/* Tracking without injection must leave the transcript exactly as the fold produced it:
 * the reporting contract is what callers with their own placement rely on. */
static void test_recall_inject_off_leaves_transcript(void)
{
   cJSON *m = make_messages(20);
   cJSON *early = cJSON_GetArrayItem(m, 1);
   cJSON_ReplaceItemInObject(early, "content",
                             cJSON_CreateString("inspect src/modules/git/retry.c closely here"));
   cJSON *last = cJSON_GetArrayItem(m, cJSON_GetArraySize(m) - 1);
   cJSON_ReplaceItemInObject(last, "content",
                             cJSON_CreateString("what about src/modules/git/retry.c ?"));

   reduce_config_t cfg = {0};
   cfg.delegate_seam = 1;
   cfg.history_fold = 1;
   cfg.fold.closet.enabled = 1;
   cfg.recall_enabled = 1;
   cfg.recall_inject = 0; /* the lever under test */

   reduce_state_t st = {0};
   reduce_result_t out;
   assert(context_reduce(m, "sys", "gpt-4o", "s1", REDUCE_SEAM_DELEGATE, &cfg, &st, &out) == 0);
   assert(out.recall_surfaced >= 1); /* still tracked and reported */
   assert(out.recall_hint != NULL);

   int n = cJSON_GetArraySize(out.messages);
   cJSON *tail = cJSON_GetArrayItem(out.messages, n - 1);
   const char *txt = cJSON_GetStringValue(cJSON_GetObjectItem(tail, "content"));
   assert(txt != NULL);
   assert(strstr(txt, "not from the user") == NULL); /* nothing appended */

   context_reduce_result_free(&out);
   fold_recall_index_free(&st.recall);
   cJSON_Delete(m);
   PASS("injection off: hint reported, transcript untouched");
}

/* THE PROGRAMME'S HEADLINE CRITERION, end to end: a coordinate evicted in an EARLIER RUN
 * can be surfaced as pageable in a later one.
 *
 * Every piece of this is unit-tested in isolation — the page table (S2d), serialization
 * (S2c), detection — but the CHAIN has never been exercised, and a chain of three
 * individually-correct parts is exactly where an integration bug hides.
 *
 * The design makes the result unambiguous: run 2's transcript mentions the coordinate
 * ONLY in its newest turn, which is in the retained tail and never evicted. So run 2's
 * own fold cannot have harvested it, and a hint can only come from the state restored
 * from run 1. Without that discipline the test would pass on a completely broken
 * restore. */
static void test_recall_survives_across_runs(void)
{
   const char *coord = "src/modules/git/retry.c";

   /* ---- run 1: the coordinate is deep in the prefix, and gets folded away ---- */
   cJSON *run1 = make_messages(20);
   cJSON *early = cJSON_GetArrayItem(run1, 1);
   cJSON_ReplaceItemInObject(
       early, "content",
       cJSON_CreateString("the retry backoff lives in src/modules/git/retry.c and needs care"));

   reduce_config_t cfg = {0};
   cfg.delegate_seam = 1;
   cfg.history_fold = 1;
   cfg.fold.closet.enabled = 1;
   cfg.recall_enabled = 1;

   reduce_state_t st1 = {0};
   st1.turn = 3;
   reduce_result_t r1;
   assert(context_reduce(run1, "sys", "gpt-4o", "s1", REDUCE_SEAM_DELEGATE, &cfg, &st1, &r1) == 0);
   assert(r1.mutated == 1 && r1.folded_msgs > 0);
   assert(st1.recall.count > 0); /* the page table saw the eviction */

   /* ---- the run ends: state is persisted ---- */
   char *saved = reduce_state_serialize(&st1);
   assert(saved != NULL);
   assert(strstr(saved, coord) != NULL);
   context_reduce_result_free(&r1);
   fold_recall_index_free(&st1.recall);
   cJSON_Delete(run1);

   /* ---- run 2: a fresh process, restoring that state ---- */
   reduce_state_t st2 = {0};
   assert(reduce_state_restore(&st2, saved) == 0);
   free(saved);
   assert(st2.recall.count > 0);
   st2.turn = 9; /* far enough ahead that the anti-thrash TTL cannot suppress the hint */

   /* This transcript never mentions the coordinate except in its newest turn, which is
    * retained verbatim and therefore never evicted — so nothing here can put the key in
    * the table. */
   cJSON *run2 = make_messages(20);
   cJSON *last = cJSON_GetArrayItem(run2, cJSON_GetArraySize(run2) - 1);
   cJSON_ReplaceItemInObject(last, "content",
                             cJSON_CreateString("remind me what we concluded about "
                                                "src/modules/git/retry.c before"));

   reduce_result_t r2;
   assert(context_reduce(run2, "sys", "gpt-4o", "s1", REDUCE_SEAM_DELEGATE, &cfg, &st2, &r2) == 0);

   /* The payoff: an eviction from the PREVIOUS run is still pageable in this one. */
   assert(r2.recall_surfaced >= 1);
   assert(r2.recall_hint != NULL);
   assert(strstr(r2.recall_hint, coord) != NULL);
   assert(strstr(r2.recall_hint, "code_span_get") != NULL);

   context_reduce_result_free(&r2);
   fold_recall_index_free(&st2.recall);
   cJSON_Delete(run2);
   PASS("a coordinate evicted in an earlier RUN is still pageable in the next");
}

/* The negative half: with no restored state, the same run 2 produces NO hint. Without
 * this the test above could pass on a bug that hinted for any mentioned path. */
static void test_recall_absent_without_restored_state(void)
{
   reduce_config_t cfg = {0};
   cfg.delegate_seam = 1;
   cfg.history_fold = 1;
   cfg.fold.closet.enabled = 1;
   cfg.recall_enabled = 1;

   reduce_state_t fresh = {0}; /* nothing restored */
   fresh.turn = 9;

   cJSON *run2 = make_messages(20);
   cJSON *last = cJSON_GetArrayItem(run2, cJSON_GetArraySize(run2) - 1);
   cJSON_ReplaceItemInObject(last, "content",
                             cJSON_CreateString("remind me what we concluded about "
                                                "src/modules/git/retry.c before"));

   reduce_result_t r;
   assert(context_reduce(run2, "sys", "gpt-4o", "s1", REDUCE_SEAM_DELEGATE, &cfg, &fresh, &r) == 0);
   assert(r.recall_hint == NULL);
   assert(r.recall_surfaced == 0);

   context_reduce_result_free(&r);
   fold_recall_index_free(&fresh.recall);
   cJSON_Delete(run2);
   PASS("no restored state: the same turn produces no hint");
}

/* ------------------------------------------------ state persistence (S2c) */

/* Round-trip: the freeze boundary and the page table survive, so a later run continues
 * the conversation instead of restarting the economizer from scratch. */
static void test_state_serialize_round_trip(void)
{
   reduce_state_t st = {0};
   st.turn = 12;
   st.freeze.active = 1;
   st.freeze.frozen_split = 9;
   st.freeze.tail_cap_msgs = 24;
   st.freeze.epochs = 3;
   st.freeze.prefix_digest = 0xfeedfacecafeb00dULL;
   fold_recall_index_add(&st.recall, "src/modules/git/retry.c");
   fold_recall_index_add(&st.recall, "memory:8817");
   st.recall.last_turn[0] = 5;
   st.recall.last_turn[1] = -1;

   char *json = reduce_state_serialize(&st);
   assert(json != NULL);
   assert(strlen(json) <= REDUCE_STATE_SERIAL_MAX);

   reduce_state_t back = {0};
   assert(reduce_state_restore(&back, json) == 0);
   assert(back.turn == 12);
   assert(back.freeze.active == 1);
   assert(back.freeze.frozen_split == 9);
   assert(back.freeze.tail_cap_msgs == 24);
   assert(back.freeze.epochs == 3);
   /* The digest must survive EXACTLY: it is the guard that stops a restored boundary
    * being reused over a prefix that has since changed. A JSON number would have lost
    * the low bits here. */
   assert(back.freeze.prefix_digest == 0xfeedfacecafeb00dULL);
   assert(back.recall.count == 2);

   free(json);
   fold_recall_index_free(&st.recall);
   fold_recall_index_free(&back.recall);
   PASS("state round-trips, including the full 64-bit prefix digest");
}

/* `reduced` is per-REQUEST provenance. Restoring it would make the next request think a
 * seam had already reduced it, and skip reduction for the rest of the conversation. */
static void test_state_never_restores_reduced(void)
{
   reduce_state_t st = {0};
   st.reduced = 1;
   st.turn = 4;
   char *json = reduce_state_serialize(&st);
   assert(json != NULL);
   assert(strstr(json, "reduced") == NULL); /* not even written */

   reduce_state_t back = {0};
   back.reduced = 1; /* pre-dirtied: restore must clear it */
   assert(reduce_state_restore(&back, json) == 0);
   assert(back.reduced == 0);

   free(json);
   fold_recall_index_free(&back.recall);
   PASS("reduced is never persisted or restored");
}

/* Unparseable input must leave NO state, not half of one: a split without its digest
 * would be trusted by the fold and could serve a stale prefix. */
static void test_state_restore_all_or_nothing(void)
{
   reduce_state_t st = {0};
   st.freeze.active = 1;
   st.freeze.frozen_split = 7;
   fold_recall_index_add(&st.recall, "src/a.c");

   assert(reduce_state_restore(&st, "{\"freeze\":{\"frozen_spl") == -1); /* truncated */
   assert(reduce_state_restore(&st, "") == -1);
   assert(reduce_state_restore(&st, NULL) == -1);
   /* Prior state is untouched by a failed restore. */
   assert(st.freeze.frozen_split == 7);
   assert(st.recall.count == 1);

   fold_recall_index_free(&st.recall);
   PASS("failed restore leaves prior state intact");
}

/* The store reads into a fixed char[8192], so oversize must be bounded HERE and the loss
 * reported — truncated JSON would not parse, turning a large page table into no state. */
static void test_state_serialize_bounded_and_reports_drops(void)
{
   reduce_state_t st = {0};
   for (int i = 0; i < 400; i++)
   {
      char key[64];
      snprintf(key, sizeof(key), "src/generated/module_%03d/file_%03d.c", i, i);
      fold_recall_index_add(&st.recall, key);
      st.recall.last_turn[i] = i; /* newer keys have higher last_turn */
   }

   char *json = reduce_state_serialize(&st);
   assert(json != NULL);
   assert(strlen(json) <= REDUCE_STATE_SERIAL_MAX);
   assert(strstr(json, "recall_dropped") != NULL);

   reduce_state_t back = {0};
   assert(reduce_state_restore(&back, json) == 0);
   assert(back.recall.count > 0);
   assert(back.recall.count < 400); /* genuinely capped */

   /* The COLDEST keys are the ones dropped: key 399 was surfaced most recently and
    * must survive, key 000 must not. */
   int has_newest = 0, has_oldest = 0;
   for (size_t i = 0; i < back.recall.count; i++)
   {
      if (strstr(back.recall.keys[i], "file_399"))
         has_newest = 1;
      if (strstr(back.recall.keys[i], "file_000"))
         has_oldest = 1;
   }
   assert(has_newest);
   assert(!has_oldest);

   free(json);
   fold_recall_index_free(&st.recall);
   fold_recall_index_free(&back.recall);
   PASS("serialization is bounded, drops the coldest keys, and says so");
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
/* Append one realistic turn: a user ask, an assistant tool_call, a bulky tool result,
 * and an assistant reply. Gives BOTH levers something to do — compress needs fat tool
 * bodies, fold needs clean user-turn boundaries. */
static void append_turn(cJSON *m, int k)
{
   char id[32];
   snprintf(id, sizeof(id), "call_%03d", k);

   cJSON *u = cJSON_CreateObject();
   cJSON_AddStringToObject(u, "role", "user");
   cJSON_AddStringToObject(u, "content", "keep going on the migration and report what changed");
   cJSON_AddItemToArray(m, u);

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
   cJSON_AddItemToArray(m, a);

   cJSON *t = cJSON_CreateObject();
   cJSON_AddStringToObject(t, "role", "tool");
   cJSON_AddStringToObject(t, "tool_call_id", id);
   char body[900];
   size_t pos = 0;
   while (pos + 48 < sizeof(body) - 96)
      pos += (size_t)snprintf(body + pos, sizeof(body) - pos, "filler output bytes here and on; ");
   snprintf(body + pos, sizeof(body) - pos, "tail at /work/src/stage_%d.c done", k);
   cJSON_AddStringToObject(t, "content", body);
   cJSON_AddItemToArray(m, t);

   cJSON *a2 = cJSON_CreateObject();
   cJSON_AddStringToObject(a2, "role", "assistant");
   cJSON_AddStringToObject(a2, "content",
                           "read the stage file and applied the change; moving to the next one "
                           "now that the previous edit is confirmed good");
   cJSON_AddItemToArray(m, a2);
}

/* THE CACHE CLAIM, tested end-to-end through the composed reducer.
 *
 * The freeze exists so the reduced PREFIX stays byte-identical turn-to-turn and the
 * provider prompt cache keeps hitting. test_context_fold.c proves that for the fold
 * lever in isolation — but production stacks compress AHEAD of fold and the fold
 * digests the COMPRESSED view, so a compressor whose output for the prefix region
 * drifts as the retained band slides would silently epoch the freeze and bust the
 * cache with every lever still reporting success. Nothing asserted that.
 *
 * The invariant, stated honestly: while the epoch counter holds, the emitted prefix
 * must be byte-identical; a changed prefix is legal ONLY on an epoch advance. That is
 * the property the cache actually depends on. Bytes are compared, not token counts —
 * a cache hit is a bytewise prefix match, so anything weaker would pass on a prefix
 * that "looks the same" and still misses.
 *
 * recall_inject is deliberately ON: the header claims tail placement is cache-safe.
 * If the hint ever landed in the prefix instead, this test fails. */
static void test_prefix_stable_across_turns(void)
{
   cJSON *m = cJSON_CreateArray();
   for (int k = 0; k < 8; k++)
      append_turn(m, k);

   reduce_config_t cfg = {0};
   cfg.delegate_seam = 1;
   cfg.history_fold = 1;
   cfg.compress = 1;
   cfg.freeze = 1;
   cfg.fold.closet.enabled = 1;
   cfg.fold.compact_head_bytes = 40;
   cfg.recall_enabled = 1;
   cfg.recall_inject = 1;

   reduce_state_t st = {0};
   st.freeze.tail_cap_msgs = 16;

   char *prev_prefix = NULL;
   int prev_epochs = -1;
   int reuses = 0; /* turns that held the prefix steady */
   int epochs = 0; /* legitimate boundary advances */

   /* claude-3-5-sonnet: priced with a cache-read discount, so the freeze guardrail
    * finds the pin cost-favorable and the freeze is actually live for this run. */
   for (int turn = 0; turn < 14; turn++)
   {
      st.turn = turn;
      reduce_result_t out;
      assert(context_reduce(m, "sys", "claude-3-5-sonnet", "s-freeze", REDUCE_SEAM_DELEGATE, &cfg,
                            &st, &out) == 0);
      st.reduced = 0; /* next turn is a fresh request, not a second seam on this one */

      if (out.mutated && out.messages)
      {
         char *prefix = cJSON_PrintUnformatted(cJSON_GetArrayItem(out.messages, 0));
         assert(prefix != NULL);
         if (prev_prefix)
         {
            if (out.epochs == prev_epochs)
            {
               /* No epoch advance -> the cache MUST still be warm. */
               assert(strcmp(prev_prefix, prefix) == 0);
               reuses++;
            }
            else
            {
               assert(out.epochs > prev_epochs); /* epochs only ever advance */
               epochs++;
            }
         }
         free(prev_prefix);
         prev_prefix = prefix;
         prev_epochs = out.epochs;
      }
      context_reduce_result_free(&out);
      append_turn(m, 100 + turn);
   }

   /* Guard the guard: a run where the fold never engaged, or never held a boundary
    * across a turn, would satisfy every assertion above vacuously. */
   assert(reuses > 0);

   free(prev_prefix);
   fold_recall_index_free(&st.recall);
   cJSON_Delete(m);
   printf("  prefix stable across turns: %d cache-warm reuses, %d epoch advance(s)\n", reuses,
          epochs);
   PASS("composed reduce keeps the emitted prefix byte-identical while frozen");
}

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
   test_recall_hint_on_retouch();
   test_recall_disabled_is_inert();
   test_recall_inject_appends_notice();
   test_recall_inject_off_leaves_transcript();
   test_recall_survives_across_runs();
   test_recall_absent_without_restored_state();
   test_state_serialize_round_trip();
   test_state_never_restores_reduced();
   test_state_restore_all_or_nothing();
   test_state_serialize_bounded_and_reports_drops();
   test_history_fold_skip_no_gain();
   test_compress_engages_where_fold_cannot();
   test_compress_measure_only_no_mutation();
   test_prefix_stable_across_turns();
   printf("All context_reduce tests passed.\n");
   return 0;
}
