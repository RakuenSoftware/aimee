/* coord_closet.h: Coordinate Closet — verbatim identifier conservation (fold §2, P1).
 *
 * When a tool result is compacted (JSON-summarized or head/tail-truncated), the
 * exact identifiers an agent needs to keep working — uuids, commit shas, paths,
 * digit-bearing key=value pairs, issue/PR refs, handle:/memory: tokens — can be
 * amputated. The closet extracts those "coordinates" from the raw text BEFORE
 * truncation and conserves them verbatim in a deterministic, bounded block that
 * rides along with the compacted body.
 *
 * Design invariants:
 *   - Deterministic: no model, no clock, no randomness. Identical input ->
 *     byte-identical render (required so it can later ride a cache-stable prefix).
 *   - No silent loss: if a nominated coordinate cannot be conserved within the
 *     byte cap, render reports COORD_EVICT_FAIL so the caller can enlarge / defer
 *     / spill / fail — it never drops a coordinate silently.
 *   - Provenance: every coordinate is stamped with {lane, turn, tool_call, result}.
 *     User-pasted content is quarantined in COORD_LANE_USER, renders untrusted, and
 *     never mints an agent-trusted label (prompt-injection guard).
 *   - Secrets never echo: values matching a secret pattern are redacted at render
 *     time (label conserved, value dropped). */
#ifndef DEC_COORD_CLOSET_H
#define DEC_COORD_CLOSET_H 1

#include <stddef.h>

#ifdef __cplusplus
extern "C"
{
#endif

   /* Provenance lane. Trust boundary: AGENT is trusted, USER is quarantined. */
   typedef enum
   {
      COORD_LANE_AGENT = 0, /* agent/tool-originated content */
      COORD_LANE_USER = 1   /* user-pasted content — untrusted */
   } coord_lane_t;

   /* Coordinate kind — also drives the fallback label when no key is in scope. */
   typedef enum
   {
      COORD_KIND_UUID = 0,
      COORD_KIND_SHA,    /* >=7 hex commit-sha-like run */
      COORD_KIND_PATH,   /* absolute / repo-relative path */
      COORD_KIND_KV,     /* digit-bearing key=value (label = key) */
      COORD_KIND_REF,    /* issue/PR ref (#778) */
      COORD_KIND_HANDLE, /* handle:<id> / memory:<id> */
      COORD_KIND_COUNT
   } coord_kind_t;

   /* Where a coordinate came from. Unknown numeric fields are set to -1. */
   typedef struct
   {
      coord_lane_t lane;
      long turn_id;
      long tool_call_id;
      int result_index;
   } coord_provenance_t;

   typedef struct
   {
      char *value; /* conserved verbatim */
      char *label; /* deterministic label */
      coord_kind_t kind;
      coord_provenance_t prov;
      size_t first_offset; /* first-occurrence byte offset in the source */
   } coord_entry_t;

   typedef struct
   {
      coord_entry_t *items;
      size_t count;
      size_t cap;
   } coord_set_t;

   /* Render outcome: did everything nominated fit within the cap? */
   typedef enum
   {
      COORD_EVICT_NONE = 0, /* all conserved */
      COORD_EVICT_FAIL = 1  /* a nominated coordinate did not fit (signalled, not silent) */
   } coord_evict_t;

   /* Runtime config, populated from the application config_t. Default-off. */
   typedef struct
   {
      int enabled;          /* 0 = off (default) */
      int budget_bytes;     /* hard byte cap for the rendered block; 0 = built-in default */
      int max_ratio_pct;    /* closet bytes <= raw_len * pct/100; 0 = default 100 (1x) */
      const char *denylist; /* extra secret patterns (comma/space separated); may be NULL */
   } coord_closet_config_t;

#define COORD_CLOSET_DEFAULT_BUDGET_BYTES  2048
#define COORD_CLOSET_DEFAULT_MAX_RATIO_PCT 100

   void coord_set_init(coord_set_t *set);
   void coord_set_free(coord_set_t *set);

   /* Scan `raw` (length `len`) for verbatim coordinates, appending them to `set`
    * with the given provenance. Duplicate values keep their earliest occurrence.
    * Deterministic. `prov` may be NULL (treated as AGENT lane, all ids -1).
    * Returns the number of distinct coordinates added. */
   size_t coord_closet_nominate(const char *raw, size_t len, const coord_provenance_t *prov,
                                coord_set_t *set);

   /* Render the conserved block into a freshly malloc'd C string (caller frees).
    * Ordering is total and deterministic: (lane, label, first_offset), tie-broken by
    * (turn_id, tool_call_id, result_index). Secret values are redacted before render.
    * Bounded by min(cfg budget, raw_len * max_ratio_pct/100). If a nominated entry
    * cannot be conserved within the cap, *why is set to COORD_EVICT_FAIL (never a
    * silent drop). Returns NULL (and sets *why) if nothing could be rendered or the
    * set is empty; `why` may be NULL if the caller does not care. */
   char *coord_closet_render(const coord_set_t *set, const coord_closet_config_t *cfg,
                             size_t raw_len, coord_evict_t *why);

   /* Returns 1 if `value` matches a built-in secret pattern (ghp_/sk-/AKIA.../
    * github_pat_/xoxb- token prefixes, or a credential-bearing path), or any
    * comma/space-separated substring in `extra_denylist` (may be NULL). */
   int coord_closet_is_secret(const char *value, const char *extra_denylist);

#ifdef __cplusplus
}
#endif

#endif /* DEC_COORD_CLOSET_H */
