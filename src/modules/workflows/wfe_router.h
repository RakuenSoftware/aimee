/* wfe_router.h -- request->workflow router (S1, ADVISORY-only).
 *
 * The PURE decision core: a deterministic prefilter + a routing table over an
 * injected catalog. The catalog enumeration (globbing $AIMEE_HOME/workflows),
 * the sampled + hard-bounded LLM classifier call, the advisory logging
 * (ie_record), and the chat-ingress hook are integration layers built ON TOP of
 * this core -- kept out of here so the decision logic is unit-testable and has
 * no I/O, no LLM, no filesystem.
 *
 * Design per the S1 router roundtable consult (2026-07-01):
 *  - the prefilter is a DEFER trigger, not a route trigger: it only emits a
 *    high-confidence `converse`, a validated `use <name>`, or DEFER (to the LLM
 *    classifier). A change-verb or a code/path token means DEFER, never an
 *    automatic route-to-change (so "how do I change my password?" defers, it does
 *    not route to a write workflow).
 *  - any named/classifier id is validated against the catalog allowlist; anything
 *    unresolved, unknown, or unsafe falls back to the read-only default
 *    (`research`). The catalog must declare exactly one default and it must be
 *    read-only.
 */
#ifndef DEC_WFE_ROUTER_H
#define DEC_WFE_ROUTER_H 1

#include <stddef.h>

#define WFE_ROUTER_ID_LEN   64
#define WFE_ROUTER_MAX_WF   64
#define WFE_ROUTER_MAX_TAGS 16
#define WFE_ROUTER_TAG_LEN  40

typedef struct
{
   char id[WFE_ROUTER_ID_LEN];
   char tags[WFE_ROUTER_MAX_TAGS][WFE_ROUTER_TAG_LEN];
   int n_tags;
   int is_default; /* the mandatory fallback; exactly one; must be read-only */
   int read_only;  /* converse/research lanes never mutate the repo */
   int enforced;   /* S2: workflow is aimee-enforced (managed panel); from YAML `enforced:` */
} wfe_router_wf_t;

typedef struct
{
   wfe_router_wf_t wf[WFE_ROUTER_MAX_WF];
   int n;
} wfe_router_catalog_t;

typedef enum
{
   WFE_PREFILTER_CONVERSE = 0, /* high-confidence conversational/read-only */
   WFE_PREFILTER_NAMED,        /* validated `use <name>` -> matched_id filled */
   WFE_PREFILTER_DEFER         /* defer to the LLM classifier */
} wfe_prefilter_outcome_t;

typedef enum
{
   WFE_ROUTE_SRC_PREFILTER = 0,
   WFE_ROUTE_SRC_CLASSIFIER,
   WFE_ROUTE_SRC_DEFAULT /* fell back to the read-only default */
} wfe_route_source_t;

typedef struct
{
   char workflow_id[WFE_ROUTER_ID_LEN];
   wfe_route_source_t source;
   int user_provided_name; /* the user typed an explicit `use <name>` */
   char reason[96];        /* short audit reason (no raw message text) */
} wfe_route_decision_t;

/* Validate catalog invariants: >=1 workflow, unique ids, EXACTLY ONE is_default,
 * the default is read_only, and `converse` + the default id both exist. Returns
 * 0 if valid, -1 + err otherwise. The router must fail closed (not route) if the
 * catalog is invalid. */
int wfe_router_catalog_validate(const wfe_router_catalog_t *cat, char *err, size_t errlen);

/* 1 if `id` is a valid workflow id: non-empty, [A-Za-z0-9_-] only, and short
 * enough to fit the id buffer (ids are logged as JSON + used as routing keys). */
int wfe_router_id_valid(const char *id);

/* Find a workflow by exact (case-sensitive) id, or NULL. */
const wfe_router_wf_t *wfe_router_find(const wfe_router_catalog_t *cat, const char *id);
/* The read-only default workflow (the safe fallback), or NULL if none. */
const wfe_router_wf_t *wfe_router_default(const wfe_router_catalog_t *cat);

/* Deterministic prefilter over the raw message. On WFE_PREFILTER_NAMED, fills
 * matched_id with the validated catalog id. reason (<=rlen) gets the matched
 * rule for audit. */
wfe_prefilter_outcome_t wfe_router_prefilter(const char *msg, const wfe_router_catalog_t *cat,
                                             char *matched_id, size_t idlen, char *reason,
                                             size_t rlen);

/* Full decision. Runs the prefilter, then resolves DEFER using the OPTIONAL
 * classifier_id (NULL if the classifier was not sampled / timed out / errored).
 * Any id not in the catalog -> the read-only default. Never returns an id that
 * is not in the catalog. */
void wfe_router_decide(const char *msg, const wfe_router_catalog_t *cat,
                       const char *classifier_id /* nullable */, wfe_route_decision_t *out);

/* Deterministic sampling for the S1 telemetry classifier: run the LLM on ~1/N of
 * deferred turns. Pure hash of (session_id, turn_index); no RNG. one_in_n<=1
 * means always. */
int wfe_router_should_sample(const char *session_id, int turn_index, int one_in_n);

/* ---- classifier prompt/parse (pure; the agent_execute call is integration) -- */

/* Build the classifier SYSTEM prompt: instructs the model to reply with exactly
 * one catalog id (or the read-only default) and nothing else. Deterministic. */
void wfe_router_classify_prompt(const wfe_router_catalog_t *cat, char *buf, size_t n);

/* Parse a classifier response into a catalog id. Accepts an exact-id reply or an
 * id embedded as a bounded token; the FIRST catalog id found wins. Returns 0 and
 * fills out_id on a valid (allowlisted) id, -1 otherwise (caller routes to the
 * read-only default). Never returns an id outside the catalog. */
int wfe_router_parse_classification(const char *response, const wfe_router_catalog_t *cat,
                                    char *out_id, size_t n);

/* Build the advisory-log payload as JSON STRUCTURAL FEATURES only -- never the
 * raw message text (PII/secrets must not be persisted to the append-only sink).
 * classifier_ms < 0 means the classifier was not run this turn. */
void wfe_router_advisory_payload(const wfe_route_decision_t *d, wfe_prefilter_outcome_t prefilter,
                                 int sampled, double classifier_ms, char *buf, size_t n);

/* ---- I/O layer (wfe_router_catalog.c) -------------------------------------
 * Build the catalog from the built-in read-only converse/research lanes plus
 * every $AIMEE_HOME/workflows/<name>.yaml's router metadata (name + enforced +
 * read_only + intent_tags). Symlinks and non-regular files in the workflows dir
 * are skipped (escape guard). `research` is the built-in default; a workflow
 * YAML that sets `default` is rejected. Returns 0 on a valid catalog, -1 + err
 * otherwise (the router must fail closed on an invalid catalog). */
int wfe_router_catalog_load(wfe_router_catalog_t *out, char *err, size_t errlen);

#endif /* DEC_WFE_ROUTER_H */
