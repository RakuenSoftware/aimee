/* roundtable_types.h: the plain-data result and options types produced by an
 * ensemble roundtable run and consumed by the roundtable module (chair, verify,
 * pipeline). These were previously defined inside delegate_ensemble.h, which
 * forced the roundtable module's headers to include a delegates header purely for
 * these types — a roundtable -> delegates header edge that, combined with
 * delegates including the roundtable headers, formed a header cycle between the
 * two modules. Owning them here (the roundtable module) breaks that cycle: both
 * delegate_ensemble.h and the roundtable headers now include this leaf header, so
 * the only cross-module edge is delegates -> roundtable.
 *
 * These are all plain data (fixed char arrays / ints; no pointers into other
 * modules), so this header depends on nothing.
 *
 * LEAF HEADER: it must not #include any other project header. Keeping it
 * dependency-free is exactly what keeps the delegates<->roundtable cycle broken;
 * adding a project include here risks re-creating it. */
#ifndef AIMEE_ROUNDTABLE_TYPES_H
#define AIMEE_ROUNDTABLE_TYPES_H 1

#define ROUNDTABLE_MAX_REVIEW_ITEMS 128
#define ROUNDTABLE_MAX_QUESTIONS    16

typedef enum
{
   ROUNDTABLE_DRAFT = 0,
   ROUNDTABLE_REVIEW = 1
} roundtable_mode_t;

typedef enum
{
   ROUNDTABLE_PARALLEL = 0,
   ROUNDTABLE_SEQUENTIAL = 1
} roundtable_turns_t;

typedef struct
{
   roundtable_mode_t mode;
   roundtable_turns_t turns;
   int max_rounds;
   int converge_threshold;
   int deadline_ms;
   int apply_review;
   const char *brief;
   int brief_truncated;
   /* Optional pre-assembled read-only context (aimee memory recall + code-graph
    * snippets) injected into every panelist prompt — a useful SEED, no longer the
    * only window: REVIEW-mode panelists now run with aimee's index-only toolset
    * (`review_indexed`) and can look things up themselves. Drafting panelists are
    * still tool-less, so for them this remains the only view of memory and the code
    * graph. The caller owns the string for the duration of the run; NULL = none. */
   const char *context;
   const char **questions;
   int question_count;
   int (*cancel_requested)(void *ctx);
   void *cancel_ctx;
   /* Originating session: panel + aggregator runs fold their cost onto it via
    * db1_cost_fold_record, so the ensemble is accounted like a delegate. */
   const char *parent_session_id;
} roundtable_opts_t;

/* Replay-verification evidence (Part A of the replayable-verification proposal).
 * A panelist attaches a STRUCTURED query — never a free-form command — so a fresh
 * verifier can replay it deterministically over the read-only code index
 * (index_find / index_find_callers / index_code_search) and re-ground the claim.
 * Plain (no pointers): memset-zeroing and struct copy stay valid. */
typedef enum
{
   EV_NONE = 0, /* no replayable evidence -> item is interpretive (caps at concern) */
   EV_SYMBOL,   /* does symbol `target` exist? -> index_find */
   EV_REFS,     /* how many call sites of `target`? -> index_find_callers (the workhorse) */
   EV_SEARCH    /* lexical code search for `target` -> index_code_search */
} ev_kind_t;

typedef struct
{
   ev_kind_t kind;
   char target[256];  /* symbol (EV_SYMBOL/EV_REFS) or search query (EV_SEARCH) */
   char project[128]; /* index project scope ("" = all indexed projects) */
   int count;         /* the count the panelist claims (EV_REFS/EV_SEARCH) */
   char idkey[65];    /* sha256-hex[:64] of sorted "file:line", or "" if unset */
   int factual;       /* 1 = replayable claim; 0 = interpretive (caps at concern) */
} review_evidence_t;

typedef struct
{
   char severity[16];
   char category[32];
   char location[128];
   char summary[256];
   char recommendation[256];
   char identity_key[128];
   char sources[256];
   int count;
   review_evidence_t evidence; /* Part A: structured replay evidence (zeroed = EV_NONE) */
} roundtable_review_item_t;

typedef struct
{
   char question[512];
   char answer[1024];
   char evidence[512];
   int answered;
} roundtable_answered_question_t;

typedef struct
{
   char *artifact;
   int rounds_run;
   int converged;
   int degraded;
   int truncated;
   int cost_capped;
   int deadline_hit;
   int cancelled;
   int best_round;
   int participants_total;  /* reference models per round (panel size) */
   int participants_failed; /* participants that returned no usable response in the final round run
                             */
   double cost_usd;
   roundtable_review_item_t items[ROUNDTABLE_MAX_REVIEW_ITEMS];
   int item_count;
   int items_round;
   int artifact_round;
   roundtable_answered_question_t answered_questions[ROUNDTABLE_MAX_QUESTIONS];
   int answered_question_count;
   char coverage_gaps[ROUNDTABLE_MAX_QUESTIONS][512];
   int coverage_gap_count;
   /* Replay verification (Part A). Items whose structured evidence did not
    * reproduce against the code index are moved here (not silently dropped), with
    * a parallel reason code. Fixed arrays (no heap; no change to result_free). */
   roundtable_review_item_t rejected[ROUNDTABLE_MAX_REVIEW_ITEMS];
   char rejected_reason[ROUNDTABLE_MAX_REVIEW_ITEMS][24];
   int rejected_count;
   int verified_count; /* kept items whose factual evidence reproduced */
   int degraded_count; /* kept but unverified (index unavailable) */
   int capped_count;   /* interpretive items capped at suggestion */
} roundtable_result_t;

#endif /* AIMEE_ROUNDTABLE_TYPES_H */
