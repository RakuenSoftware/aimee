/* test_kb_http_routes.c: unit tests for kb_http_route() (Phase 1+5). */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>

#include "config.h"
#include "cJSON.h"
#include "kb_http.h"
#include "td_search_render.h"       /* consumer side of the /v1/search contract test */
#include "kb/kb_surprising_judge.h" /* §4 judge stub seam (kb_surprising_verdict_t) */
#include "db2/lifecycle.h"          /* §2c: db2_reembed_* / db2_dim_change_reset stub types */
#include "rel_types.h"              /* REL_TYPE_NAME_MAX for the db2_ontology_* stubs below */
#include "kb_service.h"
#include "kb_bandit.h"
#include "kb_service_backend.h"
#include "kb_enroll.h"
#include "kb_paths.h"
#include "kb_pki.h"
#include "kb_tls.h"
#include "kb_client_mtls.h"

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
char *kb_search_json_ex(const char *p, const char *q, const char *e, int m, const char *f)
{
   (void)p;
   (void)q;
   (void)e;
   (void)m;
   (void)f;
   const char *src = g_test_search_populated
                         ? "{\"fusion_mode\":\"rrf\",\"results\":[{\"file_path\":\"docs/alpha.md\","
                           "\"content\":\"alpha excerpt body\",\"score\":0.875,\"doc_id\":4242}]}"
                         : "{\"fusion_mode\":\"rrf\",\"results\":[]}";
   char *r = malloc(strlen(src) + 1);
   if (r)
      strcpy(r, src);
   return r;
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
int config_resolve_embedding_dim(const config_t *cfg)
{
   (void)cfg;
   return 0;
}
int config_embedding_dim_is_pinned(const config_t *cfg)
{
   (void)cfg;
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

int canonical_index_find(const char *identifier, void *out, int max)
{
   assert(identifier);
   assert(out);
   if (strcmp(identifier, "foo") != 0 || max < 1)
      return 0;
   test_term_hit_t *hits = (test_term_hit_t *)out;
   snprintf(hits[0].project, sizeof(hits[0].project), "proj-alpha");
   snprintf(hits[0].file_path, sizeof(hits[0].file_path), "src/main.c");
   hits[0].line = 12;
   hits[0].line_end = 20;
   snprintf(hits[0].kind, sizeof(hits[0].kind), "function");
   return 1;
}

typedef struct
{
   char file[MAX_PATH_LEN];
   char dependents[64][MAX_PATH_LEN];
   int dependent_count;
   char dependencies[64][MAX_PATH_LEN];
   int dependency_count;
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
} test_code_search_hit_t;

int canonical_index_code_search(const char *query, const char *project, void *out, int max,
                                int enrich)
{
   (void)enrich;
   assert(query);
   assert(out);
   if (strcmp(query, "needle") != 0 || !project || strcmp(project, "proj-alpha") != 0)
      return 0;
   if (max < 1)
      return 0;
   test_code_search_hit_t *hits = (test_code_search_hit_t *)out;
   snprintf(hits[0].project, sizeof(hits[0].project), "proj-alpha");
   snprintf(hits[0].file_path, sizeof(hits[0].file_path), "src/search.c");
   snprintf(hits[0].snippet, sizeof(hits[0].snippet), "int needle(void) { return 1; }");
   hits[0].rank = 0.75;
   snprintf(hits[0].content_hash, sizeof(hits[0].content_hash), "deadbeefcafe");
   return 1;
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

const char *config_embedding_command(const config_t *cfg, const char *requested)
{
   if (requested && requested[0])
      return requested;
   if (cfg && cfg->embedding_command[0])
      return cfg->embedding_command;
   return "builtin";
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

int kb_ranker_fit_run(const config_t *cfg, char *id_out, int id_out_len, char **report_out)
{
   (void)cfg;
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
int kb_bandit_sample(const config_t *cfg, const char *decision_point, const char *context_json,
                     const char (*arm_ids)[KB_BANDIT_MAX_ARM_ID], int n_arms, char *decision_id_out)
{
   (void)cfg;
   (void)decision_point;
   (void)context_json;
   (void)arm_ids;
   (void)n_arms;
   if (decision_id_out)
      decision_id_out[0] = '\0';
   return -1;
}
int kb_bandit_reward(const config_t *cfg, const char *decision_point, const char *decision_id,
                     const char *arm_id, double reward)
{
   (void)cfg;
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

int db2_kb_ingest_queue_enqueue(const char *project, const char *root_path, const char *workspace,
                                int force)
{
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

int db2_kb_file_index_delete_project(const char *project)
{
   (void)project;
   return 0;
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
static int g_stub_revoked_calls = 0;
int db2_enrollment_insert(const char *scope, const char *fingerprint, const char *serial,
                          const char *expires_at, int legacy, int64_t *out_id)
{
   (void)scope;
   (void)fingerprint;
   (void)serial;
   (void)expires_at;
   (void)legacy;
   if (out_id)
      *out_id = 1;
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
   char tmp[] = "/tmp/aimee_enroll_route_XXXXXX";
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
   char tmp[] = "/tmp/aimee_redeem_route_XXXXXX";
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
   s = kb_http_route_ex("POST", "/v1/enroll/redeem", NULL, NULL, NULL, rb, (int)strlen(rb), buf,
                        sizeof(buf));
   assert(s == 200);
   assert(strstr(buf, "\"client_cert\"") && strstr(buf, "BEGIN CERTIFICATE"));
   assert(strstr(buf, "project:redeem"));

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

static void test_mtls_serve(void)
{
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

   /* Rotation: the authenticated client renews its cert for its CURRENT scope
    * (project:alpha) by signing a fresh CSR — no token, no operator action. The
    * renew handler signs with the persisted CA, so save this test's CA there. */
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
      assert(strstr(resp, "200 OK"));
      assert(strstr(resp, "client_cert"));
      assert(strstr(resp, "project:alpha")); /* renewed for the same scope */
      assert(strstr(resp, "BEGIN CERTIFICATE"));
      free(rb);
      cJSON_Delete(rj);
      free(rcsr);

      char p[360];
      snprintf(p, sizeof(p), "%s/ca.pem", cadir);
      remove(p);
      snprintf(p, sizeof(p), "%s/ca-key.pem", cadir);
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
   /* Production passes kb_default_config_dir() as the listener data_dir, and the
    * bootstrap endpoints (/v1/enroll/ca, /renew) read the CA from there — so use
    * the same dir here. Clean any leftover CA so the listener makes a fresh one. */
   const char *cfg = kb_default_config_dir();
   char cp[360];
   snprintf(cp, sizeof(cp), "%s/kb-ca/ca.pem", cfg);
   remove(cp);
   snprintf(cp, sizeof(cp), "%s/kb-ca/ca-key.pem", cfg);
   remove(cp);
   snprintf(cp, sizeof(cp), "%s/kb-ca", cfg);
   rmdir(cp);

   assert(kb_mtls_start(0, cfg, "localhost") == 0);
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
   assert(st == 200);
   assert(strstr(rbody, "\"status\":\"ok\""));
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

      setenv("AIMEE_KB_CONN", conn2, 1);
      assert(kb_client_mtls_configured() == 1);
      int st2 = -1;
      char *r = kb_client_mtls_request("GET", "/v1/health", NULL, &st2);
      assert(st2 == 200);
      assert(r && strstr(r, "\"status\":\"ok\""));
      free(r);
      unsetenv("AIMEE_KB_CONN");
      assert(kb_client_mtls_configured() == 0);
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

      /* rotate the project:beta identity -> a genuinely new cert, same scope,
       * chaining to the CA, and usable to dial. */
      char nc[KB_PKI_CERT_PEM_MAX], nk[KB_PKI_KEY_PEM_MAX];
      assert(kb_tls_renew("localhost", port, ca.cert_pem, ccert, ckey, nc, sizeof(nc), nk,
                          sizeof(nk)) == 0);
      assert(kb_pki_verify_client_cert(ca.cert_pem, nc) == 1);
      assert(strcmp(nc, ccert) != 0);
      assert(kb_tls_client_request("localhost", port, ca.cert_pem, nc, nk, "GET", "/v1/health",
                                   NULL, rbody, sizeof(rbody), &st) == 0);
      assert(st == 200);
   }

   SSL_CTX_free(cctx);
   kb_mtls_stop();
   assert(kb_mtls_bound_port() == 0);

   snprintf(cp, sizeof(cp), "%s/kb-ca/ca.pem", cfg);
   remove(cp);
   snprintf(cp, sizeof(cp), "%s/kb-ca/ca-key.pem", cfg);
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
   snprintf(m[0].content, sizeof(m[0].content), "chose needle over haystack for O(1) lookup");
   return 1;
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
   if (strcmp(symbol, "target_fn") != 0 || !project || strcmp(project, "proj-alpha") != 0)
      return 0;
   if (max < 1)
      return 0;
   test_caller_hit_t *hits = (test_caller_hit_t *)out;
   snprintf(hits[0].project, sizeof(hits[0].project), "proj-alpha");
   snprintf(hits[0].file_path, sizeof(hits[0].file_path), "src/caller.c");
   snprintf(hits[0].caller, sizeof(hits[0].caller), "caller_fn");
   hits[0].line = 44;
   return 1;
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
int memory_embed_text(const char *text, const char *command, float *out, int max_dim)
{
   (void)text;
   (void)command;
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
int pgvec_code_search_paths(const char *project, const float *vec, int dim, int limit, char *paths,
                            int path_cap, double *scores, int max)
{
   (void)project;
   (void)vec;
   (void)dim;
   (void)limit;
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
   assert(strstr(buf, "\"file_path\":\"src/search.c\"") != NULL);   /* lexical still works */
   assert(strstr(buf, "\"file_path\":\"src/semantic.c\"") == NULL); /* vector leg skipped */
}

static void test_code_project_stats_missing_project(void)
{
   char buf[256];
   int s =
       kb_http_route_ex("GET", "/v1/code/project-stats", "", NULL, NULL, NULL, 0, buf, sizeof(buf));
   assert(s == 400);
   assert(strstr(buf, "missing project") != NULL);
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
   assert(s == 400);
   assert(strstr(buf, "missing project") != NULL);
}

/* §3b lessons route: an empty ledger (the stub returns no rows) renders the
 * honesty gate, not invented lessons; a missing project 400s. */
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
   assert(s == 400);
   assert(strstr(buf, "missing project") != NULL);
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
int kb_surprising_judge(const config_t *cfg, const char *judge_cmd, const char *project,
                        const kb_graph_surprising_t *links, int n, kb_surprising_verdict_t *out,
                        char *errbuf, size_t errlen)
{
   (void)cfg;
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
   assert(s == 400);
   assert(strstr(buf, "missing project") != NULL);
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
   assert(s == 400);
   assert(strstr(buf, "missing project") != NULL);
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
   assert(s == 400);
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
   char buf[512];
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

/* Branch moved (stored != current) -> scan runs and the new SHA is persisted. */
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
   assert(strstr(buf, "\"skipped\":false") != NULL);
   assert(strcmp(g_runtime_state_set_val, "tree-bbb") == 0); /* persisted for next time */
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
   g_curator_code_queued = 0;
   const char *body = "{\"project\":\"proj-alpha\",\"root_path\":\"/tmp/repo\",\"force\":true}";
   int s = kb_http_route_ex("POST", "/v1/code/scan", NULL, NULL, NULL, body, (int)strlen(body), buf,
                            sizeof(buf));
   assert(s == 200);
   assert(strstr(buf, "\"status\":\"ok\"") != NULL);
   assert(strstr(buf, "\"files\":5") != NULL);
   assert(strstr(buf, "\"inspected\":12") != NULL);
   assert(strcmp(g_code_scan_project, "proj-alpha") == 0);
   assert(strcmp(g_code_scan_root_path, "/tmp/repo") == 0);
   assert(g_code_scan_force == 1);
   assert(g_curator_code_queued == 1);
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
   /* The push path now queues code units for the curator just like a local scan
    * (the queue reads from DB2 by project name and self-gates on the curator
    * flag), so with the stubbed config_load reporting the gate on, it enqueues. */
   assert(g_curator_code_queued == 1);
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

static void test_code_build_ok(void)
{
   char buf[1024];
   g_db_initialized = 1;
   g_pgvec_ensure_rc = 0;
   g_kb_build_rc = 0;
   g_code_scan_rc = 0;
   g_runtime_state_set_now = 0;
   const char *body =
       "{\"path\":\"/tmp/kb\",\"project\":\"proj-alpha\",\"embedding_command\":\"embed-a\","
       "\"force\":true}";
   int s = kb_http_route_ex("POST", "/v1/code/build", NULL, NULL, NULL, body, (int)strlen(body),
                            buf, sizeof(buf));
   assert(s == 200);
   assert(strcmp(g_kb_build_path, "/tmp/kb") == 0);
   /* /v1/code/build now also runs the canonical scan (matches the async ingest path). */
   assert(strcmp(g_code_scan_project, "proj-alpha") == 0);
   assert(strcmp(g_kb_build_project, "proj-alpha") == 0);
   assert(strcmp(g_kb_build_embed_cmd, "embed-a") == 0);
   assert(g_kb_build_force == 1);
   assert(g_runtime_state_set_now == 1);
   assert(strstr(buf, "\"files_scanned\":9") != NULL);
   assert(strstr(buf, "\"embeddings_added\":5") != NULL);
}

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
   assert(strcmp(g_drain_embed_cmd, "builtin") == 0);
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

static void test_maintenance_repair_ok(void)
{
   char buf[1024];
   g_db_initialized = 1;
   g_pgvec_ensure_rc = 0;
   g_kb_build_rc = 0;
   const char *body =
       "{\"path\":\"/tmp/kb\",\"project\":\"proj-alpha\",\"embedding_command\":\"embed-a\"}";
   int s = kb_http_route_ex("POST", "/v1/maintenance/repair", NULL, NULL, NULL, body,
                            (int)strlen(body), buf, sizeof(buf));
   assert(s == 200);
   assert(strcmp(g_kb_build_path, "/tmp/kb") == 0);
   assert(strcmp(g_kb_build_project, "proj-alpha") == 0);
   assert(strcmp(g_kb_build_embed_cmd, "embed-a") == 0);
   assert(g_kb_build_force == 1);
   assert(strstr(buf, "\"files_scanned\":9") != NULL);
   assert(strstr(buf, "\"embeddings_added\":5") != NULL);
}

static void test_maintenance_repair_missing_project(void)
{
   char buf[256];
   int s = kb_http_route_ex("POST", "/v1/maintenance/repair", NULL, NULL, NULL,
                            "{\"path\":\"/tmp/kb\"}", 18, buf, sizeof(buf));
   assert(s == 400);
   assert(strstr(buf, "missing project") != NULL);
}

static void test_maintenance_reconcile_ok(void)
{
   char buf[512];
   g_reconcile_rc = 0;
   int s = kb_http_route_ex("POST", "/v1/maintenance/reconcile", NULL, NULL, NULL,
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
   int s = kb_http_route_ex("POST", "/v1/maintenance/clear", NULL, NULL, NULL,
                            "{\"project\":\"proj-alpha\"}", 24, buf, sizeof(buf));
   assert(s == 200);
   assert(strcmp(g_clear_project, "proj-alpha") == 0);
   assert(strstr(buf, "\"chunks_deleted\":12") != NULL);
}

static void test_maintenance_clear_error(void)
{
   char buf[256];
   g_clear_deleted = -1;
   int s = kb_http_route_ex("POST", "/v1/maintenance/clear", NULL, NULL, NULL,
                            "{\"project\":\"proj-alpha\"}", 24, buf, sizeof(buf));
   assert(s == 500);
   assert(strstr(buf, "kb clear failed") != NULL);
   g_clear_deleted = 12;
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
   const char *body = "{\"query\":\"three db\",\"filters\":{\"status\":\"done\","
                      "\"component\":\"pgvector\",\"kind\":\"doc_summary\"}}";
   int s = kb_http_route_ex("POST", "/v1/search", NULL, NULL, NULL, body, (int)strlen(body), buf,
                            sizeof(buf));
   assert(s == 200);
   assert(strstr(buf, "\"fusion_mode_used\":\"facet\"") != NULL);
   assert(strstr(buf, "\"artifact_id\":\"art-facet-1\"") != NULL);
   assert(strstr(buf, "\"kind\":\"doc_summary\"") != NULL);
   assert(strstr(buf, "\"total_hits\":1") != NULL);
   /* Bound to the active release by default (stub returns 7) and cites it. */
   assert(strstr(buf, "\"release_id\":7") != NULL);
}

/* POST /v1/search with explicit release_id:0 searches across all releases. */
static void test_search_facet_all_releases(void)
{
   char buf[2048];
   const char *body = "{\"query\":\"q\",\"release_id\":0,\"filters\":{\"kind\":\"doc_summary\"}}";
   int s = kb_http_route_ex("POST", "/v1/search", NULL, NULL, NULL, body, (int)strlen(body), buf,
                            sizeof(buf));
   assert(s == 200);
   assert(strstr(buf, "\"fusion_mode_used\":\"facet\"") != NULL);
   assert(strstr(buf, "\"release_id\":null") != NULL);
}

/* POST /v1/search without filters keeps the existing non-facet search path. */
static void test_search_no_filters_not_facet(void)
{
   char buf[2048];
   const char *body = "{\"query\":\"three db\"}";
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
   int s = kb_http_route_ex("POST", "/v1/search", NULL, NULL, NULL, "{\"query\":\"foo\"}", 15, buf,
                            sizeof(buf));
   assert(s == 200);
   assert(strstr(buf, "\"hits\"") != NULL);
   assert(strstr(buf, "\"fusion_mode_used\"") != NULL);
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
   int s = kb_http_route_ex("POST", "/v1/search", NULL, NULL, NULL, "{\"query\":\"foo\"}", 15, buf,
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
   assert(s == 200);
   assert(strstr(buf, "\"hits\"") != NULL);
   assert(strstr(buf, "\"project\":\"proj-alpha\"") != NULL);
   assert(strstr(buf, "\"file_path\":\"src/main.c\"") != NULL);
   assert(strstr(buf, "\"line\":12") != NULL);
   assert(strstr(buf, "\"kind\":\"function\"") != NULL);
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
   assert(s == 400);
   assert(strstr(buf, "missing project") != NULL);
}
int main(void)
{
   printf("kb_http_routes: ");

   test_health();
   test_health_ex_rich();
   test_health_status_mode();
   test_version();
   test_capabilities();
   test_console_overview();
   test_accounts_routes();
   test_mint_scope_restriction();
   test_governance_routes();
   test_intelligence_calibration_readiness();
   test_intelligence_demotion_check();
   test_intelligence_bandit_export();
   test_not_found();
   test_enroll_route();
   test_enroll_redeem_route();
   test_mtls_serve();
   test_mtls_listener();
   test_bearer_auth_ok();
   test_bearer_auth_missing();
   test_bearer_auth_wrong();
   test_head_method();
   test_method_not_allowed();

   test_curator_routes();
   test_invalidations_route();
   test_search_ok();
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
   test_code_hybrid_ok();
   test_code_hybrid_memory_leg();
   test_code_hybrid_missing_query();
   test_code_hybrid_no_symbol();
   test_code_hybrid_vector_ok();
   test_code_hybrid_vector_dim_mismatch_skips();
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
   test_code_scan_skips_unchanged_branch();
   test_code_scan_runs_on_branch_move();
   test_code_scan_worktree_ignores_sha();
   test_code_scan_installs_hook();
   test_code_scan_missing_root_path();
   test_code_scan_pushed_files_ok();
   test_code_scan_pushed_files_rejects_invalid_item();
   test_code_scan_db_unavailable();
   test_code_build_ok();
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
   test_maintenance_repair_ok();
   test_maintenance_repair_missing_project();
   test_maintenance_reconcile_ok();
   test_maintenance_clear_ok();
   test_maintenance_clear_error();
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
   test_scope_admin_token_full_access();

   test_search_facet_filter();
   test_search_facet_all_releases();
   test_search_no_filters_not_facet();

   printf("ok\n");
   return 0;
}
