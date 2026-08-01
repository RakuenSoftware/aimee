/* roundtable_provider.c -- adapt optional roundtable execution to core.
 *
 * The panel-provider boundary (#1845) declares provider-neutral result types in
 * aimee/ir/panel_result.h; the roundtable module produces its own. They are
 * field-for-field parallel by design but DELIBERATELY distinct: IR owns the
 * neutral layout, the roundtable module owns its internals, and neither should
 * be able to change the other by accident. This file is the translation, and it
 * is the only place that knows both.
 *
 * It shipped with the boundary and was never finished -- the vtable's signatures
 * passed the roundtable's own types straight through, which does not compile,
 * and no makefile referenced the file so nothing said so. The mapping below is
 * exhaustive on the IR side: every aimee_panel_result_t field is assigned.
 * Roundtable-only fields (required-failed counts, tool-call tallies, the
 * evidence-coverage flag) have no IR counterpart and are dropped here rather
 * than widening the neutral type to match one provider. */
#include <aimee/delegates/panel_provider.h>

#include "delegate_ensemble.h"
#include "log.h"
#include "roundtable_activation.h"

#include <stdio.h>
#include <string.h>

/* aimee_panel_aggregate_result_t and delegate_ensemble_result_t are the same
 * seven fields. Copied field-wise anyway: a struct assignment would compile only
 * while they stay identical, and would break silently the first time one of them
 * gains a field. */
static void aggregate_result_to_ir(const delegate_ensemble_result_t *in,
                                   aimee_panel_aggregate_result_t *out)
{
   memset(out, 0, sizeof(*out));
   snprintf(out->response, sizeof(out->response), "%s", in->response);
   out->success = in->success;
   out->cost_usd = in->cost_usd;
   out->degraded = in->degraded;
   out->cost_capped = in->cost_capped;
   out->participants_total = in->participants_total;
   out->participants_failed = in->participants_failed;
}

static aimee_review_evidence_kind_t evidence_kind_to_ir(ev_kind_t k)
{
   switch (k)
   {
   case EV_SYMBOL:
      return AIMEE_REVIEW_EVIDENCE_SYMBOL;
   case EV_REFS:
      return AIMEE_REVIEW_EVIDENCE_REFS;
   case EV_SEARCH:
      return AIMEE_REVIEW_EVIDENCE_SEARCH;
   case EV_NONE:
      break;
   }
   return AIMEE_REVIEW_EVIDENCE_NONE;
}

static void review_item_to_ir(const roundtable_review_item_t *in, aimee_panel_review_item_t *out)
{
   memset(out, 0, sizeof(*out));
   snprintf(out->severity, sizeof(out->severity), "%s", in->severity);
   snprintf(out->category, sizeof(out->category), "%s", in->category);
   snprintf(out->location, sizeof(out->location), "%s", in->location);
   snprintf(out->summary, sizeof(out->summary), "%s", in->summary);
   snprintf(out->recommendation, sizeof(out->recommendation), "%s", in->recommendation);
   snprintf(out->identity_key, sizeof(out->identity_key), "%s", in->identity_key);
   snprintf(out->sources, sizeof(out->sources), "%s", in->sources);
   out->count = in->count;
   out->tool_grounded = in->tool_grounded;
   out->evidence.kind = evidence_kind_to_ir(in->evidence.kind);
   snprintf(out->evidence.target, sizeof(out->evidence.target), "%s", in->evidence.target);
   snprintf(out->evidence.project, sizeof(out->evidence.project), "%s", in->evidence.project);
   out->evidence.count = in->evidence.count;
   snprintf(out->evidence.idkey, sizeof(out->evidence.idkey), "%s", in->evidence.idkey);
   out->evidence.factual = in->evidence.factual;
}

/* Bound every array copy by BOTH capacities. The two modules size their item and
 * question arrays independently, so a roundtable that raises its cap must not be
 * able to overrun the IR struct. */
static int min_int(int a, int b)
{
   return a < b ? a : b;
}

static void panel_result_to_ir(const roundtable_result_t *in, aimee_panel_result_t *out)
{
   memset(out, 0, sizeof(*out));

   /* The one heap allocation. Ownership moves to the caller, who returns it
    * through provider_release below. */
   out->artifact = in->artifact;

   out->rounds_run = in->rounds_run;
   out->converged = in->converged;
   out->degraded = in->degraded;
   out->truncated = in->truncated;
   out->cost_capped = in->cost_capped;
   out->deadline_hit = in->deadline_hit;
   out->cancelled = in->cancelled;
   out->best_round = in->best_round;
   out->participants_total = in->participants_total;
   out->participants_failed = in->participants_failed;
   out->cost_usd = in->cost_usd;

   snprintf(out->original_request_alignment, sizeof(out->original_request_alignment), "%s",
            in->original_request_alignment);
   snprintf(out->original_request_alignment_summary,
            sizeof(out->original_request_alignment_summary), "%s",
            in->original_request_alignment_summary);
   snprintf(out->original_request_alignment_sources,
            sizeof(out->original_request_alignment_sources), "%s",
            in->original_request_alignment_sources);

   out->item_count = min_int(in->item_count, AIMEE_PANEL_MAX_REVIEW_ITEMS);
   for (int i = 0; i < out->item_count; i++)
      review_item_to_ir(&in->items[i], &out->items[i]);
   out->items_round = in->items_round;
   out->artifact_round = in->artifact_round;

   out->answered_question_count = min_int(in->answered_question_count, AIMEE_PANEL_MAX_QUESTIONS);
   for (int i = 0; i < out->answered_question_count; i++)
   {
      snprintf(out->answered_questions[i].question, sizeof(out->answered_questions[i].question),
               "%s", in->answered_questions[i].question);
      snprintf(out->answered_questions[i].answer, sizeof(out->answered_questions[i].answer), "%s",
               in->answered_questions[i].answer);
      snprintf(out->answered_questions[i].evidence, sizeof(out->answered_questions[i].evidence),
               "%s", in->answered_questions[i].evidence);
      out->answered_questions[i].answered = in->answered_questions[i].answered;
   }

   out->coverage_gap_count = min_int(in->coverage_gap_count, AIMEE_PANEL_MAX_QUESTIONS);
   for (int i = 0; i < out->coverage_gap_count; i++)
      snprintf(out->coverage_gaps[i], sizeof(out->coverage_gaps[i]), "%s", in->coverage_gaps[i]);

   out->rejected_count = min_int(in->rejected_count, AIMEE_PANEL_MAX_REVIEW_ITEMS);
   for (int i = 0; i < out->rejected_count; i++)
   {
      review_item_to_ir(&in->rejected[i], &out->rejected[i]);
      snprintf(out->rejected_reason[i], sizeof(out->rejected_reason[i]), "%s",
               in->rejected_reason[i]);
   }
   out->verified_count = in->verified_count;
   out->degraded_count = in->degraded_count;
   out->capped_count = in->capped_count;
}

static void options_from_ir(const aimee_panel_options_t *in, roundtable_opts_t *out)
{
   memset(out, 0, sizeof(*out));
   if (!in)
      return;
   out->mode = (roundtable_mode_t)in->mode;
   out->turns = (roundtable_turns_t)in->turns;
   out->max_rounds = in->max_rounds;
   out->converge_threshold = in->converge_threshold;
   out->deadline_ms = in->deadline_ms;
   out->apply_review = in->apply_review;
   out->brief = in->brief;
   out->brief_truncated = in->brief_truncated;
   out->context = in->context;
   out->questions = in->questions;
   out->question_count = in->question_count;
   out->cancel_requested = in->cancel_requested;
   out->cancel_ctx = in->cancel_ctx;
   out->parent_session_id = in->parent_session_id;
   /* required_participants has no IR counterpart. Left 0, which means "every
    * participant is required" -- the legacy default and the fail-closed reading. */
}

static int provider_aggregate(agent_config_t *agents, const ensemble_panel_t *panel,
                              const char *prompt, aimee_panel_aggregate_result_t *out)
{
   if (!out)
      return AIMEE_PANEL_PROVIDER_INVALID;
   delegate_ensemble_result_t raw;
   memset(&raw, 0, sizeof(raw));
   if (delegate_ensemble_run(agents, panel, prompt, &raw) != 0)
      return AIMEE_PANEL_PROVIDER_ERROR;
   aggregate_result_to_ir(&raw, out);
   return AIMEE_PANEL_PROVIDER_OK;
}

static int provider_run(agent_config_t *agents, const ensemble_panel_t *panel, const char *task,
                        const aimee_panel_options_t *options, aimee_panel_result_t *out)
{
   if (!out)
      return AIMEE_PANEL_PROVIDER_INVALID;
   roundtable_opts_t opts;
   options_from_ir(options, &opts);
   roundtable_result_t raw;
   memset(&raw, 0, sizeof(raw));
   if (delegate_roundtable_run(agents, panel, task, &opts, &raw) != 0)
   {
      delegate_roundtable_result_free(&raw);
      return AIMEE_PANEL_PROVIDER_ERROR;
   }
   /* Moves ownership of raw.artifact into *out, so raw is deliberately NOT freed
    * on the success path -- provider_release is what eventually frees it. */
   panel_result_to_ir(&raw, out);
   return AIMEE_PANEL_PROVIDER_OK;
}

static void provider_release(aimee_panel_result_t *result)
{
   if (!result)
      return;
   /* Only the artifact is heap-allocated; every other field in either struct is
    * by-value. Return it through the roundtable's own free so the allocator
    * stays that module's business rather than being assumed here. */
   roundtable_result_t raw;
   memset(&raw, 0, sizeof(raw));
   raw.artifact = result->artifact;
   delegate_roundtable_result_free(&raw);
   result->artifact = NULL;
}

static const aimee_panel_provider_t provider = {
    .aggregate = provider_aggregate,
    .run = provider_run,
    .release = provider_release,
};

int roundtable_provider_configure(void)
{
   roundtable_runtime_configure();
   int enabled = roundtable_module_enabled();
   if (!enabled)
   {
      if (aimee_panel_provider_available())
         (void)aimee_panel_provider_unregister(&provider);
      return 0;
   }
   int rc = aimee_panel_provider_register(&provider);
   if (rc != AIMEE_PANEL_PROVIDER_OK)
   {
      aimee_log(LOG_ERROR, "roundtable", "could not register panel provider (%d)", rc);
      return -1;
   }
   return 1;
}
