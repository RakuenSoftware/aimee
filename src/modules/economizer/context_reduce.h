/* context_reduce.h: the unified context economizer — ONE provider-agnostic
 * reduction subsystem invoked at every request-assembly choke point (the
 * inbound /v1 gateway AND the delegate turn loop), so it runs for every agent
 * (primary + delegates) and every provider.
 *
 * It is a THIN ORCHESTRATOR over independent, individually-gated levers — it does
 * not reimplement them:
 *   - inject-compress  (ingress_preinject_*: KB/code envelope, code -> file:line)
 *   - history-fold     (context_fold_view: rolling skeleton + Coordinate Closet)
 *   - dedup/compact    (messages_compact_consecutive)
 *   - freeze           (byte-identical reduced prefix for provider cache warmth)
 * plus a measurement ledger that records what each request would save.
 *
 * DESIGN CONTRACTS (from the design-gate review — see the plan):
 *  1. Idempotence / provenance. A single logical request can traverse BOTH seams
 *     (gateway then delegate loop). `context_reduce` stamps reduce_state_t.reduced
 *     once a seam mutates; a later seam that sees it set MUST NOT re-reduce (it
 *     re-measures against what it actually received). The 400-fallback rebuilds
 *     from the pristine original transcript, never from a prior reduced output.
 *  2. Per-format structural boundaries. Fold boundary selection parses each
 *     provider's native tool-call/tool-result structure (NOT detect_format(),
 *     which is a router heuristic). Invariant: no assistant tool_call/tool_use is
 *     ever separated from its result across a fold boundary.
 *  3. Immutable prefix zone. The system prompt and any designated safety turns
 *     are byte-identical post-reduction; no lossy lever touches them.
 *  4. Fixed lever order, freeze-first. freeze digests the stable ORIGINAL bytes
 *     BEFORE inject-compress mutates them, so a turn-varying envelope can never
 *     silently bust the prompt cache. Order: freeze-capture -> dedup -> fold ->
 *     inject-compress -> freeze-verify.
 *  5. Net-gain pre-check + freeze cost guardrail. Skip fold when foldable tokens
 *     are below the round-trip recovery threshold (ledger reason="skip_no_gain");
 *     disable freeze when the estimated cache-write PREMIUM is not recovered by the
 *     cache-read savings over the reuse horizon (Slice 5 —
 *     reduce_freeze_cost_favorable; freeze_guard config, default-on but inert until
 *     the default-off freeze is enabled).
 *  6. Live-traffic safety. Per-lever + per-seam gates, a measure-only shadow mode,
 *     and a caller-side hard bypass: on ANY internal error context_reduce returns
 *     non-zero with out->messages == NULL so the caller forwards the ORIGINAL
 *     request unchanged.
 *
 * All levers default OFF. Measurement is itself a gate (measure_only) so baselines
 * can be collected with zero behavior change. */
#ifndef DEC_CONTEXT_REDUCE_H
#define DEC_CONTEXT_REDUCE_H 1

#include "context_fold.h" /* fold_config_t, fold_freeze_t (reused, not duplicated) */
#include <cJSON.h>

#ifdef __cplusplus
extern "C"
{
#endif

   /* Which seam is invoking the reducer (for provenance + per-seam gating). */
   typedef enum
   {
      REDUCE_SEAM_DELEGATE = 0, /* the agent turn loop (provider-native messages) */
      REDUCE_SEAM_GATEWAY = 1,  /* the inbound /v1 proxy (client wire-format) */
   } reduce_seam_t;

   typedef struct
   {
      /* Per-lever gates (all default-off). */
      int inject_compress; /* KB/code envelope + code->file:line compression */
      int history_fold;    /* rolling history skeleton + Coordinate Closet */
      int compress;        /* boundary-free tool-result BODY compression (Slice 4) */
      int dedup;           /* merge consecutive same-role string turns */
      int freeze;          /* byte-identical reduced prefix across turns */

      /* Per-seam enable (default-off): a lever only runs at a seam that is on. */
      int gateway_seam;
      int delegate_seam;

      /* Shadow mode: compute the ledger but DO NOT mutate the messages. Lets the
       * gateway/delegate path collect baselines on live traffic with zero risk. */
      int measure_only;

      /* Net-gain pre-check: skip fold when foldable tokens < this (0 -> default).
       * Guards against routine net-negative folds once recovery round-trips and
       * the existing compact.c/tool-output caps are accounted for. */
      int min_gain_tokens;

      /* Freeze cost guardrail (Slice 5). The freeze pins the fold boundary so the
       * reduced prefix stays byte-identical (cache-warm) turn-to-turn — but a
       * boundary that keeps advancing forces provider cache-WRITES (1.25-2x input
       * rate) that can flip reduction net-negative. When enabled, freeze is pinned
       * only when the estimated cache-read savings over `horizon` reuses cover the
       * one-time cache-write cost; otherwise the turn re-derives without pinning.
       * Default-on, but only acts when the (default-off) economizer freeze is live. */
      int freeze_guard_enabled;
      int freeze_guard_horizon; /* expected reuse turns for break-even (0 -> 1) */

      fold_config_t fold; /* history-fold sub-config (reused verbatim) */
   } reduce_config_t;

/* Hard cap on the configured freeze-guard horizon, so a misconfigured long horizon
 * cannot justify an unbounded cache-write bet on a single run. */
#define FREEZE_GUARD_MAX_HORIZON 5

/* Fixed freeze-guard horizon (reuse turns for the break-even estimate). Formerly the
 * reduce.freeze_guard_horizon knob (default 1); the tier model bakes it as a constant. */
#define ECON_FREEZE_GUARD_HORIZON 1

/* How long a circuit-broken /v1 session stays disabled by the aggressive-tier gateway
 * mutator (1h). Formerly reduce.gateway_session_disable_ttl_ms; now a fixed constant. */
#define ECON_GATEWAY_SESSION_DISABLE_TTL_MS 3600000

   /* Per-CONVERSATION reducer state, owned by the caller and persisted across
    * turns within one conversation (e.g. a stack local spanning the turn loop).
    * NOT shared across agents/sessions (cross-session sharing would leak context).
    * Zero-init before first use. */
   typedef struct
   {
      fold_freeze_t freeze; /* §3 freeze boundary + 64-bit prefix digest */
      int reduced;          /* provenance: set once a seam has reduced this request
                             * set -> a second seam re-measures but does NOT re-reduce */
      int turn;             /* current turn index (freeze residency + ledger) */
   } reduce_state_t;

   /* Why a request did/didn't reduce — recorded in the ledger for auditability. */
   typedef enum
   {
      REDUCE_REASON_NONE = 0,     /* no lever enabled at this seam */
      REDUCE_REASON_REDUCED,      /* reduction applied */
      REDUCE_REASON_MEASURED,     /* measure_only: metrics computed, not mutated */
      REDUCE_REASON_SKIP_NO_GAIN, /* foldable tokens below min_gain_tokens */
      REDUCE_REASON_ALREADY,      /* provenance: a prior seam already reduced */
   } reduce_reason_t;

   /* Internal-error classification for a hard-bypass return (rc != 0). Default NONE
    * on the success paths. The reducer currently distinguishes only what its sub-
    * transforms report — an unsupported message shape is handled as a no-op (rc==0),
    * and a compress/fold sub-transform failure is reported as INTERNAL_ASSERTION;
    * ALLOC_FAILED / PARSE_FAILED are reserved for finer future classification. The
    * gateway maps this 1:1 to gateway_hard_bypass{reason} (§2.2). */
   typedef enum
   {
      REDUCE_ERR_NONE = 0,
      REDUCE_ERR_ALLOC_FAILED,
      REDUCE_ERR_PARSE_FAILED,
      REDUCE_ERR_INTERNAL_ASSERTION,
      REDUCE_ERR_FORMAT_UNSUPPORTED,
   } reduce_error_t;

   typedef struct reduce_result_s
   {
      /* Reduced view. When messages were mutated, `messages` is a NEW array the
       * caller frees via context_reduce_result_free (mutated==1). When nothing
       * changed, messages==NULL and the caller uses its original array. */
      cJSON *messages;
      int mutated;

      reduce_reason_t reason;
      reduce_error_t error; /* internal-error class on a hard-bypass (rc != 0); else NONE */

      /* Measurement (token COUNTS are caching-independent; chars/4 estimate, so
       * forecast only — the invoice-quality number is realized on/off spend). */
      int baseline_tokens; /* assembled-context tokens before reduction */
      int reduced_tokens;  /* after reduction (== baseline when measure_only) */
      int removed_tokens;  /* baseline - reduced (the saving, in tokens) */
      int foldable_tokens; /* tokens in the fold-eligible prefix region */

      /* Cost forecast bracket, priced via token_estimate_cost_ex on `model`. The
       * basis is the realized saving (removed_tokens) once a lever runs, or the
       * foldable OPPORTUNITY (foldable_tokens) in measure-only mode. floor prices
       * the basis at the provider CACHE-READ rate (cache-warm); ceiling at the
       * FRESH input rate (cache-cold). NEVER the headline — a forecast bracket;
       * both 0 when the model is unpriced. */
      double est_saved_cost_floor;
      double est_saved_cost_ceiling;

      /* Fold diagnostics (0 in measure-only; populated by the reduction slices). */
      int folded_msgs;
      int retained_msgs;
      int reused_boundary; /* 1 = freeze reused (cache-warm) */
      int epochs;
      int freeze_guarded; /* 1 = the cost guardrail disabled freeze this turn */
   } reduce_result_t;

   /* Run the economizer for one request at one seam.
    *
    *   messages       provider-native (delegate seam) or client-wire (gateway seam)
    *                  cJSON array; NEVER mutated in place.
    *   system_prompt  immutable prefix zone (never reduced); may be NULL.
    *   model          billable model id, for the cost forecast (may be NULL).
    *   session_id     conversation scope for state/ledger (may be NULL).
    *   seam           which choke point is calling.
    *   cfg            lever + seam gates (may be NULL -> all-off, no-op).
    *   st             per-conversation state (may be NULL -> freeze disabled).
    *   out            zeroed then populated; free with context_reduce_result_free.
    *
    * Returns 0 on success (including the deliberate no-op cases). Returns non-zero
    * on an internal error with out->messages == NULL, so the caller can FORWARD
    * THE ORIGINAL request unchanged (hard bypass — never break live traffic). */
   int context_reduce(cJSON *messages, const char *system_prompt, const char *model,
                      const char *session_id, reduce_seam_t seam, const reduce_config_t *cfg,
                      reduce_state_t *st, reduce_result_t *out);

   /* Freeze cost guardrail (Slice 5), exposed for unit testing. Returns 1 when
    * pinning the freeze boundary is cost-favorable — i.e. the estimated cache-read
    * savings over `horizon` reuses cover the one-time cache-WRITE PREMIUM. The guard
    * gates the freeze pointer context_reduce hands context_fold_view: when it returns
    * 0, context_reduce passes NULL, so the fold RE-DERIVES the boundary this turn
    * WITHOUT persisting it (context_fold_view only reads/commits freeze state when the
    * pointer is non-NULL). The persisted st->freeze is left as-is; a later non-guarded
    * turn that reuses it is still protected by the prefix-digest check inside
    * context_fold_view (a stale boundary fails the digest and re-epochs), so toggling
    * the guard per-turn can never serve an obsolete cache-warm prefix.
    *
    * Conservative + fail-OPEN: returns 1 (freeze on) when prefix_tokens <= 0 (no
    * churn), when model is NULL/unpriced (preserves prior always-on behavior). Returns
    * 0 only when pricing is known AND either there is no read discount (cache_read >=
    * input) OR the read savings provably do not cover the write premium. Pure: prices
    * via token_estimate_cost_ex, no state. */
   int reduce_freeze_cost_favorable(const char *model, int prefix_tokens, int horizon);

   /* The scale-invariant arithmetic core of the guardrail, exposed so the disable
    * branch (unreachable with currently-priced models) can be unit-tested with
    * synthetic per-tier costs. horizon is clamped to [1, FREEZE_GUARD_MAX_HORIZON].
    * No read discount (read >= input) -> 0; write premium (write - input) <= 0 -> 1;
    * else 1 iff horizon * (input - read) >= (write - input). */
   int reduce_freeze_favorable_rates(double input_cost, double write_cost, double read_cost,
                                     int horizon);

   /* Free a NEW messages array produced by context_reduce. Safe on a zeroed/no-op
    * result (messages==NULL). Must run AFTER the request is serialized (the result
    * may hand out non-owning references into the original array). */
   void context_reduce_result_free(reduce_result_t *out);

#ifdef __cplusplus
}
#endif

#endif /* DEC_CONTEXT_REDUCE_H */
