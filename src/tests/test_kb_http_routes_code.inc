/* test_kb_http_routes_code.inc: code-index / scan / ingest / maintenance / job
 * route tests, split out of test_kb_http_routes.c to keep that translation
 * unit under the 2000-line hard limit. Included (with
 * test_kb_http_routes_endpoints.inc) just before main(); all helpers and
 * fixtures live in test_kb_http_routes.c above the include point. */
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

static void test_code_project_stats_missing_project(void)
{
   char buf[256];
   int s =
       kb_http_route_ex("GET", "/v1/code/project-stats", "", NULL, NULL, NULL, 0, buf, sizeof(buf));
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

