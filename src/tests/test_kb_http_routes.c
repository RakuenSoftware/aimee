/* test_kb_http_routes.c: unit tests for kb_http_route() (Phase 1+5). */
#include <assert.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>

#include "config.h"
#include "cJSON.h"
#include "kb_http.h"
#include "kb_route_acl.h"
#include "kb_scope.h"
#include "td_search_render.h"       /* consumer side of the /v1/search contract test */
#include "kb/kb_surprising_judge.h" /* §4 judge stub seam (kb_surprising_verdict_t) */
#include "db2/lifecycle.h"          /* §2c: db2_reembed_* / db2_dim_change_reset stub types */
#include "db2/code_project_lifecycle.h"
#include "rel_types.h"        /* REL_TYPE_NAME_MAX for the db2_ontology_* stubs below */
#include "config_fields.h"    /* config_field_t for the pipeline-console stubs below */
#include "embed_input_type.h" /* the memory_embed_text stub's polarity argument */
#include "kb_service.h"
#include "kb/kb_service_code_embed.h"
#include "kb_bandit.h"
#include "kb_service_backend.h"
#include "kb_enroll.h"
#include "kb_identity.h"
#include "kb_paths.h"
#include "kb_pki.h"
#include "kb_tls.h"
#include "kb_client_mtls.h"
#include "runtime_secret.h"

#include <aimee/control-web/module_api.h>
#include <aimee/core/event_bus/module_runtime.h>

extern aimee_module_status_t aimee_control_web_module_handler(const aimee_module_invocation_t *,
                                                              const uint8_t *, uint32_t, uint8_t *,
                                                              uint32_t, uint32_t *, void *);

int aimee_module_invocation_cancelled(const aimee_module_invocation_t *invocation)
{
   (void)invocation;
   return 0;
}

static int control_web_module_provider(const char *method, const char *path, int *allowed)
{
   uint8_t request[AIMEE_CONTROL_WEB_REQUEST_LEN];
   uint8_t response[AIMEE_CONTROL_WEB_RESPONSE_LEN];
   uint32_t response_len = 0;
   aimee_module_invocation_t invocation = {.stage_id = AIMEE_CONTROL_WEB_STAGE_AUTHORIZE};
   if (aimee_control_web_request_encode(AIMEE_CONTROL_WEB_TARGET_CONSOLE_ADMIN, method, path,
                                        request, sizeof(request)) != 0 ||
       aimee_control_web_module_handler(&invocation, request, sizeof(request), response,
                                        sizeof(response), &response_len,
                                        NULL) != AIMEE_MODULE_STATUS_OK)
      return -1;
   return aimee_control_web_response_decode(response, response_len, allowed);
}

extern int g_test_registry_heartbeat_allow;
extern char g_test_registry_server_id[128], g_test_registry_issuer[601],
    g_test_registry_serial[129], g_test_registry_fingerprint[65];

#include "db_postgres.h"        /* aimee_pg_* types for the tenancy-route db2 stubs below */
#include "platform_test_util.h" /* platform_tmpdir: honour TMPDIR, do not leak into /tmp */

/* db2 accessor stubs: this test links the kb router but not the DB2 stack. The
 * tenancy routes hard-fail on the shim (aimee_pg_is_shim()=1) inside
 * db2_tenant_require_pg BEFORE any accessor runs, so these are unreachable at
 * runtime and exist only to satisfy the linker (proves the auth->actor->handler->
 * tenant-guard chain reaches 503; real RLS is proven by the Postgres gate). */
int aimee_pg_is_shim(void)
{
   return 1;
}
void *(db2_conn)(void)
{
   return NULL;
}

/* Real code reaches the pool through the db2_conn() macro, which expands to
 * db2_conn_at(site) so a lazy acquire can be attributed. Route the stub. */
void *db2_conn_at(const char *site)
{
   (void)site;
   return (db2_conn)();
}
/* The real symbol is db2_lease_begin_at; db2_lease_begin is a macro in db2.h
 * that records the caller's file:line for stuck-lease attribution. */
void db2_lease_begin_at(const char *site)
{
}
void db2_lease_end(void)
{
}
void db2_lease_release_idle(void)
{
}
/* /v1/health reports pool starvation, so the route layer now reads the pool.
 * A route test wants no real DB2: report an idle pool so health stays "ok" and
 * the route assertions are about routing, not pool state. */
void db2_pool_stats(int *size, int *in_use, int *waiters, long *lease_grants, long *lease_timeouts,
                    long *stuck, long *poisoned)
{
   if (size)
      *size = 0;
   if (in_use)
      *in_use = 0;
   if (waiters)
      *waiters = 0;
   if (lease_grants)
      *lease_grants = 0;
   if (lease_timeouts)
      *lease_timeouts = 0;
   if (stuck)
      *stuck = 0;
   if (poisoned)
      *poisoned = 0;
}

static int code_project_manifest_stub(const char *project, code_project_manifest_t *out)
{
   if (!project || !project[0] || !out)
      return CODE_PROJECT_LIFECYCLE_ERROR;
   memset(out, 0, sizeof(*out));
   snprintf(out->project, sizeof(out->project), "%s", project);
   out->generation = 2;
   snprintf(out->mode, sizeof(out->mode), "dry_run");
   snprintf(out->targets[0].table, sizeof(out->targets[0].table), "code_files");
   out->targets[0].rows = 3;
   out->target_count = 1;
   out->total_rows = 3;
   snprintf(out->manifest_hash, sizeof(out->manifest_hash), "sha256:test");
   return 0;
}

static char g_lifecycle_audit_principal[576];

int db2_code_project_detach(const char *project, const char *principal, int64_t *generation_out)
{
   if (!project || !project[0])
      return CODE_PROJECT_LIFECYCLE_ERROR;
   snprintf(g_lifecycle_audit_principal, sizeof(g_lifecycle_audit_principal), "%s",
            principal ? principal : "");
   if (generation_out)
      *generation_out = 2;
   return 0;
}

int db2_code_project_purge_manifest(const char *project, code_project_manifest_t *out)
{
   return code_project_manifest_stub(project, out);
}

int db2_code_project_gc_manifest(const char *project, int retention_days,
                                 code_project_manifest_t *out)
{
   (void)retention_days;
   return code_project_manifest_stub(project, out);
}

int db2_code_project_purge_confirm(const char *project, const char *expected_hash,
                                   const char *principal, const char *reason,
                                   code_project_manifest_t *out)
{
   snprintf(g_lifecycle_audit_principal, sizeof(g_lifecycle_audit_principal), "%s",
            principal ? principal : "");
   (void)reason;
   if (!expected_hash || strcmp(expected_hash, "sha256:test") != 0)
      return CODE_PROJECT_LIFECYCLE_HASH_MISMATCH;
   int rc = code_project_manifest_stub(project, out);
   if (rc == 0)
      snprintf(out->mode, sizeof(out->mode), "confirmed");
   return rc;
}

int db2_code_project_gc_confirm(const char *project, int retention_days, const char *expected_hash,
                                const char *principal, const char *reason,
                                code_project_manifest_t *out)
{
   (void)retention_days;
   return db2_code_project_purge_confirm(project, expected_hash, principal, reason, out);
}
int aimee_pg_exec(void *c, const char *s, char *e, size_t n)
{
   (void)c;
   (void)s;
   (void)e;
   (void)n;
   return 0;
}
aimee_pg_stmt_t *aimee_pg_prepare(void *c, const char *s, char *e, size_t n)
{
   (void)c;
   (void)s;
   (void)e;
   (void)n;
   return NULL;
}
void aimee_pg_finalize(aimee_pg_stmt_t *s)
{
   (void)s;
}
aimee_pg_step_t aimee_pg_step(aimee_pg_stmt_t *s, char *e, size_t n)
{
   (void)s;
   (void)e;
   (void)n;
   return AIMEE_PG_ERR;
}
int aimee_pg_bind_text(aimee_pg_stmt_t *s, const char *k, const char *v)
{
   (void)s;
   (void)k;
   (void)v;
   return 0;
}
int aimee_pg_bind_int64(aimee_pg_stmt_t *s, const char *k, int64_t v)
{
   (void)s;
   (void)k;
   (void)v;
   return 0;
}
int64_t aimee_pg_column_int64(aimee_pg_stmt_t *s, int c)
{
   (void)s;
   (void)c;
   return 0;
}
int aimee_pg_column_int(aimee_pg_stmt_t *s, int c)
{
   (void)s;
   (void)c;
   return 0;
}
const char *aimee_pg_column_text(aimee_pg_stmt_t *s, int c)
{
   (void)s;
   (void)c;
   return "";
}

#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/stat.h>

typedef struct
{
   int files_scanned;
   int files_indexed;
   int files_skipped;
   int files_removed;
   int chunks_added;
   int chunks_removed;
   int embeddings_added;
} kb_stats_t;

typedef struct
{
   const char *rel_path;
   const char *content;
} canonical_index_file_input_t;

typedef struct
{
   int rc;
   int64_t mem_pruned;
   int64_t mem_kept;
   int64_t kb_pruned;
   int64_t kb_kept;
} pgvec_kb_service_reconcile_result_t;

typedef int (*pgvec_kb_service_record_exists_fn)(int64_t record_id);

typedef struct
{
   int64_t row_id;
   int attribution_n;
} db2_demotion_candidate_t;

typedef struct
{
   long long n_decisions;
   long long n_rewards;
   double sum_reward;
   double posterior_alpha;
   double posterior_beta;
} db2_bandit_arm_stats_t;

typedef struct
{
   int64_t id;
   char tier[4];
   char kind[16];
} memory_t;

/* ── Phase 3+4 handler stubs (satisfies refs from kb_http.o) ────────────── */

int handle_post_docs(const char *body, int body_len, char *out_buf, int out_cap)
{
   (void)body;
   (void)body_len;
   snprintf(out_buf, (size_t)out_cap, "{\"doc_id\":1,\"state\":\"staged\"}");
   return 201;
}

int handle_post_docs_manifest(const char *body, int body_len, char *out_buf, int out_cap)
{
   (void)body;
   (void)body_len;
   snprintf(out_buf, (size_t)out_cap, "{\"missing\":[],\"total\":0}");
   return 200;
}

int handle_get_doc(const char *doc_id, char *out_buf, int out_cap)
{
   (void)doc_id;
   snprintf(out_buf, (size_t)out_cap, "{\"error\":\"not found\"}");
   return 404;
}

int handle_delete_doc(const char *doc_id, char *out_buf, int out_cap)
{
   (void)doc_id;
   snprintf(out_buf, (size_t)out_cap, "{\"deleted\":true}");
   return 200;
}

int handle_get_review(const char *query_string, char *out_buf, int out_cap)
{
   (void)query_string;
   snprintf(out_buf, (size_t)out_cap, "{\"docs\":[],\"next_cursor\":null}");
   return 200;
}

int handle_post_review_accept(const char *doc_id, const char *body, int body_len, char *out_buf,
                              int out_cap)
{
   (void)doc_id;
   (void)body;
   (void)body_len;
   snprintf(out_buf, (size_t)out_cap, "{\"state\":\"accepted\"}");
   return 200;
}

int handle_post_review_reject(const char *doc_id, const char *body, int body_len, char *out_buf,
                              int out_cap)
{
   (void)doc_id;
   (void)body;
   (void)body_len;
   snprintf(out_buf, (size_t)out_cap, "{\"state\":\"rejected\"}");
   return 200;
}

int handle_post_releases(const char *body, int body_len, char *out_buf, int out_cap)
{
   (void)body;
   (void)body_len;
   snprintf(out_buf, (size_t)out_cap, "{\"release_id\":1,\"state\":\"pending\"}");
   return 201;
}

int handle_post_promote(const char *release_id, char *out_buf, int out_cap)
{
   (void)release_id;
   snprintf(out_buf, (size_t)out_cap, "{\"state\":\"active\"}");
   return 200;
}

int handle_post_rollback(const char *release_id, const char *body, int body_len, char *out_buf,
                         int out_cap)
{
   (void)release_id;
   (void)body;
   (void)body_len;
   snprintf(out_buf, (size_t)out_cap, "{\"state\":\"rolled_back\"}");
   return 200;
}

int handle_get_active_release(char *out_buf, int out_cap)
{
   snprintf(out_buf, (size_t)out_cap, "{\"active_release_id\":null}");
   return 200;
}

/* ── Phase 6 handler stubs ───────────────────────────────────────────────── */

int handle_post_reflections(const char *body, int body_len, char *out_buf, int out_cap)
{
   (void)body;
   (void)body_len;
   snprintf(out_buf, (size_t)out_cap, "{\"created\":1}");
   return 201;
}

int handle_get_reflections(const char *query_string, char *out_buf, int out_cap)
{
   (void)query_string;
   snprintf(out_buf, (size_t)out_cap, "{\"items\":[]}");
   return 200;
}

int handle_post_reflection_accept(const char *artifact_id, const char *body, int body_len,
                                  char *out_buf, int out_cap)
{
   (void)artifact_id;
   (void)body;
   (void)body_len;
   snprintf(out_buf, (size_t)out_cap, "{\"state\":\"committed\"}");
   return 200;
}

int handle_post_reflection_reject(const char *artifact_id, const char *body, int body_len,
                                  char *out_buf, int out_cap)
{
   (void)artifact_id;
   (void)body;
   (void)body_len;
   snprintf(out_buf, (size_t)out_cap, "{\"state\":\"rejected\"}");
   return 200;
}

int handle_post_feedback_in_session(const char *body, int body_len, char *out_buf, int out_cap)
{
   (void)body;
   (void)body_len;
   snprintf(out_buf, (size_t)out_cap, "{\"id\":\"test-uuid\",\"state\":\"committed\"}");
   return 201;
}

char *kb_service_health_json(void)
{
   char *r = malloc(160);
   if (r)
      strcpy(r, "{\"status\":\"ok\",\"db2_ok\":true,\"pgvec_ok\":true,"
                "\"chunk_count\":7,\"embedding_count\":6}");
   return r;
}

char *kb_service_status_json(const char *project)
{
   char *r = malloc(256);
   if (r)
      snprintf(r, 256,
               "{\"status\":\"ok\",\"summary_status\":\"ok\","
               "\"project\":\"%s\",\"files\":2,\"chunks\":7,"
               "\"vector\":{\"status\":\"ok\"}}",
               project && project[0] ? project : "all");
   return r;
}

char *kb_service_ingest_status_json(void)
{
   char *r = malloc(256);
   if (r)
      strcpy(r, "{\"status\":\"ok\",\"queue\":{\"pending\":3,\"running\":1,"
                "\"done_last_24h\":9,\"failed_last_24h\":0},"
                "\"workers\":{\"configured\":2,\"active\":1},\"recent\":[]}");
   return r;
}

/* ── Phase 5 backend stubs (satisfies extern refs in kb_http.o) ─────────── */

/* When g_test_search_populated is set, the backend stub returns one populated
 * result row so the /v1/search handler emits a real hit — used by the
 * producer->consumer contract test. The row shape (file_path/content/score/
 * doc_id) mirrors what the ranked backend emits and what the handler's reshaper
 * parses. Default 0 keeps every other test on the empty-results path. */
static int g_test_search_populated = 0;
static int g_test_search_scoped_all = 0;
static char g_test_search_embedding[256];
char *kb_search_json_ex(const char *p, const char *q, const char *e, int m, const char *f)
{
   (void)p;
   (void)q;
   (void)m;
   (void)f;
   snprintf(g_test_search_embedding, sizeof(g_test_search_embedding), "%s", e ? e : "");
   const char *src = g_test_search_populated
                         ? "{\"fusion_mode\":\"rrf\",\"results\":[{\"project\":\"proj-alpha\","
                           "\"file_path\":\"docs/alpha.md\","
                           "\"content\":\"alpha excerpt body\",\"score\":0.875,\"doc_id\":4242}]}"
                         : "{\"fusion_mode\":\"rrf\",\"results\":[]}";
   char *r = malloc(strlen(src) + 1);
   if (r)
      strcpy(r, src);
   return r;
}

char *kb_search_json_scoped_ex(const char *p, int all, const char *q, const char *e, int m,
                               const char *f)
{
   if (g_test_search_scoped_all)
   {
      assert(p && strcmp(p, "proj-alpha") == 0);
      assert(all == 1);
      const char *src = "{\"fusion_mode\":\"rrf\",\"results\":["
                        "{\"project\":\"proj-alpha\",\"file_path\":\"local/first.md\","
                        "\"content\":\"local result\",\"score\":0.4,\"doc_id\":1},"
                        "{\"project\":\"proj-other\",\"file_path\":\"other/high.md\","
                        "\"content\":\"other result\",\"score\":0.99,\"doc_id\":2}]}";
      return strdup(src);
   }
   return kb_search_json_ex(p, q, e, m, f);
}

int kb_curator_implements_json(const char *topic, char *out, size_t out_cap)
{
   snprintf(out, out_cap, "{\"topic\":\"%s\",\"implements\":[{\"code_unit_id\":\"cu\"}]}",
            topic ? topic : "");
   return 1;
}
int kb_curator_synthesize_serve_json(const char *topic, char *out, size_t out_cap)
{
   snprintf(out, out_cap, "{\"topic\":\"%s\",\"synthesis_id\":\"s\",\"text\":\"t\",\"sources\":[]}",
            topic ? topic : "");
   return 0;
}
int kb_curator_contradictions_json(int limit, char *out, size_t out_cap)
{
   (void)limit;
   snprintf(out, out_cap, "{\"contradictions\":[{\"a\":\"c1\",\"b\":\"c2\"}]}");
   return 1;
}

/* §2c: /v1/reembed + search-guard db2 stubs (g_test_reembed_in_progress -> 503 guard). */
static int g_test_reembed_in_progress = 0;
int db2_reembed_in_progress_get(int *target_dim, long *started_epoch)
{
   if (g_test_reembed_in_progress)
   {
      if (target_dim)
         *target_dim = 1024;
      if (started_epoch)
         *started_epoch = 1750000000L;
      return 1; /* in progress -> search path 503s */
   }
   (void)target_dim;
   (void)started_epoch;
   return 0; /* not in progress -> search path proceeds */
}
/* structured-PDF db2 stubs live in tests/support/pdf_route_stubs.c (link-only; the real
 * SQL is exercised against the sqlite shim in test_kb_doc_pdf.c). */
int db2_dim_change_reset(int target_dim, int force, int dry_run, db2_reembed_plan_t *out)
{
   (void)force;
   (void)dry_run;
   if (out)
   {
      memset(out, 0, sizeof(*out));
      out->target_dim =
          target_dim; /* echo the resolved target so target_dim override is observable */
   }
   return 0;
}
int db2_reembed_in_progress_clear(void)
{
   g_test_reembed_in_progress = 0;
   return 0;
}
/* Test-controllable recorded/running dims for the clear-maintenance consistency gate. */
static int g_test_recorded_dim = 1024;
static int g_test_running_dim = 1024;
int db2_reembed_clear_maintenance(int force, int *was_in_progress, int *recorded, int *running)
{
   if (was_in_progress)
      *was_in_progress = g_test_reembed_in_progress;
   if (recorded)
      *recorded = g_test_recorded_dim;
   if (running)
      *running = g_test_running_dim;
   if (g_test_recorded_dim > 0 && g_test_recorded_dim != g_test_running_dim && !force)
      return -1; /* mismatch needs force */
   g_test_reembed_in_progress = 0;
   return 0;
}
int g_test_embedding_dim = 1024; /* §5 vector-leg tests flip this; default 1024 */
int db2_embedding_dim(void)
{
   return g_test_embedding_dim;
}
int db2_probe_embedder_dim(int budget_ms, int *out)
{
   (void)budget_ms;
   if (out)
      *out = 1024;
   return 0;
}
int config_resolve_embedder_dims(const config_t *cfg)
{
   (void)cfg;
   return 0;
}
int config_embedder_dims_is_pinned(const config_t *cfg)
{
   (void)cfg;
   return 0;
}

/* kb_http reads the pin through the no-arg form now; same answer as the
 * config_t stub above. */
int config_embedder_dims_pinned_current(void)
{
   return 0;
}

int db2_curator_invalidations_since(int64_t since_id, void *out, int max)
{
   (void)since_id;
   (void)out;
   (void)max;
   return 0;
}

/* Ontology-console db2 stubs + config_save: the typed_facts ontology console added
 * these refs into kb_http_console.o; the real defs pull the whole db2/config stack,
 * so stub them link-only (this test exercises routing, not the ontology backend). */
long db2_ontology_eval_count(const char *rel_type)
{
   (void)rel_type;
   return 0;
}
int db2_ontology_eval_status(const char *rel_type, char *out, size_t out_len)
{
   (void)rel_type;
   if (out && out_len)
      out[0] = '\0';
   return 0;
}
int db2_ontology_eval_candidates(int threshold, char (*out)[REL_TYPE_NAME_MAX], int max)
{
   (void)threshold;
   (void)out;
   (void)max;
   return 0;
}
int db2_ontology_approve(const char *rel_type)
{
   (void)rel_type;
   return 0;
}
int db2_ontology_map(const char *novel, const char *target)
{
   (void)novel;
   (void)target;
   return 0;
}
int db2_ontology_reject(const char *rel_type)
{
   (void)rel_type;
   return 0;
}
int config_save(const config_t *cfg)
{
   (void)cfg;
   return 0;
}

/* The console writes through config_set / config_set_typed_facts now instead of
 * mutating a config_t and calling config_save. Same contract as the stub above:
 * report success without touching a real config file. */
int config_set(const char *key, const char *value)
{
   (void)key;
   (void)value;
   return 0;
}

int config_set_typed_facts(int enabled, int auto_promote, int promote_threshold)
{
   (void)enabled;
   (void)auto_promote;
   (void)promote_threshold;
   return 0;
}

/* Pipeline-console stubs: the console's /v1/console/pipeline routes pull in the
 * curator registry and the typed config-field accessors. The real defs drag in
 * the whole curator + config stack, so stub them here — this test exercises
 * routing and the route's own key allowlist, not the curator itself. Two stage
 * shapes are enough for that: one toggleable, one embedder-gated (null key). */
cJSON *kb_curator_stages_json(void)
{
   cJSON *arr = cJSON_CreateArray();
   cJSON *a = cJSON_CreateObject();
   cJSON_AddStringToObject(a, "name", "extract_docs");
   cJSON_AddStringToObject(a, "config_key", "kb_curator_extract_docs_enabled");
   cJSON_AddItemToArray(arr, a);
   cJSON *b = cJSON_CreateObject();
   cJSON_AddStringToObject(b, "name", "embed_code");
   cJSON_AddNullToObject(b, "config_key");
   cJSON_AddItemToArray(arr, b);
   return arr;
}
cJSON *kb_curator_presets_json(void)
{
   return cJSON_CreateArray();
}
static const config_field_t g_stub_field = {"stub",         0,   0, 0, CFG_BOOL, RELOAD_HOT,
                                            FGROUP_RUNTIME, NULL};
static const config_field_t g_stub_secret_field = {
    "kb_api_bearer_token",      0, 1, 0, CFG_STRING, RELOAD_RESTART, FGROUP_RUNTIME,
    "AIMEE_KB_API_BEARER_TOKEN"};
static int g_stub_secret_configured;
static int g_stub_secret_store_calls;
const config_field_t *config_field_lookup(const char *key)
{
   /* Only the keys the pipeline route may touch resolve; anything else is
    * "unknown" so the route's own allowlist is what is under test. */
   if (!key)
      return NULL;
   if (strcmp(key, "kb_api_bearer_token") == 0)
      return &g_stub_secret_field;
   if (strncmp(key, "kb_curator_", 11) == 0 || strcmp(key, "kb_evidence_embed_enabled") == 0)
      return &g_stub_field;
   /* The KB-owned settings surface (KB_SETTINGS) plus the server-owned keys the
    * 403 cases probe — all real config keys, so the route's OWN allowlist is
    * what those assertions exercise, not a lookup miss. */
   if (strncmp(key, "embedder_", 9) == 0 || strncmp(key, "synthesis_", 10) == 0 ||
       strncmp(key, "llm_", 4) == 0 || strncmp(key, "kb_", 3) == 0 ||
       strncmp(key, "css_", 4) == 0 || strcmp(key, "typed_facts_enabled") == 0 ||
       strcmp(key, "ocr_command") == 0 || strcmp(key, "tsr_command") == 0 ||
       strcmp(key, "db2_url") == 0)
      return &g_stub_field;
   return NULL;
}
const char *config_field_secret_name(const config_field_t *f)
{
   return f ? f->secret_name : NULL;
}
cJSON *config_field_public_value_json(const config_t *cfg, const config_field_t *f)
{
   (void)cfg;
   if (config_field_secret_name(f))
      return cJSON_CreateBool(g_stub_secret_configured);
   return cJSON_CreateBool(0);
}

/* The console renders values through the live-config form now; same answer. */
cJSON *config_field_public_value_json_current(const config_field_t *f)
{
   return config_field_public_value_json(NULL, f);
}

/* Typed-facts knobs the console echoes back, read through accessors now.
 * Mirror the values this file's config_load stub sets (all zero/off). */
int config_typed_facts_enabled(void)
{
   return 0;
}
int config_kb_typed_facts_auto_promote_enabled(void)
{
   return 0;
}
int config_kb_typed_facts_promote_threshold(void)
{
   return 0;
}
int config_secret_store(const char *name, const char *value)
{
   assert(name && strcmp(name, "AIMEE_KB_API_BEARER_TOKEN") == 0);
   g_stub_secret_store_calls++;
   g_stub_secret_configured = value && value[0] ? 1 : 0;
   return 0;
}
int config_field_set_value(config_t *cfg, const config_field_t *f, const char *value)
{
   (void)cfg;
   (void)f;
   /* Mirror the real parser's contract: bools accept only true/false text. */
   return (value && (strcmp(value, "true") == 0 || strcmp(value, "false") == 0)) ? 0 : -1;
}

int index_find(const char *id, void *out, int max)
{
   (void)id;
   (void)out;
   (void)max;
   return 0;
}

int index_blast_radius(const char *p, const char *f, void *out)
{
   (void)p;
   (void)f;
   (void)out;
   return -1;
}

typedef struct
{
   char name[128];
   char root[MAX_PATH_LEN];
   char scanned_at[32];
} test_project_info_t;

int canonical_index_list_projects(void *out, int max)
{
   if (max < 1)
      return 0;
   test_project_info_t *projects = (test_project_info_t *)out;
   snprintf(projects[0].name, sizeof(projects[0].name), "proj-alpha");
   snprintf(projects[0].root, sizeof(projects[0].root), "/repo/proj-alpha");
   snprintf(projects[0].scanned_at, sizeof(projects[0].scanned_at), "2026-05-26 00:00:00");
   return 1;
}

typedef struct
{
   char project[128];
   char file_path[MAX_PATH_LEN];
   int line;
   int line_end;
   char kind[32];
} test_term_hit_t;

static char g_code_find_project[128];
static int g_code_local_first_fixture;
static int g_code_hybrid_path_collision_fixture;
static int g_code_context_memory_scope_rank = 3;
static int g_code_context_memory_anchored = 1;

int canonical_index_find(const char *identifier, void *out, int max)
{
   assert(identifier);
   assert(out);
   if (strcmp(identifier, "foo") != 0 || max < 1)
      return 0;
   test_term_hit_t *hits = (test_term_hit_t *)out;
   if (g_code_local_first_fixture)
   {
      snprintf(hits[0].project, sizeof(hits[0].project), "proj-other");
      snprintf(hits[0].file_path, sizeof(hits[0].file_path), "other/high.c");
      hits[0].line = 1;
      hits[0].line_end = 2;
      snprintf(hits[0].kind, sizeof(hits[0].kind), "function");
      return 1;
   }
   snprintf(hits[0].project, sizeof(hits[0].project), "proj-alpha");
   snprintf(hits[0].file_path, sizeof(hits[0].file_path), "src/main.c");
   hits[0].line = 12;
   hits[0].line_end = 20;
   snprintf(hits[0].kind, sizeof(hits[0].kind), "function");
   return 1;
}

int canonical_index_find_project(const char *project, const char *identifier, void *out, int max)
{
   snprintf(g_code_find_project, sizeof(g_code_find_project), "%s", project ? project : "");
   if (g_code_local_first_fixture && project && strcmp(project, "proj-alpha") == 0 && max > 0)
   {
      test_term_hit_t *hits = (test_term_hit_t *)out;
      memset(&hits[0], 0, sizeof(hits[0]));
      snprintf(hits[0].project, sizeof(hits[0].project), "proj-alpha");
      snprintf(hits[0].file_path, sizeof(hits[0].file_path), "local/first.c");
      hits[0].line = 7;
      hits[0].line_end = 9;
      snprintf(hits[0].kind, sizeof(hits[0].kind), "function");
      return 1;
   }
   return canonical_index_find(identifier, out, max);
}

int canonical_index_find_excluding_project(const char *excluded_project, const char *identifier,
                                           void *out, int max)
{
   assert(excluded_project && strcmp(excluded_project, "proj-alpha") == 0);
   return canonical_index_find(identifier, out, max);
}

int db2_code_index_project_current_generation(const char *project, int64_t *generation_out)
{
   if (!project || !project[0])
      return -2;
   if (generation_out)
      *generation_out = 2;
   return 0;
}

typedef struct
{
   char provenance[48];
   char confidence[16];
   char project[128];
   long long generation;
   char freshness[16];
} test_blast_meta_t;

typedef struct
{
   char file[MAX_PATH_LEN];
   char dependents[64][MAX_PATH_LEN];
   int dependent_count;
   char dependencies[64][MAX_PATH_LEN];
   int dependency_count;
   char project[128];
   long long generation;
   char freshness[16];
   int resolved;
   test_blast_meta_t dependent_meta[64];
   test_blast_meta_t dependency_meta[64];
} test_blast_radius_t;

int canonical_index_blast_radius(const char *project, const char *file_path, void *out)
{
   assert(project);
   assert(file_path);
   assert(out);
   if (strcmp(project, "proj-alpha") != 0 || strcmp(file_path, "src/main.c") != 0)
      return -1;
   test_blast_radius_t *br = (test_blast_radius_t *)out;
   snprintf(br->file, sizeof(br->file), "src/main.c");
   snprintf(br->dependents[0], sizeof(br->dependents[0]), "src/app.c");
   br->dependent_count = 1;
   snprintf(br->dependencies[0], sizeof(br->dependencies[0]), "src/lib.c");
   br->dependency_count = 1;
   snprintf(br->project, sizeof(br->project), "proj-alpha");
   br->generation = 9;
   snprintf(br->freshness, sizeof(br->freshness), "current");
   br->resolved = 1;
   snprintf(br->dependent_meta[0].provenance, sizeof(br->dependent_meta[0].provenance), "import");
   snprintf(br->dependent_meta[0].confidence, sizeof(br->dependent_meta[0].confidence), "high");
   snprintf(br->dependent_meta[0].project, sizeof(br->dependent_meta[0].project), "proj-alpha");
   br->dependent_meta[0].generation = 9;
   snprintf(br->dependent_meta[0].freshness, sizeof(br->dependent_meta[0].freshness), "current");
   br->dependency_meta[0] = br->dependent_meta[0];
   return 0;
}

typedef struct
{
   char name[128];
   char kind[32];
   int line;
   int line_end;
} test_definition_t;

int canonical_index_structure(const char *project, const char *file_path, void *out, int max)
{
   assert(project);
   assert(file_path);
   test_definition_t *defs = (test_definition_t *)out;
   if (strcmp(project, "proj-alpha") != 0 || strcmp(file_path, "src/main.c") != 0)
      return 0;
   if (max < 1)
      return 0;
   snprintf(defs[0].name, sizeof(defs[0].name), "main");
   snprintf(defs[0].kind, sizeof(defs[0].kind), "function");
   defs[0].line = 12;
   defs[0].line_end = 20;
   return 1;
}

int canonical_index_project_stats(const char *project, int *files_out, int *defs_out)
{
   assert(project);
   if (strcmp(project, "proj-alpha") != 0)
      return -1;
   if (files_out)
      *files_out = 9;
   if (defs_out)
      *defs_out = 4;
   return 0;
}

int canonical_index_project_lang_breakdown(const char *project, char *buf, size_t bufsz)
{
   assert(project);
   assert(buf);
   if (strcmp(project, "proj-alpha") != 0)
      return -1;
   snprintf(buf, bufsz, "[{\"lang\":\"c\",\"count\":7},{\"lang\":\"h\",\"count\":2}]");
   return 0;
}

/* Must mirror code_search_hit_t (index.h) exactly (handler casts the out buffer to it). */
typedef struct
{
   char project[128];
   char file_path[MAX_PATH_LEN];
   char snippet[512];
   double rank;
   char content_hash[80];
   int line;
} test_code_search_hit_t;

int canonical_index_code_search(const char *query, const char *project, void *out, int max,
                                int enrich)
{
   assert(query);
   assert(out);
   if (strcmp(query, "needle") != 0)
      return 0;
   if (max < 1)
      return 0;
   test_code_search_hit_t *hits = (test_code_search_hit_t *)out;
   if (g_code_hybrid_path_collision_fixture)
   {
      snprintf(hits[0].project, sizeof(hits[0].project), "%s",
               project ? "proj-alpha" : "proj-other");
      snprintf(hits[0].file_path, sizeof(hits[0].file_path), "src/main.c");
      snprintf(hits[0].snippet, sizeof(hits[0].snippet), "%s",
               project ? "local main" : "other main");
      hits[0].rank = project ? 0.5 : 0.9;
      return 1;
   }
   if (g_code_local_first_fixture && !project)
   {
      snprintf(hits[0].project, sizeof(hits[0].project), "proj-other");
      snprintf(hits[0].file_path, sizeof(hits[0].file_path), "other/high.c");
      snprintf(hits[0].snippet, sizeof(hits[0].snippet), "other ranked first");
      hits[0].rank = 0.99;
      return 1;
   }
   if (!project || strcmp(project, "proj-alpha") != 0)
      return 0;
   snprintf(hits[0].project, sizeof(hits[0].project), "proj-alpha");
   snprintf(hits[0].file_path, sizeof(hits[0].file_path), "src/search.c");
   snprintf(hits[0].snippet, sizeof(hits[0].snippet), "int needle(void) { return 1; }");
   hits[0].rank = 0.75;
   snprintf(hits[0].content_hash, sizeof(hits[0].content_hash), "deadbeefcafe");
   hits[0].line = enrich ? 17 : 0;
   return 1;
}

int canonical_index_code_search_excluding_project(const char *query, const char *excluded_project,
                                                  void *out, int max, int enrich)
{
   assert(excluded_project && strcmp(excluded_project, "proj-alpha") == 0);
   return canonical_index_code_search(query, NULL, out, max, enrich);
}

/* canonical_index_find_callers stub lives in the _code.inc (line-count limit). */

int memory_get_entity_profile(const char *e, void *out)
{
   (void)e;
   (void)out;
   return -1;
}

int memory_search_graph(const char *q, int l, void *out, int m)
{
   (void)q;
   (void)l;
   (void)out;
   (void)m;
   return 0;
}

int db2_artifact_read(const char *id, void *out, void *c, int mc, int *cc)
{
   (void)id;
   (void)out;
   (void)c;
   (void)mc;
   if (cc)
      *cc = 0;
   return -1;
}

int db2_artifact_links_read(const char *id, void *out, int m)
{
   (void)id;
   (void)out;
   (void)m;
   return 0;
}

static db2_kb_service_async_queue_stats_t g_queue_status = {
    .pending = 4, .running = 2, .done = 7, .failed = 1, .total = 14};
static db2_kb_service_async_queue_stats_t g_drain_status = {
    .pending = 1, .running = 0, .done = 10, .failed = 1, .total = 12, .processed = 3};
static char g_drain_embed_cmd[64];
static int g_drain_timeout;
static const char *g_drain_collection;
static int g_drain_rc;
static int g_job_get_rc = 1;
static int64_t g_job_get_id;
static int g_db_initialized = 1;
static int g_pgvec_ensure_rc;
static int g_kb_build_rc;
static char g_kb_build_path[256];
static char g_kb_build_project[256];
static char g_kb_build_embed_cmd[64];
static int g_kb_build_force;
static int g_code_embed_refresh_rc;
static int g_code_embed_estimated;
static int g_code_embed_embedded;
static int g_code_embed_skipped;
static char g_code_embed_project[256];
static int g_doc_refresh_rc;
static int g_doc_backfill_rc;
static char g_doc_refresh_project[256];
static int g_kb_update_rc;
static char g_kb_update_path[256];
static char g_kb_update_project[256];
static char g_kb_update_embed_cmd[64];
static int g_code_scan_rc = 5;
static int g_code_scan_inspected = 12;
static char g_code_scan_project[256];
static char g_code_scan_root_path[256];
static int g_code_scan_force;
static int g_code_scan_files_rc = 2;
static int g_code_scan_files_inspected = 2;
static int g_code_scan_files_count;
static char g_code_scan_file_project[256];
static char g_code_scan_file_root[256];
static char g_code_scan_file_rel[256];
static char g_code_scan_file_content[512];
static int g_code_scan_files_force;
static int g_runtime_state_set_now;
static int g_curator_docs_queued;
static int g_curator_code_queued;
static int g_discover_count = 1;
static char g_ingest_workspace[256];
static char g_ingest_project[256];
static char g_ingest_root[256];
static int g_ingest_force;
static int g_claim_rc;
static int g_claim_has_job = 1;
static int64_t g_complete_job_id;
static int g_complete_files;
static int g_complete_chunks;
static int g_complete_embeddings;
static int64_t g_fail_job_id;
static char g_fail_error[256];
static char g_snapshot_project[256];
static char g_delete_project[256];
static char g_delete_path[256];
static int g_delete_ids_called;
static int g_delete_point_count;
static int g_vector_index_remove_count;
static int g_documents_delete_count;
static int g_insert_chunk_count;
static int g_link_count;
static char g_file_index_project[256];
static char g_file_index_path[256];
static char g_file_index_hash[256];
static char g_file_index_content[256];
static int g_file_index_upsert_count;
static int g_batch_upsert_count;
static int g_vector_index_record_count;
static int g_worker_notify_count;
static int g_clear_deleted = 12;
static char g_clear_project[256];
static int g_reconcile_dry_run;
static int g_reconcile_rc;
static kb_service_ctx_t g_test_kb_ctx = {.worker_count = 2};
kb_service_ctx_t *g_kb_ctx = &g_test_kb_ctx;

int kb_dispatch_action_json(const char *action, const char *body, int body_len, char *out_buf,
                            int out_cap)
{
   assert(strcmp(action, "memory.directive_sweep_expired") == 0);
   assert(body && body_len == 2);
   snprintf(out_buf, (size_t)out_cap, "{\"status\":\"ok\",\"expired\":1}");
   return 200;
}

char *kb_service_workers_json(kb_service_ctx_t *ctx)
{
   (void)ctx;
   return strdup("{\"status\":\"ok\",\"configured\":2,"
                 "\"slots\":[{\"index\":0,\"active\":true,\"method\":\"v1.http\","
                 "\"elapsed_secs\":3},{\"index\":1,\"active\":false}],"
                 "\"threads\":[{\"name\":\"maintenance-timer\",\"active\":true,"
                 "\"state\":\"sleeping\"}]}");
}

int db2_kb_service_async_queue_status(db2_kb_service_async_queue_stats_t *out)
{
   if (!out)
      return -1;
   *out = g_queue_status;
   return 0;
}

int db2_kb_service_async_queue_drain(const char *embedding_cmd, int timeout_secs,
                                     const char *vector_collection,
                                     db2_kb_service_vector_upsert_fn vector_upsert,
                                     void *vector_upsert_ctx,
                                     db2_kb_service_async_queue_stats_t *out)
{
   (void)vector_upsert;
   (void)vector_upsert_ctx;
   snprintf(g_drain_embed_cmd, sizeof(g_drain_embed_cmd), "%s", embedding_cmd);
   g_drain_timeout = timeout_secs;
   g_drain_collection = vector_collection;
   if (g_drain_rc != 0)
      return g_drain_rc;
   if (out)
      *out = g_drain_status;
   return 0;
}

int db2_kb_service_async_job_get(int64_t job_id, db2_kb_service_async_job_t *out)
{
   g_job_get_id = job_id;
   if (g_job_get_rc <= 0)
      return g_job_get_rc;
   out->id = job_id;
   out->document_id = 77;
   snprintf(out->kind, sizeof(out->kind), "embed_raw");
   snprintf(out->project, sizeof(out->project), "proj-alpha");
   snprintf(out->status, sizeof(out->status), "running");
   out->attempts = 2;
   snprintf(out->last_error, sizeof(out->last_error), "retry \"later\"");
   snprintf(out->claimed_by, sizeof(out->claimed_by), "worker-1");
   snprintf(out->claimed_at, sizeof(out->claimed_at), "2026-05-25 18:00:00");
   snprintf(out->created_at, sizeof(out->created_at), "2026-05-25 17:59:00");
   snprintf(out->updated_at, sizeof(out->updated_at), "2026-05-25 18:01:00");
   return 1;
}

int db2_is_initialized(void)
{
   return g_db_initialized;
}

int pgvec_kb_service_ensure_kb_collection(int dim)
{
   /* The route now sizes the collection at the deployment's runtime embedding
    * dim (db2_embedding_dim(), i.e. g_test_embedding_dim here), not a hardcoded
    * 384, so it matches the halfvec(__EMBED_DIM__) column. */
   assert(dim == g_test_embedding_dim);
   return g_pgvec_ensure_rc;
}

int kb_build(const char *root_path, const char *project, const char *embedding_cmd,
             int force_rebuild, kb_stats_t *stats_out)
{
   snprintf(g_kb_build_path, sizeof(g_kb_build_path), "%s", root_path);
   snprintf(g_kb_build_project, sizeof(g_kb_build_project), "%s", project);
   snprintf(g_kb_build_embed_cmd, sizeof(g_kb_build_embed_cmd), "%s", embedding_cmd);
   g_kb_build_force = force_rebuild;
   if (stats_out)
   {
      stats_out->files_scanned = 9;
      stats_out->files_indexed = 7;
      stats_out->files_skipped = 1;
      stats_out->files_removed = 2;
      stats_out->chunks_added = 11;
      stats_out->chunks_removed = 3;
      stats_out->embeddings_added = 5;
   }
   return g_kb_build_rc;
}

int kb_code_embed_refresh(const char *project, const char *scope, const char **paths,
                          int path_count, int batch_size, int max_points, int dry_run,
                          kb_code_embed_result_t *out)
{
   (void)scope;
   (void)paths;
   (void)path_count;
   (void)batch_size;
   (void)max_points;
   (void)dry_run;
   snprintf(g_code_embed_project, sizeof(g_code_embed_project), "%s", project);
   memset(out, 0, sizeof(*out));
   out->estimated_points = g_code_embed_estimated;
   out->embedded = g_code_embed_embedded;
   out->skipped_unchanged = g_code_embed_skipped;
   return g_code_embed_refresh_rc;
}

int kb_doc_refresh(const char *project, const char *embedding_cmd, int max_docs)
{
   (void)embedding_cmd;
   assert(max_docs == 200);
   snprintf(g_doc_refresh_project, sizeof(g_doc_refresh_project), "%s", project);
   return g_doc_refresh_rc;
}

int kb_doc_embed_backfill(const char *project, const char *embedding_cmd, int max_chunks)
{
   (void)embedding_cmd;
   assert(max_chunks == 200);
   assert(strcmp(g_doc_refresh_project, project) == 0);
   return g_doc_backfill_rc;
}

int kb_update(const char *root_path, const char *project, const char *embedding_cmd,
              kb_stats_t *stats_out)
{
   snprintf(g_kb_update_path, sizeof(g_kb_update_path), "%s", root_path);
   snprintf(g_kb_update_project, sizeof(g_kb_update_project), "%s", project);
   snprintf(g_kb_update_embed_cmd, sizeof(g_kb_update_embed_cmd), "%s", embedding_cmd);
   if (stats_out)
   {
      stats_out->files_scanned = 8;
      stats_out->files_indexed = 6;
      stats_out->files_skipped = 1;
      stats_out->files_removed = 0;
      stats_out->chunks_added = 10;
      stats_out->chunks_removed = 2;
      stats_out->embeddings_added = 4;
   }
   return g_kb_update_rc;
}

int canonical_index_scan_project(const char *name, const char *root, int force, int *inspected_out)
{
   snprintf(g_code_scan_project, sizeof(g_code_scan_project), "%s", name ? name : "");
   snprintf(g_code_scan_root_path, sizeof(g_code_scan_root_path), "%s", root ? root : "");
   g_code_scan_force = force;
   if (inspected_out)
      *inspected_out = g_code_scan_inspected;
   return g_code_scan_rc;
}

int canonical_index_scan_files(const char *name, const char *root_label,
                               const canonical_index_file_input_t *files, int file_count, int force,
                               int *inspected_out)
{
   snprintf(g_code_scan_file_project, sizeof(g_code_scan_file_project), "%s", name ? name : "");
   snprintf(g_code_scan_file_root, sizeof(g_code_scan_file_root), "%s",
            root_label ? root_label : "");
   g_code_scan_files_force = force;
   g_code_scan_files_count = file_count;
   if (files && file_count > 0)
   {
      snprintf(g_code_scan_file_rel, sizeof(g_code_scan_file_rel), "%s",
               files[0].rel_path ? files[0].rel_path : "");
      snprintf(g_code_scan_file_content, sizeof(g_code_scan_file_content), "%s",
               files[0].content ? files[0].content : "");
   }
   if (inspected_out)
      *inspected_out = g_code_scan_files_inspected;
   return g_code_scan_files_rc;
}

int db2_kb_runtime_state_set_now(const char *key)
{
   if (key && strcmp(key, "last_ingest_at") == 0)
      g_runtime_state_set_now++;
   return 0;
}

void kb_curator_queue_docs_for_project(const char *project)
{
   (void)project;
   g_curator_docs_queued++;
}

void kb_curator_queue_code_units_for_project(const char *project, const char *root_path)
{
   (void)project;
   (void)root_path;
   g_curator_code_queued++;
}

/* §2c: flips kb.reembed_on_dim_change for the /v1/reembed 403/proceed gate test. */
static int g_test_reembed_enabled = 0;
static double g_precision_floor = 0.0; /* §4 surprising self-suppress floor (0 = off) */
int config_load(config_t *cfg)
{
   memset(cfg, 0, sizeof(*cfg));
   cfg->kb_reembed_on_dim_change = g_test_reembed_enabled;
   cfg->kb_curator_extract_docs_enabled = 1;
   cfg->kb_curator_extract_code_enabled = 1;
   cfg->demotion_enabled = 1;
   cfg->demotion_n_min = 2;
   cfg->demotion_window = 64;
   cfg->demotion_half_life_days = 30.0;
   cfg->workspace_count = 1;
   snprintf(cfg->workspaces[0], sizeof(cfg->workspaces[0]), "/workspace");
   /* §5 hybrid RRF weights: mirror config_set_defaults so /v1/code/hybrid fuses
    * both signals at equal weight (a 0 weight would disable a signal). */
   cfg->code_hybrid_weight_code = 1.0;
   cfg->code_hybrid_weight_graph = 1.0;
   cfg->code_hybrid_weight_vector = 1.0;
   cfg->code_hybrid_weight_memory = 1.0;
   cfg->code_hybrid_rrf_k = 60.0;
   cfg->code_surprising_precision_floor = g_precision_floor;
   return 0;
}

/* Accessor stubs: the production seam moved from config_load to per-field
 * accessors. These return exactly what the stub above puts in the struct, so
 * the assertions below are unchanged. */
int config_present(void)
{
   return 1; /* the config_load stub above always succeeds */
}

/* Ingress compression gate: memset-0 in the struct the stub above fills. */
int config_ingress_compress_enabled(void)
{
   return 0;
}

/* §4 surprising-links precision floor and judge command. The struct above leaves
 * both at zero/empty: floor <= 0 disables self-suppress, and an empty judge
 * command is what the hermetic judge stub below expects. */
double config_code_surprising_precision_floor(void)
{
   return g_precision_floor;
}

size_t config_kb_curator_judge_command_copy(char *out, size_t n)
{
   if (out && n)
      out[0] = '\0';
   return n;
}

/* §2c reembed-on-dim-change gate: mirrors the struct the stub above fills, so a
 * case that flips g_test_reembed_enabled moves both seams together. */
int config_kb_reembed_on_dim_change(void)
{
   return g_test_reembed_enabled;
}

/* §5 hybrid RRF weights + rank constant, mirroring the struct above. */
double config_code_hybrid_weight_code(void)
{
   return 1.0;
}

double config_code_hybrid_weight_graph(void)
{
   return 1.0;
}

double config_code_hybrid_weight_vector(void)
{
   return 1.0;
}

double config_code_hybrid_weight_memory(void)
{
   return 1.0;
}

double config_code_hybrid_rrf_k(void)
{
   return 60.0;
}

int config_code_trust_actuation_enabled(void)
{
   return 0; /* §3 actuation gate: memset-0 in the struct above */
}

int config_kb_curator_extract_docs_enabled(void)
{
   return 1;
}

int config_workspace_count(void)
{
   return 1;
}

const char *config_workspaces(int index)
{
   return index == 0 ? "/workspace" : "";
}

const char *config_embedder_command(const config_t *cfg, const char *requested)
{
   if (requested && requested[0])
      return requested;
   if (cfg && cfg->embedder_command[0])
      return cfg->embedder_command;
   const char *url = getenv("EMBEDDER_URL");
   if (url && url[0])
      return url;
   url = getenv("SYNTHESIS_ENDPOINT");
   if (url && url[0])
      return url;
   return ""; /* nothing selected: no embedder, no fabricated name */
}

/* The config_t-free form callers use now. Same resolution order, minus the
 * struct the caller no longer holds: request > env > nothing. */
const char *config_embedder_command_current(const char *requested)
{
   return config_embedder_command(NULL, requested);
}

/* kb_intel_payload reads the demotion knobs through accessors now. Mirror the
 * values the config_load stub above sets, so both seams agree. */
int config_demotion_enabled(void)
{
   return 1;
}
int config_demotion_n_min(void)
{
   return 2;
}
int config_demotion_window(void)
{
   return 64;
}
double config_demotion_half_life_days(void)
{
   return 30.0;
}

int db2_calibration_surfaces_with_data(int min_rows)
{
   assert(min_rows == 200);
   return 2;
}

/* kb_intel_payload.o now calls the ranker-fit surface; stub it (this test
 * exercises HTTP routing, not the fitter — the fitter has its own unit test). */
char *kb_ranker_export_view_json(const char *subject_kind, const char *feature_set_version)
{
   (void)subject_kind;
   (void)feature_set_version;
   return strdup("{\"status\":\"ok\",\"n_rows\":0,\"rows\":[]}");
}

int kb_ranker_fit_run(char *id_out, int id_out_len, char **report_out)
{
   if (id_out && id_out_len > 0)
      id_out[0] = '\0';
   if (report_out)
      *report_out = strdup("{\"status\":\"disabled\"}");
   return 1;
}

int db2_demotion_candidates(int n_min, db2_demotion_candidate_t *out, int max)
{
   assert(n_min == 2);
   assert(out != NULL);
   assert(max >= 2);
   out[0].row_id = 101;
   out[1].row_id = 102;
   return 2;
}

double db2_demotion_score(int64_t row_id, int window_size, double half_life_days, int n_min)
{
   assert(window_size == 64);
   assert(half_life_days == 30.0);
   assert(n_min == 2);
   return row_id == 101 ? 0.20 : 0.80;
}

int db2_memory_get(int64_t memory_id, memory_t *out)
{
   assert(out != NULL);
   memset(out, 0, sizeof(*out));
   out->id = memory_id;
   snprintf(out->kind, sizeof(out->kind), "%s", "fact");
   return 0;
}

int db2_demotion_profile_read(const char *memory_class, const char *scope_kind,
                              const char *scope_id, char *buf, size_t len)
{
   assert(strcmp(memory_class, "fact") == 0);
   assert(strcmp(scope_kind, "global") == 0);
   assert(strcmp(scope_id, "") == 0);
   snprintf(buf, len, "{\"score_percentiles\":{\"p10\":0.5}}");
   return 0;
}

/* kb_intel_payload's bandit.sample/close builders call these (kb_bandit.o unlinked):
 * stub sample as "disabled", reward as a no-op success. */
int kb_bandit_sample(const char *decision_point, const char *context_json,
                     const char (*arm_ids)[KB_BANDIT_MAX_ARM_ID], int n_arms, char *decision_id_out)
{
   (void)decision_point;
   (void)context_json;
   (void)arm_ids;
   (void)n_arms;
   if (decision_id_out)
      decision_id_out[0] = '\0';
   return -1;
}
int kb_bandit_reward(const char *decision_point, const char *decision_id, const char *arm_id,
                     double reward)
{
   (void)decision_point;
   (void)decision_id;
   (void)arm_id;
   (void)reward;
   return 0;
}

int db2_bandit_promotion_get(const char *decision_point, char *arm_out, size_t arm_out_len)
{
   (void)decision_point;
   if (arm_out && arm_out_len)
      arm_out[0] = '\0';
   return -1; /* no promotion in tests */
}
int db2_bandit_promotion_set(const char *decision_point, const char *arm_id,
                             const char *rollback_arm)
{
   (void)decision_point;
   (void)arm_id;
   (void)rollback_arm;
   return 0;
}

int db2_bandit_decision_points_list(char *buf, size_t len)
{
   /* The export asks the log which points exist; return the production-sampled one. */
   snprintf(buf, len, "[\"kb_memory_retrieval_limit\"]");
   return 0;
}

int db2_bandit_arms_list(const char *decision_point, char *buf, size_t len)
{
   assert(strcmp(decision_point, "kb_memory_retrieval_limit") == 0);
   snprintf(buf, len, "[\"10\"]");
   return 0;
}

int db2_bandit_decisions_export(const char *decision_point, int limit, char *buf, size_t len)
{
   assert(strcmp(decision_point, "kb_memory_retrieval_limit") == 0);
   assert(limit == 500);
   snprintf(buf, len, "[{\"id\":\"decision-1\",\"arm_id\":\"10\"}]");
   return 0;
}

int kb_bandit_record_replay_evidence(const char *decision_point, const char *result_json,
                                     char *id_out, size_t id_out_len)
{
   (void)decision_point;
   (void)result_json;
   if (id_out && id_out_len > 0)
      snprintf(id_out, id_out_len, "stub-artifact-id-1");
   return 0;
}

int db2_bandit_arm_stats_read(const char *decision_point, const char *arm_id,
                              db2_bandit_arm_stats_t *out)
{
   assert(strcmp(decision_point, "kb_memory_retrieval_limit") == 0);
   assert(out != NULL);
   memset(out, 0, sizeof(*out));
   if (strcmp(arm_id, "10") == 0)
   {
      out->n_decisions = 3;
      out->n_rewards = 2;
      out->sum_reward = 1.5;
      out->posterior_alpha = 2.5;
      out->posterior_beta = 1.5;
   }
   return 0;
}

int workspace_discover_projects(const char *root, int max_depth, char projects[][MAX_PATH_LEN],
                                int max_projects)
{
   (void)max_depth;
   if (!root || !projects || max_projects <= 0)
      return 0;
   snprintf(projects[0], MAX_PATH_LEN, "%s/proj-alpha", root);
   return g_discover_count;
}

int workspace_repo_index_keys(const char *root, const char *fallback_workspace, char *name_out,
                              size_t name_len, char *ws_out, size_t ws_len)
{
   (void)root;
   if (!name_out || name_len == 0 || !ws_out || ws_len == 0)
      return -1;
   snprintf(name_out, name_len, "proj-alpha");
   snprintf(ws_out, ws_len, "%s", fallback_workspace ? fallback_workspace : "");
   return 0;
}

/* Captured so a route test can assert the priority it enqueued at. */
static int g_ingest_priority = -1;

int db2_kb_ingest_queue_enqueue(const char *project, const char *root_path, const char *workspace,
                                int force, int priority)
{
   g_ingest_priority = priority;
   snprintf(g_ingest_project, sizeof(g_ingest_project), "%s", project);
   snprintf(g_ingest_root, sizeof(g_ingest_root), "%s", root_path);
   snprintf(g_ingest_workspace, sizeof(g_ingest_workspace), "%s", workspace);
   g_ingest_force = force;
   return 0;
}

int db2_kb_ingest_queue_claim_next(db2_kb_ingest_job_t *out)
{
   if (g_claim_rc < 0)
      return g_claim_rc;
   if (!g_claim_has_job)
      return 0;
   out->id = 42;
   snprintf(out->project, sizeof(out->project), "aimee");
   snprintf(out->root_path, sizeof(out->root_path), "/repo");
   snprintf(out->workspace, sizeof(out->workspace), "/workspace");
   out->force = 1;
   return 1;
}

int db2_kb_ingest_queue_complete(int64_t job_id, int files_indexed, int chunks_added,
                                 int embeddings_added)
{
   g_complete_job_id = job_id;
   g_complete_files = files_indexed;
   g_complete_chunks = chunks_added;
   g_complete_embeddings = embeddings_added;
   return 0;
}

int db2_kb_ingest_queue_fail(int64_t job_id, const char *error_message)
{
   g_fail_job_id = job_id;
   snprintf(g_fail_error, sizeof(g_fail_error), "%s", error_message);
   return 0;
}

cJSON *db2_kb_file_index_snapshot_json(const char *project)
{
   snprintf(g_snapshot_project, sizeof(g_snapshot_project), "%s", project ? project : "");
   cJSON *arr = cJSON_CreateArray();
   cJSON *file = cJSON_CreateObject();
   cJSON_AddStringToObject(file, "rel_path", "src/a.c");
   cJSON_AddStringToObject(file, "hash", "hash-a");
   cJSON_AddItemToArray(arr, file);
   return arr;
}

int db2_kb_documents_list_chunk_ids_for_file(const char *project, const char *file_path,
                                             int64_t *out, int max)
{
   snprintf(g_delete_project, sizeof(g_delete_project), "%s", project ? project : "");
   snprintf(g_delete_path, sizeof(g_delete_path), "%s", file_path ? file_path : "");
   g_delete_ids_called++;
   if (out && max >= 2)
   {
      out[0] = 101;
      out[1] = 102;
      return 2;
   }
   return 0;
}

int pgvec_kb_vector_delete_point(int64_t point_id)
{
   (void)point_id;
   g_delete_point_count++;
   return 0;
}

void db2_vector_index_op_remove(int64_t point_id)
{
   (void)point_id;
   g_vector_index_remove_count++;
}

void db2_kb_documents_delete_for_file(const char *project, const char *file_path)
{
   (void)project;
   (void)file_path;
   g_documents_delete_count++;
}

int64_t db2_kb_documents_insert_chunk(const char *project, const char *file_path,
                                      const char *file_hash, int chunk_index,
                                      const char *heading_path, int line_start, int line_end,
                                      const char *content, int token_count)
{
   (void)project;
   (void)file_path;
   (void)file_hash;
   (void)chunk_index;
   (void)heading_path;
   (void)line_start;
   (void)line_end;
   (void)content;
   (void)token_count;
   return 1000 + (++g_insert_chunk_count);
}

void db2_kb_documents_link_neighbours(int64_t doc_id, int64_t prev_id)
{
   (void)doc_id;
   (void)prev_id;
   g_link_count++;
}

int db2_kb_file_index_upsert(const char *project, const char *file_path, const char *file_hash,
                             const char *content)
{
   snprintf(g_file_index_project, sizeof(g_file_index_project), "%s", project ? project : "");
   snprintf(g_file_index_path, sizeof(g_file_index_path), "%s", file_path ? file_path : "");
   snprintf(g_file_index_hash, sizeof(g_file_index_hash), "%s", file_hash ? file_hash : "");
   snprintf(g_file_index_content, sizeof(g_file_index_content), "%s", content ? content : "");
   g_file_index_upsert_count++;
   return 0;
}

int pgvec_kb_vector_delete_project(const char *project)
{
   (void)project;
   return 0;
}

int pgvec_kb_vector_delete_current_project(const char *project)
{
   return pgvec_kb_vector_delete_project(project);
}

int db2_kb_file_index_delete_project(const char *project)
{
   (void)project;
   return 0;
}

int db2_kb_file_index_delete_current_project(const char *project)
{
   return db2_kb_file_index_delete_project(project);
}

/* ── slice-2 purge-route stubs: fence store + fan-out delete primitives ── */

static int g_fence_present = 0;
static int g_fence_live = 0;
static int g_fence_write_rc = 0;
static char g_fence_gen[128] = "";
static char g_fence_pid[128] = "";
static char g_fence_project[256] = "";
static int g_fence_heartbeats = 0;

int db2_kb_purge_fence_write(const char *project, const char *generation, const char *purge_id)
{
   if (g_fence_write_rc)
      return -1;
   snprintf(g_fence_project, sizeof(g_fence_project), "%s", project ? project : "");
   snprintf(g_fence_gen, sizeof(g_fence_gen), "%s", generation ? generation : "");
   snprintf(g_fence_pid, sizeof(g_fence_pid), "%s", purge_id ? purge_id : "");
   g_fence_present = 1;
   g_fence_live = 1;
   return 0;
}

/* Mirrors the real acquire's atomic read-decide-write against the in-memory
 * fence: refuse a live foreign fence without takeover, else publish. */
int db2_kb_purge_fence_acquire(const char *project, const char *generation, const char *purge_id,
                               int takeover, char *cur_gen, size_t gen_cap, char *cur_pid,
                               size_t pid_cap, int *replaced_out)
{
   if (cur_gen && gen_cap)
      snprintf(cur_gen, gen_cap, "%s", g_fence_gen);
   if (cur_pid && pid_cap)
      snprintf(cur_pid, pid_cap, "%s", g_fence_pid);
   if (replaced_out)
      *replaced_out = 0;
   if (g_fence_write_rc)
      return -1;
   int same = g_fence_present && strcmp(g_fence_gen, generation) == 0 &&
              strcmp(g_fence_pid, purge_id) == 0;
   if (g_fence_present && g_fence_live && !same && !takeover)
      return 0;
   if (replaced_out)
      *replaced_out = (g_fence_present && !same);
   (void)db2_kb_purge_fence_write(project, generation, purge_id);
   return 1;
}

int db2_kb_purge_fence_read(const char *project, char *gen_out, size_t gen_cap, char *pid_out,
                            size_t pid_cap, int *live_out)
{
   (void)project;
   if (live_out)
      *live_out = 0;
   if (!g_fence_present)
      return 0;
   if (gen_out && gen_cap)
      snprintf(gen_out, gen_cap, "%s", g_fence_gen);
   if (pid_out && pid_cap)
      snprintf(pid_out, pid_cap, "%s", g_fence_pid);
   if (live_out)
      *live_out = g_fence_live;
   return 1;
}

int db2_kb_purge_fence_heartbeat(const char *project, const char *generation, const char *purge_id)
{
   (void)project;
   if (!g_fence_present || !generation || !purge_id || strcmp(g_fence_gen, generation) != 0 ||
       strcmp(g_fence_pid, purge_id) != 0)
      return 0;
   g_fence_heartbeats++;
   return 1;
}

int db2_kb_purge_fence_clear(const char *project, const char *generation, const char *purge_id)
{
   (void)project;
   if (!g_fence_present || !generation || !purge_id || strcmp(g_fence_gen, generation) != 0 ||
       strcmp(g_fence_pid, purge_id) != 0)
      return 0;
   g_fence_present = 0;
   g_fence_gen[0] = g_fence_pid[0] = '\0';
   return 1;
}

static int g_purge_code_embeddings_rc = 7;
static int g_purge_curator_vectors_rc = 3;
static int g_purge_canonical_rc = 1;
static int g_purge_code_unit_jobs_rc = 4;
static int g_purge_pdf_rc = 0;
static int g_purge_minhash_rc = 0;

int pgvec_code_delete_project(const char *project)
{
   (void)project;
   return g_purge_code_embeddings_rc;
}

int pgvec_curator_code_unit_delete_project(const char *project)
{
   (void)project;
   return g_purge_curator_vectors_rc;
}

int db2_code_index_project_delete(const char *name)
{
   (void)name;
   return g_purge_canonical_rc;
}

int kb_curator_code_unit_jobs_delete_project(const char *project)
{
   (void)project;
   return g_purge_code_unit_jobs_rc;
}

int pgvec_kbpdf_delete_project(const char *project)
{
   (void)project;
   return g_purge_pdf_rc;
}

int db2_sketch_minhash_signature_delete_project(const char *project)
{
   (void)project;
   return g_purge_minhash_rc;
}

void kb_worker_notify(kb_service_ctx_t *ctx)
{
   (void)ctx;
   g_worker_notify_count++;
}

int db2_kb_service_clear_project(const char *project)
{
   snprintf(g_clear_project, sizeof(g_clear_project), "%s", project);
   return g_clear_deleted;
}

int db2_kb_service_clear_current_project(const char *project)
{
   return db2_kb_service_clear_project(project);
}

int db2_kb_service_memory_record_exists(int64_t record_id)
{
   (void)record_id;
   return 1;
}

int db2_kb_service_kb_document_exists(int64_t document_id)
{
   (void)document_id;
   return 1;
}

int pgvec_kb_service_reconcile_orphans(pgvec_kb_service_record_exists_fn mem_exists,
                                       pgvec_kb_service_record_exists_fn kb_exists, int dry_run,
                                       pgvec_kb_service_reconcile_result_t *out)
{
   assert(mem_exists != NULL);
   assert(kb_exists != NULL);
   g_reconcile_dry_run = dry_run;
   if (g_reconcile_rc != 0)
      return g_reconcile_rc;
   if (out)
   {
      out->rc = 0;
      out->mem_kept = 4;
      out->mem_pruned = 1;
      out->kb_kept = 6;
      out->kb_pruned = 2;
   }
   return 0;
}

const char *pgvec_kb_vector_collection_name(void)
{
   return "kb-test";
}

int pgvec_kb_vector_upsert_document(int64_t doc_id, const float *vec, int dim,
                                    const char *payload_json)
{
   (void)doc_id;
   (void)vec;
   (void)dim;
   (void)payload_json;
   return 0;
}

int pgvec_kb_vector_upsert_document_batch(const int64_t *doc_ids, const float *vecs, int dim,
                                          const char *const *payloads, int count)
{
   (void)doc_ids;
   (void)vecs;
   (void)dim;
   (void)payloads;
   g_batch_upsert_count += count;
   return 0;
}

void db2_vector_index_op_record(int64_t point_id, const char *collection, int64_t memory_id, int ok,
                                const char *message)
{
   (void)point_id;
   (void)collection;
   (void)memory_id;
   (void)ok;
   (void)message;
   g_vector_index_record_count++;
}

static void test_health(void)
{
   char buf[256];
   int status = kb_http_route("GET", "/v1/health", NULL, NULL, buf, sizeof(buf));
   assert(status == 200);
   assert(strstr(buf, "\"ok\"") != NULL);
}

static void test_health_ex_rich(void)
{
   char buf[256];
   int status = kb_http_route_ex("GET", "/v1/health", NULL, NULL, NULL, NULL, 0, buf, sizeof(buf));
   assert(status == 200);
   assert(strstr(buf, "\"db2_ok\":true") != NULL);
   assert(strstr(buf, "\"chunk_count\":7") != NULL);
}

static void test_health_status_mode(void)
{
   char buf[256];
   int status = kb_http_route_ex("GET", "/v1/health", "status=1&project=aimee%20core%2Fkb%3F", NULL,
                                 NULL, NULL, 0, buf, sizeof(buf));
   assert(status == 200);
   assert(strstr(buf, "\"summary_status\":\"ok\"") != NULL);
   assert(strstr(buf, "\"project\":\"aimee core/kb?\"") != NULL);
   assert(strstr(buf, "\"vector\"") != NULL);
}

static void test_version(void)
{
   char buf[256];
   int status = kb_http_route("GET", "/v1/version", NULL, NULL, buf, sizeof(buf));
   assert(status == 200);
   assert(strstr(buf, "aimee-kb") != NULL);
   assert(strstr(buf, "version") != NULL);
}

static void test_capabilities(void)
{
   char buf[256];
   int status = kb_http_route("GET", "/v1/capabilities", NULL, NULL, buf, sizeof(buf));
   assert(status == 200);
   assert(strstr(buf, "capabilities") != NULL);
   assert(strstr(buf, "memory") != NULL);
}

/* ── db2_enrollment_* stubs (satisfy refs from kb_http.o + kb_http_accounts.o +
 *    kb_tls_serve.o) with a single canned row so the accounts routes can be
 *    exercised without a live DB2. ─────────────────────────────────────────── */
#include "db2/enrollments.h"
#include "kb_identity.h"
static int g_stub_revoked_calls = 0;
static char g_stub_enrollment_expires_at[32];
int kb_http_egress_route(const char *method, const char *path, const char *body, int body_len,
                         const kb_principal_t *transport, const char *fingerprint, char *out,
                         int out_cap)
{
   (void)method;
   (void)path;
   (void)body;
   (void)body_len;
   (void)transport;
   (void)fingerprint;
   snprintf(out, (size_t)out_cap, "{\"error\":\"egress unavailable\"}");
   return 503;
}
int db2_enrollment_insert(const char *scope, const char *fingerprint, const char *cert_issuer,
                          const char *cert_serial_norm, const char *expires_at, int legacy,
                          int64_t *out_id)
{
   (void)scope;
   (void)fingerprint;
   (void)cert_issuer;
   (void)cert_serial_norm;
   snprintf(g_stub_enrollment_expires_at, sizeof(g_stub_enrollment_expires_at), "%s",
            expires_at ? expires_at : "");
   (void)legacy;
   if (out_id)
      *out_id = 1;
   return 0;
}
int db2_enrollment_renew(const char *old_fingerprint, const char *old_issuer,
                         const char *old_serial_norm, const char *scope,
                         const char *new_fingerprint, const char *new_issuer,
                         const char *new_serial_norm, int64_t *out_id)
{
   (void)old_fingerprint;
   (void)old_issuer;
   (void)old_serial_norm;
   (void)scope;
   (void)new_fingerprint;
   (void)new_issuer;
   (void)new_serial_norm;
   if (out_id)
      *out_id = 2;
   return 0;
}
static void fill_stub_row(db2_enrollment_row_t *r)
{
   memset(r, 0, sizeof(*r));
   r->id = 7;
   snprintf(r->scope, sizeof(r->scope), "project:web");
   snprintf(r->fingerprint, sizeof(r->fingerprint), "abc123");
   snprintf(r->state, sizeof(r->state), "active");
   snprintf(r->issued_at, sizeof(r->issued_at), "2026-07-04 00:00:00");
}
int db2_enrollment_list(int limit, db2_enrollment_row_t *out, int max)
{
   (void)limit;
   if (!out || max < 1)
      return -1;
   fill_stub_row(&out[0]);
   return 1;
}
int db2_enrollment_revoke(int64_t id, db2_enrollment_row_t *out)
{
   if (id != 7)
      return 1; /* not found */
   if (out)
   {
      fill_stub_row(out);
      snprintf(out->state, sizeof(out->state), "revoked");
      snprintf(out->revoked_at, sizeof(out->revoked_at), "2026-07-04 01:00:00");
   }
   return 0;
}
int db2_enrollment_is_revoked(const char *fingerprint)
{
   g_stub_revoked_calls++;
   return fingerprint && strcmp(fingerprint, "revoked-fp") == 0;
}
void db2_enrollment_touch_last_seen(const char *fingerprint, const char *scope)
{
   (void)fingerprint;
   (void)scope;
}
void db2_enrollment_cache_flush(void)
{
}
static db2_console_oidc_t g_stub_oidc;
int db2_console_oidc_get(db2_console_oidc_t *out)
{
   *out = g_stub_oidc;
   return g_stub_oidc.issuer[0] ? 0 : 1;
}
int db2_console_oidc_put(const db2_console_oidc_t *in)
{
   g_stub_oidc = *in;
   snprintf(g_stub_oidc.updated_at, sizeof(g_stub_oidc.updated_at), "2026-07-04 00:00:00");
   return 0;
}

/* audit_log() (pulled in via the revoke handler) resolves its 0600 audit.log
 * under config_default_dir(); stub it to a temp dir for the test. */
const char *config_default_dir(void)
{
   return "/tmp";
}

/* ── db2 governance stubs (decision_log + audit read) for kb_http_governance.o ─
 * Note: we do NOT include db2/artifacts.h (it re-declares db2_artifact_* which
 * this file already stubs with different signatures). Mirror just the audit row
 * struct — layout must match db2/artifacts.h. */
#include "db2/decision_log.h"
typedef struct
{
   char id[64];
   char target_surface[64];
   char target_id[160];
   char operator_id[128];
   char scope_kind[32];
   char scope_id[128];
   char applied_at[32];
   double applied_confidence;
   int flagged_for_review;
} db2_audit_event_row_t;
int db2_audit_event_list(const char *since, const char *until, const char *scope_kind, int limit,
                         db2_audit_event_row_t *out, int max);
static void fill_decision(db2_decision_log_row_t *d, int64_t id)
{
   memset(d, 0, sizeof(*d));
   d->id = id;
   snprintf(d->subject, sizeof(d->subject), "policy:x");
   snprintf(d->options, sizeof(d->options), "a|b");
   snprintf(d->chosen, sizeof(d->chosen), "a");
   snprintf(d->status, sizeof(d->status), "active");
   snprintf(d->created_at, sizeof(d->created_at), "2026-07-04 00:00:00");
}
int db2_decision_log_list_scoped(const char *subject, const char *status, int limit,
                                 db2_decision_log_row_t *out, int max)
{
   (void)subject;
   (void)limit;
   if (!out || max < 1)
      return -1;
   if (status && strcmp(status, "active") == 0)
      return 0; /* create's conflict pre-check sees no active decision */
   fill_decision(&out[0], 5);
   return 1;
}
int db2_decision_log_get(int64_t id, db2_decision_log_row_t *out)
{
   if (id != 7)
      return -1;
   fill_decision(out, 7);
   return 0;
}
int64_t db2_decision_log_active_id(const char *subject, int64_t linked_policy_id)
{
   (void)linked_policy_id;
   /* "policy:taken" already has an active decision (id 5); everything else free. */
   return (subject && strcmp(subject, "policy:taken") == 0) ? 5 : 0;
}
int db2_decision_log_record(const char *subject, const char *options, const char *chosen,
                            const char *rationale, const char *author, int64_t linked_policy_id,
                            const char *revisit_when, int64_t supersedes_id,
                            db2_decision_log_row_t *out)
{
   (void)options;
   (void)chosen;
   (void)rationale;
   (void)author;
   (void)linked_policy_id;
   (void)revisit_when;
   (void)supersedes_id;
   if (!subject || !subject[0])
      return -1;
   fill_decision(out, 8);
   snprintf(out->subject, sizeof(out->subject), "%s", subject);
   return 0;
}
int db2_decision_log_set_outcome(int64_t id, const char *outcome)
{
   (void)outcome;
   return id == 7 ? 0 : -1;
}
int db2_decision_log_set_status(int64_t id, const char *status)
{
   (void)status;
   return id == 7 ? 0 : -1;
}
int db2_decision_log_set_revisit(int64_t id, const char *revisit_when)
{
   (void)revisit_when;
   return id == 7 ? 0 : -1;
}
int db2_audit_event_list(const char *since, const char *until, const char *scope_kind, int limit,
                         db2_audit_event_row_t *out, int max)
{
   (void)until;
   (void)scope_kind;
   (void)limit;
   if (!since || !since[0] || !out || max < 1)
      return -1;
   memset(&out[0], 0, sizeof(out[0]));
   snprintf(out[0].id, sizeof(out[0].id), "evt-1");
   snprintf(out[0].target_surface, sizeof(out[0].target_surface), "memory");
   snprintf(out[0].applied_at, sizeof(out[0].applied_at), "2026-07-04 12:00:00");
   return 1;
}

static void test_mint_scope_restriction(void)
{
   /* A console-admin caller may not mint an owner/privileged scope — the guard
    * fires before kb_enroll_mint, so no enrollment store is needed here. The
    * configured + presented bearer is the console-admin scoped token. */
   char buf[4096];
   const char *ah = "Bearer scope:console-admin:c1:secret";
   const char *bt = "scope:console-admin:c1:secret";
   struct
   {
      const char *scope;
      int want;
   } cases[] = {
       {"global", 403},             /* no ':' => full access */
       {"owner:x", 403},            /* owner kind */
       {"console-admin:evil", 403}, /* privileged kind */
       {"curator:x", 403},          /* privileged kind */
   };
   for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
   {
      char body[128];
      snprintf(body, sizeof(body), "{\"host\":\"h\",\"port\":8741,\"scope\":\"%s\"}",
               cases[i].scope);
      int s = kb_http_route_ex("POST", "/v1/enroll", NULL, ah, bt, body, (int)strlen(body), buf,
                               sizeof(buf));
      if (s != cases[i].want)
         fprintf(stderr, "mint scope '%s': got %d want %d (%s)\n", cases[i].scope, s, cases[i].want,
                 buf);
      assert(s == cases[i].want);
   }
   printf("  PASS: console-admin mint scope restriction (owner/privileged -> 403)\n");
}

/* P1 slice 4: the /v1/team HTTP glue — auth -> actor construction -> tenant scope.
 * On the SQLite test shim tenant ops hard-fail (503), so this proves the router
 * builds the actor and reaches the DB tenant guard; the real RLS gating is proven
 * by the Postgres RLS gate. */
static void test_team_routes(void)
{
   char buf[4096];
   int s;
   /* No auth configured -> open mode manufactures NO actor -> a tenancy mutation
    * requires a real principal -> 401 (never an anonymous admin write). */
   s = kb_http_route_ex("POST", "/v1/team", NULL, NULL, NULL, "{\"name\":\"t\"}", 12, buf,
                        sizeof(buf));
   assert(s == 401);
   /* Unscoped owner bearer -> owner actor built -> team handler -> tenant scope ->
    * shim hard-fail -> 503 (the chain reached the DB tenant guard). */
   s = kb_http_route_ex("POST", "/v1/team", NULL, "Bearer owner-secret", "owner-secret",
                        "{\"name\":\"t\"}", 12, buf, sizeof(buf));
   assert(s == 503);
   /* A SCOPED kb-token is not a tenancy actor -> no actor -> 401. */
   s = kb_http_route_ex("POST", "/v1/team", NULL, "Bearer scope:project:x:secret",
                        "scope:project:x:secret", "{\"name\":\"t\"}", 12, buf, sizeof(buf));
   assert(s == 401);
   /* Missing name -> 400 before any DB work (owner bearer). */
   s = kb_http_route_ex("POST", "/v1/team", NULL, "Bearer owner-secret", "owner-secret", "{}", 2,
                        buf, sizeof(buf));
   assert(s == 400);
   /* An X-Aimee-* identity header never reaches route_ex (stripped at the ingress
    * seam) — verified by the kb_ingress unit test; here we confirm a normal team
    * path is not our concern for headers. Non-tenancy path falls through: */
   s = kb_http_route_ex("GET", "/v1/team", NULL, NULL, NULL, NULL, 0, buf, sizeof(buf));
   assert(s == 401); /* open mode, no actor */
   printf("  PASS: /v1/team HTTP glue (auth -> actor -> tenant scope; 401/503/400)\n");
}

static void test_governance_routes(void)
{
   char buf[65536];
   int s;
   s = kb_http_route_ex("GET", "/v1/decisions", NULL, NULL, NULL, NULL, 0, buf, sizeof(buf));
   assert(s == 200 && strstr(buf, "\"decisions\"") && strstr(buf, "\"subject\":\"policy:x\""));
   s = kb_http_route_ex("GET", "/v1/decisions/7", NULL, NULL, NULL, NULL, 0, buf, sizeof(buf));
   assert(s == 200 && strstr(buf, "\"supersede_chain\""));
   s = kb_http_route_ex("GET", "/v1/decisions/999", NULL, NULL, NULL, NULL, 0, buf, sizeof(buf));
   assert(s == 404);
   const char *cbody = "{\"subject\":\"policy:new\",\"options\":\"a|b\",\"chosen\":\"a\"}";
   s = kb_http_route_ex("POST", "/v1/decisions", NULL, NULL, NULL, cbody, (int)strlen(cbody), buf,
                        sizeof(buf));
   assert(s == 201 && strstr(buf, "\"subject\":\"policy:new\""));
   s = kb_http_route_ex("POST", "/v1/decisions", NULL, NULL, NULL, "{}", 2, buf, sizeof(buf));
   assert(s == 400);
   /* empty subject -> 400 */
   s = kb_http_route_ex("POST", "/v1/decisions", NULL, NULL, NULL,
                        "{\"subject\":\"\",\"options\":\"a\",\"chosen\":\"a\"}", 40, buf,
                        sizeof(buf));
   assert(s == 400);
   /* one-active-per-scope conflict -> 409 */
   const char *dup = "{\"subject\":\"policy:taken\",\"options\":\"a|b\",\"chosen\":\"a\"}";
   s = kb_http_route_ex("POST", "/v1/decisions", NULL, NULL, NULL, dup, (int)strlen(dup), buf,
                        sizeof(buf));
   assert(s == 409 && strstr(buf, "\"active_id\":5"));
   /* invalid status value -> 400 */
   s = kb_http_route_ex("POST", "/v1/decisions/7/status", NULL, NULL, NULL,
                        "{\"status\":\"bogus\"}", 18, buf, sizeof(buf));
   assert(s == 400);
   /* trailing path segment must not dispatch to an action -> 400 */
   s = kb_http_route_ex("POST", "/v1/decisions/7/supersede/extra", NULL, NULL, NULL, "{}", 2, buf,
                        sizeof(buf));
   assert(s == 400);
   const char *obody = "{\"outcome\":\"good\"}";
   s = kb_http_route_ex("POST", "/v1/decisions/7/outcome", NULL, NULL, NULL, obody,
                        (int)strlen(obody), buf, sizeof(buf));
   assert(s == 200);
   s = kb_http_route_ex("GET", "/v1/audit/actions", NULL, NULL, NULL, NULL, 0, buf, sizeof(buf));
   assert(s == 400);
   s = kb_http_route_ex("GET", "/v1/audit/actions", "since=2026-07-01", NULL, NULL, NULL, 0, buf,
                        sizeof(buf));
   assert(s == 200 && strstr(buf, "\"actions\"") && strstr(buf, "evt-1"));
}

static void test_accounts_routes(void)
{
   char buf[65536];
   /* GET /v1/enrollments → the canned row. */
   int s = kb_http_route_ex("GET", "/v1/enrollments", NULL, NULL, NULL, NULL, 0, buf, sizeof(buf));
   assert(s == 200);
   assert(strstr(buf, "\"enrollments\"") != NULL);
   assert(strstr(buf, "\"fingerprint\":\"abc123\"") != NULL);
   assert(strstr(buf, "\"count\":1") != NULL);

   /* POST /v1/enrollments/7/revoke → revoked row. */
   s = kb_http_route_ex("POST", "/v1/enrollments/7/revoke", NULL, NULL, NULL, NULL, 0, buf,
                        sizeof(buf));
   assert(s == 200);
   assert(strstr(buf, "\"revoked\":true") != NULL);
   assert(strstr(buf, "\"state\":\"revoked\"") != NULL);

   /* Unknown id → 404. */
   s = kb_http_route_ex("POST", "/v1/enrollments/999/revoke", NULL, NULL, NULL, NULL, 0, buf,
                        sizeof(buf));
   assert(s == 404);

   /* Bad id → 400. */
   s = kb_http_route_ex("POST", "/v1/enrollments/abc/revoke", NULL, NULL, NULL, NULL, 0, buf,
                        sizeof(buf));
   assert(s == 400);

   /* Wrong method on the list route → 405. */
   s = kb_http_route_ex("POST", "/v1/enrollments", NULL, NULL, NULL, NULL, 0, buf, sizeof(buf));
   assert(s == 405);

   /* GET /v1/scopes → aggregated. */
   s = kb_http_route_ex("GET", "/v1/scopes", NULL, NULL, NULL, NULL, 0, buf, sizeof(buf));
   assert(s == 200);
   assert(strstr(buf, "\"scopes\"") != NULL);
   assert(strstr(buf, "\"scope\":\"project:web\"") != NULL);

   /* GET /v1/config/oidc → unset -> configured:false. */
   s = kb_http_route_ex("GET", "/v1/config/oidc", NULL, NULL, NULL, NULL, 0, buf, sizeof(buf));
   assert(s == 200 && strstr(buf, "\"configured\":false"));
   /* PUT valid config -> 200 configured:true. */
   const char *ocfg =
       "{\"issuer\":\"https://idp\",\"audience\":\"kbc\",\"jwks_url\":\"https://idp/jwks\","
       "\"admin_claim\":\"groups\",\"admin_values\":[\"admins\"]}";
   s = kb_http_route_ex("PUT", "/v1/config/oidc", NULL, NULL, NULL, ocfg, (int)strlen(ocfg), buf,
                        sizeof(buf));
   assert(s == 200 && strstr(buf, "\"configured\":true") && strstr(buf, "\"admins\""));
   /* Now GET reflects it. */
   s = kb_http_route_ex("GET", "/v1/config/oidc", NULL, NULL, NULL, NULL, 0, buf, sizeof(buf));
   assert(s == 200 && strstr(buf, "\"issuer\":\"https://idp\""));
   /* PUT non-https jwks_url -> 400. */
   const char *bad =
       "{\"issuer\":\"https://idp\",\"jwks_url\":\"http://idp/jwks\",\"admin_claim\":\"groups\","
       "\"admin_values\":[\"admins\"]}";
   s = kb_http_route_ex("PUT", "/v1/config/oidc", NULL, NULL, NULL, bad, (int)strlen(bad), buf,
                        sizeof(buf));
   assert(s == 400);
   /* PUT missing fields -> 400. */
   s = kb_http_route_ex("PUT", "/v1/config/oidc", NULL, NULL, NULL, "{}", 2, buf, sizeof(buf));
   assert(s == 400);
   /* PUT with a comma in an admin value -> now accepted (JSON store) and
    * round-trips intact. */
   const char *comma =
       "{\"issuer\":\"https://idp\",\"audience\":\"kbc\",\"jwks_url\":\"https://idp/jwks\","
       "\"admin_claim\":\"groups\",\"admin_values\":[\"team,alpha\"]}";
   s = kb_http_route_ex("PUT", "/v1/config/oidc", NULL, NULL, NULL, comma, (int)strlen(comma), buf,
                        sizeof(buf));
   assert(s == 200 && strstr(buf, "\"configured\":true"));
   s = kb_http_route_ex("GET", "/v1/config/oidc", NULL, NULL, NULL, NULL, 0, buf, sizeof(buf));
   assert(s == 200 && strstr(buf, "\"team,alpha\""));
   /* PUT missing audience -> 400. */
   const char *noaud =
       "{\"issuer\":\"https://idp\",\"jwks_url\":\"https://idp/jwks\",\"admin_claim\":\"groups\","
       "\"admin_values\":[\"a\"]}";
   s = kb_http_route_ex("PUT", "/v1/config/oidc", NULL, NULL, NULL, noaud, (int)strlen(noaud), buf,
                        sizeof(buf));
   assert(s == 400);
}

static void test_console_overview(void)
{
   /* The dashboard aggregate returns a versioned, timestamped envelope with a
    * components[] array even when individual sources are unavailable (partial
    * failure is reported per-component, not a whole-request error). */
   char buf[65536];
   int status =
       kb_http_route_ex("GET", "/v1/console/overview", NULL, NULL, NULL, NULL, 0, buf, sizeof(buf));
   assert(status == 200);
   assert(strstr(buf, "\"schema\":\"console.overview.v1\"") != NULL);
   assert(strstr(buf, "\"components\"") != NULL);
   assert(strstr(buf, "\"generated_at\"") != NULL);
   /* Wrong method is rejected. */
   char b2[256];
   int s2 =
       kb_http_route_ex("POST", "/v1/console/overview", NULL, NULL, NULL, NULL, 0, b2, sizeof(b2));
   assert(s2 == 405);
}

static void test_console_admin_requires_authorization_module(void)
{
   const char *token = "scope:console-admin:test:secret";
   const char *auth = "Bearer scope:console-admin:test:secret";
   char buf[256];

   kb_route_acl_register_authorization_provider(NULL);
   int status = kb_http_route_ex("GET", "/v1/console/overview", NULL, auth, token, NULL, 0, buf,
                                 sizeof(buf));
   assert(status == 503);
   assert(strstr(buf, "control-web authorization unavailable") != NULL);

   kb_route_acl_register_authorization_provider(control_web_module_provider);
   status = kb_http_route_ex("POST", "/v1/console/overview", NULL, auth, token, NULL, 0, buf,
                             sizeof(buf));
   assert(status == 403);
   assert(strstr(buf, "not permitted") != NULL);
}

static void test_console_pipeline(void)
{
   /* GET returns the registry + presets + current config in one envelope. */
   char buf[65536];
   int status =
       kb_http_route_ex("GET", "/v1/console/pipeline", NULL, NULL, NULL, NULL, 0, buf, sizeof(buf));
   assert(status == 200);
   assert(strstr(buf, "\"stages\"") != NULL);
   assert(strstr(buf, "\"presets\"") != NULL);
   assert(strstr(buf, "\"config\"") != NULL);
   /* Every togglable stage key is reported; the embedder-gated stage has no key
    * to report, so it must not appear as one. */
   assert(strstr(buf, "\"kb_curator_extract_docs_enabled\"") != NULL);
   assert(strstr(buf, "\"kb_curator_stage_order\"") != NULL);
   assert(strstr(buf, "\"embed_code\":") == NULL);

   char b2[1024];
   /* Wrong method on each half of the pair. */
   assert(kb_http_route_ex("POST", "/v1/console/pipeline", NULL, NULL, NULL, "{}", 2, b2,
                           sizeof(b2)) == 405);
   assert(kb_http_route_ex("GET", "/v1/console/pipeline/config", NULL, NULL, NULL, NULL, 0, b2,
                           sizeof(b2)) == 405);

   /* A stage key the live registry advertises is accepted. */
   const char *ok_body = "{\"key\":\"kb_curator_extract_docs_enabled\",\"value\":true}";
   assert(kb_http_route_ex("POST", "/v1/console/pipeline/config", NULL, NULL, NULL, ok_body,
                           (int)strlen(ok_body), b2, sizeof(b2)) == 200);
   /* So is the pipeline's own stage-order key. */
   const char *order_body = "{\"key\":\"kb_curator_stage_order\",\"value\":\"extract_docs\"}";
   int order_status = kb_http_route_ex("POST", "/v1/console/pipeline/config", NULL, NULL, NULL,
                                       order_body, (int)strlen(order_body), b2, sizeof(b2));
   assert(order_status == 200 || order_status == 400); /* stub set_value only takes bool text */

   /* A config key OUTSIDE the pipeline is refused before any config write — this
    * route must not be a general config.set. */
   const char *evil = "{\"key\":\"db2_url\",\"value\":\"postgres://evil\"}";
   assert(kb_http_route_ex("POST", "/v1/console/pipeline/config", NULL, NULL, NULL, evil,
                           (int)strlen(evil), b2, sizeof(b2)) == 403);
   /* An embedder-gated stage has no config key, so its name is not settable. */
   const char *gated = "{\"key\":\"embed_code\",\"value\":true}";
   assert(kb_http_route_ex("POST", "/v1/console/pipeline/config", NULL, NULL, NULL, gated,
                           (int)strlen(gated), b2, sizeof(b2)) == 403);
   /* Missing key/value is a 400. */
   const char *empty = "{}";
   assert(kb_http_route_ex("POST", "/v1/console/pipeline/config", NULL, NULL, NULL, empty, 2, b2,
                           sizeof(b2)) == 400);
   printf("  PASS: console pipeline (registry + key allowlist)\n");
}

static void test_console_settings(void)
{
   g_stub_secret_configured = 0;
   g_stub_secret_store_calls = 0;
   /* GET reports the KB-owned fields, each with a section and a restart flag. */
   char buf[65536];
   int status =
       kb_http_route_ex("GET", "/v1/console/settings", NULL, NULL, NULL, NULL, 0, buf, sizeof(buf));
   assert(status == 200);
   assert(strstr(buf, "\"fields\"") != NULL);
   assert(strstr(buf, "\"embedder_model\"") != NULL);
   assert(strstr(buf, "\"synthesis_endpoint\"") != NULL);
   assert(strstr(buf, "\"Embedder\"") != NULL);
   /* Owned by the Typed Facts page, so it must not appear on this surface. */
   assert(strstr(buf, "\"typed_facts_enabled\"") == NULL);
   /* aimee-server's own keys must NOT appear on the kb's settings surface. */
   assert(strstr(buf, "\"kb_client_url\"") == NULL);
   assert(strstr(buf, "\"provider\"") == NULL);
   /* Vault-backed settings expose only configured state, never a credential. */
   assert(strstr(buf, "\"kb_api_bearer_token\"") != NULL);
   assert(strstr(buf, "\"secret\":true") != NULL);

   char b2[1024];
   assert(kb_http_route_ex("POST", "/v1/console/settings", NULL, NULL, NULL, "{}", 2, b2,
                           sizeof(b2)) == 405);
   assert(kb_http_route_ex("GET", "/v1/console/settings/config", NULL, NULL, NULL, NULL, 0, b2,
                           sizeof(b2)) == 405);

   /* typed_facts_enabled is KB-owned but belongs to the Typed Facts page, not
    * this surface — so the settings route refuses it like any other key it does
    * not own. */
   const char *tf = "{\"key\":\"typed_facts_enabled\",\"value\":true}";
   assert(kb_http_route_ex("POST", "/v1/console/settings/config", NULL, NULL, NULL, tf,
                           (int)strlen(tf), b2, sizeof(b2)) == 403);

   /* A KB-owned key is accepted. */
   const char *ok_body = "{\"key\":\"kb_mining_enabled\",\"value\":true}";
   assert(kb_http_route_ex("POST", "/v1/console/settings/config", NULL, NULL, NULL, ok_body,
                           (int)strlen(ok_body), b2, sizeof(b2)) == 200);
   const char *secret_literal = "do-not-echo-kb-secret";
   char secret_body[256];
   snprintf(secret_body, sizeof(secret_body), "{\"key\":\"kb_api_bearer_token\",\"value\":\"%s\"}",
            secret_literal);
   assert(kb_http_route_ex("POST", "/v1/console/settings/config", NULL, NULL, NULL, secret_body,
                           (int)strlen(secret_body), b2, sizeof(b2)) == 200);
   assert(g_stub_secret_store_calls == 1 && g_stub_secret_configured == 1);
   assert(strstr(b2, secret_literal) == NULL);
   assert(strstr(b2, "\"value\":true") != NULL);
   assert(strstr(b2, "\"secret\":true") != NULL);

   status =
       kb_http_route_ex("GET", "/v1/console/settings", NULL, NULL, NULL, NULL, 0, buf, sizeof(buf));
   assert(status == 200);
   assert(strstr(buf, secret_literal) == NULL);
   /* A key the SERVER owns is refused here, even though it is a real config key
    * and starts with kb_ — the split is by which binary reads it. */
   const char *server_key = "{\"key\":\"kb_client_url\",\"value\":\"https://kb.example\"}";
   assert(kb_http_route_ex("POST", "/v1/console/settings/config", NULL, NULL, NULL, server_key,
                           (int)strlen(server_key), b2, sizeof(b2)) == 403);
   const char *evil = "{\"key\":\"db2_url\",\"value\":\"postgres://evil\"}";
   assert(kb_http_route_ex("POST", "/v1/console/settings/config", NULL, NULL, NULL, evil,
                           (int)strlen(evil), b2, sizeof(b2)) == 403);
   assert(kb_http_route_ex("POST", "/v1/console/settings/config", NULL, NULL, NULL, "{}", 2, b2,
                           sizeof(b2)) == 400);
   printf("  PASS: console settings (kb-owned key allowlist)\n");
}

static void test_intelligence_calibration_readiness(void)
{
   char buf[512];
   int s = kb_http_route_ex("GET", "/v1/intelligence/calibration/readiness", NULL, NULL, NULL, NULL,
                            0, buf, sizeof(buf));
   assert(s == 200);
   assert(strstr(buf, "\"ready\":true") != NULL);
   assert(strstr(buf, "\"surfaces_with_data\":2") != NULL);
   assert(strstr(buf, "\"min_rows_required\":200") != NULL);
}

static void test_intelligence_demotion_check(void)
{
   char buf[1024];
   int s = kb_http_route_ex("GET", "/v1/intelligence/demotion/check", NULL, NULL, NULL, NULL, 0,
                            buf, sizeof(buf));
   assert(s == 200);
   assert(strstr(buf, "\"candidates\":2") != NULL);
   assert(strstr(buf, "\"scored\":2") != NULL);
   assert(strstr(buf, "\"would_demote\":1") != NULL);
   assert(strstr(buf, "\"kind\":\"fact\"") != NULL);
}

static void test_intelligence_bandit_export(void)
{
   char buf[4096];
   int s = kb_http_route_ex("GET", "/v1/intelligence/bandit/export", NULL, NULL, NULL, NULL, 0, buf,
                            sizeof(buf));
   assert(s == 200);
   /* Export is data-driven: the `points` breakdown reports only the point that is
    * actually sampled (no fabricated phantom — the arm_stats mock aborts on any
    * other decision point). */
   assert(strstr(buf, "\"points\":[") != NULL);
   assert(strstr(buf, "\"decision_point\":\"kb_memory_retrieval_limit\"") != NULL);
   assert(strstr(buf, "\"arm_id\":\"10\"") != NULL);
   assert(strstr(buf, "\"n_decisions\":3") != NULL);
   /* The registry section lists declared decision points (source of truth),
    * including arms and the reward function — present even with no decisions.
    * kb_fusion_mode is now a registered point, so it appears here (not as a
    * phantom with fabricated arm stats). */
   assert(strstr(buf, "\"registry\":[") != NULL);
   assert(strstr(buf, "\"reward_fn\":\"recall_sufficiency_v1\"") != NULL);
   assert(strstr(buf, "\"decision_point\":\"kb_fusion_mode\"") != NULL);
}

static void test_not_found(void)
{
   char buf[256];
   int status = kb_http_route("GET", "/v1/unknown_route", NULL, NULL, buf, sizeof(buf));
   assert(status == 404);
}

static void test_bearer_auth_ok(void)
{
   char buf[256];
   int status =
       kb_http_route("GET", "/v1/health", "Bearer secret123", "secret123", buf, sizeof(buf));
   assert(status == 200);
}

static void test_bearer_auth_missing(void)
{
   char buf[256];
   int status = kb_http_route("GET", "/v1/health", NULL, "secret123", buf, sizeof(buf));
   assert(status == 401);
}

static void test_bearer_auth_wrong(void)
{
   char buf[256];
   int status = kb_http_route("GET", "/v1/health", "Bearer wrong", "secret123", buf, sizeof(buf));
   assert(status == 401);
}

/* POST /v1/enroll: owner mints; scoped tokens denied; validation + method. */
static void test_enroll_route(void)
{
   /* Redirect the kb config dir (where the CA + token store live) to a temp dir
    * so the mint does not touch the real config. Must be set before the first
    * kb_default_config_dir() call — /v1/enroll is its only route-side caller. */
   char tmp[256];
   snprintf(tmp, sizeof tmp, "%s/aimee_enroll_route_XXXXXX", platform_tmpdir());
   assert(mkdtemp(tmp) != NULL);
   setenv("AIMEE_HOME", tmp, 1);

   char buf[1024];

   /* owner (no bearer configured == open/owner): a valid body mints a string. */
   const char *body = "{\"host\":\"kb.example.com\",\"port\":8443,\"scope\":\"project:x\"}";
   int s = kb_http_route_ex("POST", "/v1/enroll", NULL, NULL, NULL, body, (int)strlen(body), buf,
                            sizeof(buf));
   assert(s == 200);
   assert(strstr(buf, "\"connection_string\""));
   assert(strstr(buf, "aimee://kb.example.com:8443"));
   assert(strstr(buf, "ca=sha256:") && strstr(buf, "enroll="));

   /* a scoped bearer cannot mint (owner-only): 403. */
   s = kb_http_route_ex("POST", "/v1/enroll", NULL, "Bearer scope:project:x:sec",
                        "scope:project:x:sec", body, (int)strlen(body), buf, sizeof(buf));
   assert(s == 403);
   assert(strstr(buf, "owner credential"));

   /* missing host/port -> 400. */
   const char *bad = "{\"scope\":\"x\"}";
   s = kb_http_route_ex("POST", "/v1/enroll", NULL, NULL, NULL, bad, (int)strlen(bad), buf,
                        sizeof(buf));
   assert(s == 400);

   /* GET is not allowed. */
   s = kb_http_route_ex("GET", "/v1/enroll", NULL, NULL, NULL, NULL, 0, buf, sizeof(buf));
   assert(s == 405);

   /* cleanup the temp config dir's enrollment artifacts. */
   char p[256];
   snprintf(p, sizeof(p), "%s/kb-ca/ca.pem", tmp);
   remove(p);
   snprintf(p, sizeof(p), "%s/kb-ca/ca-key.pem", tmp);
   remove(p);
   snprintf(p, sizeof(p), "%s/kb-ca/ca-key.vault", tmp);
   remove(p);
   snprintf(p, sizeof(p), "%s/kb-ca", tmp);
   rmdir(p);
   snprintf(p, sizeof(p), "%s/kb-enroll-tokens", tmp);
   remove(p);
}

/* Build a PEM CSR for a fresh RSA-2048 key (the client's key stays local). */
static char *make_route_csr(void)
{
   EVP_PKEY *key = EVP_RSA_gen(2048);
   assert(key);
   X509_REQ *req = X509_REQ_new();
   assert(req && X509_REQ_set_version(req, 0) == 1);
   X509_NAME *n = X509_REQ_get_subject_name(req);
   X509_NAME_add_entry_by_txt(n, "CN", MBSTRING_ASC, (const unsigned char *)"client", -1, -1, 0);
   assert(X509_REQ_set_pubkey(req, key) == 1);
   assert(X509_REQ_sign(req, key, EVP_sha256()) > 0);
   BIO *bio = BIO_new(BIO_s_mem());
   assert(bio && PEM_write_bio_X509_REQ(bio, req) == 1);
   BUF_MEM *bm = NULL;
   BIO_get_mem_ptr(bio, &bm);
   char *out = malloc(bm->length + 1);
   memcpy(out, bm->data, bm->length);
   out[bm->length] = '\0';
   BIO_free(bio);
   X509_REQ_free(req);
   EVP_PKEY_free(key);
   return out;
}

/* POST /v1/enroll/redeem: a client redeems its token (+ CSR) for a client cert,
 * without the owner bearer. Mints via /v1/enroll, then redeems. */
static void test_enroll_redeem_route(void)
{
   char tmp[256];
   snprintf(tmp, sizeof tmp, "%s/aimee_redeem_route_XXXXXX", platform_tmpdir());
   assert(mkdtemp(tmp) != NULL);
   setenv("AIMEE_HOME", tmp, 1);
   const char *cfg = kb_default_config_dir(); /* cached (this tmp, or an earlier test's) */
   mkdir(cfg, 0700);                          /* ensure it exists */

   char buf[4096];

   /* mint a scoped token via /v1/enroll, extract it from the connection string. */
   const char *mint = "{\"host\":\"kb.example.com\",\"port\":8443,\"scope\":\"project:redeem\"}";
   int s = kb_http_route_ex("POST", "/v1/enroll", NULL, NULL, NULL, mint, (int)strlen(mint), buf,
                            sizeof(buf));
   assert(s == 200);
   cJSON *m = cJSON_Parse(buf);
   assert(m);
   kb_enroll_conn_t conn;
   assert(kb_enroll_conn_string_parse(
              cJSON_GetObjectItemCaseSensitive(m, "connection_string")->valuestring, &conn) == 0);
   char token[KB_ENROLL_TOKEN_MAX];
   snprintf(token, sizeof(token), "%s", conn.enroll_token);
   cJSON_Delete(m);

   /* client redeems with a CSR -> 200 with a client cert + the granted scope. */
   char *csr = make_route_csr();
   cJSON *rj = cJSON_CreateObject();
   cJSON_AddStringToObject(rj, "token", token);
   cJSON_AddStringToObject(rj, "csr", csr);
   char *rb = cJSON_PrintUnformatted(rj);
   g_stub_enrollment_expires_at[0] = '\0';
   s = kb_http_route_ex("POST", "/v1/enroll/redeem", NULL, NULL, NULL, rb, (int)strlen(rb), buf,
                        sizeof(buf));
   assert(s == 200);
   assert(strstr(buf, "\"client_cert\"") && strstr(buf, "BEGIN CERTIFICATE"));
   assert(strstr(buf, "project:redeem"));
   assert(strlen(g_stub_enrollment_expires_at) == 20);
   assert(g_stub_enrollment_expires_at[4] == '-' && g_stub_enrollment_expires_at[10] == 'T' &&
          g_stub_enrollment_expires_at[19] == 'Z');

   /* replaying the same (single-use) token -> 401. */
   s = kb_http_route_ex("POST", "/v1/enroll/redeem", NULL, NULL, NULL, rb, (int)strlen(rb), buf,
                        sizeof(buf));
   assert(s == 401);
   free(rb);
   cJSON_Delete(rj);
   free(csr);

   /* method / validation / bad-CSR. */
   s = kb_http_route_ex("GET", "/v1/enroll/redeem", NULL, NULL, NULL, NULL, 0, buf, sizeof(buf));
   assert(s == 405);
   const char *bad = "{\"token\":\"x\"}";
   s = kb_http_route_ex("POST", "/v1/enroll/redeem", NULL, NULL, NULL, bad, (int)strlen(bad), buf,
                        sizeof(buf));
   assert(s == 400);
   const char *badcsr = "{\"token\":\"x\",\"csr\":\"garbage\"}";
   s = kb_http_route_ex("POST", "/v1/enroll/redeem", NULL, NULL, NULL, badcsr, (int)strlen(badcsr),
                        buf, sizeof(buf));
   assert(s == 401);

   /* cleanup */
   char p[300];
   snprintf(p, sizeof(p), "%s/kb-ca/ca.pem", cfg);
   remove(p);
   snprintf(p, sizeof(p), "%s/kb-ca/ca-key.pem", cfg);
   remove(p);
   snprintf(p, sizeof(p), "%s/kb-ca/ca-key.vault", cfg);
   remove(p);
   snprintf(p, sizeof(p), "%s/kb-ca", cfg);
   rmdir(p);
   snprintf(p, sizeof(p), "%s/kb-enroll-tokens", cfg);
   remove(p);
}

/* --- mTLS serving: kb_tls_serve_conn routes a request with the scope taken
 *     from the verified client certificate. --- */
typedef struct
{
   SSL_CTX *ctx;
   int fd;
} mtls_serve_arg_t;

static void *mtls_serve_thread(void *a)
{
   mtls_serve_arg_t *s = (mtls_serve_arg_t *)a;
   kb_tls_serve_conn(s->fd, s->ctx);
   return NULL;
}

/* client connects with client_ctx, sends `req`, returns the response. */
static void mtls_request(SSL_CTX *server_ctx, SSL_CTX *client_ctx, const char *req, char *resp,
                         size_t cap)
{
   int sv[2];
   assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
   mtls_serve_arg_t sa = {server_ctx, sv[0]};
   pthread_t th;
   assert(pthread_create(&th, NULL, mtls_serve_thread, &sa) == 0);

   SSL *c = SSL_new(client_ctx);
   SSL_set_fd(c, sv[1]);
   resp[0] = '\0';
   if (SSL_connect(c) == 1)
   {
      SSL_write(c, req, (int)strlen(req));
      int total = 0;
      while ((size_t)total < cap - 1)
      {
         int n = SSL_read(c, resp + total, (int)(cap - 1 - total));
         if (n <= 0)
            break;
         total += n;
      }
      resp[total] = '\0';
   }
   SSL_shutdown(c);
   SSL_free(c);
   pthread_join(th, NULL);
   close(sv[0]);
   close(sv[1]);
}

/* Read one Content-Length-framed response without waiting for connection close. */
static int mtls_read_response(SSL *ssl, char *resp, size_t cap)
{
   size_t total = 0, expected = 0;
   while (total + 1 < cap)
   {
      int n = SSL_read(ssl, resp + total, (int)(cap - total - 1));
      if (n <= 0)
         return -1;
      total += (size_t)n;
      resp[total] = '\0';
      char *head_end = strstr(resp, "\r\n\r\n");
      if (head_end && !expected)
      {
         char *length = strstr(resp, "Content-Length: ");
         if (!length || length > head_end)
            return -1;
         expected = (size_t)(head_end + 4 - resp) + strtoul(length + 16, NULL, 10);
         if (expected >= cap)
            return -1;
      }
      if (expected && total >= expected)
      {
         resp[expected] = '\0';
         return (int)expected;
      }
   }
   return -1;
}

typedef struct
{
   pthread_mutex_t lock;
   pthread_cond_t cv;
   int ready;
   int go;
} mtls_pool_gate_t;

typedef struct
{
   mtls_pool_gate_t *gate;
   int ok;
} mtls_pool_request_arg_t;

static void *mtls_pool_request_thread(void *opaque)
{
   mtls_pool_request_arg_t *arg = opaque;
   pthread_mutex_lock(&arg->gate->lock);
   arg->gate->ready++;
   pthread_cond_broadcast(&arg->gate->cv);
   while (!arg->gate->go)
      pthread_cond_wait(&arg->gate->cv, &arg->gate->lock);
   pthread_mutex_unlock(&arg->gate->lock);
   int status = -1;
   char *response = kb_client_mtls_request("GET", "/v1/health", NULL, &status);
   arg->ok = status == 200 && response && strstr(response, "\"status\":\"ok\"");
   free(response);
   return NULL;
}

static void test_mtls_serve(void)
{
   extern void test_kb_enrollment_authority_set(int status);
   kb_pki_ca_t ca;
   assert(kb_pki_ca_generate(&ca) == 0);
   char scert[KB_PKI_CERT_PEM_MAX], skey[KB_PKI_KEY_PEM_MAX];
   assert(kb_pki_issue_server_cert(&ca, "kb.local", 3600, scert, sizeof(scert), skey,
                                   sizeof(skey)) == 0);
   char ccert[KB_PKI_CERT_PEM_MAX], ckey[KB_PKI_KEY_PEM_MAX];
   assert(kb_pki_issue_client_cert(&ca, "project:alpha", 3600, ccert, sizeof(ccert), ckey,
                                   sizeof(ckey)) == 0);
   SSL_CTX *sctx = kb_tls_server_ctx(ca.cert_pem, scert, skey);
   SSL_CTX *cctx = kb_tls_client_ctx(ca.cert_pem, ccert, ckey);
   assert(sctx && cctx);

   char resp[8192];

   /* a request the scoped cert is allowed -> 200, served over mTLS. */
   mtls_request(sctx, cctx, "GET /v1/health HTTP/1.1\r\nHost: kb\r\nConnection: close\r\n\r\n",
                resp, sizeof(resp));
   assert(strstr(resp, "200 OK"));
   assert(strstr(resp, "\"status\":\"ok\""));
   assert(strstr(resp, "Connection: close"));

   /* Scoped mTLS credentials must not acquire an owner actor merely because
    * their certificate is valid. The grant handler therefore refuses this
    * project certificate before it reaches the Postgres-only tenant gate. */
   {
      const char *grant = "{\"server_id\":\"srv-a\",\"team_id\":1,\"subject\":\"owner\","
                          "\"tier\":\"full\",\"granted_by\":\"owner\"}";
      char req[512];
      snprintf(req, sizeof(req),
               "POST /v1/write-tier-grants/set HTTP/1.1\r\nContent-Length: %zu\r\n"
               "Connection: close\r\n\r\n%s",
               strlen(grant), grant);
      mtls_request(sctx, cctx, req, resp, sizeof(resp));
      assert(strstr(resp, "401 Unauthorized"));
      assert(strstr(resp, "authentication required"));
   }

   /* The wizard-managed server certificate is intentionally unscoped. It is
    * the authenticated owner hop behind the server's UDS-only grant command,
    * so it must reach the tenant gate as owner rather than arrive actor-less.
    * This shim-backed test then returns 503 at the expected Postgres gate; 401
    * would prove the mTLS-to-owner bridge regressed again. */
   {
      char owner_cert[KB_PKI_CERT_PEM_MAX], owner_key[KB_PKI_KEY_PEM_MAX];
      assert(kb_pki_issue_client_cert(&ca, "p5-server-client", 3600, owner_cert, sizeof(owner_cert),
                                      owner_key, sizeof(owner_key)) == 0);
      SSL_CTX *owner_ctx = kb_tls_client_ctx(ca.cert_pem, owner_cert, owner_key);
      assert(owner_ctx);
      const char *grant = "{\"server_id\":\"srv-a\",\"team_id\":1,\"subject\":\"owner\","
                          "\"tier\":\"full\",\"granted_by\":\"owner\"}";
      char req[512];
      snprintf(req, sizeof(req),
               "POST /v1/write-tier-grants/set HTTP/1.1\r\nContent-Length: %zu\r\n"
               "Connection: close\r\n\r\n%s",
               strlen(grant), grant);
      mtls_request(sctx, owner_ctx, req, resp, sizeof(resp));
      assert(strstr(resp, "503 Service Unavailable"));
      assert(!strstr(resp, "authentication required"));
      SSL_CTX_free(owner_ctx);
   }

   /* HTTP/1.1 stays reusable by default. The certificate authority is checked
    * again for request N+1, so revocation takes effect before its route runs. */
   {
      int sv[2];
      assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
      mtls_serve_arg_t sa = {sctx, sv[0]};
      pthread_t th;
      assert(pthread_create(&th, NULL, mtls_serve_thread, &sa) == 0);
      SSL *c = SSL_new(cctx);
      assert(c);
      SSL_set_fd(c, sv[1]);
      assert(SSL_connect(c) == 1);
      const char *first = "GET /v1/health HTTP/1.1\r\nHost: kb\r\n\r\n";
      assert(SSL_write(c, first, (int)strlen(first)) == (int)strlen(first));
      assert(mtls_read_response(c, resp, sizeof(resp)) > 0);
      assert(strstr(resp, "200 OK") && strstr(resp, "Connection: keep-alive"));

      test_kb_enrollment_authority_set(0);
      const char *second = "GET /v1/health HTTP/1.1\r\nHost: kb\r\n\r\n";
      assert(SSL_write(c, second, (int)strlen(second)) == (int)strlen(second));
      assert(mtls_read_response(c, resp, sizeof(resp)) > 0);
      assert(strstr(resp, "403 Forbidden") && strstr(resp, "Connection: close"));
      assert(!strstr(resp, "\"status\":\"ok\""));
      test_kb_enrollment_authority_set(1);
      SSL_shutdown(c);
      SSL_free(c);
      pthread_join(th, NULL);
      close(sv[0]);
      close(sv[1]);
   }

   /* Pipelining remains fail-closed: callers must wait for each response. */
   mtls_request(sctx, cctx,
                "GET /v1/health HTTP/1.1\r\nHost: kb\r\n\r\n"
                "GET /v1/health HTTP/1.1\r\nHost: kb\r\nConnection: close\r\n\r\n",
                resp, sizeof(resp));
   assert(strstr(resp, "400 Bad Request"));
   mtls_request(sctx, cctx, "GET /v1/health HTTP/1.1\r\nHost: kb\rX-Test: no\r\n\r\n", resp,
                sizeof(resp));
   assert(strstr(resp, "400 Bad Request"));

   /* Strict framing bounds fail before routing: at most 64 headers, a 4 KiB
    * request target, and a 1 MiB body. */
   char oversized_headers[8192];
   size_t oversized_len = (size_t)snprintf(oversized_headers, sizeof(oversized_headers),
                                           "GET /v1/health HTTP/1.1\r\nHost: kb\r\n");
   for (int i = 0; i < 65; i++)
      oversized_len +=
          (size_t)snprintf(oversized_headers + oversized_len,
                           sizeof(oversized_headers) - oversized_len, "X-Test-%d: a\r\n", i);
   snprintf(oversized_headers + oversized_len, sizeof(oversized_headers) - oversized_len, "\r\n");
   mtls_request(sctx, cctx, oversized_headers, resp, sizeof(resp));
   assert(strstr(resp, "400 Bad Request"));

   char oversized_uri[4200];
   const char oversized_uri_suffix[] = " HTTP/1.1\r\nHost: kb\r\n\r\n";
   memcpy(oversized_uri, "GET /", 5);
   memset(oversized_uri + 5, 'a', 4096);
   memcpy(oversized_uri + 5 + 4096, oversized_uri_suffix, sizeof(oversized_uri_suffix));
   mtls_request(sctx, cctx, oversized_uri, resp, sizeof(resp));
   assert(strstr(resp, "400 Bad Request"));

   mtls_request(sctx, cctx,
                "POST /v1/health HTTP/1.1\r\nHost: kb\r\nContent-Length: 1048577\r\n\r\n", resp,
                sizeof(resp));
   assert(strstr(resp, "413 Payload Too Large"));

   /* P5-A heartbeat carries immutable peer-certificate metadata to the primary
    * authority function; none of it is accepted from JSON or the CN label. */
   {
      const char *hb = "{\"server_id\":\"srv-a\",\"health\":\"ok\",\"version\":\"1.0\"}";
      char req[512], want_issuer[601], raw_serial[129], want_serial[129], want_fp[65];
      assert(kb_pki_cert_metadata(ccert, want_issuer, sizeof(want_issuer), raw_serial,
                                  sizeof(raw_serial)) == 0);
      assert(kb_cert_serial_normalize(raw_serial, want_serial, sizeof(want_serial)) == 0);
      assert(kb_pki_ca_fingerprint(ccert, want_fp, sizeof(want_fp)) == 0);
      snprintf(req, sizeof(req),
               "POST /v1/server/heartbeat HTTP/1.1\r\nContent-Length: %zu\r\nConnection: "
               "close\r\n\r\n%s",
               strlen(hb), hb);
      g_test_registry_heartbeat_allow = 1;
      mtls_request(sctx, cctx, req, resp, sizeof(resp));
      assert(strstr(resp, "200 OK"));
      assert(strcmp(g_test_registry_server_id, "srv-a") == 0);
      assert(strcmp(g_test_registry_issuer, want_issuer) == 0);
      assert(strcmp(g_test_registry_serial, want_serial) == 0);
      assert(strcmp(g_test_registry_fingerprint, want_fp) == 0);
      g_test_registry_heartbeat_allow = 0;
      mtls_request(sctx, cctx, req, resp, sizeof(resp));
      assert(strstr(resp, "403 Forbidden"));
   }

   /* a CROSS-scope request (project=otherproj) is denied 403 — the scope came
    * from the client certificate (project:alpha), not the request. */
   mtls_request(sctx, cctx,
                "GET /v1/health?status=1&project=otherproj HTTP/1.1\r\nConnection: close\r\n\r\n",
                resp, sizeof(resp));
   assert(strstr(resp, "403"));
   assert(strstr(resp, "forbidden"));

   /* same-scope (project=alpha) is allowed. */
   mtls_request(sctx, cctx,
                "GET /v1/health?status=1&project=alpha HTTP/1.1\r\nConnection: close\r\n\r\n", resp,
                sizeof(resp));
   assert(!strstr(resp, "403"));

   /* Bootstrap: a CERT-LESS client (still enrolling) may reach
    * /v1/enroll/redeem but nothing else. */
   {
      SSL_CTX *nocert = SSL_CTX_new(TLS_client_method());
      assert(nocert);
      SSL_CTX_set_min_proto_version(nocert, TLS1_2_VERSION);
      BIO *b = BIO_new_mem_buf(ca.cert_pem, -1);
      X509 *cax = PEM_read_bio_X509(b, NULL, NULL, NULL);
      BIO_free(b);
      assert(cax && X509_STORE_add_cert(SSL_CTX_get_cert_store(nocert), cax) == 1);
      X509_free(cax);
      SSL_CTX_set_verify(nocert, SSL_VERIFY_PEER, NULL);

      /* a normal route without a client cert -> 401 (identity required). */
      mtls_request(sctx, nocert, "GET /v1/health HTTP/1.1\r\nConnection: close\r\n\r\n", resp,
                   sizeof(resp));
      assert(strstr(resp, "401"));
      assert(strstr(resp, "client certificate required"));

      /* but /v1/enroll/redeem IS reachable cert-less: a bad body -> 400 (it
       * reached the route), proving the bootstrap path is open. */
      mtls_request(sctx, nocert,
                   "POST /v1/enroll/redeem HTTP/1.1\r\nContent-Length: 2\r\nConnection: "
                   "close\r\n\r\n{}",
                   resp, sizeof(resp));
      assert(strstr(resp, "400"));
      assert(!strstr(resp, "client certificate required"));
      SSL_CTX_free(nocert);
   }

   /* Rotation without its authoritative PostgreSQL enrollment transaction must
    * fail closed even though TLS and CA signing are available.  The real-PG P2b
    * gate covers the successful lineage-preserving renewal path. */
   {
      char cadir[320];
      snprintf(cadir, sizeof(cadir), "%s/kb-ca", kb_default_config_dir());
      assert(kb_pki_ca_save(cadir, &ca) == 0);

      char *rcsr = make_route_csr();
      cJSON *rj = cJSON_CreateObject();
      cJSON_AddStringToObject(rj, "csr", rcsr);
      char *rb = cJSON_PrintUnformatted(rj);
      char req[8192];
      snprintf(req, sizeof(req),
               "POST /v1/enroll/renew HTTP/1.1\r\nContent-Length: %zu\r\nConnection: "
               "close\r\n\r\n%s",
               strlen(rb), rb);
      mtls_request(sctx, cctx, req, resp, sizeof(resp));
      assert(strstr(resp, "503 Service Unavailable"));
      assert(strstr(resp, "renew persistence unavailable"));
      free(rb);
      cJSON_Delete(rj);
      free(rcsr);

      char p[360];
      snprintf(p, sizeof(p), "%s/ca.pem", cadir);
      remove(p);
      snprintf(p, sizeof(p), "%s/ca-key.pem", cadir);
      remove(p);
      snprintf(p, sizeof(p), "%s/ca-key.vault", cadir);
      remove(p);
      rmdir(cadir);
   }

   SSL_CTX_free(sctx);
   SSL_CTX_free(cctx);
}

/* connect to 127.0.0.1:port, do an mTLS request, return the response. */
static void mtls_tcp_request(int port, SSL_CTX *client_ctx, const char *req, char *resp, size_t cap)
{
   resp[0] = '\0';
   int fd = socket(AF_INET, SOCK_STREAM, 0);
   assert(fd >= 0);
   struct sockaddr_in sa;
   memset(&sa, 0, sizeof(sa));
   sa.sin_family = AF_INET;
   sa.sin_port = htons((uint16_t)port);
   sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
   if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) == 0)
   {
      SSL *c = SSL_new(client_ctx);
      SSL_set_fd(c, fd);
      if (SSL_connect(c) == 1)
      {
         SSL_write(c, req, (int)strlen(req));
         int total = 0;
         while ((size_t)total < cap - 1)
         {
            int n = SSL_read(c, resp + total, (int)(cap - 1 - total));
            if (n <= 0)
               break;
            total += n;
         }
         resp[total] = '\0';
      }
      SSL_shutdown(c);
      SSL_free(c);
   }
   close(fd);
}

/* End-to-end: the real kb_mtls listener accepts a TLS connection on a TCP port
 * and serves /v1 with the scope taken from the client cert. */
static void test_mtls_listener(void)
{
   extern void test_kb_enrollment_authority_set(int status);
   /* Production passes kb_default_config_dir() as the listener data_dir, and the
    * bootstrap endpoints (/v1/enroll/ca, /renew) read the CA from there — so use
    * the same dir here. Clean any leftover CA so the listener makes a fresh one. */
   const char *cfg = kb_default_config_dir();
   char cp[360];
   snprintf(cp, sizeof(cp), "%s/kb-ca/ca.pem", cfg);
   remove(cp);
   snprintf(cp, sizeof(cp), "%s/kb-ca/ca-key.pem", cfg);
   remove(cp);
   snprintf(cp, sizeof(cp), "%s/kb-ca/ca-key.vault", cfg);
   remove(cp);
   snprintf(cp, sizeof(cp), "%s/kb-ca", cfg);
   rmdir(cp);

   setenv("AIMEE_KB_MTLS_MAX_CONNECTIONS", "0", 1);
   assert(kb_mtls_start(0, cfg, "localhost") == -1);
   setenv("AIMEE_KB_MTLS_MAX_CONNECTIONS", "8", 1);
   assert(kb_mtls_start(0, cfg, "localhost") == 0);
   int connection_limit = 0, connections_live = -1, connections_queued = -1;
   kb_mtls_connection_stats(&connection_limit, &connections_live, &connections_queued);
   assert(connection_limit == 8);
   assert(connections_live == 0);
   assert(connections_queued == 0);
   int port = kb_mtls_bound_port();
   assert(port > 0);

   char capath[360];
   snprintf(capath, sizeof(capath), "%s/kb-ca", cfg);
   kb_pki_ca_t ca;
   assert(kb_pki_ca_load(capath, &ca) == 0);
   char fp[KB_PKI_FP_HEX];
   assert(kb_pki_ca_fingerprint(ca.cert_pem, fp, sizeof(fp)) == 0);

   /* TOFU bootstrap: a cert-less client fetches the CA and pins it by
    * fingerprint; a wrong pin is rejected (MITM defense). */
   char fetched[KB_PKI_CERT_PEM_MAX];
   assert(kb_tls_fetch_ca("localhost", port, fp, fetched, sizeof(fetched)) == 0);
   assert(strcmp(fetched, ca.cert_pem) == 0);
   char badfp[KB_PKI_FP_HEX];
   memset(badfp, '0', 64);
   badfp[64] = '\0';
   assert(kb_tls_fetch_ca("localhost", port, badfp, fetched, sizeof(fetched)) == -1);

   /* a client cert issued by the (now pinned) CA. */
   char ccert[KB_PKI_CERT_PEM_MAX], ckey[KB_PKI_KEY_PEM_MAX];
   assert(kb_pki_issue_client_cert(&ca, "project:beta", 3600, ccert, sizeof(ccert), ckey,
                                   sizeof(ckey)) == 0);
   SSL_CTX *cctx = kb_tls_client_ctx(ca.cert_pem, ccert, ckey);
   assert(cctx);

   char resp[8192];
   mtls_tcp_request(port, cctx, "GET /v1/health HTTP/1.1\r\nHost: kb\r\nConnection: close\r\n\r\n",
                    resp, sizeof(resp));
   assert(strstr(resp, "200 OK"));
   assert(strstr(resp, "\"status\":\"ok\""));

   /* the high-level client dialer reaches the same listener with its cert. */
   int st = -1;
   char rbody[4096];
   assert(kb_tls_client_request("localhost", port, ca.cert_pem, ccert, ckey, "GET", "/v1/health",
                                NULL, rbody, sizeof(rbody), &st) == 0);
   if (st != 200)
      fprintf(stderr, "high-level mTLS health status=%d body=%s\n", st, rbody);
   assert(st == 200);
   assert(strstr(rbody, "\"status\":\"ok\""));

   /* Managed server identities carry ordinary data-plane calls over this same
    * mTLS connection. A detached-workspace ingest batch is deliberately much
    * larger than the old 64 KiB request buffer, so exercise the high-level
    * client with a representative payload instead of accepting health-only
    * connectivity as proof that indexing works. */
   {
      const size_t padding_len = 128 * 1024;
      char *body = malloc(padding_len + 256);
      assert(body);
      int prefix = snprintf(body, padding_len + 256,
                            "{\"server_id\":\"srv-large\",\"health\":\"ready\","
                            "\"version\":\"test\",\"padding\":\"");
      assert(prefix > 0);
      memset(body + prefix, 'x', padding_len);
      memcpy(body + prefix + padding_len, "\"}", 3);
      assert(strlen(body) > 64 * 1024 && strlen(body) < 1024 * 1024);
      g_test_registry_heartbeat_allow = 1;
      assert(kb_tls_client_request("localhost", port, ca.cert_pem, ccert, ckey, "POST",
                                   "/v1/server/heartbeat", body, rbody, sizeof(rbody), &st) == 0);
      assert(st == 200 && strstr(rbody, "\"ok\":true"));
      g_test_registry_heartbeat_allow = 0;
      free(body);
   }

   /* The reusable client primitive reads Content-Length exactly instead of
    * waiting for EOF, then safely carries a second request on the same TLS
    * connection. */
   kb_tls_client_conn_t *persistent =
       kb_tls_client_conn_open("localhost", port, ca.cert_pem, ccert, ckey);
   assert(persistent);
   int reusable = 0;
   assert(kb_tls_client_conn_request(persistent, "GET", "/v1/health", NULL, NULL, 0, rbody,
                                     sizeof(rbody), &st, &reusable) == 0);
   assert(st == 200 && reusable == 1 && strstr(rbody, "\"status\":\"ok\""));
   assert(kb_tls_client_conn_request(persistent, "GET", "/v1/health", NULL, NULL, 1, rbody,
                                     sizeof(rbody), &st, &reusable) == 0);
   assert(st == 200 && reusable == 0 && strstr(rbody, "\"status\":\"ok\""));
   kb_tls_client_conn_close(persistent);

   kb_tls_client_conn_t *bounded =
       kb_tls_client_conn_open("localhost", port, ca.cert_pem, ccert, ckey);
   assert(bounded);
   char tiny_response[4];
   assert(kb_tls_client_conn_request(bounded, "GET", "/v1/health", NULL, NULL, 1, tiny_response,
                                     sizeof(tiny_response), &st, &reusable) == -1);
   assert(reusable == 0);
   kb_tls_client_conn_close(bounded);

   /* A long-lived identity context can explicitly carry the negotiated session
    * into a replacement connection. */
   SSL_CTX *shared_client_ctx = kb_tls_client_ctx(ca.cert_pem, ccert, ckey);
   assert(shared_client_ctx);
   kb_tls_client_conn_t *session_one =
       kb_tls_client_conn_open_ctx("localhost", port, shared_client_ctx);
   assert(session_one);
   assert(kb_tls_client_conn_set_timeout(NULL, 600000) == -1);
   assert(kb_tls_client_conn_set_timeout(session_one, 0) == -1);
   assert(kb_tls_client_conn_set_timeout(session_one, 600000) == 0);
   assert(kb_tls_client_conn_request(session_one, "GET", "/v1/health", NULL, NULL, 0, rbody,
                                     sizeof(rbody), &st, &reusable) == 0);
   assert(reusable == 1);
   SSL_SESSION *saved_session = kb_tls_client_conn_get1_session(session_one);
   assert(saved_session);
   kb_tls_client_conn_close(session_one);
   kb_tls_client_conn_t *session_two =
       kb_tls_client_conn_open_session("localhost", port, shared_client_ctx, saved_session);
   assert(session_two);
   assert(kb_tls_client_conn_session_reused(session_two) == 1);
   assert(kb_tls_client_conn_request(session_two, "GET", "/v1/health", NULL, NULL, 1, rbody,
                                     sizeof(rbody), &st, &reusable) == 0);
   kb_tls_client_conn_close(session_two);
   SSL_SESSION_free(saved_session);
   SSL_CTX_free(shared_client_ctx);

   /* Primary authority is consulted before dispatch. Unknown/revoked peers are
    * forbidden and an authority outage is retryable but never fail-open. */
   test_kb_enrollment_authority_set(0);
   mtls_tcp_request(port, cctx, "GET /v1/health HTTP/1.1\r\nHost: kb\r\nConnection: close\r\n\r\n",
                    resp, sizeof(resp));
   assert(strstr(resp, "403 Forbidden"));
   assert(!strstr(resp, "\"status\":\"ok\""));
   test_kb_enrollment_authority_set(-1);
   mtls_tcp_request(port, cctx, "GET /v1/health HTTP/1.1\r\nHost: kb\r\nConnection: close\r\n\r\n",
                    resp, sizeof(resp));
   assert(strstr(resp, "503 Service Unavailable"));
   assert(!strstr(resp, "\"status\":\"ok\""));
   test_kb_enrollment_authority_set(1);
   /* the dialer's request carries the client cert's scope (project:beta): a
    * cross-scope request is denied. */
   assert(kb_tls_client_request("localhost", port, ca.cert_pem, ccert, ckey, "GET",
                                "/v1/health?status=1&project=otherproj", NULL, rbody, sizeof(rbody),
                                &st) == 0);
   assert(st == 403);

   /* FULL CLIENT ENROLLMENT: mint a token into the listener's store, build the
    * connection string, and run kb_tls_enroll — TOFU CA pin + CSR + redeem in
    * one call — then dial with the resulting identity. */
   {
      char store[400];
      snprintf(store, sizeof(store), "%s/kb-enroll-tokens", cfg);
      char token[KB_ENROLL_TOKEN_MAX];
      assert(kb_enroll_store_issue(store, "project:gamma", token, sizeof(token)) == 0);
      char conn[600];
      assert(kb_enroll_conn_string_build("localhost", port, fp, token, conn, sizeof(conn)) > 0);

      char eca[KB_PKI_CERT_PEM_MAX], ecert[KB_PKI_CERT_PEM_MAX], ekey[KB_PKI_KEY_PEM_MAX];
      assert(kb_tls_enroll(conn, eca, sizeof(eca), ecert, sizeof(ecert), ekey, sizeof(ekey)) == 0);
      assert(strcmp(eca, ca.cert_pem) == 0);                      /* pinned the right CA */
      assert(kb_pki_verify_client_cert(ca.cert_pem, ecert) == 1); /* issued cert chains */

      /* the freshly-enrolled identity dials the kb with its scope (project:gamma). */
      assert(kb_tls_client_request("localhost", port, eca, ecert, ekey, "GET", "/v1/health", NULL,
                                   rbody, sizeof(rbody), &st) == 0);
      assert(st == 200);
      assert(kb_tls_client_request("localhost", port, eca, ecert, ekey, "GET",
                                   "/v1/health?status=1&project=elsewhere", NULL, rbody,
                                   sizeof(rbody), &st) == 0);
      assert(st == 403); /* scope from the enrolled cert is enforced */
      remove(store);
   }

   /* SERVER INTEGRATION: the aimee-server kb_client mTLS transport
    * (kb_client_mtls.c) enrolls from AIMEE_KB_CONN and routes a /v1 call. */
   {
      char store2[400];
      snprintf(store2, sizeof(store2), "%s/kb-enroll-tokens", cfg);
      char token2[KB_ENROLL_TOKEN_MAX];
      assert(kb_enroll_store_issue(store2, "project:delta", token2, sizeof(token2)) == 0);
      char conn2[600];
      assert(kb_enroll_conn_string_build("localhost", port, fp, token2, conn2, sizeof(conn2)) > 0);

      char identity_file[160];
      snprintf(identity_file, sizeof(identity_file), "/tmp/aimee-kb-client-identity-%ld.json",
               (long)getpid());
      unlink(identity_file);
      kb_client_mtls_set_identity_path_for_test(identity_file);
      assert(runtime_secret_store("AIMEE_KB_CONN", conn2) == 0);
      setenv("AIMEE_TRANSPORT_KB_POOL_ENABLED", "0", 1);
      assert(kb_client_mtls_configured() == 1);
      int st2 = -1;
      char *r = kb_client_mtls_request_timeout("GET", "/v1/health", NULL, 600000, &st2);
      assert(st2 == 200);
      assert(r && strstr(r, "\"status\":\"ok\""));
      free(r);
      struct stat identity_stat;
      assert(stat(identity_file, &identity_stat) == 0 && S_ISREG(identity_stat.st_mode));
      assert((identity_stat.st_mode & 0777) == 0600 && identity_stat.st_uid == geteuid());
      FILE *identity_stream = fopen(identity_file, "r");
      assert(identity_stream);
      char identity_json[32768];
      size_t identity_n = fread(identity_json, 1, sizeof(identity_json) - 1, identity_stream);
      assert(!ferror(identity_stream) && feof(identity_stream));
      fclose(identity_stream);
      identity_json[identity_n] = '\0';
      assert(strstr(identity_json, "\"version\":1") && strstr(identity_json, "PRIVATE KEY"));
      assert(strstr(identity_json, token2) == NULL); /* never persist the one-time credential */

      /* A REFUSAL MUST ARRIVE WITH ITS REASON. The transport used to return NULL for
       * any non-2xx, which made kb_client_v1_post_json_keep_error a no-op on this
       * path: every kb refusal reached the operator as a generic fallback string
       * while the kb's own explanation was discarded here. `aimee kb reembed` on a
       * managed appliance answered "knowledge service reembed failed" instead of the
       * kb's 403 naming the config key to set. */
      int st_err = -1;
      char *err_body = kb_client_mtls_request_timeout("GET", "/v1/definitely-not-a-route", NULL,
                                                      600000, &st_err);
      assert(st_err >= 400);    /* the status still reports the failure */
      assert(err_body != NULL); /* and the body survives to be surfaced */
      assert(strstr(err_body, "error") != NULL);
      free(err_body);

      /* Simulate a full server process restart. The enrollment token was spent
       * by the first request, so this can pass only by validating and loading
       * the owner-only identity file. */
      kb_client_mtls_reset_for_test();
      r = kb_client_mtls_request("GET", "/v1/health", NULL, &st2);
      assert(st2 == 200 && r && strstr(r, "\"status\":\"ok\""));
      free(r);
      int pool_total = -1, pool_idle = -1, pool_busy = -1, pool_waiters = -1;
      unsigned long pool_exhausted = 1;
      kb_client_mtls_pool_stats(&pool_total, &pool_idle, &pool_busy, &pool_waiters,
                                &pool_exhausted);
      assert(pool_total == 0 && pool_idle == 0 && pool_busy == 0 && pool_waiters == 0);

      /* The hot rollout flag opts this identity into reuse without a restart. */
      setenv("AIMEE_TRANSPORT_KB_POOL_ENABLED", "1", 1);
      r = kb_client_mtls_request_timeout("GET", "/v1/health", NULL, 600000, &st2);
      assert(st2 == 200 && r);
      free(r);
      g_test_registry_heartbeat_allow = 1;
      assert(kb_client_mtls_heartbeat("srv-delta", "ready", "test") == 0);
      assert(strcmp(g_test_registry_server_id, "srv-delta") == 0);
      g_test_registry_heartbeat_allow = 0;
      kb_client_mtls_pool_stats(&pool_total, &pool_idle, &pool_busy, &pool_waiters,
                                &pool_exhausted);
      assert(pool_total == 1 && pool_idle == 1 && pool_busy == 0 && pool_waiters == 0);
      assert(pool_exhausted == 0);

      enum
      {
         POOL_TEST_CLIENTS = 16
      };
      mtls_pool_gate_t gate = {PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, 0, 0};
      pthread_t pool_threads[POOL_TEST_CLIENTS];
      mtls_pool_request_arg_t pool_args[POOL_TEST_CLIENTS];
      for (int i = 0; i < POOL_TEST_CLIENTS; i++)
      {
         pool_args[i] = (mtls_pool_request_arg_t){.gate = &gate, .ok = 0};
         assert(pthread_create(&pool_threads[i], NULL, mtls_pool_request_thread, &pool_args[i]) ==
                0);
      }
      pthread_mutex_lock(&gate.lock);
      while (gate.ready != POOL_TEST_CLIENTS)
         pthread_cond_wait(&gate.cv, &gate.lock);
      gate.go = 1;
      pthread_cond_broadcast(&gate.cv);
      pthread_mutex_unlock(&gate.lock);
      for (int i = 0; i < POOL_TEST_CLIENTS; i++)
      {
         pthread_join(pool_threads[i], NULL);
         assert(pool_args[i].ok);
      }
      pthread_cond_destroy(&gate.cv);
      pthread_mutex_destroy(&gate.lock);
      kb_client_mtls_pool_stats(&pool_total, &pool_idle, &pool_busy, &pool_waiters,
                                &pool_exhausted);
      assert(pool_total >= 1 && pool_total <= 2 && pool_total == pool_idle && pool_busy == 0 &&
             pool_waiters == 0);
      unsigned long pool_handshakes = 0, pool_resumed = 0;
      kb_client_mtls_tls_stats(&pool_handshakes, &pool_resumed);
      assert(pool_handshakes >= 1 && pool_resumed <= pool_handshakes);
      /* Disabling live restores one-shot requests and drains every idle socket. */
      setenv("AIMEE_TRANSPORT_KB_POOL_ENABLED", "0", 1);
      r = kb_client_mtls_request("GET", "/v1/health", NULL, &st2);
      assert(st2 == 200 && r);
      free(r);
      kb_client_mtls_pool_stats(&pool_total, &pool_idle, &pool_busy, &pool_waiters,
                                &pool_exhausted);
      assert(pool_total == 0 && pool_idle == 0);
      unsetenv("AIMEE_TRANSPORT_KB_POOL_ENABLED");
      runtime_secret_remove("AIMEE_KB_CONN");

      /* A wizard-managed v2 identity owns its endpoint and stable registry
       * binding, so it remains configured with no one-time connection string
       * in the process environment. */
      cJSON *managed = cJSON_CreateObject();
      assert(managed);
      cJSON_AddNumberToObject(managed, "version", 2);
      cJSON_AddStringToObject(managed, "state", "ready");
      cJSON_AddStringToObject(managed, "host", "localhost");
      cJSON_AddNumberToObject(managed, "port", port);
      cJSON_AddStringToObject(managed, "server_id", "managed-server-test");
      cJSON_AddNumberToObject(managed, "team_id", 42);
      cJSON_AddStringToObject(managed, "ca", ca.cert_pem);
      cJSON_AddStringToObject(managed, "cert", ccert);
      cJSON_AddStringToObject(managed, "key", ckey);
      char *managed_json = cJSON_PrintUnformatted(managed);
      cJSON_Delete(managed);
      assert(managed_json);
      identity_stream = fopen(identity_file, "w");
      assert(identity_stream && fputs(managed_json, identity_stream) >= 0 &&
             fclose(identity_stream) == 0);
      free(managed_json);
      assert(chmod(identity_file, 0600) == 0);
      kb_client_mtls_reset_for_test();
      assert(kb_client_mtls_configured() == 1);
      char managed_server[128];
      long long managed_team = 0;
      assert(kb_client_mtls_managed_metadata(managed_server, sizeof(managed_server),
                                             &managed_team) == 1);
      assert(strcmp(managed_server, "managed-server-test") == 0 && managed_team == 42);
      r = kb_client_mtls_request("GET", "/v1/health", NULL, &st2);
      assert(st2 == 200 && r && strstr(r, "\"status\":\"ok\""));
      free(r);

      kb_client_mtls_set_identity_path_for_test(NULL);
      assert(kb_client_mtls_configured() == 0);
      unlink(identity_file);
      remove(store2);
   }

   /* CERT ROTATION: expiry check + renew over mTLS for the same scope. */
   {
      /* a 1-hour cert expires within 2 hours, not within 1 second; the 10y CA
       * is not near expiry. */
      char sc[KB_PKI_CERT_PEM_MAX], sk[KB_PKI_KEY_PEM_MAX];
      assert(kb_pki_issue_client_cert(&ca, "project:beta", 3600, sc, sizeof(sc), sk, sizeof(sk)) ==
             0);
      assert(kb_tls_cert_expires_within(sc, 7200) == 1);
      assert(kb_tls_cert_expires_within(sc, 1) == 0);
      assert(kb_tls_cert_expires_within(ca.cert_pem, 60L * 60 * 24 * 14) == 0);

      /* With no authoritative enrollment DB in this unit fixture, renewal is
       * refused before a new certificate can escape. */
      char nc[KB_PKI_CERT_PEM_MAX], nk[KB_PKI_KEY_PEM_MAX];
      assert(kb_tls_renew("localhost", port, ca.cert_pem, ccert, ckey, nc, sizeof(nc), nk,
                          sizeof(nk)) != 0);
   }

   SSL_CTX_free(cctx);
   kb_mtls_stop();
   unsetenv("AIMEE_KB_MTLS_MAX_CONNECTIONS");
   assert(kb_mtls_bound_port() == 0);

   snprintf(cp, sizeof(cp), "%s/kb-ca/ca.pem", cfg);
   remove(cp);
   snprintf(cp, sizeof(cp), "%s/kb-ca/ca-key.pem", cfg);
   remove(cp);
   snprintf(cp, sizeof(cp), "%s/kb-ca/ca-key.vault", cfg);
   remove(cp);
   snprintf(cp, sizeof(cp), "%s/kb-ca", cfg);
   rmdir(cp);
}

static void test_head_method(void)
{
   char buf[256];
   int status = kb_http_route("HEAD", "/v1/health", NULL, NULL, buf, sizeof(buf));
   assert(status == 200);
}

static void test_method_not_allowed(void)
{
   char buf[256];
   int status = kb_http_route("POST", "/v1/health", NULL, NULL, buf, sizeof(buf));
   assert(status == 405);
}

/* ── Phase 5 route tests ─────────────────────────────────────────────────── */

static void test_invalidations_route(void)
{
   char buf[2048];
   int s = kb_http_route_ex("GET", "/v1/invalidations", "since=0", NULL, NULL, NULL, 0, buf,
                            sizeof(buf));
   assert(s == 200 && strstr(buf, "invalidations") && strstr(buf, "next_cursor"));
   assert(kb_http_route_ex("POST", "/v1/invalidations", NULL, NULL, NULL, "{}", 2, buf,
                           sizeof(buf)) == 405);
   printf("  PASS: /v1/invalidations route\n");
}

static void test_curator_routes(void)
{
   char buf[2048];
   const char *b = "{\"topic\":\"pgvector\"}";
   int s1 = kb_http_route_ex("POST", "/v1/implements", NULL, NULL, NULL, b, (int)strlen(b), buf,
                             sizeof(buf));
   assert(s1 == 200 && strstr(buf, "implements"));
   assert(kb_http_route_ex("POST", "/v1/implements", NULL, NULL, NULL, "{}", 2, buf, sizeof(buf)) ==
          400);
   assert(kb_http_route_ex("GET", "/v1/implements", NULL, NULL, NULL, NULL, 0, buf, sizeof(buf)) ==
          405);

   int s2 = kb_http_route_ex("POST", "/v1/synthesize", NULL, NULL, NULL, b, (int)strlen(b), buf,
                             sizeof(buf));
   assert(s2 == 200 && strstr(buf, "synthesis_id"));

   int s3 = kb_http_route_ex("POST", "/v1/contradictions", NULL, NULL, NULL, "{\"limit\":5}", 11,
                             buf, sizeof(buf));
   assert(s3 == 200 && strstr(buf, "contradictions"));
   assert(kb_http_route_ex("GET", "/v1/contradictions", NULL, NULL, NULL, NULL, 0, buf,
                           sizeof(buf)) == 405);
   printf("  PASS: /v1/implements,/v1/synthesize,/v1/contradictions routes\n");
}

/* test_kb_http_routes_code.inc: code-index / scan / ingest / maintenance / job
 * route tests, split out of test_kb_http_routes.c to keep that translation
 * unit under the 2000-line hard limit. Included (with
 * test_kb_http_routes_endpoints.inc) just before main(); all helpers and
 * fixtures live in test_kb_http_routes.c above the include point. */
/* §4 surprising self-suppress: rolling judge stats the runtime_state stub serves for
 * the surprising_judged:/surprising_confirmed: keys (-1 = key absent). */
static int g_sj_judged = -1;
static int g_sj_confirmed = -1;
/* Full memory_t layout (mirrors headers/memory.h) so the stub writes each field
 * at the offset the handler — compiled against the real struct — reads. The main
 * file keeps a truncated local memory_t for other stubs, so we cast a void* here
 * rather than redeclare the type, exactly like canonical_index_code_search. Lives
 * here (with the hybrid test it feeds) to keep test_kb_http_routes.c under the
 * 2000-line build-integrity limit. */
typedef struct
{
   int64_t id;
   char tier[4];
   char kind[16];
   char key[512];
   char headline[512];
   char content[2048];
   char use_cases[1024];
   double confidence;
   int use_count;
   char last_used_at[32];
   char created_at[32];
   char updated_at[32];
   char source_session[128];
   double salience;
   char provenance_category[32];
   double retrieval_score;
   int hybrid_rank;
} test_full_memory_t;

int db2_memory_find_facts_like(const char *query, int limit, void *out, int max)
{
   assert(out);
   if (!query || strcmp(query, "needle") != 0 || limit < 1 || max < 1)
      return 0;
   test_full_memory_t *m = (test_full_memory_t *)out;
   memset(&m[0], 0, sizeof(m[0]));
   m[0].id = 7;
   snprintf(m[0].kind, sizeof(m[0].kind), "decision");
   snprintf(m[0].headline, sizeof(m[0].headline), "why needle exists");
   snprintf(m[0].content, sizeof(m[0].content), "%schose needle over haystack for O(1) lookup",
            g_code_context_memory_anchored ? "src/search.c: " : "");
   return 1;
}

int memory_find_facts_visible_ex(const char *query, const char *workspace, const char *project,
                                 int include_all, int limit, void *out, int max)
{
   (void)workspace;
   assert(project == NULL || strcmp(project, "proj-alpha") == 0);
   assert(include_all == 0 || include_all == 1);
   int n = db2_memory_find_facts_like(query, limit, out, max);
   if (n > 0)
      ((test_full_memory_t *)out)[0].confidence = 0.91;
   return n;
}

int memory_scope_visibility_rank(int64_t memory_id, const char *workspace, const char *project)
{
   (void)workspace;
   return memory_id == 7 && project && strcmp(project, "proj-alpha") == 0
              ? g_code_context_memory_scope_rank
              : 0;
}

/* canonical_index_find_callers stub (used by the callers + hybrid route tests
 * below) — moved here from test_kb_http_routes.c to keep that file under the
 * 2000-line build-integrity limit; cast a void* like the other canonical stubs. */
typedef struct
{
   char project[128];
   char file_path[MAX_PATH_LEN];
   char caller[128];
   int line;
} test_caller_hit_t;

int canonical_index_find_callers(const char *project, const char *symbol, void *out, int max)
{
   assert(symbol);
   assert(out);
   if (strcmp(symbol, "target_fn") != 0)
      return 0;
   if (max < 1)
      return 0;
   test_caller_hit_t *hits = (test_caller_hit_t *)out;
   if (g_code_local_first_fixture && !project)
   {
      snprintf(hits[0].project, sizeof(hits[0].project), "proj-other");
      snprintf(hits[0].file_path, sizeof(hits[0].file_path), "other/caller.c");
      snprintf(hits[0].caller, sizeof(hits[0].caller), "other_caller");
      hits[0].line = 2;
      return 1;
   }
   if (!project || strcmp(project, "proj-alpha") != 0)
      return 0;
   snprintf(hits[0].project, sizeof(hits[0].project), "proj-alpha");
   snprintf(hits[0].file_path, sizeof(hits[0].file_path), "src/caller.c");
   snprintf(hits[0].caller, sizeof(hits[0].caller), "caller_fn");
   hits[0].line = 44;
   return 1;
}

int canonical_index_find_callers_excluding_project(const char *excluded_project, const char *symbol,
                                                   void *out, int max)
{
   assert(excluded_project && strcmp(excluded_project, "proj-alpha") == 0);
   return canonical_index_find_callers(NULL, symbol, out, max);
}

/* Cross-repo dependency stubs (S5): kb_http.o's route table keeps
 * handle_get_code_cross_repo_deps live, so the S4a/S4b/S2b entry points it calls
 * must resolve at link time. The full engine is covered by the dedicated
 * test_cross_repo_* units; here we only need empty, well-formed results. The
 * db2/cross_repo headers' types are passed as void pointers and int (same
 * void-cast pattern as the canonical_index stubs above) so we avoid pulling the
 * real headers/memory.h + headers/index.h, which conflict with this file's
 * truncated local typedefs. */
int canonical_index_cross_repo_deps(const char *project, const void *opts, void *out_edges,
                                    size_t *out_n, int *truncated)
{
   (void)project;
   (void)opts;
   if (out_edges)
      *(void **)out_edges = NULL;
   if (out_n)
      *out_n = 0;
   if (truncated)
      *truncated = 0;
   return 0;
}

int canonical_index_cross_repo_deps_ex(const char *project, const void *opts, void *out_edges,
                                       size_t *out_n, int *truncated, void *out_amb,
                                       size_t *out_amb_n)
{
   (void)project;
   (void)opts;
   if (out_edges)
      *(void **)out_edges = NULL;
   if (out_n)
      *out_n = 0;
   if (truncated)
      *truncated = 0;
   if (out_amb)
      *(void **)out_amb = NULL;
   if (out_amb_n)
      *out_amb_n = 0;
   return 0;
}

int db2_cross_repo_review_list(const char *caller_repo, const char *status, void *out, int max,
                               int64_t *overflow_dropped)
{
   (void)caller_repo;
   (void)status;
   (void)out;
   (void)max;
   if (overflow_dropped)
      *overflow_dropped = 0;
   return 0;
}

const char *xrepo_tier_name(int t)
{
   (void)t;
   return "none";
}

/* S7: trust-write entry points kb_http_code.o references from the repo-trust
 * handler (full behavior lives in test_cross_repo_stats). */
int db2_cross_repo_set_trust(const char *project, const char *new_trust, const char *actor,
                             const char *request_id, char *prior_out, size_t prior_cap,
                             int *changed_out)
{
   (void)project;
   (void)new_trust;
   (void)actor;
   (void)request_id;
   if (prior_out && prior_cap)
      prior_out[0] = '\0';
   if (changed_out)
      *changed_out = 0;
   return 0;
}

int db2_cross_repo_recompute_blocked_symbols(int k, int m, int len_min)
{
   (void)k;
   (void)m;
   (void)len_min;
   return 0;
}

/* Vector-leg stubs (§5): the query embedder + pgvec code search. OFF by default
 * (g_vec_enabled=0 -> memory_embed_text returns 0 -> the leg is skipped), so the
 * existing hybrid tests are unaffected; test_code_hybrid_vector_ok flips it on. */
static int g_vec_enabled = 0;
static int g_vec_search_unavailable = 0;
static int g_vec_unauthorized = 0;
int memory_embed_text(const char *text, const char *command, embed_input_type_t input_type,
                      float *out, int max_dim)
{
   (void)text;
   (void)command;
   (void)input_type;
   if (!g_vec_enabled || !out || max_dim <= 0)
      return 0;
   int d = 2560; /* the stub embedder's FIXED output dim (independent of the corpus
                  * dim) so the route's qdim==db2_embedding_dim() gate can mismatch. */
   if (d > max_dim)
      return 0;
   for (int i = 0; i < d; i++)
      out[i] = 0.01f * (float)(i % 7);
   return d;
}
int memory_embedder_last_result_unauthorized(void)
{
   return g_vec_unauthorized;
}
int pgvec_code_search_paths(const char *project, const float *vec, int dim, int limit, char *paths,
                            int path_cap, double *scores, int max)
{
   (void)project;
   (void)vec;
   (void)dim;
   (void)limit;
   if (g_vec_search_unavailable)
      return -1;
   if (!g_vec_enabled || !paths || path_cap <= 0 || !scores || max < 2)
      return 0;
   /* Two hits: src/search.c OVERLAPS the lexical leg (-> 2-signal consensus) and
    * src/semantic.c is vector-only (a file the lexical+graph legs would miss). */
   snprintf(paths + 0 * path_cap, (size_t)path_cap, "src/search.c");
   scores[0] = 0.91;
   snprintf(paths + 1 * path_cap, (size_t)path_cap, "src/semantic.c");
   scores[1] = 0.88;
   return 2;
}

/* §4 surprising-links candidate gather. Returns two high-cosine pairs for
 * proj-alpha: (file:x,file:y) are ABSENT from the projection edges (hub/a/b/c) so
 * they're disconnected -> surprising; (hub,a) is 1 hop -> filtered by d_min. */
int pgvec_code_similar_pairs(const char *project, int k, double min_cosine, int anchor_cap,
                             char *a_keys, char *b_keys, int key_cap, double *cosines, int max)
{
   (void)k;
   (void)min_cosine;
   (void)anchor_cap;
   if (!a_keys || !b_keys || key_cap <= 0 || !cosines || max < 2 || !project)
      return 0;
   if (strcmp(project, "proj-vecdown") == 0)
      return -1; /* simulate a vector-store outage (no connection / query error) */
   if (strcmp(project, "proj-alpha") == 0)
   {
      snprintf(a_keys + 0 * key_cap, (size_t)key_cap, "file:x");
      snprintf(b_keys + 0 * key_cap, (size_t)key_cap, "file:y");
      cosines[0] = 0.95;
      snprintf(a_keys + 1 * key_cap, (size_t)key_cap, "hub");
      snprintf(b_keys + 1 * key_cap, (size_t)key_cap, "a");
      cosines[1] = 0.90;
      return 2;
   }
   if (strcmp(project, "proj-hub") == 0)
   {
      /* The two files are similar; their only projection edges are to the project
       * hub, so the route must EXCLUDE those and report them disconnected. */
      snprintf(a_keys + 0 * key_cap, (size_t)key_cap, "file:f1");
      snprintf(b_keys + 0 * key_cap, (size_t)key_cap, "file:f2");
      cosines[0] = 0.97;
      return 1;
   }
   return 0;
}

/* Mirror of code_projection_edge_t (db2/code_projection.h) so the stub writes
 * fields at the offsets the handler reads; cast a void* rather than include the
 * db2 header (same pattern as the canonical_index stubs). */
typedef struct
{
   char source[512];
   char relation[64];
   char target[512];
   int structural_weight;
} test_projection_edge_t;

typedef struct
{
   const char *s, *r, *t;
   int w;
} test_seed_edge_t;

int db2_code_projection_list_edges(const char *project, void *out, int max)
{
   assert(out);
   if (max < 4)
      return 0;
   test_projection_edge_t *e = (test_projection_edge_t *)out;
   /* hub: out=2 (->a,->b), in=1 (c->); a,b,c: degree 1 each. */
   static const test_seed_edge_t alpha[] = {
       {"hub", "calls", "a", 1}, {"hub", "calls", "b", 1}, {"c", "calls", "hub", 1}};
   /* A single recursive edge: exercises the source==target==node self-loop path. */
   static const test_seed_edge_t selfloop[] = {{"rec", "calls", "rec", 1}};
   /* Two files linked ONLY through the project containment hub: the surprising
    * route must drop these `project:` edges before computing hop distance. */
   static const test_seed_edge_t hubonly[] = {{"project:proj-hub", "contains", "file:f1", 1},
                                              {"project:proj-hub", "contains", "file:f2", 1}};
   const test_seed_edge_t *rows = NULL;
   int n = 0;
   if (project && strcmp(project, "proj-alpha") == 0)
   {
      rows = alpha;
      n = (int)(sizeof(alpha) / sizeof(alpha[0]));
   }
   else if (project && strcmp(project, "proj-selfloop") == 0)
   {
      rows = selfloop;
      n = (int)(sizeof(selfloop) / sizeof(selfloop[0]));
   }
   else if (project && strcmp(project, "proj-hub") == 0)
   {
      rows = hubonly;
      n = (int)(sizeof(hubonly) / sizeof(hubonly[0]));
   }
   else
      return 0;
   for (int i = 0; i < n && i < max; i++)
   {
      memset(&e[i], 0, sizeof(e[i]));
      snprintf(e[i].source, sizeof(e[i].source), "%s", rows[i].s);
      snprintf(e[i].relation, sizeof(e[i].relation), "%s", rows[i].r);
      snprintf(e[i].target, sizeof(e[i].target), "%s", rows[i].t);
      e[i].structural_weight = rows[i].w;
   }
   return n;
}

/* graph-feedback S1 (self-audit route) stubs. The audit route reaches these DB2
 * projection helpers; the hermetic fixture reports a visible generation with no
 * persisted communities and no source hash. Signatures are ABI-compatible with
 * db2/code_projection.h (int64_t==long long, code_projection_community_t*==void*,
 * size_t) — the header is deliberately not included here (same reason the
 * list_edges stub above uses void*). The real prompt_sanitizer.o IS linked (pure,
 * no deps), so sanitize_for_prompt is exercised for real, not stubbed. */
long long db2_code_projection_visible_id(const char *project)
{
   (void)project;
   return 1;
}
int db2_code_projection_communities_list(long long gen_id, void *out, int max)
{
   (void)gen_id;
   (void)out;
   (void)max;
   return 0;
}
int db2_code_projection_visible_source_hash(const char *project, char *out, size_t out_len)
{
   (void)project;
   if (out && out_len)
      out[0] = '\0';
   return 0;
}

/* graph-feedback S2 (snapshot-diff route) stubs. ABI-compatible with
 * db2/code_projection.h; the hermetic fixture reports no arbitrary-generation
 * edges, no generation metadata (so a diff call 409s), and no generation list. */
int db2_code_projection_list_edges_for_gen(long long gen_id, void *out, int max)
{
   (void)gen_id;
   (void)out;
   (void)max;
   return 0;
}
int db2_code_projection_generation_meta(long long gen_id, void *out)
{
   (void)gen_id;
   (void)out;
   return 1; /* no such generation */
}
int db2_code_projection_generations_list(const char *project, void *out, int max)
{
   (void)project;
   (void)out;
   (void)max;
   return 0;
}

/* graph-feedback S3b (lessons route) stub: no outcome records in the hermetic
 * fixture, so the lessons artifact renders empty ("no lessons yet"). */
int64_t db2_lessons_record_outcome(const char *session_id, const char *turn_id,
                                   const char *project_id, int64_t generation_id,
                                   const char *answer_outcome, const char *correction_text,
                                   const char *finding_id, const char *actor_id,
                                   const char *actor_source, int confirmed)
{
   (void)session_id;
   (void)turn_id;
   (void)project_id;
   (void)generation_id;
   (void)answer_outcome;
   (void)correction_text;
   (void)finding_id;
   (void)actor_id;
   (void)actor_source;
   (void)confirmed;
   return 1;
}
int db2_lessons_record_citation(int64_t outcome_id, const char *node_id, const char *stance)
{
   (void)outcome_id;
   (void)node_id;
   (void)stance;
   return 0;
}
int db2_lessons_list_outcomes(const char *project_id, long long community_gen, void *out, int max)
{
   (void)project_id;
   (void)community_gen;
   (void)out;
   (void)max;
   return 0;
}

/* Hermetic mirror of the §3 provenance helper (kb_service_graph.c). The real
 * definition lives in a db2-heavy unit; this fake keeps the route test pure
 * while preserving the only branch the projection route exercises: a
 * code_projection edge is always "structural". */
const char *kb_graph_edge_provenance(const char *edge_origin, int structural_weight)
{
   if (structural_weight > 0 || (edge_origin && strcmp(edge_origin, "code_projection") == 0))
      return "structural";
   if (edge_origin && strcmp(edge_origin, "session") == 0)
      return "ambiguous";
   return "inferred";
}

static void test_code_structure_ok(void)
{
   char buf[512];
   int s = kb_http_route_ex("GET", "/v1/code/structure",
                            "project=proj-alpha&file_path=src/main.c&max_results=4", NULL, NULL,
                            NULL, 0, buf, sizeof(buf));
   assert(s == 200);
   assert(strstr(buf, "\"definitions\"") != NULL);
   assert(strstr(buf, "\"name\":\"main\"") != NULL);
   assert(strstr(buf, "\"kind\":\"function\"") != NULL);
   assert(strstr(buf, "\"line\":12") != NULL);
}

static void test_code_search_missing_query(void)
{
   char buf[256];
   int s = kb_http_route_ex("GET", "/v1/code/search", "", NULL, NULL, NULL, 0, buf, sizeof(buf));
   assert(s == 400);
   assert(strstr(buf, "missing query") != NULL);
}

static void test_code_search_ok(void)
{
   char buf[512];
   int s =
       kb_http_route_ex("GET", "/v1/code/search", "query=needle&project=proj-alpha&max_results=4",
                        NULL, NULL, NULL, 0, buf, sizeof(buf));
   assert(s == 200);
   assert(strstr(buf, "\"status\":\"ok\"") != NULL);
   assert(strstr(buf, "\"hits\"") != NULL);
   assert(strstr(buf, "\"file_path\":\"src/search.c\"") != NULL);
   assert(strstr(buf, "\"snippet\":\"int needle") != NULL);
   assert(strstr(buf, "\"rank\":0.75") != NULL);
   /* P2 Layer-1: the file content hash is surfaced for citation/drift. */
   assert(strstr(buf, "\"content_hash\":\"deadbeefcafe\"") != NULL);
   assert(strstr(buf, "\"next_cursor\":null") != NULL);
}

static void test_code_callers_missing_symbol(void)
{
   char buf[256];
   int s = kb_http_route_ex("GET", "/v1/code/callers", "", NULL, NULL, NULL, 0, buf, sizeof(buf));
   assert(s == 400);
   assert(strstr(buf, "missing symbol") != NULL);
}

static void test_code_callers_ok(void)
{
   char buf[512];
   int s = kb_http_route_ex("GET", "/v1/code/callers",
                            "symbol=target_fn&project=proj-alpha&max_results=4", NULL, NULL, NULL,
                            0, buf, sizeof(buf));
   assert(s == 200);
   assert(strstr(buf, "\"status\":\"ok\"") != NULL);
   assert(strstr(buf, "\"hits\"") != NULL);
   assert(strstr(buf, "\"file_path\":\"src/caller.c\"") != NULL);
   assert(strstr(buf, "\"caller\":\"caller_fn\"") != NULL);
   assert(strstr(buf, "\"line\":44") != NULL);
   assert(strstr(buf, "\"next_cursor\":null") != NULL);
}

static void test_code_scope_all_keeps_active_project_first(void)
{
   char buf[4096];
   g_code_local_first_fixture = 1;

   int s = kb_http_route_ex("GET", "/v1/code/find",
                            "identifier=foo&scope=all&project=proj-alpha&max_results=2", NULL, NULL,
                            NULL, 0, buf, sizeof(buf));
   assert(s == 200);
   const char *local = strstr(buf, "local/first.c");
   const char *other = strstr(buf, "other/high.c");
   assert(local && other && local < other);

   s = kb_http_route_ex("GET", "/v1/code/search",
                        "query=needle&scope=all&project=proj-alpha&max_results=2", NULL, NULL, NULL,
                        0, buf, sizeof(buf));
   assert(s == 200);
   local = strstr(buf, "src/search.c");
   other = strstr(buf, "other/high.c");
   assert(local && other && local < other);

   s = kb_http_route_ex("GET", "/v1/code/callers",
                        "symbol=target_fn&scope=all&project=proj-alpha&max_results=2", NULL, NULL,
                        NULL, 0, buf, sizeof(buf));
   assert(s == 200);
   local = strstr(buf, "src/caller.c");
   other = strstr(buf, "other/caller.c");
   assert(local && other && local < other);

   g_code_local_first_fixture = 0;
}

/* §5 hybrid retrieval: fuse lexical-code + graph-callers (RRF) + memory "why". */
static void test_code_hybrid_ok(void)
{
   char buf[2048];
   int s = kb_http_route_ex("GET", "/v1/code/hybrid",
                            "query=needle&symbol=target_fn&project=proj-alpha&max_results=10", NULL,
                            NULL, NULL, 0, buf, sizeof(buf));
   assert(s == 200);
   assert(strstr(buf, "\"status\":\"ok\"") != NULL);
   assert(strstr(buf, "\"results\"") != NULL);
   /* Both signals contribute a file, each labeled + enriched from its source. */
   assert(strstr(buf, "\"file_path\":\"src/search.c\"") != NULL); /* lexical-code leg */
   assert(strstr(buf, "\"signals\":[\"code\"]") != NULL);
   assert(strstr(buf, "\"snippet\":\"int needle") != NULL);
   assert(strstr(buf, "\"file_path\":\"src/caller.c\"") != NULL); /* graph-callers leg */
   assert(strstr(buf, "\"signals\":[\"graph\"]") != NULL);
   assert(strstr(buf, "\"caller\":\"caller_fn\"") != NULL);
   /* Memory recall surfaces as typed "why" context, not a fused row. */
   assert(strstr(buf, "\"why\"") != NULL);
   assert(strstr(buf, "why needle exists") != NULL);
}

/* §6 cross-session memory fusion: the knowledge graph connects the seed symbol to a
 * memory-extracted entity that resolves to a file the code/graph/vector legs never
 * see, fused in as a ranked "memory" signal (not just a why annotation). */
static void test_code_hybrid_memory_leg(void)
{
   char buf[2048];
   int s = kb_http_route_ex("GET", "/v1/code/hybrid",
                            "query=needle&symbol=target_fn&project=proj-alpha&max_results=10", NULL,
                            NULL, NULL, 0, buf, sizeof(buf));
   assert(s == 200);
   assert(strstr(buf, "\"file_path\":\"src/design_notes.c\"") != NULL);
   assert(strstr(buf, "\"signals\":[\"memory\"]") != NULL);
}

static void test_code_hybrid_keeps_same_path_projects_distinct(void)
{
   char buf[4096];
   g_code_hybrid_path_collision_fixture = 1;
   int s = kb_http_route_ex("GET", "/v1/code/hybrid",
                            "query=needle&scope=all&project=proj-alpha&max_results=2", NULL, NULL,
                            NULL, 0, buf, sizeof(buf));
   g_code_hybrid_path_collision_fixture = 0;
   assert(s == 200);
   const char *local = strstr(buf, "\"project\":\"proj-alpha\"");
   const char *other = strstr(buf, "\"project\":\"proj-other\"");
   assert(local && other && local < other);
   const char *first_path = strstr(buf, "\"file_path\":\"src/main.c\"");
   assert(first_path != NULL);
   assert(strstr(first_path + 1, "\"file_path\":\"src/main.c\"") != NULL);
}

static void test_code_hybrid_missing_query(void)
{
   char buf[256];
   int s = kb_http_route_ex("GET", "/v1/code/hybrid", "symbol=target_fn", NULL, NULL, NULL, 0, buf,
                            sizeof(buf));
   assert(s == 400);
   assert(strstr(buf, "missing query") != NULL);
}

/* No symbol => graph leg empty; lexical-code + memory still fuse/return. */
static void test_code_hybrid_no_symbol(void)
{
   char buf[2048];
   int s = kb_http_route_ex("GET", "/v1/code/hybrid", "query=needle&project=proj-alpha", NULL, NULL,
                            NULL, 0, buf, sizeof(buf));
   assert(s == 200);
   assert(strstr(buf, "\"file_path\":\"src/search.c\"") != NULL);
   assert(strstr(buf, "\"file_path\":\"src/caller.c\"") == NULL); /* no graph leg */
   assert(strstr(buf, "why needle exists") != NULL);
}

/* §5 vector leg: with a dim-matched embedder it fuses as a 3rd signal — a
 * vector-only file surfaces (labeled "vector" + score) and a file that BOTH the
 * lexical and vector legs return fuses to a 2-signal consensus row. */
static void test_code_hybrid_vector_ok(void)
{
   g_test_embedding_dim = 2560; /* matches the embed stub's returned dim */
   g_vec_enabled = 1;
   char buf[4096];
   int s = kb_http_route_ex("GET", "/v1/code/hybrid",
                            "query=needle&symbol=target_fn&project=proj-alpha&max_results=10", NULL,
                            NULL, NULL, 0, buf, sizeof(buf));
   g_vec_enabled = 0;
   g_test_embedding_dim = 1024;
   assert(s == 200);
   assert(strstr(buf, "\"vector_status\":\"ok\"") != NULL);
   /* The vector-only file appears, labeled and carrying its vector score. */
   assert(strstr(buf, "\"file_path\":\"src/semantic.c\"") != NULL);
   assert(strstr(buf, "\"vector\"") != NULL);
   assert(strstr(buf, "\"vector_score\"") != NULL);
   /* src/search.c was returned by BOTH the lexical and vector legs -> consensus. */
   assert(strstr(buf, "\"signals\":[\"code\",\"vector\"]") != NULL);
}

/* The dim gate: a wrong-dim embedder (output != corpus dim) disables the leg, so
 * the vector-only file never appears and the route degrades to code+graph. */
static void test_code_hybrid_vector_dim_mismatch_skips(void)
{
   g_test_embedding_dim = 1024; /* corpus dim != the stub's 2560 output */
   g_vec_enabled = 1;
   char buf[4096];
   int s = kb_http_route_ex("GET", "/v1/code/hybrid", "query=needle&project=proj-alpha", NULL, NULL,
                            NULL, 0, buf, sizeof(buf));
   g_vec_enabled = 0;
   assert(s == 200);
   assert(strstr(buf, "\"vector_status\":\"stale\"") != NULL);
   assert(strstr(buf, "\"file_path\":\"src/search.c\"") != NULL);   /* lexical still works */
   assert(strstr(buf, "\"file_path\":\"src/semantic.c\"") == NULL); /* vector leg skipped */
}

static void test_code_context_vector_store_outage_is_not_empty(void)
{
   g_test_embedding_dim = 2560;
   g_vec_enabled = 1;
   g_vec_search_unavailable = 1;
   char buf[4096];
   int s =
       kb_http_route_ex("GET", "/v1/code/context", "query=definitely-unrelated&project=proj-alpha",
                        NULL, NULL, NULL, 0, buf, sizeof(buf));
   g_vec_search_unavailable = 0;
   g_vec_enabled = 0;
   g_test_embedding_dim = 1024;
   assert(s == 503);
   assert(strstr(buf, "\"status\":\"unavailable\"") != NULL);
   assert(strstr(buf, "\"dependency\":\"vector_store\"") != NULL);
   assert(strstr(buf, "\"retry_after_ms\":1000") != NULL);
   assert(strstr(buf, "no_answer") == NULL);
}

static void test_code_context_dimension_mismatch_is_stale(void)
{
   g_test_embedding_dim = 1024;
   g_vec_enabled = 1;
   char buf[4096];
   int s =
       kb_http_route_ex("GET", "/v1/code/context", "query=definitely-unrelated&project=proj-alpha",
                        NULL, NULL, NULL, 0, buf, sizeof(buf));
   g_vec_enabled = 0;
   assert(s == 409);
   assert(strstr(buf, "\"status\":\"stale\"") != NULL);
   assert(strstr(buf, "\"dependency\":\"embedder\"") != NULL);
   assert(strstr(buf, "\"observed_dimension\":2560") != NULL);
   assert(strstr(buf, "\"current_dimension\":1024") != NULL);
   assert(strstr(buf, "\"retryable\":false") != NULL);
}

static void test_code_context_bounded_current_project(void)
{
   char buf[8192];
   int s = kb_http_route_ex("GET", "/v1/code/context",
                            "query=needle&symbol=target_fn&project=proj-alpha&max_results=99", NULL,
                            NULL, NULL, 0, buf, sizeof(buf));
   assert(s == 200);
   assert(strstr(buf, "\"status\":\"ok\"") != NULL);
   assert(strstr(buf, "\"project\":\"proj-alpha\"") != NULL);
   assert(strstr(buf, "\"generation\":2") != NULL);
   assert(strstr(buf, "\"freshness\":\"current\"") != NULL);
   assert(strstr(buf, "\"max_results\":4") != NULL);
   assert(strstr(buf, "\"max_tokens\":1200") != NULL);
   assert(strstr(buf, "\"accepted\":true") != NULL);
   assert(strstr(buf, "\"provenance\":[\"code\"]") != NULL);
   assert(strstr(buf, "\"line_start\":17") != NULL);
   assert(strstr(buf, "\"scope\":\"project\"") != NULL);
   assert(strstr(buf, "\"anchor\":{\"project\":\"proj-alpha\",\"file_path\":\"src/search.c\"") !=
          NULL);
   assert(strstr(buf, "\"file_path\":\"src/design_notes.c\"") == NULL);
   assert(strstr(buf, "other/high.c") == NULL);

   s = kb_http_route_ex("GET", "/v1/code/context", "query=needle&project=proj-alpha&generation=1",
                        NULL, NULL, NULL, 0, buf, sizeof(buf));
   assert(s == 409);
   assert(strstr(buf, "stale_generation") != NULL);
   assert(strstr(buf, "\"current_generation\":2") != NULL);

   s = kb_http_route_ex("GET", "/v1/code/context", "query=needle&project=proj-alpha&generation=2",
                        NULL, NULL, NULL, 0, buf, sizeof(buf));
   assert(s == 200);
}

/* With NO embedder configured, the route reports the missing dependency — it does not
 * answer "nothing matched".
 *
 * This test used to assert the opposite, and passed only because an unconfigured kb
 * resolved to a builtin lexical embedder: the vector leg looked live, returned nothing,
 * and the route called that an abstention. So "I have not configured retrieval" and "I
 * searched and found nothing" were the same answer, which is the confusion the builtin
 * caused everywhere. There is no builtin now, and a kb with no embedder refuses to
 * start, so the honest report is the dependency.
 *
 * Genuine abstention — a live embedder that matches nothing — is not reachable through
 * this stub set: g_vec_enabled=1 returns canned hits regardless of the query. */
static void test_code_context_without_an_embedder_reports_the_dependency(void)
{
   char buf[2048];
   int s =
       kb_http_route_ex("GET", "/v1/code/context", "query=definitely-unrelated&project=proj-alpha",
                        NULL, NULL, NULL, 0, buf, sizeof(buf));
   assert(s == 503);
   assert(strstr(buf, "\"dependency\":\"embedder\"") != NULL);
   /* Never a false negative: "no answer" would tell the caller the corpus lacks it. */
   assert(strstr(buf, "no_answer") == NULL);
}

static void test_code_context_embedder_outage_is_not_no_answer(void)
{
   char buf[2048];
   assert(setenv("EMBEDDER_URL", "http://embedder-down", 1) == 0);
   g_vec_enabled = 0;
   int s =
       kb_http_route_ex("GET", "/v1/code/context", "query=definitely-unrelated&project=proj-alpha",
                        NULL, NULL, NULL, 0, buf, sizeof(buf));
   unsetenv("EMBEDDER_URL");
   assert(s == 503);
   assert(strstr(buf, "\"status\":\"unavailable\"") != NULL);
   assert(strstr(buf, "\"dependency\":\"embedder\"") != NULL);
   assert(strstr(buf, "\"retryable\":true") != NULL);
   assert(strstr(buf, "\"retry_after_ms\":1000") != NULL);
   assert(strstr(buf, "no_answer") == NULL);
}

static void test_code_context_embedder_auth_is_unauthorized(void)
{
   char buf[2048];
   assert(setenv("EMBEDDER_URL", "http://embedder-auth", 1) == 0);
   g_vec_enabled = 0;
   g_vec_unauthorized = 1;
   int s =
       kb_http_route_ex("GET", "/v1/code/context", "query=definitely-unrelated&project=proj-alpha",
                        NULL, NULL, NULL, 0, buf, sizeof(buf));
   g_vec_unauthorized = 0;
   unsetenv("EMBEDDER_URL");
   assert(s == 401);
   assert(strstr(buf, "\"status\":\"unauthorized\"") != NULL);
   assert(strstr(buf, "\"dependency\":\"embedder\"") != NULL);
   assert(strstr(buf, "\"retryable\":false") != NULL);
   assert(strstr(buf, "retry_after_ms") == NULL);
   assert(strstr(buf, "no_answer") == NULL);
}

static void test_code_context_does_not_substitute_global_memory(void)
{
   char buf[8192];
   g_code_context_memory_scope_rank = 1;
   int s = kb_http_route_ex("GET", "/v1/code/context", "query=needle&project=proj-alpha", NULL,
                            NULL, NULL, 0, buf, sizeof(buf));
   g_code_context_memory_scope_rank = 3;
   assert(s == 200);
   assert(strstr(buf, "\"status\":\"ok\"") != NULL); /* code still answers */
   assert(strstr(buf, "why needle exists") == NULL);
   assert(strstr(buf, "\"why\":[]") != NULL);
}

static void test_code_context_requires_verified_memory_anchor(void)
{
   char buf[8192];
   g_code_context_memory_anchored = 0;
   int s = kb_http_route_ex("GET", "/v1/code/context", "query=needle&project=proj-alpha", NULL,
                            NULL, NULL, 0, buf, sizeof(buf));
   g_code_context_memory_anchored = 1;
   assert(s == 200);
   assert(strstr(buf, "\"status\":\"ok\"") != NULL);
   assert(strstr(buf, "why needle exists") == NULL);
   assert(strstr(buf, "\"why\":[]") != NULL);
}

static void test_code_project_stats_missing_project(void)
{
   char buf[256];
   int s =
       kb_http_route_ex("GET", "/v1/code/project-stats", "", NULL, NULL, NULL, 0, buf, sizeof(buf));
   assert(s == 409);
   assert(strstr(buf, "scope_required") != NULL);
}

/* §4 graph analytics: hub/degree-centrality ranking over the projection graph. */
static void test_code_graph_hubs_ok(void)
{
   char buf[1024];
   int s = kb_http_route_ex("GET", "/v1/code/graph/hubs", "project=proj-alpha&max_results=10", NULL,
                            NULL, NULL, 0, buf, sizeof(buf));
   assert(s == 200);
   assert(strstr(buf, "\"status\":\"ok\"") != NULL);
   assert(strstr(buf, "\"edge_count\":3") != NULL);
   assert(strstr(buf, "\"truncated\":false") != NULL);
   /* hub is the top node: degree 3 (out 2, in 1). */
   assert(strstr(buf, "\"node\":\"hub\"") != NULL);
   assert(strstr(buf, "\"degree\":3") != NULL);
   assert(strstr(buf, "\"out_degree\":2") != NULL);
   assert(strstr(buf, "\"in_degree\":1") != NULL);
   /* hub must appear before the leaf nodes in the ranked array. */
   const char *hub = strstr(buf, "\"node\":\"hub\"");
   const char *a = strstr(buf, "\"node\":\"a\"");
   assert(hub && a && hub < a);
}

static void test_code_graph_hubs_missing_project(void)
{
   char buf[256];
   int s =
       kb_http_route_ex("GET", "/v1/code/graph/hubs", "", NULL, NULL, NULL, 0, buf, sizeof(buf));
   assert(s == 409);
   assert(strstr(buf, "scope_required") != NULL);
}

/* §3b lessons route: an empty ledger (the stub returns no rows) renders the
 * honesty gate, not invented lessons; a missing active project is explicit. */
static void test_code_lessons_empty(void)
{
   char buf[1024];
   int s = kb_http_route_ex("GET", "/v1/code/lessons", "project=proj-alpha", NULL, NULL, NULL, 0,
                            buf, sizeof(buf));
   assert(s == 200);
   assert(strstr(buf, "\"status\":\"ok\"") != NULL);
   assert(strstr(buf, "\"clean\":true") != NULL);
   assert(strstr(buf, "no lessons yet") != NULL);
}

static void test_code_lessons_missing_project(void)
{
   char buf[256];
   int s = kb_http_route_ex("GET", "/v1/code/lessons", "", NULL, NULL, NULL, 0, buf, sizeof(buf));
   assert(s == 409);
   assert(strstr(buf, "scope_required") != NULL);
}

/* §6 memory-fusion leg stubs. The real db2_entity_edge_explain_t / db2_entity_node_t
 * (db2/entity_*.h) pull in memory.h's edge_t, which conflicts with this file's
 * simplified memory_t; so mirror the layouts and take void* (same pattern as the
 * code-projection stub). A symbol seed -> one knowledge-graph edge to a memory-
 * extracted entity that resolves to a file the code/graph/vector legs never see. */
typedef struct
{
   int64_t id;
   char source[512];
   char relation[64];
   char target[512];
   int weight;
   int structural_weight;
   double utility_score;
   char edge_origin[32];
} test_entity_edge_explain_t;
typedef struct
{
   char node_key[512];
   int node_kind;
   char project[256];
   char display_name[256];
   char full_key[512];
   char file_path[512];
   char symbol[256];
   char node_origin[32];
   int64_t last_seen_generation_id;
} test_entity_node_t;

int db2_entity_node_key_symbol(const char *project, const char *name, char *out, size_t cap)
{
   (void)project;
   if (!name || !out || cap == 0)
      return -1;
   snprintf(out, cap, "symbol:proj:%s", name);
   return 0;
}
int db2_entity_edge_explain_by_entity(const char *entity, void *out, int max)
{
   if (!entity || !out || max < 1)
      return 0;
   if (strcmp(entity, "symbol:proj:target_fn") != 0)
      return 0;
   test_entity_edge_explain_t *e = (test_entity_edge_explain_t *)out;
   memset(&e[0], 0, sizeof(e[0]));
   snprintf(e[0].source, sizeof(e[0].source), "%s", entity);
   snprintf(e[0].relation, sizeof(e[0].relation), "relates_to");
   snprintf(e[0].target, sizeof(e[0].target), "mement:proj:design");
   e[0].weight = 80;
   e[0].structural_weight = 0;
   return 1;
}
int db2_entity_node_get(const char *node_key, void *out)
{
   if (!node_key || !out)
      return -1;
   if (strcmp(node_key, "mement:proj:design") != 0)
      return -1;
   test_entity_node_t *n = (test_entity_node_t *)out;
   memset(n, 0, sizeof(*n));
   snprintf(n->node_key, sizeof(n->node_key), "%s", node_key);
   snprintf(n->file_path, sizeof(n->file_path), "src/design_notes.c");
   snprintf(n->node_origin, sizeof(n->node_origin), "memory_extraction");
   return 0;
}

/* §4 judge stub: confirm the first link (the disconnected file:x/file:y pair) with a
 * canned verdict so the route test stays hermetic (no curator-LLM link). Mirrors the
 * real kb_surprising_judge writing verdicts parallel to the links. */
int kb_surprising_judge(const char *judge_cmd, const char *project,
                        const kb_graph_surprising_t *links, int n, kb_surprising_verdict_t *out,
                        char *errbuf, size_t errlen)
{
   (void)judge_cmd;
   (void)project;
   (void)links;
   (void)errbuf;
   (void)errlen;
   if (!out || n <= 0)
      return -1;
   for (int i = 0; i < n; i++)
      memset(&out[i], 0, sizeof(out[i]));
   out[0].sent = 1;
   out[0].judged = 1;
   out[0].confirmed = 1;
   out[0].shared_symbols = 2;
   snprintf(out[0].reason, sizeof(out[0].reason), "parallel auth implementations");
   return 1;
}

/* §8 read-only node-neighborhood projection. Seed edges: hub->a, hub->b, c->hub. */
static void test_code_graph_node_ok(void)
{
   char buf[2048];
   int s = kb_http_route_ex("GET", "/v1/code/graph", "project=proj-alpha&node=hub&max_results=10",
                            NULL, NULL, NULL, 0, buf, sizeof(buf));
   assert(s == 200);
   assert(strstr(buf, "\"node\":\"hub\"") != NULL);
   /* hub --calls--> a, b (out); c --calls--> hub (in). */
   assert(strstr(buf, "\"neighbor\":\"a\"") != NULL);
   assert(strstr(buf, "\"neighbor\":\"b\"") != NULL);
   assert(strstr(buf, "\"neighbor\":\"c\"") != NULL);
   assert(strstr(buf, "\"direction\":\"out\"") != NULL);
   assert(strstr(buf, "\"direction\":\"in\"") != NULL);
   assert(strstr(buf, "\"relation\":\"calls\"") != NULL);
   /* projection edges are AST-derived -> §3 provenance "structural". */
   assert(strstr(buf, "\"provenance\":\"structural\"") != NULL);
   assert(strstr(buf, "\"neighbor_count\":3") != NULL);
   /* all 3 incident edges fit under max_results=10 -> complete neighborhood. */
   assert(strstr(buf, "\"match_count\":3") != NULL);
   assert(strstr(buf, "\"truncated\":false") != NULL);
}

/* The page cap (max_results) must drive `truncated` independently of the DB scan
 * window: 3 incident edges, max_results=1 -> 1 emitted, truncated=true. */
static void test_code_graph_node_capped_truncates(void)
{
   char buf[2048];
   int s = kb_http_route_ex("GET", "/v1/code/graph", "project=proj-alpha&node=hub&max_results=1",
                            NULL, NULL, NULL, 0, buf, sizeof(buf));
   assert(s == 200);
   assert(strstr(buf, "\"neighbor_count\":1") != NULL);
   assert(strstr(buf, "\"match_count\":3") != NULL);
   assert(strstr(buf, "\"truncated\":true") != NULL);
}

/* A recursive edge (rec->rec) is emitted once with direction "self". */
static void test_code_graph_node_self_loop(void)
{
   char buf[2048];
   int s = kb_http_route_ex("GET", "/v1/code/graph", "project=proj-selfloop&node=rec", NULL, NULL,
                            NULL, 0, buf, sizeof(buf));
   assert(s == 200);
   assert(strstr(buf, "\"neighbor\":\"rec\"") != NULL);
   assert(strstr(buf, "\"direction\":\"self\"") != NULL);
   assert(strstr(buf, "\"neighbor_count\":1") != NULL);
   /* exactly one entry: the else-if ladder must not double-emit a self-loop. */
   assert(strstr(buf, "\"direction\":\"out\"") == NULL);
   assert(strstr(buf, "\"direction\":\"in\"") == NULL);
}

/* §4 surprising links: high-cosine + graph-distant pairs. With d_min=2 the (hub,a)
 * 1-hop pair is filtered and only the disconnected (file:x,file:y) pair surfaces. */
static void test_code_graph_surprising_ok(void)
{
   char buf[4096];
   int s = kb_http_route_ex("GET", "/v1/code/graph/surprising",
                            "project=proj-alpha&percentile=0&d_min=2&max_results=10", NULL, NULL,
                            NULL, 0, buf, sizeof(buf));
   assert(s == 200);
   assert(strstr(buf, "\"candidate_count\":2") != NULL); /* both pairs gathered */
   /* the disconnected pair is surprising; the 1-hop (hub,a) pair is not. */
   assert(strstr(buf, "\"a\":\"file:x\"") != NULL);
   assert(strstr(buf, "\"b\":\"file:y\"") != NULL);
   assert(strstr(buf, "\"disconnected\":true") != NULL);
   assert(strstr(buf, "\"link_count\":1") != NULL);
   /* hub/a are graph-adjacent (1 hop < d_min=2) -> excluded from the links. */
   assert(strstr(buf, "\"a\":\"hub\"") == NULL);
}

/* Project-hub exclusion: two files whose ONLY edges are to the project containment
 * node. With those dropped the coupling graph is empty, so the pair is disconnected
 * and surfaces even at d_min=3 (without exclusion they'd be 2 hops via the hub and
 * be filtered). edge_count==0 proves the hub edges were excluded. */
static void test_code_graph_surprising_hub_excluded(void)
{
   char buf[2048];
   int s = kb_http_route_ex("GET", "/v1/code/graph/surprising",
                            "project=proj-hub&percentile=0&d_min=3&max_results=10", NULL, NULL,
                            NULL, 0, buf, sizeof(buf));
   assert(s == 200);
   assert(strstr(buf, "\"edge_count\":0") != NULL); /* both edges were project-incident */
   assert(strstr(buf, "\"a\":\"file:f1\"") != NULL);
   assert(strstr(buf, "\"disconnected\":true") != NULL);
   assert(strstr(buf, "\"link_count\":1") != NULL);
}

static void test_code_graph_surprising_missing_project(void)
{
   char buf[256];
   int s = kb_http_route_ex("GET", "/v1/code/graph/surprising", "", NULL, NULL, NULL, 0, buf,
                            sizeof(buf));
   assert(s == 409);
   assert(strstr(buf, "scope_required") != NULL);
}

/* judge=true runs the §4 confirmation: the stubbed judge confirms the first link, so
 * the route annotates it with confirmed/reason/shared_symbols + a top-level judged. */
static void test_code_graph_surprising_judge(void)
{
   char buf[4096];
   int s = kb_http_route_ex("GET", "/v1/code/graph/surprising",
                            "project=proj-alpha&percentile=0&d_min=2&judge=true&max_results=10",
                            NULL, NULL, NULL, 0, buf, sizeof(buf));
   assert(s == 200);
   assert(strstr(buf, "\"confirmed\":true") != NULL);
   assert(strstr(buf, "\"shared_symbols\":2") != NULL);
   assert(strstr(buf, "parallel auth implementations") != NULL);
   assert(strstr(buf, "\"judged\":1") != NULL);
}

/* §4 self-suppress: with the floor enabled and the rolling judge-sampled precision
 * below it (2/100 = 2% < 10%), an UNJUDGED request returns no links + suppressed:true. */
static void test_code_graph_surprising_self_suppress(void)
{
   char buf[4096];
   g_precision_floor = 0.10;
   g_sj_judged = 100;
   g_sj_confirmed = 2;
   int s = kb_http_route_ex("GET", "/v1/code/graph/surprising",
                            "project=proj-alpha&percentile=0&d_min=2&max_results=10", NULL, NULL,
                            NULL, 0, buf, sizeof(buf));
   assert(s == 200);
   assert(strstr(buf, "\"suppressed\":true") != NULL);
   assert(strstr(buf, "\"link_count\":0") != NULL);
   assert(strstr(buf, "\"sampled_precision\":0.02") != NULL);

   /* precision at/above the floor -> NOT suppressed, links shown. */
   g_sj_confirmed = 50; /* 50% */
   s = kb_http_route_ex("GET", "/v1/code/graph/surprising",
                        "project=proj-alpha&percentile=0&d_min=2&max_results=10", NULL, NULL, NULL,
                        0, buf, sizeof(buf));
   assert(s == 200);
   assert(strstr(buf, "\"suppressed\":true") == NULL);
   assert(strstr(buf, "\"link_count\":1") != NULL); /* the disconnected pair surfaces */

   g_precision_floor = 0.0; /* reset for other tests */
   g_sj_judged = -1;
   g_sj_confirmed = -1;
}

/* A vector-store failure (pgvec gather returns <0) is surfaced as 503, not a silent
 * empty 200 — an outage must be distinguishable from "no surprising links". */
static void test_code_graph_surprising_vecstore_down(void)
{
   char buf[512];
   int s = kb_http_route_ex("GET", "/v1/code/graph/surprising", "project=proj-vecdown", NULL, NULL,
                            NULL, 0, buf, sizeof(buf));
   assert(s == 503);
   assert(strstr(buf, "vector store unavailable") != NULL);
}

static void test_code_graph_node_missing_params(void)
{
   char buf[512];
   int s = kb_http_route_ex("GET", "/v1/code/graph", "project=proj-alpha", NULL, NULL, NULL, 0, buf,
                            sizeof(buf));
   assert(s == 400);
   assert(strstr(buf, "missing node") != NULL);
   s = kb_http_route_ex("GET", "/v1/code/graph", "node=hub", NULL, NULL, NULL, 0, buf, sizeof(buf));
   assert(s == 409);
   assert(strstr(buf, "scope_required") != NULL);
}

static void test_code_project_stats_ok(void)
{
   char buf[512];
   int s = kb_http_route_ex("GET", "/v1/code/project-stats", "project=proj-alpha", NULL, NULL, NULL,
                            0, buf, sizeof(buf));
   assert(s == 200);
   assert(strstr(buf, "\"status\":\"ok\"") != NULL);
   assert(strstr(buf, "\"project\":\"proj-alpha\"") != NULL);
   assert(strstr(buf, "\"files\":9") != NULL);
   assert(strstr(buf, "\"definitions\":4") != NULL);
   assert(strstr(buf, "\"langs\"") != NULL);
   assert(strstr(buf, "\"lang\":\"c\"") != NULL);
   assert(strstr(buf, "\"count\":7") != NULL);
}

static void test_code_project_stats_error_is_json(void)
{
   char buf[512];
   int s = kb_http_route_ex("GET", "/v1/code/project-stats", "project=proj-missing", NULL, NULL,
                            NULL, 0, buf, sizeof(buf));
   assert(s == 503);
   cJSON *json = cJSON_Parse(buf);
   assert(json);
   cJSON *error = cJSON_GetObjectItemCaseSensitive(json, "error");
   assert(cJSON_IsString(error));
   assert(strstr(error->valuestring, "canonical index unavailable") != NULL);
   cJSON_Delete(json);
}

static void test_blast_radius_missing_params(void)
{
   char buf[256];
   int s = kb_http_route_ex("GET", "/v1/code/blast-radius", NULL, NULL, NULL, NULL, 0, buf,
                            sizeof(buf));
   assert(s == 409);
   assert(strstr(buf, "scope_required") != NULL);
}

static void test_blast_radius_not_found(void)
{
   char buf[256];
   int s = kb_http_route_ex("GET", "/v1/code/blast-radius", "project=myproj&file_path=src/foo.c",
                            NULL, NULL, NULL, 0, buf, sizeof(buf));
   assert(s == 404);
}

static void test_blast_radius_ok(void)
{
   char buf[2048];
   int s =
       kb_http_route_ex("GET", "/v1/code/blast-radius", "project=proj-alpha&file_path=src/main.c",
                        NULL, NULL, NULL, 0, buf, sizeof(buf));
   assert(s == 200);
   assert(strstr(buf, "\"file\":\"src/main.c\"") != NULL);
   assert(strstr(buf, "\"dependents\"") != NULL);
   assert(strstr(buf, "\"src/app.c\"") != NULL);
   assert(strstr(buf, "\"dependent_count\":1") != NULL);
   assert(strstr(buf, "\"dependencies\"") != NULL);
   assert(strstr(buf, "\"src/lib.c\"") != NULL);
   assert(strstr(buf, "\"dependency_count\":1") != NULL);
   assert(strstr(buf, "\"resolved\":true") != NULL);
   assert(strstr(buf, "\"generation\":9") != NULL);
   assert(strstr(buf, "\"dependent_edges\"") != NULL);
   assert(strstr(buf, "\"provenance\":\"import\"") != NULL);
   assert(strstr(buf, "\"freshness\":\"current\"") != NULL);
}

/* §6 live-idempotency stubs: the scan route gates the git re-walk on the default-
 * branch SHA. g_branch_sha drives git_resolve_default_sha (empty -> unresolvable, the
 * gate is skipped); g_stored_sha is the persisted last-indexed SHA. */
static char g_branch_sha[128] = "";
static char g_stored_sha[128] = "";
static char g_runtime_state_set_val[128] = "";
int git_resolve_default_sha(const char *root, char *out, size_t outlen)
{
   (void)root;
   if (!g_branch_sha[0] || !out || outlen == 0)
      return -1;
   snprintf(out, outlen, "%s", g_branch_sha);
   return 0;
}
int db2_kb_runtime_state_get(const char *key, char *out, size_t out_len)
{
   if (!out || out_len == 0)
      return -1;
   if (key && strncmp(key, "surprising_judged:", 18) == 0)
   {
      if (g_sj_judged < 0)
         return -1;
      snprintf(out, out_len, "%d", g_sj_judged);
      return 0;
   }
   if (key && strncmp(key, "surprising_confirmed:", 21) == 0)
   {
      if (g_sj_confirmed < 0)
         return -1;
      snprintf(out, out_len, "%d", g_sj_confirmed);
      return 0;
   }
   snprintf(out, out_len, "%s", g_stored_sha);
   return g_stored_sha[0] ? 0 : -1;
}
int db2_kb_runtime_state_set(const char *key, const char *value)
{
   (void)key;
   snprintf(g_runtime_state_set_val, sizeof(g_runtime_state_set_val), "%s", value ? value : "");
   return 0;
}
/* Faithful mirror of the pure gate (its real logic is unit-tested in test_code_collect). */
int code_default_branch_changed(const char *stored_sha, const char *current_sha)
{
   if (!current_sha || !current_sha[0])
      return 0;
   if (!stored_sha || !stored_sha[0])
      return 1;
   return strcmp(stored_sha, current_sha) != 0;
}
static int g_worktree_mode = 0; /* drives the SHA-gate bypass under the worktree opt-in */
int code_index_source_is_worktree(void)
{
   return g_worktree_mode;
}
static int g_hook_install_rc = 0; /* return for the §6 post-merge hook installer stub */
static char g_hook_project[128] = "";
int code_index_install_branch_hook(const char *project_root, const char *project_name)
{
   (void)project_root;
   snprintf(g_hook_project, sizeof(g_hook_project), "%s", project_name ? project_name : "");
   return g_hook_install_rc;
}

/* Unchanged default branch (stored SHA == current) + !force -> skip the re-walk. */
static void test_code_scan_skips_unchanged_branch(void)
{
   char buf[512];
   g_db_initialized = 1;
   g_code_scan_rc = 5;
   snprintf(g_branch_sha, sizeof(g_branch_sha), "tree-aaa");
   snprintf(g_stored_sha, sizeof(g_stored_sha), "tree-aaa");
   const char *body = "{\"project\":\"proj-alpha\",\"root_path\":\"/tmp/repo\"}";
   int s = kb_http_route_ex("POST", "/v1/code/scan", NULL, NULL, NULL, body, (int)strlen(body), buf,
                            sizeof(buf));
   g_branch_sha[0] = '\0'; /* reset so later tests see an unresolvable SHA (gate off) */
   assert(s == 200);
   assert(strstr(buf, "\"skipped\":true") != NULL);
   assert(strstr(buf, "default branch unchanged") != NULL);
}

/* Branch moved (stored != current) -> the walk is QUEUED rather than skipped.
 * The SHA is deliberately NOT persisted here: the route has not done the walk,
 * and recording it would claim the project was indexed at that SHA before any
 * of the work ran -- a later failure would then leave the claim standing and
 * every !force scan would skip a project that was never ingested. The worker
 * records it after the walk succeeds. */
static void test_code_scan_runs_on_branch_move(void)
{
   char buf[512];
   g_db_initialized = 1;
   g_code_scan_rc = 5;
   g_code_scan_inspected = 3;
   g_runtime_state_set_val[0] = '\0';
   snprintf(g_branch_sha, sizeof(g_branch_sha), "tree-bbb");
   snprintf(g_stored_sha, sizeof(g_stored_sha), "tree-aaa");
   const char *body = "{\"project\":\"proj-alpha\",\"root_path\":\"/tmp/repo\"}";
   int s = kb_http_route_ex("POST", "/v1/code/scan", NULL, NULL, NULL, body, (int)strlen(body), buf,
                            sizeof(buf));
   g_branch_sha[0] = '\0';
   assert(s == 200);
   assert(strstr(buf, "\"skipped\":false") != NULL); /* not declined: accepted and queued */
   assert(strstr(buf, "\"queued\":true") != NULL);
   assert(g_runtime_state_set_val[0] == '\0'); /* the worker persists it, not the route */
}

/* Under the worktree opt-in the branch-SHA gate is bypassed: even an unchanged
 * default-branch SHA must NOT skip, because the index tracks the working tree. */
static void test_code_scan_worktree_ignores_sha(void)
{
   char buf[512];
   g_db_initialized = 1;
   g_code_scan_rc = 5;
   g_worktree_mode = 1;
   snprintf(g_branch_sha, sizeof(g_branch_sha), "tree-aaa");
   snprintf(g_stored_sha, sizeof(g_stored_sha), "tree-aaa"); /* would skip in default mode */
   const char *body = "{\"project\":\"proj-alpha\",\"root_path\":\"/tmp/repo\"}";
   int s = kb_http_route_ex("POST", "/v1/code/scan", NULL, NULL, NULL, body, (int)strlen(body), buf,
                            sizeof(buf));
   g_worktree_mode = 0;
   g_branch_sha[0] = '\0';
   assert(s == 200);
   assert(strstr(buf, "\"skipped\":false") != NULL); /* worktree mode never skips */
}

/* §6 live: install_hook=true on a git repo (resolved SHA) installs the post-merge
 * reindex hook and reports it; absent the opt-in the hook is never installed. */
static void test_code_scan_installs_hook(void)
{
   char buf[512];
   g_db_initialized = 1;
   g_code_scan_rc = 5;
   g_hook_install_rc = 0;
   g_hook_project[0] = '\0';
   snprintf(g_branch_sha, sizeof(g_branch_sha), "tree-ccc");
   snprintf(g_stored_sha, sizeof(g_stored_sha), "tree-bbb"); /* moved -> scans */
   const char *body =
       "{\"project\":\"proj-alpha\",\"root_path\":\"/tmp/repo\",\"install_hook\":true}";
   int s = kb_http_route_ex("POST", "/v1/code/scan", NULL, NULL, NULL, body, (int)strlen(body), buf,
                            sizeof(buf));
   g_branch_sha[0] = '\0';
   assert(s == 200);
   assert(strstr(buf, "\"hook_installed\":true") != NULL);
   assert(strcmp(g_hook_project, "proj-alpha") == 0);

   /* without the opt-in: no install, hook_installed:false. */
   g_hook_project[0] = '\0';
   snprintf(g_branch_sha, sizeof(g_branch_sha), "tree-ddd");
   snprintf(g_stored_sha, sizeof(g_stored_sha), "tree-ccc");
   const char *body2 = "{\"project\":\"proj-alpha\",\"root_path\":\"/tmp/repo\"}";
   s = kb_http_route_ex("POST", "/v1/code/scan", NULL, NULL, NULL, body2, (int)strlen(body2), buf,
                        sizeof(buf));
   g_branch_sha[0] = '\0';
   assert(s == 200);
   assert(strstr(buf, "\"hook_installed\":false") != NULL);
   assert(g_hook_project[0] == '\0'); /* installer not called */
}

static void test_code_scan_ok(void)
{
   char buf[512];
   g_db_initialized = 1;
   g_code_scan_rc = 5;
   g_code_scan_inspected = 12;
   g_code_scan_force = 0;
   g_code_scan_project[0] = '\0';
   g_ingest_project[0] = '\0';
   g_ingest_root[0] = '\0';
   g_ingest_force = 0;
   g_ingest_priority = -1;
   g_curator_code_queued = 0;
   const char *body = "{\"project\":\"proj-alpha\",\"root_path\":\"/tmp/repo\",\"force\":true}";
   int s = kb_http_route_ex("POST", "/v1/code/scan", NULL, NULL, NULL, body, (int)strlen(body), buf,
                            sizeof(buf));
   /* A root_path scan QUEUES the walk; it does not perform it on this request
    * thread. Doing it inline held a db2 connection for the whole walk and made
    * the caller wait out a timeout it could not size -- which was then recorded
    * as the KB being unreachable. The route's promise is that the files are
    * queued and ready to be ingested, so it answers immediately with no counts. */
   assert(s == 200);
   assert(strstr(buf, "\"status\":\"ok\"") != NULL);
   assert(strstr(buf, "\"reason\":\"queued\"") != NULL);
   assert(strstr(buf, "\"skipped\":false") != NULL); /* accepted, not declined */
   assert(strstr(buf, "\"queued\":true") != NULL);
   assert(strstr(buf, "\"files\":0") != NULL);
   /* The inline scanner is NOT called for this path any more. */
   assert(g_code_scan_project[0] == '\0');
   /* The work reached the queue, with the caller's project, root and force, at
    * interactive priority because someone is waiting on the result. */
   assert(strcmp(g_ingest_project, "proj-alpha") == 0);
   assert(strcmp(g_ingest_root, "/tmp/repo") == 0);
   assert(g_ingest_force == 1);
   assert(g_ingest_priority == DB2_KB_INGEST_PRIO_INTERACTIVE);
   /* A scan INDEXES; it does not curate. Curation is enqueued by the embed stage
    * once the project is fully embedded, so the pipeline order is
    * indexed -> embedded -> curated rather than indexed -> curated (racing embed).
    * Enqueuing here also put a one-row-per-symbol INSERT on the synchronous path
    * of this request -- ~173,000 rows and ~215s on a 4,018-file corpus, against a
    * client that times out at 300s. */
   assert(g_curator_code_queued == 0);
}

static void test_code_project_lifecycle_routes(void)
{
   const char *owner = "owner-secret";
   const char *owner_auth = "Bearer owner-secret";
   char buf[2048];
   const char *dry = "{\"project\":\"proj-alpha\"}";
   int s = kb_http_route_ex("POST", "/v1/code/project/purge", NULL, owner_auth, owner, dry,
                            (int)strlen(dry), buf, sizeof(buf));
   assert(s == 200);
   assert(strstr(buf, "\"mode\":\"dry_run\"") != NULL);
   assert(strstr(buf, "\"manifest_hash\":\"sha256:test\"") != NULL);
   assert(strstr(buf, "\"code_files\":3") != NULL);

   const char *confirm = "{\"project\":\"proj-alpha\",\"confirm_hash\":\"sha256:test\","
                         "\"principal\":\"forged-body-actor\",\"reason\":\"approved cleanup\"}";
   g_lifecycle_audit_principal[0] = '\0';
   s = kb_http_route_ex("POST", "/v1/code/project/purge", NULL, owner_auth, owner, confirm,
                        (int)strlen(confirm), buf, sizeof(buf));
   assert(s == 200);
   assert(strstr(buf, "\"mode\":\"confirmed\"") != NULL);
   assert(strcmp(g_lifecycle_audit_principal, "owner") == 0);

   g_lifecycle_audit_principal[0] = '\0';
   s = kb_http_route_ex("POST", "/v1/code/project/detach", NULL, owner_auth, owner, dry,
                        (int)strlen(dry), buf, sizeof(buf));
   assert(s == 200);
   assert(strstr(buf, "\"state\":\"detached\"") != NULL);
   assert(strcmp(g_lifecycle_audit_principal, "owner") == 0);

   char oversized_project[257];
   memset(oversized_project, 'p', sizeof(oversized_project) - 1);
   oversized_project[sizeof(oversized_project) - 1] = '\0';
   char oversized_body[320];
   snprintf(oversized_body, sizeof(oversized_body), "{\"project\":\"%s\"}", oversized_project);
   s = kb_http_route_ex("POST", "/v1/code/project/purge", NULL, owner_auth, owner, oversized_body,
                        (int)strlen(oversized_body), buf, sizeof(buf));
   assert(s == 400);
   assert(strstr(buf, "project must be at most 255 characters") != NULL);

   /* Auth-off mode has no verified actor and must not permit anonymous lifecycle
    * operations, including dry runs that reveal an exact destructive manifest. */
   s = kb_http_route_ex("POST", "/v1/code/project/purge", NULL, NULL, NULL, dry, (int)strlen(dry),
                        buf, sizeof(buf));
   assert(s == 403);
   assert(strstr(buf, "owner credential") != NULL);

   const char *scoped = "scope:project:proj-alpha:secret";
   s = kb_http_route_ex("POST", "/v1/code/project/detach", NULL,
                        "Bearer scope:project:proj-alpha:secret", scoped, dry, (int)strlen(dry),
                        buf, sizeof(buf));
   assert(s == 403);
   assert(strstr(buf, "owner credential") != NULL);
}

static void test_code_scan_missing_root_path(void)
{
   char buf[256];
   const char *body = "{\"project\":\"proj-alpha\"}";
   int s = kb_http_route_ex("POST", "/v1/code/scan", NULL, NULL, NULL, body, (int)strlen(body), buf,
                            sizeof(buf));
   assert(s == 400);
   assert(strstr(buf, "missing root_path") != NULL);
}

static void test_code_scan_pushed_files_ok(void)
{
   char buf[512];
   g_db_initialized = 1;
   g_code_scan_files_rc = 2;
   g_code_scan_files_inspected = 2;
   g_code_scan_files_force = 0;
   g_code_scan_files_count = 0;
   g_code_scan_file_project[0] = '\0';
   g_code_scan_file_root[0] = '\0';
   g_code_scan_file_rel[0] = '\0';
   g_code_scan_file_content[0] = '\0';
   g_curator_code_queued = 0;
   const char *body =
       "{\"project\":\"proj-alpha\",\"root_path\":\"remote-root\",\"force\":true,"
       "\"files\":[{\"rel_path\":\"src/a.c\",\"content\":\"int pushed(void) { return 1; }\"},"
       "{\"rel_path\":\"src/b.c\",\"content\":\"int other(void) { return 2; }\"}]}";
   int s = kb_http_route_ex("POST", "/v1/code/scan", NULL, NULL, NULL, body, (int)strlen(body), buf,
                            sizeof(buf));
   assert(s == 200);
   assert(strstr(buf, "\"files\":2") != NULL);
   assert(strstr(buf, "\"inspected\":2") != NULL);
   assert(strcmp(g_code_scan_file_project, "proj-alpha") == 0);
   assert(strcmp(g_code_scan_file_root, "remote-root") == 0);
   assert(g_code_scan_files_count == 2);
   assert(strcmp(g_code_scan_file_rel, "src/a.c") == 0);
   assert(strstr(g_code_scan_file_content, "pushed") != NULL);
   assert(g_code_scan_files_force == 1);
   /* Same for a thin-client push: it indexes, it does not curate. The embed sweep
    * reaches these projects by NAME, so nothing is lost by not enqueuing here --
    * the push path never needed its own enqueue, it needed the embed stage to own
    * one. */
   assert(g_curator_code_queued == 0);
}

static void test_code_scan_pushed_files_rejects_invalid_item(void)
{
   char buf[256];
   g_db_initialized = 1;
   const char *body = "{\"project\":\"proj-alpha\",\"files\":[{\"rel_path\":\"src/a.c\"}]}";
   int s = kb_http_route_ex("POST", "/v1/code/scan", NULL, NULL, NULL, body, (int)strlen(body), buf,
                            sizeof(buf));
   assert(s == 400);
   assert(strstr(buf, "invalid files array") != NULL);
}

static void test_code_scan_db_unavailable(void)
{
   char buf[256];
   g_db_initialized = 0;
   const char *body = "{\"project\":\"proj-alpha\",\"root_path\":\"/tmp/repo\"}";
   int s = kb_http_route_ex("POST", "/v1/code/scan", NULL, NULL, NULL, body, (int)strlen(body), buf,
                            sizeof(buf));
   assert(s == 503);
   assert(strstr(buf, "knowledge service store") != NULL);
   g_db_initialized = 1;
}

/* test_code_build_ok, test_code_build_rejects_partial_project_embedding and
 * test_maintenance_repair_ok were REMOVED, not ported: they asserted that /v1/code/build did the
 * work inline (kb_build called with the caller's path, a 503 when inline code embedding came back
 * partial). That contract is gone -- the route commits the work to the queue and answers
 * immediately, and embedding completes when it completes. Keeping them adapted would have pinned
 * the shape that made a long build report itself as a failure. The replacements are
 * test_code_build_queues_instead_of_embedding_inline and
 * test_maintenance_repair_queues_too. */

static void test_code_update_ok(void)
{
   char buf[1024];
   g_db_initialized = 1;
   g_kb_update_rc = 0;
   g_code_scan_rc = 0;
   g_runtime_state_set_now = 0;
   g_curator_docs_queued = 0;
   const char *body =
       "{\"path\":\"/tmp/kb\",\"project\":\"proj-alpha\",\"embedding_command\":\"embed-a\"}";
   int s = kb_http_route_ex("POST", "/v1/code/update", NULL, NULL, NULL, body, (int)strlen(body),
                            buf, sizeof(buf));
   assert(s == 200);
   assert(strcmp(g_kb_update_path, "/tmp/kb") == 0);
   assert(strcmp(g_kb_update_project, "proj-alpha") == 0);
   assert(strcmp(g_kb_update_embed_cmd, "embed-a") == 0);
   assert(g_runtime_state_set_now == 1);
   assert(g_curator_docs_queued == 1);
   assert(strstr(buf, "\"files_indexed\":6") != NULL);
   assert(strstr(buf, "\"embeddings_added\":4") != NULL);
}

static void test_ingest_enqueue_ok(void)
{
   char buf[1024];
   g_db_initialized = 1;
   g_discover_count = 1;
   g_worker_notify_count = 0;
   const char *body = "{\"workspace\":\"/workspace\",\"force\":true}";
   int s = kb_http_route_ex("POST", "/v1/ingest", NULL, NULL, NULL, body, (int)strlen(body), buf,
                            sizeof(buf));
   assert(s == 202);
   assert(strcmp(g_ingest_project, "proj-alpha") == 0);
   assert(strcmp(g_ingest_root, "/workspace/proj-alpha") == 0);
   assert(strcmp(g_ingest_workspace, "/workspace") == 0);
   assert(g_ingest_force == 1);
   /* An HTTP ingest is a request someone is waiting on, so it must enqueue ABOVE
    * the background sweep — otherwise it queues behind a whole reindex. */
   assert(g_ingest_priority == DB2_KB_INGEST_PRIO_INTERACTIVE);
   assert(g_ingest_priority > DB2_KB_INGEST_PRIO_BULK);
   assert(g_worker_notify_count == 1);
   assert(strstr(buf, "\"projects_queued\":1") != NULL);
}

static void test_pipeline_status_ok(void)
{
   char buf[512];
   g_queue_status = (db2_kb_service_async_queue_stats_t){
       .pending = 4,
       .running = 2,
       .done = 7,
       .failed = 1,
       .total = 14,
   };
   int s =
       kb_http_route_ex("GET", "/v1/pipeline/status", NULL, NULL, NULL, NULL, 0, buf, sizeof(buf));
   assert(s == 200);
   assert(strstr(buf, "queue_depth") != NULL);
   assert(strstr(buf, "\"state\":\"running\"") != NULL);
   assert(strstr(buf, "\"queue_depth\":6") != NULL);
   assert(strstr(buf, "\"running\":2") != NULL);
}

static void test_ingest_status_ok(void)
{
   char buf[512];
   int s =
       kb_http_route_ex("GET", "/v1/ingest/status", NULL, NULL, NULL, NULL, 0, buf, sizeof(buf));
   assert(s == 200);
   assert(strstr(buf, "\"done_last_24h\":9") != NULL);
   assert(strstr(buf, "\"active\":1") != NULL);
}

static void test_ingest_status_wrong_method(void)
{
   char buf[256];
   int s =
       kb_http_route_ex("POST", "/v1/ingest/status", NULL, NULL, NULL, "{}", 2, buf, sizeof(buf));
   assert(s == 405);
}

static void test_workers_ok(void)
{
   char buf[512];
   int s = kb_http_route_ex("GET", "/v1/workers", NULL, NULL, NULL, NULL, 0, buf, sizeof(buf));
   assert(s == 200);
   assert(strstr(buf, "\"configured\":2") != NULL);
   assert(strstr(buf, "\"method\":\"v1.http\"") != NULL);
   assert(strstr(buf, "\"maintenance-timer\"") != NULL);
}

static void test_workers_wrong_method(void)
{
   char buf[256];
   int s = kb_http_route_ex("POST", "/v1/workers", NULL, NULL, NULL, "{}", 2, buf, sizeof(buf));
   assert(s == 405);
}

static void test_pipeline_status_failed(void)
{
   char buf[512];
   g_queue_status = (db2_kb_service_async_queue_stats_t){
       .failed = 2,
       .total = 2,
   };
   int s =
       kb_http_route_ex("GET", "/v1/pipeline/status", NULL, NULL, NULL, NULL, 0, buf, sizeof(buf));
   assert(s == 200);
   assert(strstr(buf, "\"state\":\"failed\"") != NULL);
   assert(strstr(buf, "\"queue_depth\":0") != NULL);
}

static void test_drain_ok(void)
{
   char buf[512];
   const char *body = "{\"embedding_command\":\"test-embed\",\"timeout\":9}";
   g_drain_rc = 0;
   int s = kb_http_route_ex("POST", "/v1/drain", NULL, NULL, NULL, body, (int)strlen(body), buf,
                            sizeof(buf));
   assert(s == 200);
   assert(strcmp(g_drain_embed_cmd, "test-embed") == 0);
   assert(g_drain_timeout == 9);
   assert(strcmp(g_drain_collection, "kb-test") == 0);
   assert(strstr(buf, "\"processed\":3") != NULL);
   assert(strstr(buf, "\"pending\":1") != NULL);
}

static void test_drain_default_embedding_command(void)
{
   char buf[512];
   g_drain_embed_cmd[0] = '\0';
   g_drain_rc = 0;
   int s = kb_http_route_ex("POST", "/v1/drain", NULL, NULL, NULL, "{}", 2, buf, sizeof(buf));
   assert(s == 200);
   /* No embedder configured resolves to the empty string, not to a name nothing
    * implements — the drain then embeds nothing rather than exec'ing it. */
   assert(g_drain_embed_cmd[0] == '\0');
}

static void test_drain_error(void)
{
   char buf[512];
   g_drain_rc = -1;
   int s = kb_http_route_ex("POST", "/v1/drain", NULL, NULL, NULL, "{}", 2, buf, sizeof(buf));
   assert(s == 500);
   assert(strstr(buf, "queue drain failed") != NULL);
   g_drain_rc = 0;
}

/* Every /v1/maintenance/ route is destructive and owner-gated, so the tests that
 * exercise the mechanics below must first authenticate as the owner: an unscoped
 * credential whose presented value matches the configured one. */
#define OWNER_AUTH "Bearer owner-secret"
#define OWNER_TOK  "owner-secret"

/* The gate itself: a scoped service bearer — what an aimee-server carries — must
 * never reach a maintenance route, and auth-off must not fall open either. Both
 * are checked on every route in the family, because the gate is by prefix and a
 * per-route regression would otherwise hide behind its neighbours. */

/* A scoped credential must not reach another project's data, including on the
 * POST routes that name their target in the body rather than the query string.
 * These read/write a project's index, so a bypass here is cross-tenant. */
static void test_scoped_token_cannot_cross_project_via_body(void)
{
   const char *scoped_auth = "Bearer scope:project:proj-alpha:secret";
   const char *scoped_tok = "scope:project:proj-alpha:secret";
   static const struct
   {
      const char *path;
      const char *foreign;
      const char *own;
   } cases[] = {
       {"/v1/code/build", "{\"path\":\"/tmp/kb\",\"project\":\"proj-beta\"}",
        "{\"path\":\"/tmp/kb\",\"project\":\"proj-alpha\"}"},
       {"/v1/code/update", "{\"path\":\"/tmp/kb\",\"project\":\"proj-beta\"}",
        "{\"path\":\"/tmp/kb\",\"project\":\"proj-alpha\"}"},
       {"/v1/code/scan", "{\"project\":\"proj-beta\",\"root_path\":\"/tmp/repo\"}",
        "{\"project\":\"proj-alpha\",\"root_path\":\"/tmp/repo\"}"},
   };
   char buf[2048];
   for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
   {
      /* Another project, named only in the body and with no query string —
       * the shape that used to resolve to "no scope named" and be allowed. */
      int s = kb_http_route_ex("POST", cases[i].path, NULL, scoped_auth, scoped_tok,
                               cases[i].foreign, (int)strlen(cases[i].foreign), buf, sizeof(buf));
      assert(s == 403);
      assert(strstr(buf, "cannot access project:proj-beta") != NULL);

      /* The credential's own project is unaffected. */
      s = kb_http_route_ex("POST", cases[i].path, NULL, scoped_auth, scoped_tok, cases[i].own,
                           (int)strlen(cases[i].own), buf, sizeof(buf));
      assert(s != 403);
   }
}

/* POST /v1/ingest with no workspace (or "all") targets every project in every
 * configured workspace, and with force:true that CLEARS each one's vectors, kb
 * rows and file index before re-queueing. Naming no workspace also names no
 * scope, so the generic layer cannot deny it — the route must. */
static void test_scoped_token_cannot_ingest_all_projects(void)
{
   const char *scoped_auth = "Bearer scope:project:proj-alpha:secret";
   const char *scoped_tok = "scope:project:proj-alpha:secret";
   char buf[1024];

   static const char *const all_bodies[] = {
       "{}",                      /* workspace absent */
       "{\"workspace\":\"all\"}", /* explicit all */
       "{\"force\":true}",        /* the destructive shape */
       "{\"workspace\":\"all\",\"force\":true}",
   };
   for (size_t i = 0; i < sizeof(all_bodies) / sizeof(all_bodies[0]); i++)
   {
      int s = kb_http_route_ex("POST", "/v1/ingest", NULL, scoped_auth, scoped_tok, all_bodies[i],
                               (int)strlen(all_bodies[i]), buf, sizeof(buf));
      assert(s == 403);
      assert(strstr(buf, "cannot ingest all projects") != NULL);
   }

   /* A named workspace still goes through the ordinary scope check rather than
    * this one, and the owner is not restricted at all. */
   const char *named = "{\"workspace\":\"ws-a\"}";
   int s = kb_http_route_ex("POST", "/v1/ingest", NULL, OWNER_AUTH, OWNER_TOK, named,
                            (int)strlen(named), buf, sizeof(buf));
   assert(s != 403);
   s = kb_http_route_ex("POST", "/v1/ingest", NULL, OWNER_AUTH, OWNER_TOK, "{\"force\":true}", 15,
                        buf, sizeof(buf));
   assert(s != 403); /* the owner may still ingest everything */
}

/* A `service` credential is aimee-server's data-plane identity: it reaches any
 * project and may ingest the whole deployment, because indexing everything is
 * the job it exists for. What it must NOT gain is privilege — it is still a
 * scoped token, so every administrative gate keeps refusing it. That asymmetry
 * is the entire point of issuing one, so both halves are pinned here. */
static void test_service_scope_is_data_plane_not_admin(void)
{
   const char *svc_auth = "Bearer scope:service:aimee-server:secret";
   const char *svc_tok = "scope:service:aimee-server:secret";
   char buf[2048];

   /* MAY: any project, named in the body — the shape a project-scoped token is
    * refused for. */
   const char *other = "{\"path\":\"/tmp/kb\",\"project\":\"proj-beta\"}";
   int s = kb_http_route_ex("POST", "/v1/code/build", NULL, svc_auth, svc_tok, other,
                            (int)strlen(other), buf, sizeof(buf));
   assert(s != 403);

   /* MAY: ingest every project (workspace absent, and the destructive force
    * shape) — routine work for the indexer. */
   s = kb_http_route_ex("POST", "/v1/ingest", NULL, svc_auth, svc_tok, "{}", 2, buf, sizeof(buf));
   assert(s != 403);
   s = kb_http_route_ex("POST", "/v1/ingest", NULL, svc_auth, svc_tok, "{\"force\":true}", 15, buf,
                        sizeof(buf));
   assert(s != 403);

   /* MUST NOT: any maintenance route. This is the boundary that makes a service
    * credential worth issuing instead of the owner token. */
   static const char *const admin_routes[] = {
       "/v1/maintenance/repair",          "/v1/maintenance/reconcile",
       "/v1/maintenance/clear",           "/v1/maintenance/purge-project",
       "/v1/maintenance/purge-heartbeat", "/v1/maintenance/purge-finalize",
       "/v1/maintenance/purge-cancel",
   };
   const char *body = "{\"project\":\"proj-alpha\",\"path\":\"/tmp/kb\","
                      "\"generation\":\"g1\",\"purge_id\":\"p1\"}";
   for (size_t i = 0; i < sizeof(admin_routes) / sizeof(admin_routes[0]); i++)
   {
      s = kb_http_route_ex("POST", admin_routes[i], NULL, svc_auth, svc_tok, body,
                           (int)strlen(body), buf, sizeof(buf));
      assert(s == 403);
      assert(strstr(buf, "owner credential") != NULL);
   }

   /* MUST NOT: widen into non-data-plane scopes. A service token spans projects
    * and workspaces only — never another kind's resources. */
   assert(kb_scope_authorized("service", "aimee-server", "project", "anything") == 1);
   assert(kb_scope_authorized("service", "aimee-server", "workspace", "anything") == 1);
   assert(kb_scope_authorized("service", "aimee-server", "user", "someone") == 0);
   assert(kb_scope_authorized("service", "aimee-server", "console-admin", "x") == 0);
   assert(kb_scope_authorized("service", "aimee-server", "curator", "x") == 0);

   /* MUST NOT: be minted by the console — only the owner issues one. */
   const char *mint = "{\"host\":\"h\",\"port\":1,\"scope\":\"service:aimee-server\"}";
   s = kb_http_route_ex("POST", "/v1/enroll", NULL, "Bearer scope:console-admin:c:secret",
                        "scope:console-admin:c:secret", mint, (int)strlen(mint), buf, sizeof(buf));
   assert(s == 403);
}

static void test_maintenance_routes_are_owner_gated(void)
{
   static const char *const routes[] = {
       "/v1/maintenance/repair",          "/v1/maintenance/reconcile",
       "/v1/maintenance/clear",           "/v1/maintenance/purge-project",
       "/v1/maintenance/purge-heartbeat", "/v1/maintenance/purge-finalize",
       "/v1/maintenance/purge-cancel",
   };
   const char *body = "{\"project\":\"proj-alpha\",\"path\":\"/tmp/kb\","
                      "\"generation\":\"g1\",\"purge_id\":\"p1\"}";
   char buf[1024];
   for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++)
   {
      /* A project-scoped service credential is authenticated but not admin. */
      int s = kb_http_route_ex("POST", routes[i], NULL, "Bearer scope:project:proj-alpha:secret",
                               "scope:project:proj-alpha:secret", body, (int)strlen(body), buf,
                               sizeof(buf));
      assert(s == 403);
      assert(strstr(buf, "owner credential") != NULL);

      /* Auth-off manufactures no actor, so it is refused rather than open. */
      s = kb_http_route_ex("POST", routes[i], NULL, NULL, NULL, body, (int)strlen(body), buf,
                           sizeof(buf));
      assert(s == 403);
      assert(strstr(buf, "owner credential") != NULL);

      /* The owner is not refused — it gets past the gate into the handler,
       * whatever that handler then decides about the request itself. */
      s = kb_http_route_ex("POST", routes[i], NULL, OWNER_AUTH, OWNER_TOK, body, (int)strlen(body),
                           buf, sizeof(buf));
      assert(s != 403);
   }

   /* The gate is POST-shaped like the routes it guards, but it is keyed on the
    * path prefix, so an unknown maintenance verb is refused too rather than
    * falling through to a 404 that confirms the route does not exist. */
   int s = kb_http_route_ex(
       "POST", "/v1/maintenance/not-a-real-route", NULL, "Bearer scope:project:proj-alpha:secret",
       "scope:project:proj-alpha:secret", body, (int)strlen(body), buf, sizeof(buf));
   assert(s == 403);
}

static void test_maintenance_repair_missing_project(void)
{
   char buf[256];
   int s = kb_http_route_ex("POST", "/v1/maintenance/repair", NULL, OWNER_AUTH, OWNER_TOK,
                            "{\"path\":\"/tmp/kb\"}", 18, buf, sizeof(buf));
   assert(s == 400);
   assert(strstr(buf, "missing project") != NULL);
}

static void test_maintenance_reconcile_ok(void)
{
   char buf[512];
   g_reconcile_rc = 0;
   int s = kb_http_route_ex("POST", "/v1/maintenance/reconcile", NULL, OWNER_AUTH, OWNER_TOK,
                            "{\"dry_run\":true}", 16, buf, sizeof(buf));
   assert(s == 200);
   assert(g_reconcile_dry_run == 1);
   assert(strstr(buf, "\"dry_run\":true") != NULL);
   assert(strstr(buf, "\"pruned\":1") != NULL);
   assert(strstr(buf, "\"kept\":6") != NULL);
}

static void test_maintenance_clear_ok(void)
{
   char buf[512];
   g_clear_deleted = 12;
   int s = kb_http_route_ex("POST", "/v1/maintenance/clear", NULL, OWNER_AUTH, OWNER_TOK,
                            "{\"project\":\"proj-alpha\"}", 24, buf, sizeof(buf));
   assert(s == 200);
   assert(strcmp(g_clear_project, "proj-alpha") == 0);
   assert(strstr(buf, "\"chunks_deleted\":12") != NULL);
}

static void test_maintenance_clear_error(void)
{
   char buf[256];
   g_clear_deleted = -1;
   int s = kb_http_route_ex("POST", "/v1/maintenance/clear", NULL, OWNER_AUTH, OWNER_TOK,
                            "{\"project\":\"proj-alpha\"}", 24, buf, sizeof(buf));
   assert(s == 500);
   assert(strstr(buf, "kb clear failed") != NULL);
   g_clear_deleted = 12;
}

/* ── slice-2 purge routes: fence lifecycle + fan-out map shape ─────────── */

static void purge_fence_reset(void)
{
   g_fence_present = 0;
   g_fence_live = 0;
   g_fence_write_rc = 0;
   g_fence_gen[0] = g_fence_pid[0] = g_fence_project[0] = '\0';
   g_fence_heartbeats = 0;
}

static void test_purge_project_writes_fence(void)
{
   purge_fence_reset();
   g_clear_deleted = 12;
   char buf[2048];
   int s =
       kb_http_route_ex("POST", "/v1/maintenance/purge-project", NULL, OWNER_AUTH, OWNER_TOK,
                        "{\"project\":\"proj-alpha\",\"generation\":\"g1\",\"purge_id\":\"p1\"}",
                        -1, buf, sizeof(buf));
   assert(s == 200);
   /* Fence written for the project and NOT cleared by the purge route. */
   assert(g_fence_present == 1);
   assert(strcmp(g_fence_project, "proj-alpha") == 0);
   assert(strcmp(g_fence_gen, "g1") == 0);
   assert(strcmp(g_fence_pid, "p1") == 0);
   assert(strstr(buf, "\"status\":\"ok\"") != NULL);
   assert(strstr(buf, "\"ok\":true") != NULL);
   assert(strstr(buf, "\"fence_replaced\":false") != NULL);
   /* Per-store map, in fan-out order, counts from the stubs (0 for the
    * no-count primitives pdf_vectors/minhash/vectors). */
   assert(strstr(buf, "\"stores\":{") != NULL);
   assert(strstr(buf, "\"chunks\":12") != NULL);
   assert(strstr(buf, "\"file_index\":0") != NULL);
   assert(strstr(buf, "\"vectors\":0") != NULL);
   assert(strstr(buf, "\"code_embeddings\":7") != NULL);
   assert(strstr(buf, "\"curator_code_unit_vectors\":3") != NULL);
   assert(strstr(buf, "\"canonical_index\":1") != NULL);
   assert(strstr(buf, "\"code_unit_jobs\":4") != NULL);
   assert(strstr(buf, "\"pdf_vectors\":0") != NULL);
   assert(strstr(buf, "\"minhash\":0") != NULL);
   assert(strcmp(g_clear_project, "proj-alpha") == 0);
}

static void test_purge_project_live_fence_409(void)
{
   purge_fence_reset();
   assert(db2_kb_purge_fence_write("proj-alpha", "g0", "p0") == 0);
   g_fence_live = 1;
   char buf[1024];
   int s =
       kb_http_route_ex("POST", "/v1/maintenance/purge-project", NULL, OWNER_AUTH, OWNER_TOK,
                        "{\"project\":\"proj-alpha\",\"generation\":\"g1\",\"purge_id\":\"p1\"}",
                        -1, buf, sizeof(buf));
   assert(s == 409);
   assert(strstr(buf, "purge fence held") != NULL);
   assert(strstr(buf, "\"generation\":\"g0\"") != NULL);
   assert(strstr(buf, "\"purge_id\":\"p0\"") != NULL);
   /* Refused: the original owner's fence is untouched. */
   assert(strcmp(g_fence_gen, "g0") == 0);
   assert(strcmp(g_fence_pid, "p0") == 0);
}

static void test_purge_project_takeover_displaces(void)
{
   purge_fence_reset();
   assert(db2_kb_purge_fence_write("proj-alpha", "g0", "p0") == 0);
   g_fence_live = 1;
   char buf[2048];
   int s = kb_http_route_ex("POST", "/v1/maintenance/purge-project", NULL, OWNER_AUTH, OWNER_TOK,
                            "{\"project\":\"proj-alpha\",\"generation\":\"g1\",\"purge_id\":"
                            "\"p1\",\"takeover\":true}",
                            -1, buf, sizeof(buf));
   assert(s == 200);
   assert(strstr(buf, "\"fence_replaced\":true") != NULL);
   assert(strstr(buf, "\"displaced\":{\"generation\":\"g0\",\"purge_id\":\"p0\"}") != NULL);
   /* New owner holds the fence now. */
   assert(strcmp(g_fence_gen, "g1") == 0);
   assert(strcmp(g_fence_pid, "p1") == 0);
}

static void test_purge_project_stale_fence_replaced(void)
{
   purge_fence_reset();
   assert(db2_kb_purge_fence_write("proj-alpha", "g0", "p0") == 0);
   g_fence_live = 0; /* stale heartbeat: no takeover needed */
   char buf[2048];
   int s =
       kb_http_route_ex("POST", "/v1/maintenance/purge-project", NULL, OWNER_AUTH, OWNER_TOK,
                        "{\"project\":\"proj-alpha\",\"generation\":\"g1\",\"purge_id\":\"p1\"}",
                        -1, buf, sizeof(buf));
   assert(s == 200);
   assert(strstr(buf, "\"fence_replaced\":true") != NULL);
   assert(strcmp(g_fence_gen, "g1") == 0);
}

static void test_purge_project_store_error_continues(void)
{
   purge_fence_reset();
   g_purge_canonical_rc = -1; /* one store fails: fan-out continues, ok:false */
   char buf[2048];
   int s =
       kb_http_route_ex("POST", "/v1/maintenance/purge-project", NULL, OWNER_AUTH, OWNER_TOK,
                        "{\"project\":\"proj-alpha\",\"generation\":\"g1\",\"purge_id\":\"p1\"}",
                        -1, buf, sizeof(buf));
   assert(s == 200);
   assert(strstr(buf, "\"ok\":false") != NULL);
   assert(strstr(buf, "\"canonical_index\":{\"error\":\"delete failed\"}") != NULL);
   /* Stores after the failing one still ran and report counts. */
   assert(strstr(buf, "\"code_unit_jobs\":4") != NULL);
   assert(strstr(buf, "\"minhash\":0") != NULL);
   g_purge_canonical_rc = 1;
}

static void test_purge_project_bad_request(void)
{
   purge_fence_reset();
   char buf[512];
   int s =
       kb_http_route_ex("POST", "/v1/maintenance/purge-project", NULL, OWNER_AUTH, OWNER_TOK,
                        "{\"project\":\"proj-alpha\",\"purge_id\":\"p1\"}", -1, buf, sizeof(buf));
   assert(s == 400);
   assert(strstr(buf, "generation") != NULL);
   /* Space-carrying tokens would corrupt the space-separated fence value. */
   s = kb_http_route_ex("POST", "/v1/maintenance/purge-project", NULL, OWNER_AUTH, OWNER_TOK,
                        "{\"project\":\"proj-alpha\",\"generation\":\"g 1\",\"purge_id\":\"p1\"}",
                        -1, buf, sizeof(buf));
   assert(s == 400);
   assert(g_fence_present == 0);
}

static void test_purge_heartbeat_match_and_mismatch(void)
{
   purge_fence_reset();
   assert(db2_kb_purge_fence_write("proj-alpha", "g1", "p1") == 0);
   char buf[1024];
   int s =
       kb_http_route_ex("POST", "/v1/maintenance/purge-heartbeat", NULL, OWNER_AUTH, OWNER_TOK,
                        "{\"project\":\"proj-alpha\",\"generation\":\"g1\",\"purge_id\":\"p1\"}",
                        -1, buf, sizeof(buf));
   assert(s == 200);
   assert(strstr(buf, "\"refreshed\":true") != NULL);
   assert(g_fence_heartbeats == 1);
   /* Displaced owner: mismatch no-ops and echoes the current fence. */
   s = kb_http_route_ex("POST", "/v1/maintenance/purge-heartbeat", NULL, OWNER_AUTH, OWNER_TOK,
                        "{\"project\":\"proj-alpha\",\"generation\":\"g0\",\"purge_id\":\"p0\"}",
                        -1, buf, sizeof(buf));
   assert(s == 200);
   assert(strstr(buf, "\"refreshed\":false") != NULL);
   assert(strstr(buf, "\"fence\":\"g1 p1\"") != NULL);
   assert(g_fence_heartbeats == 1);
}

static void test_purge_finalize_mismatch_noop(void)
{
   purge_fence_reset();
   assert(db2_kb_purge_fence_write("proj-alpha", "g1", "p1") == 0);
   char buf[1024];
   int s =
       kb_http_route_ex("POST", "/v1/maintenance/purge-finalize", NULL, OWNER_AUTH, OWNER_TOK,
                        "{\"project\":\"proj-alpha\",\"generation\":\"g0\",\"purge_id\":\"p1\"}",
                        -1, buf, sizeof(buf));
   assert(s == 200);
   assert(strstr(buf, "\"cleared\":false") != NULL);
   assert(strstr(buf, "\"fence\":\"g1 p1\"") != NULL);
   assert(g_fence_present == 1); /* displaced owner cannot clear the fence */
}

static void test_purge_finalize_match_clears(void)
{
   purge_fence_reset();
   assert(db2_kb_purge_fence_write("proj-alpha", "g1", "p1") == 0);
   char buf[1024];
   int s =
       kb_http_route_ex("POST", "/v1/maintenance/purge-finalize", NULL, OWNER_AUTH, OWNER_TOK,
                        "{\"project\":\"proj-alpha\",\"generation\":\"g1\",\"purge_id\":\"p1\"}",
                        -1, buf, sizeof(buf));
   assert(s == 200);
   assert(strstr(buf, "\"cleared\":true") != NULL);
   assert(g_fence_present == 0);
}

static void test_purge_cancel_match_clears(void)
{
   purge_fence_reset();
   assert(db2_kb_purge_fence_write("proj-alpha", "g1", "p1") == 0);
   char buf[1024];
   int s =
       kb_http_route_ex("POST", "/v1/maintenance/purge-cancel", NULL, OWNER_AUTH, OWNER_TOK,
                        "{\"project\":\"proj-alpha\",\"generation\":\"g1\",\"purge_id\":\"p1\"}",
                        -1, buf, sizeof(buf));
   assert(s == 200);
   assert(strstr(buf, "\"cleared\":true") != NULL);
   assert(g_fence_present == 0);
   /* Idempotent: a second cancel is a mismatch/absent no-op. */
   s = kb_http_route_ex("POST", "/v1/maintenance/purge-cancel", NULL, OWNER_AUTH, OWNER_TOK,
                        "{\"project\":\"proj-alpha\",\"generation\":\"g1\",\"purge_id\":\"p1\"}",
                        -1, buf, sizeof(buf));
   assert(s == 200);
   assert(strstr(buf, "\"cleared\":false") != NULL);
}

static void test_job_not_found(void)
{
   char buf[256];
   g_job_get_rc = 0;
   int s = kb_http_route_ex("GET", "/v1/jobs/job-1", NULL, NULL, NULL, NULL, 0, buf, sizeof(buf));
   assert(s == 404);
   assert(strstr(buf, "not found") != NULL);
   s = kb_http_route_ex("GET", "/v1/jobs/999", NULL, NULL, NULL, NULL, 0, buf, sizeof(buf));
   assert(s == 404);
   assert(g_job_get_id == 999);
   assert(strstr(buf, "not found") != NULL);
   g_job_get_rc = 1;
}

static void test_job_status_ok(void)
{
   char buf[1024];
   int s = kb_http_route_ex("GET", "/v1/jobs/42", NULL, NULL, NULL, NULL, 0, buf, sizeof(buf));
   assert(s == 200);
   assert(g_job_get_id == 42);
   assert(strstr(buf, "\"id\":42") != NULL);
   assert(strstr(buf, "\"document_id\":77") != NULL);
   assert(strstr(buf, "\"project\":\"proj-alpha\"") != NULL);
   assert(strstr(buf, "\"status\":\"running\"") != NULL);
   assert(strstr(buf, "\"last_error\":\"retry \\\"later\\\"\"") != NULL);
}

static void test_job_status_error(void)
{
   char buf[256];
   g_job_get_rc = -1;
   int s = kb_http_route_ex("GET", "/v1/jobs/42", NULL, NULL, NULL, NULL, 0, buf, sizeof(buf));
   assert(s == 503);
   assert(strstr(buf, "job status unavailable") != NULL);
   g_job_get_rc = 1;
}
static void test_entity_profile_not_found(void)
{
   char buf[256];
   int s =
       kb_http_route_ex("GET", "/v1/entities/nobody", NULL, NULL, NULL, NULL, 0, buf, sizeof(buf));
   assert(s == 404);
}

static void test_entity_search_ok(void)
{
   char buf[512];
   int s = kb_http_route_ex("POST", "/v1/entities/search", NULL, NULL, NULL,
                            "{\"query\":\"alice\"}", 16, buf, sizeof(buf));
   assert(s == 200);
   assert(strstr(buf, "\"entities\"") != NULL);
}

static void test_phase5_auth_rejected(void)
{
   char buf[256];
   int s = kb_http_route_ex("POST", "/v1/search", NULL, NULL, "secret", "{\"query\":\"foo\"}", 15,
                            buf, sizeof(buf));
   assert(s == 401);
}

static void test_phase1_fallthrough(void)
{
   char buf[256];
   int s = kb_http_route_ex("GET", "/v1/health", NULL, NULL, NULL, NULL, 0, buf, sizeof(buf));
   assert(s == 200);
   assert(strstr(buf, "\"ok\"") != NULL);
}

/* ── Bearer-token scope isolation ────────────────────────────────────────── */

static void test_scope_token_cross_scope_denied(void)
{
   /* A token scoped project:alpha is configured. A request targeting
    * workspace:beta must be rejected with 403; a request in its own scope
    * (project:alpha) passes. */
   const char *tok = "scope:project:alpha:s3cr3t";
   const char *auth = "Bearer scope:project:alpha:s3cr3t";
   char buf[1024];

   /* cross-scope (workspace=beta) → 403 */
   int s = kb_http_route_ex("GET", "/v1/search", "query=x&workspace=beta", auth, tok, NULL, 0, buf,
                            sizeof(buf));
   assert(s == 403);
   assert(strstr(buf, "forbidden") != NULL);

   /* cross-scope (project=gamma) → 403 */
   s = kb_http_route_ex("GET", "/v1/search", "query=x&project=gamma", auth, tok, NULL, 0, buf,
                        sizeof(buf));
   assert(s == 403);

   /* in-scope (project=alpha) → not 403 (auth + scope both pass) */
   s = kb_http_route_ex("GET", "/v1/search", "query=x&project=alpha", auth, tok, NULL, 0, buf,
                        sizeof(buf));
   assert(s != 403 && s != 401);
}

static void test_scope_token_secret_auth(void)
{
   /* The presented credential may be the full configured token or its secret
    * part; a wrong secret is 401. */
   const char *tok = "scope:project:alpha:s3cr3t";
   char buf[512];

   /* full token matches */
   assert(kb_http_route_ex("GET", "/v1/health", NULL, "Bearer scope:project:alpha:s3cr3t", tok,
                           NULL, 0, buf, sizeof(buf)) != 401);
   /* bare secret matches */
   assert(kb_http_route_ex("GET", "/v1/health", NULL, "Bearer s3cr3t", tok, NULL, 0, buf,
                           sizeof(buf)) != 401);
   /* wrong secret → 401 */
   assert(kb_http_route_ex("GET", "/v1/health", NULL, "Bearer nope", tok, NULL, 0, buf,
                           sizeof(buf)) == 401);
}

static void test_scope_token_resolves_current_code_project(void)
{
   const char *tok = "scope:project:alpha:s3cr3t";
   const char *auth = "Bearer scope:project:alpha:s3cr3t";
   char buf[1024];

   g_code_find_project[0] = '\0';
   int s = kb_http_route_ex("GET", "/v1/code/find", "identifier=foo", auth, tok, NULL, 0, buf,
                            sizeof(buf));
   assert(s == 200);
   assert(strcmp(g_code_find_project, "alpha") == 0);

   s = kb_http_route_ex("GET", "/v1/code/find", "identifier=foo&scope=all", auth, tok, NULL, 0, buf,
                        sizeof(buf));
   assert(s == 403);
   assert(strstr(buf, "scoped credential cannot request all projects") != NULL);
}

static void test_scope_admin_token_full_access(void)
{
   /* An unscoped (admin) token reaches any scope. */
   const char *tok = "plain-admin-secret";
   const char *auth = "Bearer plain-admin-secret";
   char buf[512];
   int s = kb_http_route_ex("GET", "/v1/search", "query=x&workspace=beta", auth, tok, NULL, 0, buf,
                            sizeof(buf));
   assert(s != 403 && s != 401);
}
/* test_kb_http_routes_endpoints.inc: OpenAPI / reflection / feedback / facet
 * route tests split out of test_kb_http_routes.c to keep that .c under the
 * 2000-line hard limit. Included mid-file (same translation unit) so the
 * shared mock harness and request helpers above remain in scope. */

static void test_openapi_json_ok(void)
{
   char buf[2048];
   int s = kb_http_route_ex("GET", "/v1/openapi.json", NULL, NULL, NULL, NULL, 0, buf, sizeof(buf));
   assert(s == 200);
   /* The response is YAML (openapi-v1.yaml content); check a known header field. */
   assert(strstr(buf, "openapi:") != NULL || strstr(buf, "aimee-kb") != NULL);
}

static void test_openapi_yaml_ok(void)
{
   char buf[2048];
   int s = kb_http_route_ex("GET", "/v1/openapi.yaml", NULL, NULL, NULL, NULL, 0, buf, sizeof(buf));
   assert(s == 200);
   assert(strstr(buf, "openapi:") != NULL || strstr(buf, "aimee-kb") != NULL);
}

static void test_openapi_method_not_allowed(void)
{
   char buf[64];
   int s =
       kb_http_route_ex("POST", "/v1/openapi.json", NULL, NULL, NULL, "{}", 2, buf, sizeof(buf));
   assert(s == 405);
}

/* ── Scope isolation: /v1/feedback/in-session cross-user check ──────────── */

static void test_feedback_scope_isolation(void)
{
   /* Scope isolation contract: /v1/feedback/in-session accepts a scope_user
    * field; the handler stores artifacts at scope_kind="user" with that
    * scope_id. Here we verify the routing and response contract — each
    * submission must succeed (201) and produce an artifact id in the response.
    * Full DB-level isolation is exercised by tests/test_kb_http_routes_db.c
    * which links the real kb_http_reflections.c against a DB2 shim. */
   char buf_a[256], buf_b[256];
   const char *body_a = "{\"kind\":\"feedback_positive\",\"session_id\":\"s-user-a\","
                        "\"scope_user\":\"user-a\",\"content\":\"good\"}";
   const char *body_b = "{\"kind\":\"feedback_negative\",\"session_id\":\"s-user-b\","
                        "\"scope_user\":\"user-b\",\"content\":\"bad\"}";

   int sa = kb_http_route_ex("POST", "/v1/feedback/in-session", NULL, NULL, NULL, body_a,
                             (int)strlen(body_a), buf_a, sizeof(buf_a));
   int sb = kb_http_route_ex("POST", "/v1/feedback/in-session", NULL, NULL, NULL, body_b,
                             (int)strlen(body_b), buf_b, sizeof(buf_b));

   assert(sa == 201);
   assert(sb == 201);
   assert(strstr(buf_a, "\"id\"") != NULL);
   assert(strstr(buf_b, "\"id\"") != NULL);
   /* Wrong method must be rejected. */
   int sd = kb_http_route_ex("DELETE", "/v1/feedback/in-session", NULL, NULL, NULL, NULL, 0, buf_a,
                             sizeof(buf_a));
   assert(sd == 404 || sd == 405);
}

/* ── Phase 6 route tests ─────────────────────────────────────────────────── */

static void test_post_reflections_ok(void)
{
   char buf[256];
   const char *body =
       "{\"entries\":[{\"kind\":\"session_summary\",\"confidence\":0.8,\"payload\":{}}]}";
   int s = kb_http_route_ex("POST", "/v1/reflections", NULL, NULL, NULL, body, (int)strlen(body),
                            buf, sizeof(buf));
   assert(s == 201);
   assert(strstr(buf, "\"created\"") != NULL);
}

static void test_get_reflections_ok(void)
{
   char buf[256];
   int s = kb_http_route_ex("GET", "/v1/reflections", NULL, NULL, NULL, NULL, 0, buf, sizeof(buf));
   assert(s == 200);
   assert(strstr(buf, "\"items\"") != NULL);
}

static void test_reflection_accept_ok(void)
{
   char buf[256];
   int s = kb_http_route_ex("POST", "/v1/reflections/some-uuid/accept", NULL, NULL, NULL, "{}", 2,
                            buf, sizeof(buf));
   assert(s == 200);
   assert(strstr(buf, "committed") != NULL);
}

static void test_reflection_reject_ok(void)
{
   char buf[256];
   int s = kb_http_route_ex("POST", "/v1/reflections/some-uuid/reject", NULL, NULL, NULL,
                            "{\"verdict_tag\":\"wrong\"}", 23, buf, sizeof(buf));
   assert(s == 200);
   assert(strstr(buf, "rejected") != NULL);
}

static void test_reflection_wrong_method(void)
{
   char buf[256];
   int s =
       kb_http_route_ex("DELETE", "/v1/reflections", NULL, NULL, NULL, NULL, 0, buf, sizeof(buf));
   assert(s == 404 || s == 405);
}

static void test_feedback_in_session_ok(void)
{
   char buf[256];
   const char *body =
       "{\"kind\":\"feedback_negative\",\"session_id\":\"s1\",\"content\":\"wrong\"}";
   int s = kb_http_route_ex("POST", "/v1/feedback/in-session", NULL, NULL, NULL, body,
                            (int)strlen(body), buf, sizeof(buf));
   assert(s == 201);
   assert(strstr(buf, "\"id\"") != NULL);
}

/* POST /v1/search with typed facets routes through the artifact facet filter
 * and returns facet-shaped hits (deep-curator). */
static void test_search_facet_filter(void)
{
   char buf[2048];
   const char *body =
       "{\"query\":\"three db\",\"project\":\"proj-alpha\",\"filters\":{\"status\":\"done\","
       "\"component\":\"pgvector\",\"kind\":\"doc_summary\"}}";
   int s = kb_http_route_ex("POST", "/v1/search", NULL, NULL, NULL, body, (int)strlen(body), buf,
                            sizeof(buf));
   assert(s == 200);
   assert(strstr(buf, "\"fusion_mode_used\":\"facet\"") != NULL);
   assert(strstr(buf, "\"artifact_id\":\"art-facet-1\"") != NULL);
   assert(strstr(buf, "\"project\":\"proj-alpha\"") != NULL);
   assert(strstr(buf, "\"kind\":\"doc_summary\"") != NULL);
   assert(strstr(buf, "\"total_hits\":1") != NULL);
   /* Bound to the active release by default (stub returns 7) and cites it. */
   assert(strstr(buf, "\"release_id\":7") != NULL);
}

/* POST /v1/search with explicit release_id:0 searches across all releases. */
static void test_search_facet_all_releases(void)
{
   char buf[2048];
   const char *body = "{\"query\":\"q\",\"scope\":\"all\",\"release_id\":0,\"filters\":{\"kind\":"
                      "\"doc_summary\"}}";
   int s = kb_http_route_ex("POST", "/v1/search", NULL, NULL, NULL, body, (int)strlen(body), buf,
                            sizeof(buf));
   assert(s == 200);
   assert(strstr(buf, "\"fusion_mode_used\":\"facet\"") != NULL);
   assert(strstr(buf, "\"release_id\":null") != NULL);
}

static void test_search_facet_scope_all_keeps_active_project_first(void)
{
   char buf[2048];
   const char *body =
       "{\"query\":\"q\",\"project\":\"proj-alpha\",\"scope\":\"all\",\"max_results\":2,"
       "\"filters\":{\"kind\":\"doc_summary\"}}";
   int s = kb_http_route_ex("POST", "/v1/search", NULL, NULL, NULL, body, (int)strlen(body), buf,
                            sizeof(buf));
   assert(s == 200);
   const char *local = strstr(buf, "\"scope_id\":\"proj-alpha\"");
   const char *other = strstr(buf, "\"scope_id\":\"proj-other\"");
   assert(local && other && local < other);
   assert(strstr(buf, "\"project\":\"proj-alpha\"") != NULL);
   assert(strstr(buf, "\"project\":\"proj-other\"") != NULL);
}

/* POST /v1/search without filters keeps the existing non-facet search path. */
static void test_search_no_filters_not_facet(void)
{
   char buf[2048];
   const char *body = "{\"query\":\"three db\",\"project\":\"proj-alpha\"}";
   int s = kb_http_route_ex("POST", "/v1/search", NULL, NULL, NULL, body, (int)strlen(body), buf,
                            sizeof(buf));
   assert(s == 200);
   assert(strstr(buf, "\"fusion_mode_used\":\"rrf\"") != NULL);
   assert(strstr(buf, "\"fusion_mode_used\":\"facet\"") == NULL);
}

/* test_kb_http_routes_search.inc: search, §2c re-embed/maintenance, artifact,
 * and code-find route tests, split out of test_kb_http_routes.c to keep that
 * translation unit under the 2000-line hard limit. Included (with the code /
 * endpoints .inc files) just before main(); all helpers and fixtures live in
 * test_kb_http_routes.c above the include point. */
static void test_search_ok(void)
{
   char buf[1024];
   const char *unscoped = "{\"query\":\"foo\"}";
   int s = kb_http_route_ex("POST", "/v1/search", NULL, NULL, NULL, unscoped, (int)strlen(unscoped),
                            buf, sizeof(buf));
   assert(s == 409);
   assert(strstr(buf, "scope_required") != NULL);

   const char *body = "{\"query\":\"foo\",\"project\":\"proj-alpha\"}";
   s = kb_http_route_ex("POST", "/v1/search", NULL, NULL, NULL, body, (int)strlen(body), buf,
                        sizeof(buf));
   assert(s == 200);
   assert(strstr(buf, "\"hits\"") != NULL);
   assert(strstr(buf, "\"fusion_mode_used\"") != NULL);
}

static void test_search_scope_all_keeps_active_project_first(void)
{
   char buf[2048];
   g_test_search_scoped_all = 1;
   const char *body =
       "{\"query\":\"foo\",\"scope\":\"all\",\"project\":\"proj-alpha\",\"max_results\":2}";
   int s = kb_http_route_ex("POST", "/v1/search", NULL, NULL, NULL, body, (int)strlen(body), buf,
                            sizeof(buf));
   g_test_search_scoped_all = 0;
   assert(s == 200);
   const char *local = strstr(buf, "local/first.md");
   const char *other = strstr(buf, "other/high.md");
   assert(local && other && local < other);
   assert(strstr(buf, "\"project\":\"proj-alpha\"") != NULL);
   assert(strstr(buf, "\"project\":\"proj-other\"") != NULL);
}

/* A managed KB normally has no raw embedding_command in aimee.yaml: the
 * wizard supplies SYNTHESIS_ENDPOINT. Search must resolve that deployment default
 * before entering the ranked backend, or it silently queries a 1024-dim corpus
 * with the 384-dim builtin vector. */
static void test_search_uses_managed_embedder(void)
{
   char buf[1024];
   unsetenv("EMBEDDER_URL");
   setenv("SYNTHESIS_ENDPOINT", "http://managed-llm:8742", 1);
   g_test_search_embedding[0] = '\0';
   const char *body = "{\"query\":\"foo\",\"project\":\"proj-alpha\"}";
   int s = kb_http_route_ex("POST", "/v1/search", NULL, NULL, NULL, body, (int)strlen(body), buf,
                            sizeof(buf));
   assert(s == 200);
   assert(strcmp(g_test_search_embedding, "http://managed-llm:8742") == 0);
   unsetenv("SYNTHESIS_ENDPOINT");
}

/* Producer->consumer contract: the /v1/search ranked handler and the kb_search
 * agent tool must agree on the response shape. A refactor once left the tool
 * unwrapping a top-level {"result"} field the endpoint never emits, so every
 * kb_search call errored and the learning-to-rank capture (which reads doc_id
 * off each hit) silently went dead. This test drives the REAL handler to emit a
 * populated hit, then feeds that exact JSON through the REAL tool-side parse
 * helpers (td_render_search_hits / td_extract_hit_docs). If either side renames
 * a field or drops doc_id, or the tool reverts to reading {"result"}, it fails.
 * That is the seam neither the handler test nor the render unit test covered. */
static void test_search_hits_tool_contract(void)
{
   char buf[2048];
   g_test_search_populated = 1;
   const char *body = "{\"query\":\"foo\",\"project\":\"proj-alpha\"}";
   int s = kb_http_route_ex("POST", "/v1/search", NULL, NULL, NULL, body, (int)strlen(body), buf,
                            sizeof(buf));
   g_test_search_populated = 0;
   assert(s == 200);

   /* Producer side: the emitted hit carries exactly the fields the tool reads. */
   cJSON *resp = cJSON_Parse(buf);
   assert(resp);
   cJSON *hits = cJSON_GetObjectItemCaseSensitive(resp, "hits");
   assert(cJSON_IsArray(hits) && cJSON_GetArraySize(hits) == 1);
   cJSON *h0 = cJSON_GetArrayItem(hits, 0);
   assert(cJSON_IsString(cJSON_GetObjectItemCaseSensitive(h0, "artifact_id")));
   assert(cJSON_IsNumber(cJSON_GetObjectItemCaseSensitive(h0, "score")));
   assert(cJSON_IsNumber(cJSON_GetObjectItemCaseSensitive(h0, "doc_id")));
   assert(cJSON_IsString(cJSON_GetObjectItemCaseSensitive(h0, "excerpt")));

   /* Consumer side (selection + render): route the WHOLE response through the
    * tool's real result-selection (td_search_result_from_response) — the exact
    * function that once read the wrong field and errored. It must return
    * non-error text naming the artifact and its excerpt. */
   char *rendered = td_search_result_from_response(resp, "foo");
   assert(rendered);
   assert(strncmp(rendered, "error:", 6) != 0);
   assert(!strstr(rendered, "No knowledge-base results"));
   assert(strstr(rendered, "docs/alpha.md"));
   assert(strstr(rendered, "alpha excerpt body"));
   free(rendered);

   /* Consumer side (capture): the doc_id the handler emitted is the one the LTR
    * capture attributes, with the matching excerpt as its overlap snippet. */
   int64_t ids[4];
   const char *snips[4];
   int cn = td_extract_hit_docs(hits, ids, snips, 4);
   assert(cn == 1);
   assert(ids[0] == 4242);
   assert(strcmp(snips[0], "alpha excerpt body") == 0);

   cJSON_Delete(resp);
}

/* §2c: while a dim-change re-embed is in flight the vector store is being
 * rebuilt at the new dim; /v1/search must refuse with 503 (maintenance) rather
 * than serve partial/empty results — and the guard runs before query parsing,
 * so even a well-formed query is refused. */
static void test_search_503_while_reembed_in_progress(void)
{
   char buf[1024];
   g_test_reembed_in_progress = 1;
   int s = kb_http_route_ex("POST", "/v1/search", NULL, NULL, NULL, "{\"query\":\"foo\"}", 15, buf,
                            sizeof(buf));
   g_test_reembed_in_progress = 0;
   assert(s == 503);
   assert(strstr(buf, "\"maintenance\"") != NULL);
   /* guard precedes parsing: no hits payload is produced */
   assert(strstr(buf, "\"hits\"") == NULL);
}

/* §2c: /v1/reembed is server-gated by kb.reembed_on_dim_change (default off) and
 * defaults to a dry-run when confirm is absent. */
static void test_reembed_wrong_method(void)
{
   char buf[256];
   int s = kb_http_route_ex("GET", "/v1/reembed", NULL, NULL, NULL, NULL, 0, buf, sizeof(buf));
   assert(s == 405);
}

static void test_reembed_disabled_by_default(void)
{
   char buf[512];
   g_test_reembed_enabled = 0;
   int s = kb_http_route_ex("POST", "/v1/reembed", NULL, NULL, NULL, "{\"confirm\":true}", 16, buf,
                            sizeof(buf));
   assert(s == 403);
   assert(strstr(buf, "reembed_on_dim_change") != NULL);
}

static void test_reembed_enabled_no_confirm_is_dry_run(void)
{
   char buf[1024];
   g_test_reembed_enabled = 1;
   int s = kb_http_route_ex("POST", "/v1/reembed", NULL, NULL, NULL, "{}", 2, buf, sizeof(buf));
   g_test_reembed_enabled = 0;
   assert(s == 200);
   assert(strstr(buf, "\"dry_run\":true") != NULL);
}

/* §2c: an explicit target_dim is authoritative — it bypasses the embedder probe,
 * so the resolved/echoed target reflects the operator's value, not the stub's 1024. */
static void test_reembed_target_dim_override(void)
{
   char buf[1024];
   g_test_reembed_enabled = 1;
   int s = kb_http_route_ex("POST", "/v1/reembed", NULL, NULL, NULL, "{\"target_dim\":2560}", 19,
                            buf, sizeof(buf));
   g_test_reembed_enabled = 0;
   assert(s == 200);
   assert(strstr(buf, "\"target_dim\":2560") != NULL);
}

/* §2c: --clear-maintenance is a standalone escape hatch — it works even with the
 * reset toggle off, reports whether a marker was present, and clears it. */
static void test_reembed_clear_maintenance(void)
{
   char buf[512];
   g_test_reembed_enabled = 0; /* toggle off: escape hatch must still work */
   g_test_reembed_in_progress = 1;
   g_test_recorded_dim = 1024;
   g_test_running_dim = 1024; /* consistent dims: clears without force */
   int s = kb_http_route_ex("POST", "/v1/reembed", NULL, NULL, NULL, "{\"clear_maintenance\":true}",
                            26, buf, sizeof(buf));
   assert(s == 200);
   assert(strstr(buf, "\"cleared\":true") != NULL);
   assert(strstr(buf, "\"was_in_progress\":true") != NULL);
   assert(strstr(buf, "\"dim_consistent\":true") != NULL);
   assert(g_test_reembed_in_progress == 0); /* stub clear ran */
   /* idempotent: clearing again reports nothing was in progress */
   s = kb_http_route_ex("POST", "/v1/reembed", NULL, NULL, NULL, "{\"clear_maintenance\":true}", 26,
                        buf, sizeof(buf));
   assert(s == 200);
   assert(strstr(buf, "\"was_in_progress\":false") != NULL);
}

/* §2c: clearing into an inconsistent store (recorded dim != running dim) is refused
 * with 409 unless force makes the dangerous mid-transition clear explicit. */
static void test_reembed_clear_maintenance_dim_mismatch_needs_force(void)
{
   char buf[512];
   g_test_reembed_in_progress = 1;
   g_test_recorded_dim = 768;
   g_test_running_dim = 1024; /* mismatch */
   int s = kb_http_route_ex("POST", "/v1/reembed", NULL, NULL, NULL, "{\"clear_maintenance\":true}",
                            26, buf, sizeof(buf));
   assert(s == 409);
   assert(strstr(buf, "\"cleared\":false") != NULL);
   assert(strstr(buf, "\"dim_consistent\":false") != NULL);
   assert(g_test_reembed_in_progress == 1); /* not cleared */
   /* force overrides the mismatch gate */
   s = kb_http_route_ex("POST", "/v1/reembed", NULL, NULL, NULL,
                        "{\"clear_maintenance\":true,\"force\":true}", 39, buf, sizeof(buf));
   assert(s == 200);
   assert(strstr(buf, "\"cleared\":true") != NULL);
   assert(g_test_reembed_in_progress == 0);
   g_test_recorded_dim = 1024;
   g_test_running_dim = 1024; /* restore default for other tests */
}

static void test_search_missing_query(void)
{
   char buf[256];
   int s = kb_http_route_ex("POST", "/v1/search", NULL, NULL, NULL, "{}", 2, buf, sizeof(buf));
   assert(s == 400);
}

static void test_search_wrong_method(void)
{
   char buf[256];
   int s = kb_http_route_ex("GET", "/v1/search", NULL, NULL, NULL, NULL, 0, buf, sizeof(buf));
   assert(s == 405);
}

static void test_artifact_not_found(void)
{
   char buf[256];
   int s = kb_http_route_ex("GET", "/v1/artifacts/no-such-uuid", NULL, NULL, NULL, NULL, 0, buf,
                            sizeof(buf));
   assert(s == 404);
}

static void test_artifact_links_ok(void)
{
   char buf[512];
   int s = kb_http_route_ex("GET", "/v1/artifacts/some-uuid/links", NULL, NULL, NULL, NULL, 0, buf,
                            sizeof(buf));
   assert(s == 200);
   assert(strstr(buf, "\"links\"") != NULL);
}

static void test_code_find_missing_identifier(void)
{
   char buf[256];
   int s = kb_http_route_ex("GET", "/v1/code/find", "", NULL, NULL, NULL, 0, buf, sizeof(buf));
   assert(s == 400);
}

static void test_code_find_ok(void)
{
   char buf[512];
   int s = kb_http_route_ex("GET", "/v1/code/find", "identifier=foo", NULL, NULL, NULL, 0, buf,
                            sizeof(buf));
   assert(s == 409);
   assert(strstr(buf, "scope_required") != NULL);

   s = kb_http_route_ex("GET", "/v1/code/find", "identifier=foo&scope=all", NULL, NULL, NULL, 0,
                        buf, sizeof(buf));
   assert(s == 200);
   assert(strstr(buf, "\"hits\"") != NULL);
   assert(strstr(buf, "\"project\":\"proj-alpha\"") != NULL);
   assert(strstr(buf, "\"file_path\":\"src/main.c\"") != NULL);
   assert(strstr(buf, "\"line\":12") != NULL);
   assert(strstr(buf, "\"kind\":\"function\"") != NULL);

   g_code_find_project[0] = '\0';
   s = kb_http_route_ex("GET", "/v1/code/find", "identifier=foo&project=proj-alpha", NULL, NULL,
                        NULL, 0, buf, sizeof(buf));
   assert(s == 200);
   assert(strcmp(g_code_find_project, "proj-alpha") == 0);
   assert(strstr(buf, "\"project\":\"proj-alpha\"") != NULL);

   s = kb_http_route_ex("GET", "/v1/code/find", "identifier=foo&project=proj-alpha&generation=1",
                        NULL, NULL, NULL, 0, buf, sizeof(buf));
   assert(s == 409);
   assert(strstr(buf, "stale_generation") != NULL);
   assert(strstr(buf, "\"current_generation\":2") != NULL);

   s = kb_http_route_ex("GET", "/v1/code/find", "identifier=foo&project=proj-alpha&generation=2",
                        NULL, NULL, NULL, 0, buf, sizeof(buf));
   assert(s == 200);

   s = kb_http_route_ex("GET", "/v1/code/find",
                        "identifier=foo&project=proj-alpha&scope=all&generation=1", NULL, NULL,
                        NULL, 0, buf, sizeof(buf));
   assert(s == 409);
   assert(strstr(buf, "stale_generation") != NULL);
   assert(strstr(buf, "\"current_generation\":2") != NULL);

   s = kb_http_route_ex("GET", "/v1/code/find",
                        "identifier=foo&project=proj-alpha&scope=all&generation=2", NULL, NULL,
                        NULL, 0, buf, sizeof(buf));
   assert(s == 200);
}

static void test_code_projects_wrong_method(void)
{
   char buf[256];
   int s = kb_http_route_ex("POST", "/v1/code/projects", "", NULL, NULL, NULL, 0, buf, sizeof(buf));
   assert(s == 405);
}

static void test_code_projects_ok(void)
{
   char buf[512];
   int s = kb_http_route_ex("GET", "/v1/code/projects", "max_results=4", NULL, NULL, NULL, 0, buf,
                            sizeof(buf));
   assert(s == 200);
   assert(strstr(buf, "\"status\":\"ok\"") != NULL);
   assert(strstr(buf, "\"projects\"") != NULL);
   assert(strstr(buf, "\"name\":\"proj-alpha\"") != NULL);
   assert(strstr(buf, "\"root\":\"/repo/proj-alpha\"") != NULL);
   assert(strstr(buf, "\"scanned_at\":\"2026-05-26 00:00:00\"") != NULL);
   assert(strstr(buf, "\"next_cursor\":null") != NULL);
}

static void test_code_structure_missing_params(void)
{
   char buf[256];
   int s = kb_http_route_ex("GET", "/v1/code/structure", "", NULL, NULL, NULL, 0, buf, sizeof(buf));
   assert(s == 409);
   assert(strstr(buf, "scope_required") != NULL);
}
/* handle_connection (plain-HTTP listener): a Content-Length over
 * KB_HTTP_BODY_MAX must be rejected up front with 413 — never silently
 * truncated (truncation turned every large /v1/code/scan push into an opaque
 * 400 "invalid json"). */
extern void handle_connection(int fd);

static void *conn_serve_thread(void *a)
{
   handle_connection(*(int *)a);
   return NULL;
}

static void test_http_body_too_large_413(void)
{
   int sv[2];
   assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
   pthread_t th;
   assert(pthread_create(&th, NULL, conn_serve_thread, &sv[0]) == 0);

   const char *req = "POST /v1/code/scan HTTP/1.1\r\n"
                     "Content-Length: 999999999999\r\n"
                     "\r\n";
   assert(write(sv[1], req, strlen(req)) == (ssize_t)strlen(req));
   pthread_join(th, NULL); /* handler replies before reading any body */
   close(sv[0]);           /* EOF for the reader below */

   char resp[2048];
   int total = 0, n;
   while ((n = (int)read(sv[1], resp + total, sizeof(resp) - 1 - (size_t)total)) > 0)
      total += n;
   resp[total] = '\0';
   close(sv[1]);

   assert(strstr(resp, "413"));
   assert(strstr(resp, "request body too large"));
}

/* A slow upload must not monopolize the listener and prevent an independent
 * health request from being accepted. This regresses the serial-listener bug
 * where bursts beyond the listen backlog were dropped while one DB-backed
 * request ran. */
static int reserve_tcp_port(void)
{
   int fd = socket(AF_INET, SOCK_STREAM, 0);
   assert(fd >= 0);
   struct sockaddr_in sa;
   memset(&sa, 0, sizeof(sa));
   sa.sin_family = AF_INET;
   sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
   sa.sin_port = 0;
   assert(bind(fd, (struct sockaddr *)&sa, sizeof(sa)) == 0);
   socklen_t n = sizeof(sa);
   assert(getsockname(fd, (struct sockaddr *)&sa, &n) == 0);
   int port = ntohs(sa.sin_port);
   close(fd);
   return port;
}

static int connect_local_port(int port)
{
   int fd = socket(AF_INET, SOCK_STREAM, 0);
   assert(fd >= 0);
   struct sockaddr_in sa;
   memset(&sa, 0, sizeof(sa));
   sa.sin_family = AF_INET;
   sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
   sa.sin_port = htons((uint16_t)port);
   assert(connect(fd, (struct sockaddr *)&sa, sizeof(sa)) == 0);
   return fd;
}

static void test_http_listener_concurrent_requests(void)
{
   int port = reserve_tcp_port();
   assert(kb_http_start(port, NULL) == 0);

   int slow = connect_local_port(port);
   const char *partial = "POST /v1/health HTTP/1.1\r\nContent-Length: 1\r\n\r\n";
   assert(write(slow, partial, strlen(partial)) == (ssize_t)strlen(partial));

   int fast = connect_local_port(port);
   struct timeval tv = {.tv_sec = 2, .tv_usec = 0};
   assert(setsockopt(fast, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) == 0);
   const char *health = "GET /v1/health HTTP/1.1\r\nContent-Length: 0\r\n\r\n";
   assert(write(fast, health, strlen(health)) == (ssize_t)strlen(health));
   char resp[2048];
   int nr = (int)read(fast, resp, sizeof(resp) - 1);
   assert(nr > 0);
   resp[nr] = '\0';
   assert(strstr(resp, "200 OK") != NULL);
   close(fast);

   assert(write(slow, "x", 1) == 1);
   assert(setsockopt(slow, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) == 0);
   nr = (int)read(slow, resp, sizeof(resp) - 1);
   assert(nr > 0);
   close(slow);
   kb_http_stop();
}

/* ---- embedding is asynchronous, full stop ------------------------------------
 *
 * RED-GREEN. /v1/code/build used to do the whole build inline: doc embedding,
 * canonical scan, then code embedding. On a 3825-file checkout that is minutes
 * of embedder time held open on one HTTP request, and the first bound anywhere
 * in that chain to expire turned a build that was progressing normally into a
 * hard failure -- observed as "knowledge service /v1/code/build did not respond"
 * with the embedder logging BrokenPipeError after the kb dropped a connection
 * whose embed batch had merely taken longer than the client's patience.
 *
 * A build's cost is a property of the CORPUS, not of the service being healthy.
 * So the route commits the work and answers immediately; the queue worker does
 * the same build -- doc vectors, canonical index, AND code vectors -- and it
 * completes when it completes.
 *
 * INTERACTIVE priority is load-bearing: an explicit build must jump the periodic
 * sweep. Starvation behind the global backlog is what made someone inline this
 * work in the first place, and priority is the fix for that, not blocking. */
static void test_code_build_queues_instead_of_embedding_inline(void)
{
   char buf[1024];
   g_ingest_root[0] = 0;
   g_db_initialized = 1;
   g_pgvec_ensure_rc = 0;
   g_ingest_force = 0;
   g_ingest_priority = -1;
   g_ingest_project[0] = '\0';
   const char *body = "{\"path\":\"/tmp/repo\",\"project\":\"proj-build\",\"force\":true}";
   int s = kb_http_route_ex("POST", "/v1/code/build", NULL, NULL, NULL, body, (int)strlen(body),
                            buf, sizeof(buf));
   assert(s == 200);
   assert(strstr(buf, "\"queued\":true") != NULL);
   /* The work was handed to the queue, not done here. */
   assert(strcmp(g_ingest_project, "proj-build") == 0);
   assert(strcmp(g_ingest_root, "/tmp/repo") == 0);
   assert(g_ingest_force == 1);
   /* An explicit request must not sit behind the periodic sweep. */
   assert(g_ingest_priority == DB2_KB_INGEST_PRIO_INTERACTIVE);
}

/* Repair embeds too, so it queues on the same grounds: an operator gets a
 * durable commitment, not a request held open across minutes of embedder time
 * that reports failure the moment a bound expires. */
static void test_maintenance_repair_queues_too(void)
{
   char buf[1024];
   g_ingest_root[0] = 0;
   g_db_initialized = 1;
   g_pgvec_ensure_rc = 0;
   g_ingest_force = 0;
   g_ingest_priority = -1;
   g_ingest_project[0] = '\0';
   const char *body = "{\"path\":\"/tmp/repo\",\"project\":\"proj-repair\"}";
   /* Maintenance is owner-only; the credential is orthogonal to the async
    * change but the route rightly refuses without it. */
   int s = kb_http_route_ex("POST", "/v1/maintenance/repair", NULL, OWNER_AUTH, OWNER_TOK, body,
                            (int)strlen(body), buf, sizeof(buf));
   assert(s == 200);
   assert(strcmp(g_ingest_project, "proj-repair") == 0);
   /* Repair is always a forced rebuild; that must survive the hand-off. */
   assert(g_ingest_force == 1);
   assert(g_ingest_priority == DB2_KB_INGEST_PRIO_INTERACTIVE);
}

int main(void)
{
   /* Match kb_main's process contract. TLS readiness probes deliberately open
    * and close sockets; under parallel suite load a server write can race that
    * close, and the default SIGPIPE disposition would kill the fixture instead
    * of letting OpenSSL report the failed connection. */
   signal(SIGPIPE, SIG_IGN);
   kb_route_acl_register_authorization_provider(control_web_module_provider);
   printf("kb_http_routes: ");

   test_health();
   test_health_ex_rich();
   test_health_status_mode();
   test_version();
   test_capabilities();
   test_console_overview();
   test_console_admin_requires_authorization_module();
   test_console_pipeline();
   test_console_settings();
   test_accounts_routes();
   test_mint_scope_restriction();
   test_team_routes();
   test_governance_routes();
   test_intelligence_calibration_readiness();
   test_intelligence_demotion_check();
   test_intelligence_bandit_export();
   test_not_found();
   test_enroll_route();
   test_enroll_redeem_route();
   test_mtls_serve();
   test_mtls_listener();
   test_http_body_too_large_413();
   test_http_listener_concurrent_requests();
   test_bearer_auth_ok();
   test_bearer_auth_missing();
   test_bearer_auth_wrong();
   test_head_method();
   test_method_not_allowed();

   test_curator_routes();
   test_invalidations_route();
   test_search_ok();
   test_search_scope_all_keeps_active_project_first();
   test_search_uses_managed_embedder();
   test_search_hits_tool_contract();
   test_search_503_while_reembed_in_progress();
   test_reembed_wrong_method();
   test_reembed_disabled_by_default();
   test_reembed_enabled_no_confirm_is_dry_run();
   test_reembed_target_dim_override();
   test_reembed_clear_maintenance();
   test_reembed_clear_maintenance_dim_mismatch_needs_force();
   test_search_missing_query();
   test_search_wrong_method();
   test_artifact_not_found();
   test_artifact_links_ok();
   test_code_find_missing_identifier();
   test_code_find_ok();
   test_code_projects_wrong_method();
   test_code_projects_ok();
   test_code_structure_missing_params();
   test_code_structure_ok();
   test_code_search_missing_query();
   test_code_search_ok();
   test_code_callers_missing_symbol();
   test_code_callers_ok();
   test_code_scope_all_keeps_active_project_first();
   test_code_hybrid_ok();
   test_code_hybrid_memory_leg();
   test_code_hybrid_keeps_same_path_projects_distinct();
   test_code_hybrid_missing_query();
   test_code_hybrid_no_symbol();
   test_code_hybrid_vector_ok();
   test_code_hybrid_vector_dim_mismatch_skips();
   test_code_context_vector_store_outage_is_not_empty();
   test_code_context_dimension_mismatch_is_stale();
   test_code_context_bounded_current_project();
   test_code_context_without_an_embedder_reports_the_dependency();
   test_code_context_embedder_outage_is_not_no_answer();
   test_code_context_embedder_auth_is_unauthorized();
   test_code_context_does_not_substitute_global_memory();
   test_code_context_requires_verified_memory_anchor();
   test_code_graph_hubs_ok();
   test_code_graph_hubs_missing_project();
   test_code_lessons_empty();
   test_code_lessons_missing_project();
   test_code_graph_surprising_ok();
   test_code_graph_surprising_hub_excluded();
   test_code_graph_surprising_missing_project();
   test_code_graph_surprising_vecstore_down();
   test_code_graph_surprising_judge();
   test_code_graph_surprising_self_suppress();
   test_code_graph_node_ok();
   test_code_graph_node_capped_truncates();
   test_code_graph_node_self_loop();
   test_code_graph_node_missing_params();
   test_code_project_stats_missing_project();
   test_code_project_stats_ok();
   test_code_project_stats_error_is_json();
   test_blast_radius_missing_params();
   test_blast_radius_not_found();
   test_blast_radius_ok();
   test_code_scan_ok();
   test_code_project_lifecycle_routes();
   test_code_scan_skips_unchanged_branch();
   test_code_scan_runs_on_branch_move();
   test_code_scan_worktree_ignores_sha();
   test_code_scan_installs_hook();
   test_code_scan_missing_root_path();
   test_code_scan_pushed_files_ok();
   test_code_scan_pushed_files_rejects_invalid_item();
   test_code_scan_db_unavailable();
   test_code_update_ok();
   test_ingest_enqueue_ok();
   test_ingest_status_ok();
   test_ingest_status_wrong_method();
   test_workers_ok();
   test_workers_wrong_method();
   test_pipeline_status_ok();
   test_pipeline_status_failed();
   test_drain_ok();
   test_drain_default_embedding_command();
   test_drain_error();
   test_scoped_token_cannot_cross_project_via_body();
   test_scoped_token_cannot_ingest_all_projects();
   test_service_scope_is_data_plane_not_admin();
   test_maintenance_routes_are_owner_gated();
   test_maintenance_repair_missing_project();
   test_maintenance_reconcile_ok();
   test_maintenance_clear_ok();
   test_maintenance_clear_error();
   test_purge_project_writes_fence();
   test_purge_project_live_fence_409();
   test_purge_project_takeover_displaces();
   test_purge_project_stale_fence_replaced();
   test_purge_project_store_error_continues();
   test_purge_project_bad_request();
   test_purge_heartbeat_match_and_mismatch();
   test_purge_finalize_mismatch_noop();
   test_purge_finalize_match_clears();
   test_purge_cancel_match_clears();
   test_job_not_found();
   test_job_status_ok();
   test_job_status_error();
   test_entity_profile_not_found();
   test_entity_search_ok();
   test_phase5_auth_rejected();
   test_phase1_fallthrough();

   test_post_reflections_ok();
   test_get_reflections_ok();
   test_reflection_accept_ok();
   test_reflection_reject_ok();
   test_reflection_wrong_method();
   test_feedback_in_session_ok();

   test_openapi_json_ok();
   test_openapi_yaml_ok();
   test_openapi_method_not_allowed();
   test_feedback_scope_isolation();

   test_scope_token_cross_scope_denied();
   test_scope_token_secret_auth();
   test_scope_token_resolves_current_code_project();
   test_scope_admin_token_full_access();

   test_search_facet_filter();
   test_search_facet_all_releases();
   test_search_facet_scope_all_keeps_active_project_first();
   test_search_no_filters_not_facet();

   test_code_build_queues_instead_of_embedding_inline();
   test_maintenance_repair_queues_too();
   printf("ok\n");
   return 0;
}
