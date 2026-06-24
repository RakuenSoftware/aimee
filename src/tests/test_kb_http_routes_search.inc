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
