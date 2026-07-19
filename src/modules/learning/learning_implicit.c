/* learning_implicit.c: implicit-signal detectors for the learning router.
 * Each detector is gated by a per-heuristic default-off config flag.
 * Detected signals are recorded via learning_router_record_signal() which
 * handles corroboration and rate-limiting; the cross-source-learning
 * substrate consumes the resulting evidence rows.
 *
 * See docs/proposals/pending/learning-signals-router-phase-2-fixtures.md. */
#include "aimee.h"
#include "config.h"
#include "dogfood.h"
#include "learning.h"
#include "learning_implicit.h"
#include <string.h>
#include <time.h>

static void emit_implicit(const char *signal_type, const char *polarity, const char *title,
                          const char *description, const char *target_key, int64_t target_memory_id,
                          const char *workflow_project, const char *workflow_signal_type)
{
   learning_signal_input_t input;
   memset(&input, 0, sizeof(input));
   snprintf(input.signal_type, sizeof(input.signal_type), "%s", signal_type);
   snprintf(input.source, sizeof(input.source), "%s", "implicit");
   snprintf(input.polarity, sizeof(input.polarity), "%s", polarity);
   if (title)
      snprintf(input.title, sizeof(input.title), "%s", title);
   if (description)
      snprintf(input.description, sizeof(input.description), "%s", description);
   if (target_key)
      snprintf(input.target_key, sizeof(input.target_key), "%s", target_key);
   input.target_memory_id = target_memory_id;
   if (workflow_project)
      snprintf(input.workflow_project, sizeof(input.workflow_project), "%s", workflow_project);
   if (workflow_signal_type)
      snprintf(input.workflow_signal_type, sizeof(input.workflow_signal_type), "%s",
               workflow_signal_type);
   input.evidence_refs_json = "[]";
   learning_router_record_signal(&input, NULL);
}

void learning_implicit_detect_turn(const char *user_text)
{
   if (!user_text || !user_text[0])
      return;
   config_t cfg;
   if (config_load(&cfg) != 0)
      return;
   if (!cfg.learning_implicit_citation_repair && !cfg.learning_implicit_citation_continuation)
      return;

   struct timespec t0, t1;
   clock_gettime(CLOCK_MONOTONIC, &t0);

   dogfood_autolabel_kind_t kind = dogfood_classify_next_turn(user_text);
   if (kind == DOGFOOD_AUTOLABEL_REPAIR && cfg.learning_implicit_citation_repair)
      emit_implicit("citation_then_repair", "negative", "Implicit citation_then_repair signal",
                    "User turn classified as repair/correction after memory retrieval.", NULL, 0,
                    NULL, NULL);
   else if (kind == DOGFOOD_AUTOLABEL_CONTINUATION && cfg.learning_implicit_citation_continuation)
      emit_implicit(
          "citation_then_continuation", "positive", "Implicit citation_then_continuation signal",
          "User turn classified as continuation after memory retrieval.", NULL, 0, NULL, NULL);

   clock_gettime(CLOCK_MONOTONIC, &t1);
   double ms = (t1.tv_sec - t0.tv_sec) * 1000.0 + (t1.tv_nsec - t0.tv_nsec) / 1.0e6;
   learning_router_record_detection_ms(ms);
}

void learning_implicit_record_repeat_question(const char *session_id, const char *tool,
                                              const char *query_hash)
{
   if (!session_id || !tool || !query_hash || !query_hash[0])
      return;
   config_t full;
   if (config_load(&full) != 0 || !full.learning_implicit_repeat_question)
      return;

   struct timespec t0, t1;
   clock_gettime(CLOCK_MONOTONIC, &t0);

   dogfood_config_t dcfg;
   dogfood_config_from(&full, &dcfg);
   if (dogfood_query_is_repeat(&dcfg, session_id, tool, query_hash))
      emit_implicit("repeat_question", "negative", "Implicit repeat_question signal",
                    "Same (session, tool, query_hash) already appeared this month.", tool, 0, NULL,
                    NULL);

   clock_gettime(CLOCK_MONOTONIC, &t1);
   double ms = (t1.tv_sec - t0.tv_sec) * 1000.0 + (t1.tv_nsec - t0.tv_nsec) / 1.0e6;
   learning_router_record_detection_ms(ms);
}

void learning_implicit_record_correction(const char *target_key, int64_t target_memory_id)
{
   config_t cfg;
   if (config_load(&cfg) != 0 || !cfg.learning_implicit_repeated_correction)
      return;

   struct timespec t0, t1;
   clock_gettime(CLOCK_MONOTONIC, &t0);

   emit_implicit("repeated_correction", "negative", "Implicit repeated_correction signal",
                 "Correction signal recorded; cross-source substrate tracks recurrence.",
                 target_key, target_memory_id, NULL, NULL);

   clock_gettime(CLOCK_MONOTONIC, &t1);
   double ms = (t1.tv_sec - t0.tv_sec) * 1000.0 + (t1.tv_nsec - t0.tv_nsec) / 1.0e6;
   learning_router_record_detection_ms(ms);
}

void learning_implicit_record_workflow(const char *workspace, const char *signal_type,
                                       const char *description)
{
   if (!workspace || !workspace[0] || !signal_type || !signal_type[0])
      return;
   config_t cfg;
   if (config_load(&cfg) != 0 || !cfg.learning_implicit_workflow_repetition)
      return;

   struct timespec t0, t1;
   clock_gettime(CLOCK_MONOTONIC, &t0);

   emit_implicit("workflow_repetition", "positive", "Implicit workflow_repetition signal",
                 description ? description : "", workspace, 0, workspace, signal_type);

   clock_gettime(CLOCK_MONOTONIC, &t1);
   double ms = (t1.tv_sec - t0.tv_sec) * 1000.0 + (t1.tv_nsec - t0.tv_nsec) / 1.0e6;
   learning_router_record_detection_ms(ms);
}
