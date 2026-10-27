/* test_curator_code_unit.c: unit tests for deep-curator Phase 2 code-unit
 * extraction:
 *   1. bulk code-unit enqueue gate and database semantics
 *   2. kb_curator_extract_code_unit_one empty queue and failure paths
 *   3. ON CONFLICT deduplication, grounding, scoping and transaction safety */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <sqlite3.h>

#include <aimee/core/event_bus/module_runtime.h>
#include "modules/kb/c/kb_store_test_shim.h"
#include "cJSON.h"
#include "config.h"
#include "kb_curator_extract.h"
#include "kb_curator_queue.h"
#include "kb_curator_grounding.h"
#include "kb_curator_provider.h"
#include "kb_curator_sidecar.h"
#include "support/mock_agent_http.h"
#include "platform_test_util.h" /* platform_tmpdir: honour TMPDIR, do not leak into /tmp */

/* Model wire fixtures enter through the real provider decoder, never a shell. */
static const char *model_payload;
static const char *model_finish = "stop";
static int model_thinking_only;
static int model_calls;

static int code_model_post(const char *url, const char *auth, const char *body, char **out,
                           int timeout_ms, const char *extra)
{
   (void)auth;
   (void)timeout_ms;
   (void)extra;
   assert(strstr(url, "/chat/completions"));
   cJSON *request = cJSON_Parse(body);
   assert(request);
   const cJSON *messages = cJSON_GetObjectItemCaseSensitive(request, "messages");
   const cJSON *system = cJSON_GetArrayItem(messages, 0);
   const char *policy = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(system, "content"));
   assert(policy && strstr(policy, "code analysis assistant"));
   const cJSON *input = cJSON_GetArrayItem(messages, 1);
   const char *text = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(input, "content"));
   assert(text && strstr(text, "target_fn") && strstr(text, "input"));
   cJSON_Delete(request);
   cJSON *response = cJSON_CreateObject();
   cJSON *choices = cJSON_AddArrayToObject(response, "choices");
   cJSON *choice = cJSON_CreateObject();
   cJSON_AddItemToArray(choices, choice);
   cJSON_AddStringToObject(choice, "finish_reason", model_finish);
   cJSON *message = cJSON_AddObjectToObject(choice, "message");
   cJSON_AddStringToObject(message, "role", "assistant");
   cJSON_AddStringToObject(message, model_thinking_only ? "reasoning_content" : "content",
                           model_payload);
   *out = cJSON_PrintUnformatted(response);
   cJSON_Delete(response);
   model_calls++;
   return 200;
}

static void code_model_start(const char *payload)
{
   model_payload = payload;
   model_calls = 0;
   assert(config_set_kb_curator_provider_base_url("http://code-fixture.invalid/v1") == 0);
   assert(config_set_kb_curator_provider_model("code-fixture") == 0);
   mock_agent_http_reset();
   mock_agent_http_set_post_handler(code_model_post);
}

static void code_model_stop(void)
{
   assert(model_calls == 1);
   mock_agent_http_reset();
   model_payload = NULL;
   assert(config_set_kb_curator_provider_base_url("") == 0);
   assert(config_set_kb_curator_provider_model("") == 0);
}

/* The deep-curator code-extract gate is now ON by compiled default, but the
 * gate-off tests below need it OFF. Point AIMEE_HOME at an isolated temp config
 * with the extract gates disabled so the queue functions
 * call reports the gate off deterministically (and the run never touches the
 * developer's real ~/.config/aimee). */
static void ccu_set_extract_code_gate(int on)
{
   assert(config_set_kb_curator_extract_code_enabled(on) == 0);
   assert(config_set_kb_curator_extract_docs_enabled(0) == 0);
   if (!on)
      assert(config_set_synthesis_endpoint("") == 0);
}

static void test_force_curator_gate_off(void)
{
   static char dir[256];
   snprintf(dir, sizeof dir, "%s/aimee-curtest-XXXXXX", platform_tmpdir());
   static int done = 0;
   if (done)
      return;
   done = 1;
   if (mkdtemp(dir))
      setenv("AIMEE_HOME", dir, 1);
   ccu_set_extract_code_gate(0);
}

/* Forward declarations (headers live in src/, not src/headers/). The opts struct
 * and kb_curator_extract_code_unit_one come from kb_curator_extract.h (included
 * above). */
int kb_curator_queue_code_units_for_project(const char *project, const char *root_path);
int kb_store_artifact_count(const char *kind, const char *state);

extern aimee_module_status_t aimee_kb_synthesis_module_handler(const aimee_module_invocation_t *,
                                                               const uint8_t *, uint32_t, uint8_t *,
                                                               uint32_t, uint32_t *, void *);

int aimee_module_invocation_cancelled(const aimee_module_invocation_t *invocation)
{
   (void)invocation;
   return 0;
}

static int grounding_provider_fail;

static int grounding_module_provider(aimee_kb_synthesis_claim_kind_t claim_kind,
                                     const char *const *claims, uint32_t claim_count,
                                     const char *const *callees, uint32_t callee_count,
                                     aimee_kb_synthesis_grounding_decision_t *decision)
{
   if (grounding_provider_fail)
      return -1;
   uint8_t request[AIMEE_KB_SYNTHESIS_REQUEST_LEN];
   uint8_t response[AIMEE_KB_SYNTHESIS_RESPONSE_LEN];
   uint32_t response_len = 0;
   aimee_module_invocation_t invocation = {.stage_id = AIMEE_KB_SYNTHESIS_STAGE_GROUNDING};
   if (aimee_kb_synthesis_request_encode(claim_kind, claims, claim_count, callees, callee_count,
                                         request, sizeof(request)) != 0 ||
       aimee_kb_synthesis_module_handler(&invocation, request, sizeof(request), response,
                                         sizeof(response), &response_len,
                                         NULL) != AIMEE_MODULE_STATUS_OK)
      return -1;
   return aimee_kb_synthesis_response_decode(response, response_len, decision);
}

/* ── gate-off tests (no DB needed) ─────────────────────────────────────── */

static void test_queue_code_units_for_project_gate_off(void)
{
   int rc = kb_curator_queue_code_units_for_project("proj", "/repo");
   assert(rc == 0);
   printf("  PASS: test_queue_code_units_for_project_gate_off\n");
}

/* ── DB-backed tests ────────────────────────────────────────────────────── */

/* Read one column of a kb_code_unit_jobs row into buf. Returns 0 on success. */
static int ccu_test_job_field(sqlite3 *db, long long id, const char *col, char *buf, size_t buflen)
{
   char sql[256];
   snprintf(sql, sizeof(sql), "SELECT %s FROM kb_code_unit_jobs WHERE id=%lld", col, id);
   sqlite3_stmt *st = NULL;
   if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK)
      return -1;
   int rc = -1;
   if (sqlite3_step(st) == SQLITE_ROW)
   {
      const unsigned char *t = sqlite3_column_text(st, 0);
      snprintf(buf, buflen, "%s", t ? (const char *)t : "");
      rc = 0;
   }
   sqlite3_finalize(st);
   return rc;
}

/* ccu_reclaim_stale_running: the code-unit analogue of the extract_doc reclaim.
 * ccu_claim_job only ever selects status='pending', so a job orphaned in
 * 'running' (worker crash/restart, wedged sidecar) is never retried and pins a
 * kb_store pool member past its ceiling.
 *
 * MUST run before any other test that calls kb_curator_extract_code_unit_one:
 * the reclaim throttles on a process-wide static, and a successful reclaim in an
 * earlier test would arm it and make this one silently skip. max_attempts=1 puts
 * every reclaimed row on the terminal branch so the follow-on claim stays quiet
 * and these assertions are about the reclaim alone. */
static void test_reclaim_stale_running_code_unit(void)
{
   kb_store_test_shim_open();
   sqlite3 *db = (sqlite3 *)kb_store_test_shim_handle();
   assert(db != NULL);

   /* (a) orphan: stale claim, attempts exhausted -> 'failed', and its existing
    *     diagnostic is PRESERVED rather than overwritten with the reclaim's own
    *     message — that error is why the attempt died. */
   assert(sqlite3_exec(db,
                       "INSERT INTO kb_code_unit_jobs (id,project,file_path,symbol,status,attempts,"
                       "last_error,claimed_by,claimed_at) VALUES (9101,'p','a.c','fn_a','running',"
                       "1,'mkstemp failed for /tmp/x: No space left on device','kb.curator.drain',"
                       "datetime('now','-60 minutes'))",
                       NULL, NULL, NULL) == SQLITE_OK);

   /* (b) orphan with no diagnostic -> reclaim explains why it is terminal. */
   assert(sqlite3_exec(db,
                       "INSERT INTO kb_code_unit_jobs (id,project,file_path,symbol,status,attempts,"
                       "last_error,claimed_by,claimed_at) VALUES (9102,'p','b.c','fn_b','running',"
                       "1,'','kb.curator.drain',datetime('now','-60 minutes'))",
                       NULL, NULL, NULL) == SQLITE_OK);

   /* (c) claimed just now: in-flight, NOT orphaned — reclaiming would yank live
    *     work out from under a running sidecar. */
   assert(sqlite3_exec(db,
                       "INSERT INTO kb_code_unit_jobs (id,project,file_path,symbol,status,attempts,"
                       "last_error,claimed_by,claimed_at) VALUES (9103,'p','c.c','fn_c','running',"
                       "1,'','kb.curator.drain',datetime('now'))",
                       NULL, NULL, NULL) == SQLITE_OK);

   kb_curator_extract_opts_t opts;
   memset(&opts, 0, sizeof(opts));
   opts.max_attempts = 1;
   opts.max_tokens = 256;
   (void)kb_curator_extract_code_unit_one(&opts);

   char buf[256];
   assert(ccu_test_job_field(db, 9101, "status", buf, sizeof(buf)) == 0);
   assert(strcmp(buf, "failed") == 0);
   assert(ccu_test_job_field(db, 9101, "last_error", buf, sizeof(buf)) == 0);
   assert(strstr(buf, "No space left on device") != NULL); /* diagnostic survived */

   assert(ccu_test_job_field(db, 9102, "status", buf, sizeof(buf)) == 0);
   assert(strcmp(buf, "failed") == 0);
   assert(ccu_test_job_field(db, 9102, "last_error", buf, sizeof(buf)) == 0);
   assert(strcmp(buf, "stale running lease reclaimed after max attempts") == 0);

   assert(ccu_test_job_field(db, 9103, "status", buf, sizeof(buf)) == 0);
   assert(strcmp(buf, "running") == 0); /* in-flight untouched */

   kb_store_test_shim_close();
   printf("  PASS: test_reclaim_stale_running_code_unit (orphans reclaimed; diagnostic preserved; "
          "in-flight untouched)\n");
}

static void test_extract_code_unit_one_empty_queue(void)
{
   kb_store_test_shim_open();

   kb_curator_extract_opts_t opts;
   memset(&opts, 0, sizeof(opts));
   opts.max_attempts = 3;
   opts.max_tokens = 256;

   int rc = kb_curator_extract_code_unit_one(&opts);
   assert(rc == 0);

   kb_store_test_shim_close();
   printf("  PASS: test_extract_code_unit_one_empty_queue\n");
}

static void test_provider_outage_requeues_code_unit(void)
{
   kb_store_test_shim_open();
   sqlite3 *db = (sqlite3 *)kb_store_test_shim_handle();
   assert(db != NULL);
   assert(sqlite3_exec(db,
                       "INSERT INTO kb_code_unit_jobs (id,project,file_path,symbol,status,attempts,"
                       "claimed_by,claimed_at) VALUES (9602,'p','out.c','out_fn','running',3,"
                       "'kb.curator.drain',datetime('now'))",
                       NULL, NULL, NULL) == SQLITE_OK);

   kb_curator_provider_backoff_recovered();
   kb_curator_mark_retry_provider_unavailable_code(
       9602, 3, "llm-chat.py exit 1: HTTP 503 from http://aimee-llm:8742/v1/chat/completions");

   char buf[256];
   assert(ccu_test_job_field(db, 9602, "status", buf, sizeof(buf)) == 0);
   assert(strcmp(buf, "pending") == 0);
   assert(ccu_test_job_field(db, 9602, "attempts", buf, sizeof(buf)) == 0);
   assert(strcmp(buf, "2") == 0);
   assert(kb_curator_provider_backoff_active());
   kb_curator_provider_backoff_recovered();

   kb_store_test_shim_close();
   printf("  PASS: provider outage requeues code unit without spending attempt budget\n");
}

static void test_queue_dedup_via_conflict(void)
{
   kb_store_test_shim_open();
   sqlite3 *db = (sqlite3 *)kb_store_test_shim_handle();
   assert(db != NULL);

   const char *ins = "INSERT INTO kb_code_unit_jobs"
                     " (project, file_path, symbol, kind, line)"
                     " VALUES ('p','src/bar.c','bar_fn','function',5)"
                     " ON CONFLICT DO NOTHING";
   assert(sqlite3_exec(db, ins, NULL, NULL, NULL) == SQLITE_OK);
   assert(sqlite3_exec(db, ins, NULL, NULL, NULL) == SQLITE_OK);

   sqlite3_stmt *st;
   assert(sqlite3_prepare_v2(db,
                             "SELECT COUNT(*) FROM kb_code_unit_jobs"
                             " WHERE project='p' AND file_path='src/bar.c' AND symbol='bar_fn'",
                             -1, &st, NULL) == SQLITE_OK);
   assert(sqlite3_step(st) == SQLITE_ROW);
   int count = sqlite3_column_int(st, 0);
   sqlite3_finalize(st);
   assert(count == 1);

   kb_store_test_shim_close();
   printf("  PASS: test_queue_dedup_via_conflict\n");
}

/* ── event-bus grounding seam tests (no DB) ─────────────────────────────── */

static void test_grounding_requires_provider(void)
{
   const char *callees[] = {"write"};
   aimee_kb_synthesis_grounding_decision_t decision;
   kb_curator_grounding_register_provider(NULL);
   assert(kb_curator_grounding_decide(NULL, callees, 1, &decision) == -1);
   printf("  PASS: test_grounding_requires_provider\n");
}

static void test_grounding_module_decisions(void)
{
   const char *se_callees[] = {"strlen", "write"};
   const char *clean_callees[] = {"strlen", "memcpy"};
   aimee_kb_synthesis_grounding_decision_t decision;

   cJSON *claims_none = cJSON_Parse("{\"side_effects\":[]}");
   assert(kb_curator_grounding_decide(claims_none, se_callees, 2, &decision) == 0);
   assert(decision.contradicts == 1);
   assert(strcmp(decision.reason, "write") == 0);
   assert(kb_curator_grounding_decide(claims_none, clean_callees, 2, &decision) == 0);
   assert(decision.contradicts == 0);
   assert(decision.reason[0] == '\0');
   cJSON_Delete(claims_none);

   /* Honest non-empty claim never contradicts, even with a side-effecting edge. */
   cJSON *honest = cJSON_Parse("{\"side_effects\":[\"writes\"]}");
   assert(kb_curator_grounding_decide(honest, se_callees, 2, &decision) == 0);
   assert(decision.contradicts == 0);
   cJSON_Delete(honest);

   printf("  PASS: test_grounding_module_decisions\n");
}

/* ── DB-backed full-path gate tests ─────────────────────────────────────── */

/* Run the actual code-unit extract path with a canonical provider that returns a
 * single code_unit whose side_effects field is |side_effects_json|. When
 * |callee| is non-NULL, seed a code_calls edge target_fn -> callee so the
 * structural grounding gate has something to check. Reports the resulting
 * artifact/audit counts. */
static void run_extract_scenario(const char *side_effects_json, const char *callee,
                                 int *proposed_out, int *rejected_out, int *audit_out,
                                 int *pending_out)
{
   kb_store_test_shim_open();
   sqlite3 *db = (sqlite3 *)kb_store_test_shim_handle();
   assert(db != NULL);

   /* A real source file on disk: ccu_read_body() fopen()s job.file_path. */
   char src_path[256];
   snprintf(src_path, sizeof src_path, "%s/aimee_ccu_src_XXXXXX", platform_tmpdir());
   int src_fd = mkstemp(src_path);
   assert(src_fd >= 0);
   const char *src_body = "int target_fn(void) { return 0; }\n";
   assert(write(src_fd, src_body, strlen(src_body)) == (ssize_t)strlen(src_body));
   close(src_fd);

   /* Provider-native response is admitted before artifact projection. */
   char resp[1024];
   snprintf(resp, sizeof(resp),
            "{\"status\":\"ok\",\"artifacts\":[{\"kind\":\"code_unit\",\"confidence\":0.9,"
            "\"payload\":{\"intent\":\"t\",\"side_effects\":%s,\"invariants\":[],"
            "\"domain_concepts\":[\"x\"]}}]}",
            side_effects_json);
   code_model_start(resp);

   /* Seed project + file + (optionally) a structural call edge. */
   char sql[2048];
   snprintf(sql, sizeof(sql),
            "INSERT INTO projects (id, name, root, scanned_at) VALUES (1,'testproj','/repo','t');"
            "INSERT INTO files (id, project_id, path, scanned_at) VALUES (1,1,'%s','t');"
            "INSERT INTO terms (file_id,name,kind,line)"
            " VALUES (1,'target_fn','definition',1);",
            src_path);
   assert(sqlite3_exec(db, sql, NULL, NULL, NULL) == SQLITE_OK);
   if (callee)
   {
      snprintf(sql, sizeof(sql),
               "INSERT INTO code_calls (file_id, caller, callee, line)"
               " VALUES (1,'target_fn','%s',2)",
               callee);
      assert(sqlite3_exec(db, sql, NULL, NULL, NULL) == SQLITE_OK);
   }

   snprintf(sql, sizeof(sql),
            "INSERT INTO kb_code_unit_jobs (project, file_path, symbol, kind, line)"
            " VALUES ('testproj','%s','target_fn','function',1)",
            src_path);
   assert(sqlite3_exec(db, sql, NULL, NULL, NULL) == SQLITE_OK);

   kb_curator_extract_opts_t opts;
   memset(&opts, 0, sizeof(opts));
   opts.max_attempts = 3;
   opts.max_tokens = 256;

   int rc = kb_curator_extract_code_unit_one(&opts);
   assert(rc == 1); /* claimed and processed one job */

   *proposed_out = kb_store_artifact_count("code_unit", "proposed");
   *rejected_out = kb_store_artifact_count("code_unit", "rejected");

   sqlite3_stmt *st;
   assert(sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM audit_events", -1, &st, NULL) == SQLITE_OK);
   assert(sqlite3_step(st) == SQLITE_ROW);
   *audit_out = sqlite3_column_int(st, 0);
   sqlite3_finalize(st);
   if (pending_out)
   {
      assert(sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM kb_code_unit_jobs WHERE status='pending'",
                                -1, &st, NULL) == SQLITE_OK);
      assert(sqlite3_step(st) == SQLITE_ROW);
      *pending_out = sqlite3_column_int(st, 0);
      sqlite3_finalize(st);
   }

   kb_store_test_shim_close();
   unlink(src_path);
   code_model_stop();
}

static void test_extract_rejects_false_no_side_effects(void)
{
   /* Claims no side effects, but the call graph shows a write() edge. */
   int proposed = -1, rejected = -1, audit = -1;
   run_extract_scenario("[]", "write", &proposed, &rejected, &audit, NULL);
   assert(proposed == 0);
   assert(rejected == 1);
   assert(audit == 1); /* rejection recorded in the audit_events log */
   printf("  PASS: test_extract_rejects_false_no_side_effects\n");
}

static void test_extract_accepts_honest_claim(void)
{
   /* Honest non-empty claim with the same edge: committed, not rejected. */
   int proposed = -1, rejected = -1, audit = -1;
   run_extract_scenario("[\"writes to disk\"]", "write", &proposed, &rejected, &audit, NULL);
   assert(proposed == 1);
   assert(rejected == 0);
   assert(audit == 0);
   printf("  PASS: test_extract_accepts_honest_claim\n");
}

static void test_extract_accepts_pure_function(void)
{
   /* Claims no side effects and only calls a pure function: committed. */
   int proposed = -1, rejected = -1, audit = -1;
   run_extract_scenario("[]", "strlen", &proposed, &rejected, &audit, NULL);
   assert(proposed == 1);
   assert(rejected == 0);
   assert(audit == 0);
   printf("  PASS: test_extract_accepts_pure_function\n");
}

static void test_extract_retries_when_grounding_module_fails(void)
{
   int proposed = -1, rejected = -1, audit = -1, pending = -1;
   grounding_provider_fail = 1;
   run_extract_scenario("[]", "write", &proposed, &rejected, &audit, &pending);
   grounding_provider_fail = 0;
   assert(proposed == 0);
   assert(rejected == 0);
   assert(audit == 0);
   assert(pending == 1);
   printf("  PASS: grounding module failure rolls back and requeues extraction\n");
}

static void test_declined_or_incomplete_code_is_not_artifact(void)
{
   const char *stops[] = {"content_filter", "length", "tool_calls"};
   for (size_t i = 0; i < sizeof(stops) / sizeof(stops[0]); i++)
   {
      int proposed, rejected, audit, pending;
      model_finish = stops[i];
      run_extract_scenario("[]", NULL, &proposed, &rejected, &audit, &pending);
      assert(proposed == 0 && rejected == 0 && audit == 0 && pending == 1);
   }
   model_finish = "stop";
   model_thinking_only = 1;
   int proposed, rejected, audit, pending;
   run_extract_scenario("[]", NULL, &proposed, &rejected, &audit, &pending);
   assert(proposed == 0 && rejected == 0 && audit == 0 && pending == 1);
   model_thinking_only = 0;
   puts("  PASS: refused, truncated, tool-pending and THINKING cannot create code artifacts");
}

/* The curator drain runs server-side, where thin-client-ingested files do not
 * exist on disk. The body must come from KB_STORE's file_contents (pushed at ingest),
 * not an open() of the project-relative path. Seed file_contents but NO disk file
 * and a non-existent project root: extraction must still succeed, proving the
 * body was read from KB_STORE. (Regression for ~36k curator jobs failing with
 * "cannot read body".) */
static void test_extract_reads_body_from_kb_store_when_file_absent(void)
{
   kb_store_test_shim_open();
   sqlite3 *db = (sqlite3 *)kb_store_test_shim_handle();
   assert(db != NULL);

   const char *src_body = "int target_fn(void) { return 0; }\n";
   const char *resp =
       "{\"status\":\"ok\",\"artifacts\":[{\"kind\":\"code_unit\",\"confidence\":0.9,"
       "\"payload\":{\"intent\":\"t\",\"side_effects\":[],\"invariants\":[],"
       "\"domain_concepts\":[\"x\"]}}]}";
   code_model_start(resp);

   /* Project root that does not exist on disk + a project-relative path with no
    * on-disk file — only KB_STORE file_contents has the body. */
   const char *sql =
       "INSERT INTO projects (id, name, root, scanned_at)"
       " VALUES (1,'testproj','/nonexistent-root-xyzzy','t');"
       "INSERT INTO files (id, project_id, path, scanned_at) VALUES (1,1,'src/foo.c','t');"
       "INSERT INTO terms (file_id,name,kind,line) VALUES (1,'target_fn','definition',1);"
       "INSERT INTO file_contents (file_id, content)"
       " VALUES (1,'int target_fn(void) { return 0; }\n');"
       "INSERT INTO kb_code_unit_jobs (project, file_path, symbol, kind, line)"
       " VALUES ('testproj','src/foo.c','target_fn','function',1);";
   assert(sqlite3_exec(db, sql, NULL, NULL, NULL) == SQLITE_OK);
   (void)src_body;

   kb_curator_extract_opts_t opts;
   memset(&opts, 0, sizeof(opts));
   opts.max_attempts = 3;
   opts.max_tokens = 256;

   int rc = kb_curator_extract_code_unit_one(&opts);
   assert(rc == 1); /* claimed + processed (body came from KB_STORE, not disk) */
   assert(kb_store_artifact_count("code_unit", "proposed") == 1);

   kb_store_test_shim_close();
   code_model_stop();
   printf("  PASS: test_extract_reads_body_from_kb_store_when_file_absent\n");
}

/* ── main ─────────────────────────────────────────────────────────────── */

/* A job whose attempts are exhausted goes to status='failed' — which is neither
 * 'pending' nor 'done'. The queue counters used to report only those two, so a
 * curator that failed EVERY job read as pending=0 done=0: identical to an idle,
 * healthy queue. Health said "ok" while indexing was dead.
 *
 * Assert the failing jobs are counted AND that a sample diagnostic comes back,
 * because the count alone does not tell an operator what to fix. */
static void test_queue_counts_surface_failures(void)
{
   kb_store_test_shim_open();
   sqlite3 *db = (sqlite3 *)kb_store_test_shim_handle();
   assert(db != NULL);

   assert(sqlite3_exec(db, "INSERT INTO projects (name,root,scanned_at) VALUES ('p','/p','t')",
                       NULL, NULL, NULL) == SQLITE_OK);
   assert(sqlite3_exec(db,
                       "INSERT INTO kb_documents"
                       " (id,project,file_path,file_hash,chunk_index,content) VALUES"
                       " (9305,'p','a.md','h1',0,'a'),(9306,'p','b.md','h2',0,'b')",
                       NULL, NULL, NULL) == SQLITE_OK);

   assert(sqlite3_exec(db,
                       "INSERT INTO kb_code_unit_jobs (id,project,file_path,symbol,status,attempts,"
                       "last_error,updated_at) VALUES "
                       "(9301,'p','a.c','fn_a','failed',3,'older code-unit failure',"
                       "'2026-07-28 10:00:00'),"
                       "(9302,'p','b.c','fn_b','failed',3,'newer code-unit failure',"
                       "'2026-07-28 10:01:00'),"
                       "(9303,'p','c.c','fn_c','pending',1,'newest but pending',"
                       "'2026-07-28 10:04:00'),"
                       "(9304,'p','d.c','fn_d','done',1,'newest but recovered',"
                       "'2026-07-28 10:05:00')",
                       NULL, NULL, NULL) == SQLITE_OK);
   assert(sqlite3_exec(db,
                       "INSERT INTO kb_async_jobs (id,kind,document_id,project,status,attempts,"
                       "last_error,updated_at) VALUES "
                       "(9305,'extract_doc',9305,'p','failed',3,'latest extract failure',"
                       "'2026-07-28 10:02:00'),"
                       "(9306,'extract_doc',9306,'p','done',1,'later recovered extract error',"
                       "'2026-07-28 10:03:00')",
                       NULL, NULL, NULL) == SQLITE_OK);

   kb_curator_queue_counts_t qc;
   memset(&qc, 0, sizeof(qc));
   kb_curator_queue_counts(&qc);

   /* The two dead jobs are visible instead of vanishing between the buckets. */
   assert(qc.code_unit_failing == 2);
   assert(qc.code_unit_pending == 1);
   assert(qc.code_unit_done == 1);
   assert(qc.extract_failing == 1);

   /* The newest terminal reason across both queues travels with the count;
    * newer pending/done historical errors are deliberately ignored. */
   assert(strcmp(qc.last_error, "latest extract failure") == 0);
   assert(strstr(qc.last_error, "recovered") == NULL);

   kb_store_test_shim_close();
   printf("  PASS: test_queue_counts_surface_failures (failed jobs counted, not silently dropped "
          "between pending and done)\n");
}

static void test_stale_generation_job_is_not_claimed(void)
{
   kb_store_test_shim_open();
   sqlite3 *db = (sqlite3 *)kb_store_test_shim_handle();
   assert(db != NULL);
   assert(sqlite3_exec(db,
                       "INSERT INTO projects"
                       " (id,name,root,scanned_at,current_generation)"
                       " VALUES (1,'p','/p','t',2);"
                       "INSERT INTO files"
                       " (id,project_id,generation,path,scanned_at)"
                       " VALUES (1,1,2,'same.c','t');"
                       "INSERT INTO terms (file_id,name,kind,line)"
                       " VALUES (1,'same_fn','definition',1);"
                       "INSERT INTO kb_code_unit_jobs"
                       " (project,generation,file_path,symbol,status)"
                       " VALUES ('p',1,'same.c','same_fn','pending')",
                       NULL, NULL, NULL) == SQLITE_OK);

   kb_curator_extract_opts_t opts;
   memset(&opts, 0, sizeof(opts));
   opts.max_attempts = 3;
   opts.max_tokens = 256;
   assert(kb_curator_extract_code_unit_one(&opts) == 0);

   kb_store_test_shim_close();
   printf("  PASS: stale-generation code-unit job is not claimed for a re-added path\n");
}

static int ccu_count(sqlite3 *db, const char *sql)
{
   sqlite3_stmt *st = NULL;
   if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK)
      return -1;
   int n = (sqlite3_step(st) == SQLITE_ROW) ? sqlite3_column_int(st, 0) : -1;
   sqlite3_finalize(st);
   return n;
}

/* The rows this enqueue writes have exactly one consumer: extract_code, which runs
 * the synthesis sidecar. With the stage ENABLED but no endpoint configured, not one
 * of them can ever reach 'done' -- and the enqueue is not free. It runs on the
 * synchronous path of /v1/code/scan and inserts one row per symbol: measured on a
 * 4,018-file corpus, ~173,000 rows and 100 MB of table, adding ~215s to every scan
 * for work that cannot start. It compounds too, since the enqueue's anti-join scans
 * the table every later scan keeps growing.
 *
 * Both halves matter. Asserting only "nothing was enqueued" would pass just as well
 * if the fixture were wrong and there had been nothing to enqueue in the first
 * place, so the same corpus is enqueued again WITH an endpoint configured. */
static void test_queue_code_units_skipped_without_synthesis_endpoint(void)
{
   kb_store_test_shim_open();
   sqlite3 *db = (sqlite3 *)kb_store_test_shim_handle();
   assert(db != NULL);
   assert(sqlite3_exec(db,
                       "INSERT INTO projects"
                       " (id,name,root,scanned_at,lifecycle_state,current_generation)"
                       " VALUES (1,'p','/p','t','current',1);"
                       "INSERT INTO files"
                       " (id,project_id,generation,path,scanned_at)"
                       " VALUES (1,1,1,'a.c','t');"
                       "INSERT INTO terms (file_id,name,kind,line)"
                       " VALUES (1,'fn_one','definition',1),(1,'fn_two','definition',2)",
                       NULL, NULL, NULL) == SQLITE_OK);

   ccu_set_extract_code_gate(1);
   unsetenv("SYNTHESIS_ENDPOINT");

   /* The fixture really does have rows to enqueue, so "nothing was enqueued" below
    * cannot pass for the trivial reason that there was nothing to insert. */
   assert(ccu_count(db, "SELECT count(*) FROM terms") == 2);

   assert(kb_curator_queue_code_units_for_project("p", "/p") == 0);
   assert(ccu_count(db, "SELECT count(*) FROM kb_code_unit_jobs") == 0);

   /* Configured (not necessarily reachable): the rows become real work again, and a
    * transient outage is the drain's provider gate to handle, not this one's.
    *
    * Asserted as "gets past the gate", not "inserts 2 rows": the enqueue is one
    * PostgreSQL statement (DISTINCT ON, ::int) that the sqlite shim cannot prepare,
    * so it returns -1 here. That still separates the two paths exactly where it
    * matters -- unconfigured short-circuits BEFORE the statement (0, no DB touched),
    * configured reaches it (-1 from prepare). Coverage that the statement itself
    * enqueues correctly belongs on a PostgreSQL-backed test, not this shim. */
   setenv("SYNTHESIS_ENDPOINT", "http://synth.invalid:8742", 1);
   assert(kb_curator_queue_code_units_for_project("p", "/p") == -1);
   assert(ccu_count(db, "SELECT count(*) FROM kb_code_unit_jobs") == 0);
   unsetenv("SYNTHESIS_ENDPOINT");

   ccu_set_extract_code_gate(0);
   kb_store_test_shim_close();
   printf("  PASS: code-unit enqueue skipped when no synthesis endpoint can consume the rows\n");
}

int main(void)
{
   if (kb_store_test_shim_skip_on_postgres("curator_code_unit"))
      return 0;

   printf("curator_code_unit:\n");

   test_force_curator_gate_off();
   /* MUST precede any other kb_curator_extract_code_unit_one caller: the
    * reclaim's throttle is a process-wide static that a successful earlier
    * reclaim would arm, silently skipping this one. */
   test_reclaim_stale_running_code_unit();
   test_queue_code_units_for_project_gate_off();
   test_extract_code_unit_one_empty_queue();
   test_provider_outage_requeues_code_unit();
   test_queue_dedup_via_conflict();

   test_grounding_requires_provider();
   kb_curator_grounding_register_provider(grounding_module_provider);
   test_grounding_module_decisions();

   test_extract_rejects_false_no_side_effects();
   test_extract_accepts_honest_claim();
   test_extract_accepts_pure_function();
   test_extract_retries_when_grounding_module_fails();
   test_extract_reads_body_from_kb_store_when_file_absent();
   test_declined_or_incomplete_code_is_not_artifact();

   test_queue_counts_surface_failures();
   test_stale_generation_job_is_not_claimed();
   test_queue_code_units_skipped_without_synthesis_endpoint();
   printf("ok\n");
   return 0;
}
