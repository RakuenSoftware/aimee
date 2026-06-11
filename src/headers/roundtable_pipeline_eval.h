/* roundtable_pipeline_eval.h: pure decision logic for the roundtable authoring
 * pipeline outer loop (proposal section 3). No DB or JSON dependency — operates
 * on a captured terminal envelope so it is trivially unit-testable.
 *
 * The single canonical validity predicate lives here (#41/#44); every
 * eligibility consumer (done-bar, chunk-aggregate, gate digest) must call
 * roundtable_terminal_envelope_valid() rather than re-list flags. */
#ifndef DEC_ROUNDTABLE_PIPELINE_EVAL_H
#define DEC_ROUNDTABLE_PIPELINE_EVAL_H 1

#ifdef __cplusplus
extern "C"
{
#endif

/* ROUNDTABLE_MAX_QUESTIONS in delegate_ensemble.h is 16; the questions-answered
 * done-bar refuses a brief with more accepted questions than this (#14). */
#define RTP_MAX_BRIEF_QUESTIONS 16

   /* The captured terminal envelope (#44): the durable record of a roundtable
    * child run's terminal state, built from the parsed result plus
    * capture/serialization flags. Validity is evaluated over THIS, not raw
    * engine fields. */
   typedef struct
   {
      int present;     /* a terminal result was captured at all */
      int parse_ok;    /* the result JSON parsed */
      int has_error;   /* the result carried an "error" field */
      int lost_result; /* capture genuinely failed (run evicted / no payload) */
      int is_draft;    /* draft-mode pass (artifact, no items) vs review */
      int artifact_present;
      /* engine terminal flags */
      int converged;
      int degraded;
      int truncated;
      int items_truncated; /* HTTP serializer only sent a prefix of items (#38) */
      int cost_capped;
      int deadline_hit;
      int cancelled;
      /* review counts */
      int item_count;
      int blocking_count;
      int suggestion_count;
      int nit_count;
      int answered_count;
      int coverage_gap_count;
      int items_round;
      int artifact_round;
      int best_round;
      int rounds_run;
      double cost_usd;
      int cost_known;
   } rtp_envelope_t;

   /* The ONE canonical predicate. Returns 1 iff the envelope is valid evidence
    * for a done-bar decision. False on any of: not captured, parse failure,
    * error, truncated, items_truncated, degraded, cost_capped, deadline_hit,
    * cancelled, lost_result, or an items/artifact provenance mismatch. */
   int roundtable_terminal_envelope_valid(const rtp_envelope_t *e);

   /* Done-bar evaluation (proposal section 3). */
   typedef enum
   {
      RTP_DONEBAR_MET = 0,     /* leave the review loop, advance to the gate */
      RTP_DONEBAR_BLOCKED = 1, /* not done; revise and re-review */
      RTP_DONEBAR_INVALID = 2  /* envelope invalid, or brief/config unusable */
   } rtp_donebar_result_t;

   /* done_bar: one of the RTP_DONEBAR_* strings (roundtable_pipeline.h).
    * accepted_question_count is used only by the questions-answered bar. */
   rtp_donebar_result_t rtp_donebar_eval(const char *done_bar, const rtp_envelope_t *e,
                                         int accepted_question_count);

   /* Chunked-review aggregate (#28): the phase can pass only when every chunk
    * has a valid completed result AND the whole-artifact synthesis ran. */
   int rtp_chunk_aggregate_done(int chunk_total, int chunk_done, int synthesis_done,
                                int any_chunk_invalid);

   /* Outer-loop next action (proposal section 3). */
   typedef enum
   {
      RTP_ACT_PASS = 0,    /* done-bar met -> surface gate / advance */
      RTP_ACT_REVISE = 1,  /* blocking items remain -> revise + re-review */
      RTP_ACT_RETRY = 2,   /* infra fault -> re-run same pass id, new attempt */
      RTP_ACT_ESCALATE = 3 /* ceiling/cost/invalid-evidence -> escalate to human */
   } rtp_action_t;

   typedef struct
   {
      const char *done_bar;
      int max_passes;            /* 0 = unbounded (default) */
      int max_attempts_per_pass; /* >= 1 */
      double max_phase_cost_usd; /* per-phase cap; 0 = unbounded */
      double max_total_cost_usd; /* whole-pipeline cap; 0 = unbounded (#46) */
   } rtp_loop_cfg_t;

   typedef struct
   {
      int pass_no;           /* current outer pass, 1-based */
      int attempt_no;        /* current attempt within the pass, 1-based */
      double phase_cost_usd; /* cumulative phase spend so far */
      double total_cost_usd; /* cumulative whole-pipeline spend so far (#46) */
      int accepted_question_count;
   } rtp_loop_state_t;

   /* Decide the next outer-loop action from the captured envelope + config.
    * A cost backstop never auto-passes a not-done artifact (proposal section 3). */
   rtp_action_t rtp_loop_decide(const rtp_loop_cfg_t *cfg, const rtp_loop_state_t *st,
                                const rtp_envelope_t *e);

   /* Gate-resolution authority (#53/#54). The driving agent holds CAP_DELEGATE
    * but must NOT be able to pass its own gate, because passing triggers the
    * merge. v1 uses an operator principal (no spare capability bit): resolution
    * is allowed iff a local operator is enrolled+active and the caller presents
    * the matching principal. Returns 1 iff allowed. */
   int rtp_gate_authority_ok(const char *provided_principal, const char *active_operator_id,
                             int operator_active);

   /* Panel diversity + context-budget resolution (proposal section 7, #36). A
    * resolved participant carries its provider identity and context window; the
    * summary reports distinct providers (diversity) and the minimum budget used
    * to size review chunks. */
   typedef struct
   {
      const char *provider; /* resolved provider id; NULL/"" if unresolved */
      int context_tokens;   /* resolved context window; 0 if unknown */
   } rtp_participant_t;

   typedef struct
   {
      int requested;
      int resolved;
      int distinct_providers;
      int min_context_tokens; /* smallest resolved budget, or fallback applied */
      int used_fallback;      /* an unknown budget was replaced by the fallback */
      int unknown_unresolved; /* a participant name did not resolve to an agent */
   } rtp_panel_t;

   /* Summarize a resolved panel. fallback_tokens is the conservative budget used
    * when a participant's context window is unknown; if <= 0, an unknown budget
    * is a hard validation error and this returns -1 (#36). Returns 0 otherwise. */
   int rtp_panel_summarize(const rtp_participant_t *parts, int n, int fallback_tokens,
                           rtp_panel_t *out);

   /* A panel is diverse iff >= 2 distinct resolved providers converged on it; a
    * single-provider panel can agree on its own blind spots (section 7). */
   int rtp_panel_diverse(const rtp_panel_t *p);

#ifdef __cplusplus
}
#endif

#endif /* DEC_ROUNDTABLE_PIPELINE_EVAL_H */
