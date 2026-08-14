/* economizer_module_client.h: the C client for the Go economizer module.
 *
 * The reduction itself now lives in server-go/modules/economizer and is reached
 * over the event bus. This header is the whole C-side surface: it builds
 * requests, makes calls, and installs module-owned results.
 *
 * FAIL-OPEN IS THE CONTRACT. An unreachable module, a timeout, a malformed reply
 * or an over-size body all leave `messages` untouched and report "no reduction",
 * so a turn proceeds with its original context rather than failing. Losing the
 * economizer costs tokens; failing the turn costs the user's work. */
#ifndef DEC_ECONOMIZER_MODULE_CLIENT_H
#define DEC_ECONOMIZER_MODULE_CLIENT_H 1

#include <cJSON.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

/* Fixed by the process contract at 4096 + ordinal*256 + stage; economizer is
 * inventory ordinal 27, so these are not a free choice. */
#define AIMEE_ECONOMIZER_EVENT_REDUCE 11009u
#define AIMEE_ECONOMIZER_STAGE_REDUCE 1u
#define AIMEE_ECONOMIZER_EVENT_JSON_COMPACT 11010u
#define AIMEE_ECONOMIZER_STAGE_JSON_COMPACT 2u
#define AIMEE_ECONOMIZER_EVENT_TOOL_RECALL  11011u
#define AIMEE_ECONOMIZER_STAGE_TOOL_RECALL  3u
#define AIMEE_ECONOMIZER_EVENT_TOOL_STATS   11012u
#define AIMEE_ECONOMIZER_STAGE_TOOL_STATS   4u
#define AIMEE_ECONOMIZER_EVENT_RECORD_BUILD 11013u
#define AIMEE_ECONOMIZER_STAGE_RECORD_BUILD 5u

#define ECON_MODULE_JSON_MAX_INPUT  (16u * 1024u * 1024u)
#define ECON_MODULE_TOOL_OUTPUT_MAX (2u * 1024u * 1024u)

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
      /* The gateway seam's apply verdict, decided by the module because the
       * structural check it rests on needs the reduced array. "none" means
       * apply; any other value is a hard bypass and names the reason, using the
       * same labels as gw_bypass_reason_str so it goes straight to telemetry.
       *
       * EMPTY on the delegate seam, which has no such decision, and empty when
       * the module was not reached — neither is a verdict, and neither may be
       * read as one. */
      char bypass[32];

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

   /* Compact one strict JSON value in Go without changing any non-whitespace
    * source byte. Returns 0 with a newly allocated NUL-terminated buffer when
    * the result is strictly shorter. Every failure, including an unavailable
    * module and an already-compact value, returns non-zero and leaves `*output`
    * NULL so the caller keeps its original bytes. */
   int econ_module_json_compact(const void *input, size_t input_len, uint8_t **output,
                                size_t *output_len);

   /* Resolve a lossless tool-output spill through the Go module. `*output` is a
    * newly allocated NUL-terminated byte string on success. */
   int econ_module_tool_recall(const char *spill_dir, const char *ref, char **output, char *err,
                               size_t err_len);

   typedef struct
   {
      long long recognized;
      long long applied;
      long long applied_raw;
      long long applied_final;
      long long family_test;
      long long family_diag;
      long long recovered;
      long long recovered_bytes;
      long long saved_bytes;
      long long net_saved_bytes;
   } econ_module_tool_totals_t;

   /* Read process-local condensation counters from the Go owner. */
   int econ_module_tool_stats(econ_module_tool_totals_t *out);

#define ECON_MODULE_CLOSET_EVICT_NONE 0
#define ECON_MODULE_CLOSET_EVICT_FAIL 1

   /* Build session-compaction record data in the Go owner. Returns a newly
    * allocated cJSON object with files/errors/decisions, or NULL so the caller
    * can use its legacy heuristic fallback. */
   cJSON *econ_module_record_build(const cJSON *messages, int start_idx, int end_idx,
                                   char **closet_out, int *closet_evict_out);

#ifdef __cplusplus
}
#endif

#endif /* DEC_ECONOMIZER_MODULE_CLIENT_H */
