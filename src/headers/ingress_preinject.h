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

/* Prepend `envelope` to `instructions` (the request system prompt), returning a
 * fresh malloc'd string the caller frees. If envelope is NULL/blank, returns a
 * malloc'd copy of instructions (or NULL when instructions is also NULL). Does
 * not free its arguments. Pure. */
char *ingress_preinject_apply(const char *instructions, const char *envelope);

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

#endif /* DEC_INGRESS_PREINJECT_H */
