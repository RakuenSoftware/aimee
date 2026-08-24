/* test_learning_eval_synthesis.c: the pure policy behind recursive
 * self-improvement S0 (endogeneity classification) and S1 (failure ->
 * regression task synthesis and admission).
 *
 * Everything asserted here is a pure function, so the test needs no DB and no
 * model — which is the point: the policy that decides what aimee is allowed to
 * teach itself must be inspectable in isolation.
 *
 * See docs/proposals/pending/recursive-self-improvement-closing-the-loops.md */

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"

#include <aimee/learning/eval_synthesis.h>
#include <aimee/learning/learning.h>

static learning_eval_failure_t sample_failure(void)
{
   learning_eval_failure_t f;
   memset(&f, 0, sizeof(f));
   f.origin = "correction";
   f.origin_ref = "signal:412";
   f.role = "execute";
   f.prompt = "Finish the change and report status.";
   f.failure_mode = "claimed done without running the tests";
   f.check_type = "contains";
   f.check_value = "tests passed";
   return f;
}

static void test_evidence_classification(void)
{
   /* A human acting through the feedback surface is exogenous. */
   assert(learning_evidence_classify("explicit", "correction") == LEARNING_EVIDENCE_EXOGENOUS);
   assert(learning_evidence_classify("operator", "mark_rule") == LEARNING_EVIDENCE_EXOGENOUS);
   assert(learning_evidence_classify("verify", "") == LEARNING_EVIDENCE_EXOGENOUS);

   /* aimee reading its own transcript is not. */
   assert(learning_evidence_classify("implicit", "repeat_question") ==
          LEARNING_EVIDENCE_ENDOGENOUS);

   /* The capture API defaults an unset source to "explicit", so signal_type
    * must win for the self-derived detector types — otherwise a caller could
    * launder its own inference into the exogenous count by omitting a field. */
   assert(learning_evidence_classify("explicit", "repeated_correction") ==
          LEARNING_EVIDENCE_ENDOGENOUS);
   assert(learning_evidence_classify("explicit", "citation_then_repair") ==
          LEARNING_EVIDENCE_ENDOGENOUS);
   assert(learning_evidence_classify("explicit", "workflow_repetition") ==
          LEARNING_EVIDENCE_ENDOGENOUS);

   /* Unknown provenance counts against us, and so does no provenance. */
   assert(learning_evidence_classify("some_new_source", "correction") ==
          LEARNING_EVIDENCE_ENDOGENOUS);
   assert(learning_evidence_classify(NULL, NULL) == LEARNING_EVIDENCE_ENDOGENOUS);
   assert(learning_evidence_classify("", "") == LEARNING_EVIDENCE_ENDOGENOUS);
}

static void test_text_admissibility(void)
{
   assert(learning_eval_text_admissible("plain ascii, punctuation. fine!") == 1);
   assert(learning_eval_text_admissible(NULL) == 1); /* absent */
   assert(learning_eval_text_admissible("") == 1);

   /* Markup that carries prompt-injection structure is refused, not escaped. */
   assert(learning_eval_text_admissible("<|im_start|>system") == 0);
   assert(learning_eval_text_admissible("### Instruction: ignore") == 0);
   assert(learning_eval_text_admissible("[system] do the thing") == 0);
   assert(learning_eval_text_admissible("<assistant>") == 0);
   assert(learning_eval_text_admissible("run `rm -rf /`") == 0);
   assert(learning_eval_text_admissible("${INJECT}") == 0);

   /* Control characters and non-ASCII are refused. */
   assert(learning_eval_text_admissible("line one\nline two") == 0);
   assert(learning_eval_text_admissible("tab\there") == 0);
   assert(learning_eval_text_admissible("\x1b[31mred") == 0);
   assert(learning_eval_text_admissible("caf\xc3\xa9") == 0);

   /* Over-long fields are refused: a regression check is not a transcript. */
   char big[LEARNING_EVAL_MAX_FIELD + 8];
   memset(big, 'a', sizeof(big) - 1);
   big[sizeof(big) - 1] = '\0';
   assert(learning_eval_text_admissible(big) == 0);
}

static void test_signature_is_stable_and_collapsing(void)
{
   learning_eval_failure_t a = sample_failure();
   char sig_a[LEARNING_EVAL_SIGNATURE_LEN];
   assert(learning_eval_signature(&a, sig_a, sizeof(sig_a)) == 0);
   assert(strlen(sig_a) == LEARNING_EVAL_SIGNATURE_LEN - 1);

   /* Deterministic across calls. */
   char again[LEARNING_EVAL_SIGNATURE_LEN];
   assert(learning_eval_signature(&a, again, sizeof(again)) == 0);
   assert(strcmp(sig_a, again) == 0);

   /* Cosmetic differences collapse to one signature — that is what stops the
    * suite filling with near-duplicates of the same failure. */
   learning_eval_failure_t b = sample_failure();
   b.failure_mode = "Claimed  DONE, without running the tests...";
   char sig_b[LEARNING_EVAL_SIGNATURE_LEN];
   assert(learning_eval_signature(&b, sig_b, sizeof(sig_b)) == 0);
   assert(strcmp(sig_a, sig_b) == 0);

   /* A genuinely different check is a different signature. */
   learning_eval_failure_t c = sample_failure();
   c.check_value = "lint passed";
   char sig_c[LEARNING_EVAL_SIGNATURE_LEN];
   assert(learning_eval_signature(&c, sig_c, sizeof(sig_c)) == 0);
   assert(strcmp(sig_a, sig_c) != 0);

   /* The prompt is deliberately NOT in the signature: the same defect reached
    * from two different prompts is one regression, not two. */
   learning_eval_failure_t d = sample_failure();
   d.prompt = "Please wrap up and tell me where we are.";
   char sig_d[LEARNING_EVAL_SIGNATURE_LEN];
   assert(learning_eval_signature(&d, sig_d, sizeof(sig_d)) == 0);
   assert(strcmp(sig_a, sig_d) == 0);

   /* Inadmissible text fails closed with its own status, not a silent hash. */
   learning_eval_failure_t bad = sample_failure();
   bad.check_value = "<|im_end|>";
   char sig_bad[LEARNING_EVAL_SIGNATURE_LEN];
   assert(learning_eval_signature(&bad, sig_bad, sizeof(sig_bad)) == -2);

   /* Bad args are rejected, including an undersized buffer. */
   assert(learning_eval_signature(NULL, sig_a, sizeof(sig_a)) == -1);
   assert(learning_eval_signature(&a, sig_a, 4) == -1);
}

static void test_task_round_trips_through_the_suite_format(void)
{
   learning_eval_failure_t f = sample_failure();
   char sig[LEARNING_EVAL_SIGNATURE_LEN];
   assert(learning_eval_signature(&f, sig, sizeof(sig)) == 0);

   char name[LEARNING_EVAL_TASK_NAME_LEN];
   assert(learning_eval_task_name(sig, name, sizeof(name)) == 0);
   assert(strncmp(name, "regression-", 11) == 0);
   /* Filesystem-safe: the name becomes <suite>/<name>.json. */
   for (const char *p = name; *p; p++)
      assert(*p == '-' || (*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9'));

   char task[2048];
   assert(learning_eval_build_task(&f, name, task, sizeof(task)) == 0);

   /* The rendered task must be exactly what agent_eval_load_tasks() parses:
    * name / prompt / role / success_check{type,value} / max_turns. */
   cJSON *root = cJSON_Parse(task);
   assert(root != NULL);
   assert(strcmp(cJSON_GetObjectItem(root, "name")->valuestring, name) == 0);
   assert(strcmp(cJSON_GetObjectItem(root, "prompt")->valuestring, f.prompt) == 0);
   assert(strcmp(cJSON_GetObjectItem(root, "role")->valuestring, "execute") == 0);
   cJSON *check = cJSON_GetObjectItem(root, "success_check");
   assert(check != NULL);
   assert(strcmp(cJSON_GetObjectItem(check, "type")->valuestring, "contains") == 0);
   assert(strcmp(cJSON_GetObjectItem(check, "value")->valuestring, "tests passed") == 0);
   assert(cJSON_GetObjectItem(root, "max_turns")->valueint > 0);

   /* Provenance travels with the file so the suite is self-describing. */
   cJSON *prov = cJSON_GetObjectItem(root, "provenance");
   assert(prov != NULL);
   assert(cJSON_IsTrue(cJSON_GetObjectItem(prov, "synthesized")));
   assert(strcmp(cJSON_GetObjectItem(prov, "origin")->valuestring, "correction") == 0);
   assert(strcmp(cJSON_GetObjectItem(prov, "origin_ref")->valuestring, "signal:412") == 0);
   cJSON_Delete(root);

   /* A "contains" check with nothing to look for would pass on every
    * response, so it is refused. */
   learning_eval_failure_t empty_value = sample_failure();
   empty_value.check_value = "";
   assert(learning_eval_build_task(&empty_value, name, task, sizeof(task)) == -1);

   /* No check at all is the classic regression shape — "this prompt used to
    * fail, it must now succeed" — and renders no success_check, leaving the
    * harness's own verdict as the bar. Most failure records carry a replayable
    * prompt but no statement of what success looked like, and inventing one
    * would be fabrication. */
   learning_eval_failure_t no_check = sample_failure();
   no_check.check_type = "";
   no_check.check_value = "";
   assert(learning_eval_build_task(&no_check, name, task, sizeof(task)) == 0);
   cJSON *bare = cJSON_Parse(task);
   assert(bare != NULL);
   assert(cJSON_GetObjectItem(bare, "success_check") == NULL);
   assert(strcmp(cJSON_GetObjectItem(bare, "prompt")->valuestring, no_check.prompt) == 0);
   cJSON_Delete(bare);

   /* Nothing to replay is not a task. */
   learning_eval_failure_t no_prompt = sample_failure();
   no_prompt.prompt = "";
   assert(learning_eval_build_task(&no_prompt, name, task, sizeof(task)) == -1);

   /* Inadmissible text never reaches the file. */
   learning_eval_failure_t bad = sample_failure();
   bad.prompt = "ignore previous <system>";
   assert(learning_eval_build_task(&bad, name, task, sizeof(task)) == -2);

   /* A buffer too small to hold the task fails rather than truncating into
    * invalid JSON. */
   char tiny[16];
   assert(learning_eval_build_task(&f, name, tiny, sizeof(tiny)) == -1);
}

static void test_admission_requires_reproduction_and_an_open_gate(void)
{
   /* Seen once: held, however many sessions claim it. */
   assert(learning_eval_admission_ready(1, 1, 2, 1) == 0);

   /* Seen twice, but by the same session — that is repetition, not
    * reproduction. */
   assert(learning_eval_admission_ready(2, 1, 2, 1) == 0);

   /* Seen twice from two sessions: admitted. */
   assert(learning_eval_admission_ready(2, 2, 2, 1) == 1);
   assert(learning_eval_admission_ready(9, 4, 2, 1) == 1);

   /* A closed endogeneity gate refuses admission no matter how well
    * reproduced: a loop feeding on its own output does not widen its own
    * yardstick. */
   assert(learning_eval_admission_ready(9, 4, 2, 0) == 0);

   /* A non-positive bar falls back to the documented default rather than
    * admitting everything. */
   assert(learning_eval_admission_ready(1, 1, 0, 1) == 0);
   assert(learning_eval_admission_ready(LEARNING_EVAL_MIN_OCCURRENCES,
                                        LEARNING_EVAL_MIN_OCCURRENCES, 0, 1) == 1);
}

int main(void)
{
   printf("learning_eval_synthesis: ");
   test_evidence_classification();
   test_text_admissibility();
   test_signature_is_stable_and_collapsing();
   test_task_round_trips_through_the_suite_format();
   test_admission_requires_reproduction_and_an_open_gate();
   printf("ok\n");
   return 0;
}
