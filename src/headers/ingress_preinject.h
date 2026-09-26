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
 * Default-on: gated by config `ingress_preinject_enabled` and a
 * per-request disable (the `x-aimee-preinject: 0` header, surfaced by the
 * caller as request_disabled) so the A/B bench harness can toggle it live.
 *
 * The shared Go memory owner renders context. This transitional host adapter
 * retains authenticated retrieval and the final native integrity check.
 */
#ifndef DEC_INGRESS_PREINJECT_H
#define DEC_INGRESS_PREINJECT_H 1

#include "cJSON.h"
#include "index.h" /* code_search_hit_t */
#include <stddef.h>

/* Authenticated transport for Go-owned source revalidation at the wire fence. */
int ingress_preinject_revalidate_sources(void);
int ingress_preinject_acquire_send_guard(void **state);
void ingress_preinject_release_send_guard(void *state);
int ingress_preinject_accept_native_projection(const cJSON *projection);
void ingress_preinject_finish_sources(void);
/* Bind one final provider attempt through Go and synchronously accept its
 * preparation/admission in the existing WORM owner. Empty attempt means the
 * request did not require a memory receipt; no receipt coverage is implied. */
int ingress_preinject_prepare_attempt(const void *body, size_t body_len, const char *route,
                                      const char *provider, const char *model, char attempt[33]);
cJSON *ingress_preinject_receipt_options(const char *request_id, int forget, int include_payload);
cJSON *ingress_preinject_receipt(const char *request_id);
int ingress_preinject_started_attempt(const char *attempt);
int ingress_preinject_observe_attempt(const char *attempt, int http_status, const char *response,
                                      size_t response_len);
int ingress_preinject_observe_commitment(const char *attempt, int http_status, const char *digest,
                                         size_t response_len, const char *representation);

/* Extract the recall seed query from a parsed chat `messages` array: the text
 * of the last user-role message. Returns a malloc'd string (caller frees) or
 * NULL when there is no usable user text. Pure (no kb). */
char *ingress_preinject_query_from_messages(const cJSON *messages);

/* Extract the PRIOR turn's answer: the text of the last assistant-role message
 * in `messages` (the current turn's answer does not exist yet). Returns a
 * malloc'd string (caller frees) or NULL. Pure (no kb). Used by the
 * retrieval-outcome bridge for per-document overlap attribution. */
char *ingress_preinject_last_assistant_from_messages(const cJSON *messages);

/* Build the envelope for a turn seeded by `query`. Honors
 * `ingress_preinject_enabled` (config) and `request_disabled` (per-request
 * override): returns NULL when disabled, when query is blank, or when recall
 * yields no context. Otherwise runs the recall/context-block path, derives a
 * confidence tier, and returns a malloc'd <aimee-context> envelope.
 * HTTP callers install a request_context_t before assembly. Required plan or
 * assembly failures mark that request refused for final provider dispatch;
 * successful inactive/empty plans may return NULL without refusing. */
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
int ingress_preinject_mint_turn_id(char *buf, size_t len);
void ingress_preinject_set_turn_id(const char *turn_id);
const char *ingress_preinject_turn_id(void);

/* Per-turn aimee session id, recovered at HTTP ingress from the primary provider's
 * "aimee-sess-<sid>" auth token (S2 binding seam). Like the turn id it is a
 * per-request thread-local the HTTP layer sets (or "" to clear) on every request,
 * so a reused worker thread never leaks one turn's session onto the next. "" when
 * the request carries no aimee-session token (a non-primary / unidentified turn). */
void ingress_preinject_set_session_id(const char *session_id);
/* Host native-turn adapter forwards task obligations from the user request.
 * Bounded copy; NULL clears. Validation and coverage remain Go-owned. */
void ingress_preinject_set_task_requirements(const cJSON *request);
const char *ingress_preinject_session_id(void);

/* Resolve the current request's thread-local working directory to the same
 * canonical identities used by code indexing and scoped memory. Returns 0 only
 * when an active project is known; callers must not fall back to global recall. */
int ingress_preinject_resolve_active_scope(char *workspace, size_t workspace_len, char *project,
                                           size_t project_len);

#endif /* DEC_INGRESS_PREINJECT_H */
