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
#include "db2/lifecycle.h" /* §2c: db2_reembed_* / db2_dim_change_reset stub types */
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

char *kb_search_json_ex(const char *p, const char *q, const char *e, int m, const char *f)
{
   (void)p;
   (void)q;
   (void)e;
   (void)m;
   (void)f;
   char *r = malloc(64);
   if (r)
      strcpy(r, "{\"fusion_mode\":\"rrf\",\"results\":[]}");
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

/* §2c: stubs for the new /v1/reembed + /v1/search-guard db2 refs in kb_http.o.
 * g_test_reembed_in_progress lets a test drive the maintenance marker so the
 * /v1/search 503 guard can be exercised deterministically. */
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
int db2_embedding_dim(void)
{
   return 1024;
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

/* Must mirror code_search_hit_t (index.h) exactly — the handler casts the out
 * buffer to it; a layout mismatch would corrupt the read. */
typedef struct
{
   char project[128];
   char file_path[MAX_PATH_LEN];
   char snippet[512];
   double rank;
   char content_hash[80];
} test_code_search_hit_t;

int canonical_index_code_search(const char *query, const char *project, void *out, int max)
{
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
   assert(dim == 384);
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

/* §2c: lets a test flip kb.reembed_on_dim_change so the /v1/reembed gate
 * (403 when off, proceeds when on) can be exercised both ways. */
static int g_test_reembed_enabled = 0;
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

/* kb_intel_payload's bandit.sample/close builders call these; this test does not
 * link kb_bandit.o. Stub sample as "disabled" and reward as a no-op success. */
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
   /* The data-driven export asks the log which points exist; return the one
    * that is actually sampled in production (not the legacy kb_fusion_mode). */
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

#include "test_kb_http_routes_code.inc"
#include "test_kb_http_routes_endpoints.inc"
#include "test_kb_http_routes_search.inc"
int main(void)
{
   printf("kb_http_routes: ");

   test_health();
   test_health_ex_rich();
   test_health_status_mode();
   test_version();
   test_capabilities();
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
   test_code_project_stats_missing_project();
   test_code_project_stats_ok();
   test_code_project_stats_error_is_json();
   test_blast_radius_missing_params();
   test_blast_radius_not_found();
   test_blast_radius_ok();
   test_code_scan_ok();
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
