/* server/ingress_preinject.h: P1 context pre-injection for the model ingresses.
 *
 * Turns the Codex/OpenAI ingress from a transparent proxy into a context-aware
 * one: before forwarding a turn to the primary model, fusion recall produces a
 * compact <aimee-context> envelope (recommended files/symbols + an explore-with
 * pointer at Aimee's own tools) that is prepended to the request's system
 * prompt. The goal is to stop the external agent re-exploring the repo on every
 * turn — it reasons over the already-loaded context and, when it needs more,
 * explores THROUGH Aimee's MCP tools rather than raw grep.
 *
 * Opt-in: gated by config `ingress_preinject_enabled` (default off) and a
 * per-request disable (the `x-aimee-preinject: 0` header, surfaced by the
 * caller as request_disabled) so the A/B bench harness can toggle it live.
 *
 * The pure helpers (confidence tiering + envelope formatting) are unit-tested
 * without any kb dependency; the builder wires them to the live recall path.
 */
#ifndef DEC_INGRESS_PREINJECT_H
#define DEC_INGRESS_PREINJECT_H 1

#include "cJSON.h"
#include "index.h" /* code_search_hit_t */
#include <stddef.h>

/* P0 Envelope IR (ingress-compression §1.1). ingress_preinject_build() no longer
 * appends straight into one opaque string: it gathers its sources into a typed
 * entry list and renders the <aimee-context> block from that list. This is a
 * pure refactor — the rendered bytes are identical to before — that gives later
 * phases a typed thing to dispatch a compressor over instead of re-parsing a
 * rendered string. */

/* The source a resident entry came from. Rendered in this group order. NB the
 * proposal's §1.1 enum lists code/memory/audit (+future tool_result); FACTS is
 * added here because today's envelope already emits a typed-facts group. */
typedef enum
{
   ING_SRC_CODE,   /* a code-search hit          */
   ING_SRC_MEMORY, /* a memory preview           */
   ING_SRC_FACTS,  /* the typed-facts block      */
   ING_SRC_AUDIT,  /* the audit-context block    */
} ingress_source_kind_t;

/* Which fold produced the resident form — one value per lossiness class, reserved
 * per proposal §6.5 B2. P0 applies no folding, so every entry is ING_XF_NONE;
 * P1a/P1b populate the rest. Declared now so the IR is the contract a compressor
 * dispatches over. */
typedef enum
{
   ING_XF_NONE = 0,
   ING_XF_CODE_WHITESPACE_COLLAPSE,
   ING_XF_CODE_COMMENT_STRIP,
   ING_XF_CODE_SIGNATURE_SPAN,
   ING_XF_JSON_FOLD,
} ingress_transform_t;

/* One resident entry. P0 carries only what the renderer reads (kind, header,
 * preview) plus the reserved transform tag; the richer §1.1 fields
 * (record_id/handle, sensitivity, original_ref, budget, metrics) are added by the
 * phase that first consumes them, to avoid unread fields. */
typedef struct
{
   ingress_source_kind_t kind;
   ingress_transform_t transform; /* P0: always ING_XF_NONE */
   const char *header;            /* group header (e.g. "recommended (code):\n"). The
                                   * renderer prepends it to every attempted candidate of
                                   * the group but keeps it only on the first one that fits
                                   * the budget, so it lands exactly once (the old
                                   * `wrote_header` rule). "" when preview is self-headed. */
   char *preview;                 /* malloc'd per-record body; the renderer reads it,
                                   * the caller frees it */
} ingress_entry_t;

/* Render a typed entry list into the pre-envelope block string, applying the
 * existing budget gate, per-group header, single blank-line separators between
 * non-empty groups, the context-budget footer, and the truncation note — byte
 * for byte as ingress_preinject_build() did inline. Pure: no kb, no config, no
 * globals; frees nothing it is given. Returns a malloc'd block, or NULL when
 * nothing was rendered (empty list / everything omitted) — the caller treats
 * NULL or "" as "no injection". Writes the omitted-entry count to
 * *omitted_count_out when non-NULL. */
char *ingress_render_block(const ingress_entry_t *entries, int count, size_t envelope_budget,
                           int headline_missing_count, int *omitted_count_out);

/* Map a recall relevance score in [0,1] to a confidence tier string
 * ("high" | "medium" | "low"). Pure; thresholds documented in the .c. */
const char *ingress_preinject_confidence(double top_score);

/* Format code-search hits into a `recommended (code):` block — one
 * `  - <file>` line per hit, each followed by a trimmed single-line snippet.
 * This is the primary pre-injection signal: the agent sees which files matter
 * for the turn before it explores. Returns a malloc'd string the caller frees,
 * or NULL when there are no hits. Pure (no kb). */
char *ingress_preinject_format_code_block(const code_search_hit_t *hits, int n);

/* Format the <aimee-context …> envelope from an already-packed context block
 * and a confidence tier. Returns a malloc'd string the caller frees, or NULL
 * when context_block is NULL/blank (no envelope → no injection). Pure. */
char *ingress_preinject_format_envelope(const char *context_block, const char *confidence);

/* Extract the recall seed query from a parsed chat `messages` array: the text
 * of the last user-role message. Returns a malloc'd string (caller frees) or
 * NULL when there is no usable user text. Pure (no kb). */
char *ingress_preinject_query_from_messages(const cJSON *messages);

/* Build the envelope for a turn seeded by `query`. Honors
 * `ingress_preinject_enabled` (config) and `request_disabled` (per-request
 * override): returns NULL when disabled, when query is blank, or when recall
 * yields no context. Otherwise runs the recall/context-block path, derives a
 * confidence tier, and returns a malloc'd <aimee-context> envelope. */
char *ingress_preinject_build(const char *query, int request_disabled);

/* Merge `envelope` with `instructions` (the request system prompt), returning a
 * fresh malloc'd string the caller frees. Default: PREPENDS the envelope. When
 * the cache-prefix placement lever (ingress_cache_placement_enabled, §2) is on,
 * APPENDS it instead (delegates to ingress_preinject_append) so the stable
 * instructions prefix stays cacheable. If envelope is NULL/blank, returns a
 * malloc'd copy of instructions (or NULL when instructions is also NULL). Does
 * not free its arguments. Reads config (the placement flag); not otherwise
 * stateful. */
char *ingress_preinject_apply(const char *instructions, const char *envelope);

/* Cache-prefix placement variant (ingress-compression §2): APPEND `envelope`
 * after `instructions` (stable prefix first, volatile envelope last) so the
 * provider's automatic prefix cache is not invalidated by the per-turn envelope.
 * Same contract as ingress_preinject_apply otherwise (malloc'd result; blank
 * envelope → copy of instructions; pure). */
char *ingress_preinject_append(const char *instructions, const char *envelope);

/* Per-request override (thread-local): the HTTP layer sets this from the
 * `x-aimee-preinject: 0` request header before dispatching the turn, so a
 * single request can disable pre-injection without touching the server config
 * (used by the A/B bench). ingress_preinject_build() consults it in addition to
 * its `request_disabled` argument and the config flag. Set per request; it does
 * not auto-reset, so the HTTP layer sets it (to 0 or 1) on every request. */
void ingress_preinject_set_request_disabled(int disabled);

/* Auditable-correctness P1: the per-turn retrieval-event id (a UUID).
 *
 * mint generates a fresh UUID into `buf` (>=37 bytes). set/turn_id are a
 * thread-local seam, mirroring the request-disabled override: the HTTP layer
 * mints a turn_id and calls set() before dispatching, so the same id can be
 * surfaced to the client (the `X-Aimee-Retrieval-Event` response header) AND
 * keyed onto the retrieval_event emitted during context assembly. When the HTTP
 * layer has not set one (e.g. a direct ingress_preinject_build call),
 * ingress_preinject_build mints its own. Set per request; like the disable
 * override it does not auto-reset — the HTTP layer sets it (or "" to clear) on
 * every request. */
void ingress_preinject_mint_turn_id(char *buf, size_t len);
void ingress_preinject_set_turn_id(const char *turn_id);
const char *ingress_preinject_turn_id(void);

/* Per-turn aimee session id, recovered at HTTP ingress from the primary provider's
 * "aimee-sess-<sid>" auth token (S2 binding seam). Like the turn id it is a
 * per-request thread-local the HTTP layer sets (or "" to clear) on every request,
 * so a reused worker thread never leaks one turn's session onto the next. "" when
 * the request carries no aimee-session token (a non-primary / unidentified turn). */
void ingress_preinject_set_session_id(const char *session_id);
const char *ingress_preinject_session_id(void);

#endif /* DEC_INGRESS_PREINJECT_H */
