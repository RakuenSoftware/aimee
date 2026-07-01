/* compact.h: tool-result compaction — JSON structural summaries, head/tail truncation */
#ifndef COMPACT_H
#define COMPACT_H 1

#include <stddef.h>

/* Default thresholds. Config can override these. */
#define COMPACT_DEFAULT_THRESHOLD  4096 /* bytes: pass through unchanged below this */
#define COMPACT_DEFAULT_HEAD_BYTES 512  /* leading bytes to keep in plain-text results */
#define COMPACT_DEFAULT_TAIL_BYTES 1024 /* trailing bytes to keep in plain-text results */
#define COMPACT_MAX_PER_TOOL       8    /* max per-tool threshold overrides */

/* Upper bound on a JSON structural-summary body. A summary can be LARGER than a
 * tiny raw input (the only way compact_body output exceeds raw_len), so callers
 * sizing the output buffer must allow raw_len + this much headroom to guarantee
 * the summary is never itself truncated. ENFORCED: compact_json_summary() builds
 * into a fixed `char summary[2048]` with bounded snprintf, so its result is always
 * < this value — keep these in sync, and see test_compact's json_summary_bounded. */
#define COMPACT_JSON_SUMMARY_MAX 2048

/* Per-tool threshold override.
 * Set threshold to -1 to disable compaction entirely for a specific tool. */
typedef struct
{
   char tool[64];
   int threshold; /* override in bytes; -1 = no compaction for this tool */
} compact_tool_override_t;

typedef struct
{
   int enabled;    /* 0 = compaction off; non-zero = on (default: on) */
   int threshold;  /* 0 = use COMPACT_DEFAULT_THRESHOLD */
   int head_bytes; /* 0 = use COMPACT_DEFAULT_HEAD_BYTES */
   int tail_bytes; /* 0 = use COMPACT_DEFAULT_TAIL_BYTES */
   compact_tool_override_t per_tool[COMPACT_MAX_PER_TOOL];
   int per_tool_count;
} compact_config_t;

/* compact_body: THE single tool-result body-shrink algorithm. Both reduction
 * seams call it — the eager per-result path (agent_compress_tool_result) and the
 * economizer's lazy whole-history lever (context_compress_view). It is a pure
 * transformer over the caller's output buffer:
 *
 *   - Never allocates `out`; the caller owns and frees it.
 *   - Never retains a pointer into `raw` or `cfg` past return.
 *   - Never writes past `out_cap` (always NUL-terminates when out_cap > 0).
 *   - Never applies a hard cap and never touches the Coordinate Closet — those
 *     are per-seam policy the callers own (the eager seam caps to
 *     agent_tool_output_cap and nominates from the FULL raw; the lazy seam runs a
 *     net-shrink guard and nominates from the full body). Nomination is gated on
 *     each seam's own shrink-happened decision, which is why it lives in the
 *     callers, not here.
 *   - Output length is <= raw_len EXCEPT the JSON-summary path, which is bounded
 *     by COMPACT_JSON_SUMMARY_MAX; size `out_cap >= raw_len + COMPACT_JSON_SUMMARY_MAX`
 *     to guarantee no truncation of any strategy.
 *
 * Strategy, in order:
 *   1. Pass-through: content <= effective threshold (or compaction disabled).
 *   2. JSON structural summary: content parses as a JSON object/array.
 *   3. Head+tail: keep head_bytes + tail_bytes with a truncation notice between.
 *
 * cfg may be NULL to use built-in defaults. tool_name may be NULL to skip
 * per-tool overrides (the lazy economizer seam passes NULL — it operates on
 * deep-copied, mixed-origin history where a single tool identity does not apply;
 * per-tool thresholds therefore govern the eager seam only). Returns the number of
 * bytes written to `out` (0 for an empty body, which the caller treats as a
 * pass-through). On an internal allocation failure the body is copied through
 * (bounded by out_cap) rather than emptied, so a non-empty body is never dropped.
 *
 * NOTE: the former context_used/context_budget dynamic-threshold parameters were
 * removed with the move to this core — they were never passed non-zero by any
 * production caller (dead capability), so eager output is byte-identical. */
size_t compact_body(const char *raw, size_t raw_len, const char *tool_name,
                    const compact_config_t *cfg, char *out, size_t out_cap);

#endif /* COMPACT_H */
