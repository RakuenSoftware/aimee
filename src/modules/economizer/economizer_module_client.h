/* economizer_module_client.h: the C client for the Go economizer module.
 *
 * The reduction itself now lives in server-go/modules/economizer and is reached
 * over the event bus (stage economizer-reduce, kind 11009). This header is the
 * whole C-side surface: it builds the request, makes the call, and installs the
 * reduced view.
 *
 * FAIL-OPEN IS THE CONTRACT. An unreachable module, a timeout, a malformed reply
 * or an over-size body all leave `messages` untouched and report "no reduction",
 * so a turn proceeds with its original context rather than failing. Losing the
 * economizer costs tokens; failing the turn costs the user's work. */
#ifndef DEC_ECONOMIZER_MODULE_CLIENT_H
#define DEC_ECONOMIZER_MODULE_CLIENT_H 1

#include <cJSON.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C"
{
#endif

/* Fixed by the process contract at 4096 + ordinal*256 + stage; economizer is
 * inventory ordinal 27, so these are not a free choice. */
#define AIMEE_ECONOMIZER_EVENT_REDUCE 11009u
#define AIMEE_ECONOMIZER_STAGE_REDUCE 1u

/* A whole transcript crosses this boundary, so the cap is generous. Above it the
 * caller keeps its original context (fail-open) rather than truncating a request
 * to fit an RPC. */
#define ECON_MODULE_CALL_MAX_BODY   (24u * 1024u * 1024u)
#define ECON_MODULE_CALL_TIMEOUT_MS 5000

/* Upper bound on the serialized reducer state the module hands back. The module
 * bounds its own blob; this is the caller's buffer for carrying it. */
#define ECON_MODULE_STATE_MAX 6144

   /* Which seam the request arrived on. The module refuses an unknown seam
    * rather than defaulting, so a typo cannot silently reduce at the wrong one. */
   typedef enum
   {
      ECON_MODULE_SEAM_GATEWAY = 0,
      ECON_MODULE_SEAM_DELEGATE = 1
   } econ_module_seam_t;

   /* Everything the module needs, resolved by the caller. The module reads no
    * ambient config and holds no state, so a request is self-contained. */
   typedef struct
   {
      int history_fold;
      int compress;
      int measure_only;
      int min_gain_tokens;

      int freeze_guard_enabled;
      int freeze_guard_horizon;
      /* Provider rates for the freeze guardrail. priced == 0 means the model is
       * unknown and the guard fails open. */
      int priced;
      double input_cost;
      double write_cost;
      double read_cost;

      int recall_enabled;
      int recall_ttl_turns;
      int recall_inject;

      int retained_msgs;
      int min_fold_msgs;
      int excerpt_bytes;
      int register_enabled;
      int compact_head_bytes;
      int compact_tail_bytes;

      int closet_enabled;
      int closet_budget_bytes;
      int closet_max_ratio_pct;
      const char *closet_denylist; /* borrowed; may be NULL */

      int turn;
      /* Serialized reducer state from the previous turn, or NULL on the first.
       * Borrowed. */
      const char *state;
   } econ_module_request_t;

   /* The ledger, plus the state to persist for the next turn. */
   typedef struct
   {
      int mutated; /* 1 when `messages` was replaced */
      char reason[24];

      int baseline_tokens;
      int reduced_tokens;
      int removed_tokens;
      int foldable_tokens;

      int folded_msgs;
      int retained_msgs;
      /* Why the call ended the way it did (aimee_module_call_result_t). Set on
       * EVERY path, including the ones that return non-zero, because "the module
       * was not reachable" and "the module ran and found nothing to do" are the
       * same silence from the caller's side and need telling apart. 1 is
       * CAPABILITY_ABSENT: nothing is serving the reduce stage. */
      int call_result;

      int reused_boundary;
      int epochs;
      int freeze_guarded;
      int closet_evicted;

      int recall_surfaced;
      char *recall_hint; /* owned; free with econ_module_result_free */
      char *state;       /* owned; persist verbatim, free with econ_module_result_free */
   } econ_module_result_t;

   /* Reduce `messages`, returning a NEW array in `*reduced` or NULL.
    *
    * `messages` is never modified — the stored conversation must stay whole, and
    * only the REQUEST view is reduced. `*reduced` is NULL whenever the module
    * declined, timed out, or was unreachable, which is the caller's signal to
    * dispatch its original array. That mirrors context_reduce's contract, so the
    * call site's ownership handling is unchanged.
    *
    * A returned array is owned by the caller (cJSON_Delete).
    *
    * Returns 0 when the call completed (whether or not it reduced), non-zero when
    * the module could not be reached or its reply was unusable. Either way the
    * caller may proceed; the return value only distinguishes "the module said no"
    * from "the module did not answer", which matters for telemetry, not safety.
    *
    * `out` may be NULL when the caller wants the transform without the ledger. */
   int econ_module_reduce(const cJSON *messages, const char *system_prompt, econ_module_seam_t seam,
                          const econ_module_request_t *req, cJSON **reduced,
                          econ_module_result_t *out);

   void econ_module_result_free(econ_module_result_t *out);

#ifdef __cplusplus
}
#endif

#endif /* DEC_ECONOMIZER_MODULE_CLIENT_H */
