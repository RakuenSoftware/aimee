/* test_curator_code_unit.c: unit tests for deep-curator Phase 2 code-unit
 * extraction:
 *   1. kb_curator_queue_code_unit — gate-off returns 0 without DB access
 *   2. kb_curator_queue_code_units_for_project — gate-off returns 0
 *   3. null argument guard
 *   4. kb_curator_extract_code_unit_one — empty queue returns 0
 *   5. ON CONFLICT DO NOTHING deduplication via raw shim SQL */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <sqlite3.h>

#include <aimee/core/event_bus/module_runtime.h>
#include "db2_test_shim.h"
#include "cJSON.h"
#include "config.h"
#include "kb_curator_extract.h"
#include "kb_curator_queue.h"
#include "kb_curator_grounding.h"
#include "kb_curator_provider.h"
#include "kb_curator_sidecar.h"
#include "platform_test_util.h" /* platform_tmpdir: honour TMPDIR, do not leak into /tmp */

/* The deep-curator code-extract gate is now ON by compiled default, but the
 * gate-off tests below need it OFF. Point AIMEE_HOME at an isolated temp config
 * with the extract gates disabled so the real config_load() the queue functions
 * call reports the gate off deterministically (and the run never touches the
 * developer's real ~/.config/aimee). */
/* The single place this test binary names config_t. Every case that needs a
 * different extract-code gate goes through here rather than loading its own copy:
 * config_t is a secret of the config module, and one reader is the budget. */
static void ccu_set_extract_code_gate(int on)
{
   config_t cfg;
   config_load(&cfg);
   cfg.kb_curator_extract_code_enabled = on;
   cfg.kb_curator_extract_docs_enabled = 0;
   if (!on)
      cfg.synthesis_endpoint[0] = '\0';
   config_save(&cfg);
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
int kb_curator_queue_code_unit(const char *project, const char *file_path, const char *symbol,
                               int line);
int kb_curator_queue_code_units_for_project(const char *project, const char *root_path);
int db2_artifact_count(const char *kind, const char *state);

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

static void test_queue_code_unit_gate_off(void)
{
   /* With gate off (default config), function must return 0 immediately. */
   int rc = kb_curator_queue_code_unit("proj", "src/foo.c", "foo_func", 10);
   assert(rc == 0);
   printf("  PASS: test_queue_code_unit_gate_off\n");
}

static void test_queue_code_units_for_project_gate_off(void)
{
   int rc = kb_curator_queue_code_units_for_project("proj", "/repo");
   assert(rc == 0);
   printf("  PASS: test_queue_code_units_for_project_gate_off\n");
}

static void test_queue_null_args(void)
{
   assert(kb_curator_queue_code_unit(NULL, "src/foo.c", "foo", 1) == 0);
   assert(kb_curator_queue_code_unit("proj", NULL, "foo", 1) == 0);
   assert(kb_curator_queue_code_unit("proj", "src/foo.c", NULL, 1) == 0);
   printf("  PASS: test_queue_null_args\n");
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
 * db2 pool member past its ceiling.
 *
 * MUST run before any other test that calls kb_curator_extract_code_unit_one:
 * the reclaim throttles on a process-wide static, and a successful reclaim in an
 * earlier test would arm it and make this one silently skip. max_attempts=1 puts
 * every reclaimed row on the terminal branch so the follow-on claim stays quiet
 * and these assertions are about the reclaim alone. */
static void test_reclaim_stale_running_code_unit(void)
{
   db2_test_shim_open();
   sqlite3 *db = (sqlite3 *)db2_test_shim_handle();
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

   db2_test_shim_close();
   printf("  PASS: test_reclaim_stale_running_code_unit (orphans reclaimed; diagnostic preserved; "
          "in-flight untouched)\n");
}

static void test_extract_code_unit_one_empty_queue(void)
{
   db2_test_shim_open();

   kb_curator_extract_opts_t opts;
   memset(&opts, 0, sizeof(opts));
   opts.max_attempts = 3;
   opts.max_tokens = 256;

   int rc = kb_curator_extract_code_unit_one(&opts);
   assert(rc == 0);

   db2_test_shim_close();
   printf("  PASS: test_extract_code_unit_one_empty_queue\n");
}

static void test_provider_outage_requeues_code_unit(void)
{
   db2_test_shim_open();
   sqlite3 *db = (sqlite3 *)db2_test_shim_handle();
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

   db2_test_shim_close();
   printf("  PASS: provider outage requeues code unit without spending attempt budget\n");
}

static void test_queue_dedup_via_conflict(void)
{
   db2_test_shim_open();
   sqlite3 *db = (sqlite3 *)db2_test_shim_handle();
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

   db2_test_shim_close();
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

/* Run the code-unit extract path once with a stubbed sidecar that returns a
 * single code_unit whose side_effects field is |side_effects_json|. When
 * |callee| is non-NULL, seed a code_calls edge target_fn -> callee so the
 * structural grounding gate has something to check. Reports the resulting
 * artifact/audit counts. */
static void run_extract_scenario(const char *side_effects_json, const char *callee,
                                 int *proposed_out, int *rejected_out, int *audit_out,
                                 int *pending_out)
{
   db2_test_shim_open();
   sqlite3 *db = (sqlite3 *)db2_test_shim_handle();
   assert(db != NULL);

   /* A real source file on disk: ccu_read_body() fopen()s job.file_path. */
   char src_path[256];
   snprintf(src_path, sizeof src_path, "%s/aimee_ccu_src_XXXXXX", platform_tmpdir());
   int src_fd = mkstemp(src_path);
   assert(src_fd >= 0);
   const char *src_body = "int target_fn(void) { return 0; }\n";
   assert(write(src_fd, src_body, strlen(src_body)) == (ssize_t)strlen(src_body));
   close(src_fd);

   /* The stubbed sidecar response. `cat <file>` ignores the redirected stdin
    * the C harness supplies and just prints this canned JSON. */
   char resp_path[256];
   snprintf(resp_path, sizeof resp_path, "%s/aimee_ccu_resp_XXXXXX", platform_tmpdir());
   int resp_fd = mkstemp(resp_path);
   assert(resp_fd >= 0);
   char resp[1024];
   snprintf(resp, sizeof(resp),
            "{\"status\":\"ok\",\"artifacts\":[{\"kind\":\"code_unit\",\"confidence\":0.9,"
            "\"payload\":{\"intent\":\"t\",\"side_effects\":%s,\"invariants\":[],"
            "\"domain_concepts\":[\"x\"]}}]}",
            side_effects_json);
   assert(write(resp_fd, resp, strlen(resp)) == (ssize_t)strlen(resp));
   close(resp_fd);

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
   snprintf(opts.extract_command, sizeof(opts.extract_command), "cat %s", resp_path);

   int rc = kb_curator_extract_code_unit_one(&opts);
   assert(rc == 1); /* claimed and processed one job */

   *proposed_out = db2_artifact_count("code_unit", "proposed");
   *rejected_out = db2_artifact_count("code_unit", "rejected");

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

   db2_test_shim_close();
   unlink(src_path);
   unlink(resp_path);
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

/* The curator drain runs server-side, where thin-client-ingested files do not
 * exist on disk. The body must come from DB2's file_contents (pushed at ingest),
 * not an open() of the project-relative path. Seed file_contents but NO disk file
 * and a non-existent project root: extraction must still succeed, proving the
 * body was read from DB2. (Regression for ~36k curator jobs failing with
 * "cannot read body".) */
static void test_extract_reads_body_from_db2_when_file_absent(void)
{
   db2_test_shim_open();
   sqlite3 *db = (sqlite3 *)db2_test_shim_handle();
   assert(db != NULL);

   const char *src_body = "int target_fn(void) { return 0; }\n";
   char resp_path[256];
   snprintf(resp_path, sizeof resp_path, "%s/aimee_ccu_resp_XXXXXX", platform_tmpdir());
   int resp_fd = mkstemp(resp_path);
   assert(resp_fd >= 0);
   const char *resp =
       "{\"status\":\"ok\",\"artifacts\":[{\"kind\":\"code_unit\",\"confidence\":0.9,"
       "\"payload\":{\"intent\":\"t\",\"side_effects\":[],\"invariants\":[],"
       "\"domain_concepts\":[\"x\"]}}]}";
   assert(write(resp_fd, resp, strlen(resp)) == (ssize_t)strlen(resp));
   close(resp_fd);

   /* Project root that does not exist on disk + a project-relative path with no
    * on-disk file — only DB2 file_contents has the body. */
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
   snprintf(opts.extract_command, sizeof(opts.extract_command), "cat %s", resp_path);

   int rc = kb_curator_extract_code_unit_one(&opts);
   assert(rc == 1); /* claimed + processed (body came from DB2, not disk) */
   assert(db2_artifact_count("code_unit", "proposed") == 1);

   db2_test_shim_close();
   unlink(resp_path);
   printf("  PASS: test_extract_reads_body_from_db2_when_file_absent\n");
}

/* ── main ─────────────────────────────────────────────────────────────── */

/* The sidecar resolver must (a) honour an explicit command, (b) pick a READABLE
 * candidate even when it is not executable — invoked as `python3 <path>`, the
 * old access(X_OK) check wrongly rejected the shipped 0644 script and stalled
 * every extract job — and (c) fall back to the cwd-relative path when none match. */
/* kb_curator_describe_wait_status: pclose(3) hands back a wait(2)-encoded status,
 * not an exit code. Reporting it raw wrote "sidecar exited 256" into
 * kb_code_unit_jobs.last_error for a sidecar that merely exit(1)'d — an opaque
 * number that reads like an exotic fault and hides the real failure mode. These
 * assert each status class decodes to something an operator can act on. */
static void test_describe_wait_status(void)
{
   char out[256];

   /* (a) The regression: exit(1) is 1<<8 == 256 in wait encoding. */
   kb_curator_describe_wait_status(1 << 8, 300, out, sizeof(out));
   assert(strcmp(out, "sidecar exited 1") == 0);

   /* (b) Success-shaped status still renders an exit line if a caller asks. */
   kb_curator_describe_wait_status(0, 300, out, sizeof(out));
   assert(strcmp(out, "sidecar exited 0") == 0);

   /* (c) timeout(1) reports its wall-clock cap as exit 124 — named only when the
    *     caller says it wrapped the command, and echoing the cap it was given.
    *     Hedged, because timeout also PROPAGATES a command's own exit 124: the
    *     two are indistinguishable, so the message must not assert either. */
   kb_curator_describe_wait_status(124 << 8, 300, out, sizeof(out));
   assert(strcmp(out,
                 "sidecar exited 124 (timeout after 300s, or the command itself exited 124)") == 0);

   /* (d) Same status WITHOUT a timeout wrapper is just the command's exit code;
    *     claiming a timeout there would be a lie (kb_curator_sidecar.c does not
    *     wrap, and passes 0). */
   kb_curator_describe_wait_status(124 << 8, 0, out, sizeof(out));
   assert(strcmp(out, "sidecar exited 124") == 0);

   /* (e) A real signal death — the kernel says so via WIFSIGNALED, so the
    *     message may state it as fact. SIGKILL is the OOM-killer signature we
    *     care about on a box where the model shares RAM with the drain. */
   kb_curator_describe_wait_status(9 /* WIFSIGNALED: low 7 bits = signo */, 300, out, sizeof(out));
   assert(strcmp(out, "sidecar killed by signal 9 (Killed)") == 0);

   /* (f) 128+n is the SHELL's convention for a signal-killed child — but a
    *     process can also exit(137) itself, and the two are indistinguishable
    *     here. So report the exit code as fact and the signal as a reading:
    *     asserting a kill outright would fake an OOM that never happened. */
   kb_curator_describe_wait_status((128 + 9) << 8, 300, out, sizeof(out));
   assert(strcmp(out, "sidecar exited 137 (128+9: likely killed by signal 9, Killed)") == 0);

   /* (g) pclose itself failing is distinct from any child status. */
   kb_curator_describe_wait_status(-1, 300, out, sizeof(out));
   assert(strncmp(out, "pclose failed:", 14) == 0);

   /* (h) Null/zero-length buffers must not be written through. */
   kb_curator_describe_wait_status(1 << 8, 300, NULL, 0);
   kb_curator_describe_wait_status(1 << 8, 300, out, 0);

   printf("  PASS: test_describe_wait_status\n");
}

/* kb_curator_shell_quote: bounding the sidecar with timeout(1) means a SECOND
 * shell parses the command line, so the configured command must be quoted rather
 * than interpolated raw. Without this, adding the timeout wrapper would silently
 * re-interpret commands that worked fine under a bare popen — an embedded quote
 * ends the string early, and $VAR expands twice. */
static void test_shell_quote(void)
{
   char out[2176];

   /* (a) an ordinary command is wrapped, contents untouched. */
   assert(kb_curator_shell_quote("python3 /opt/aimee/scripts/curator-extract.py", out,
                                 sizeof(out)) == 0);
   assert(strcmp(out, "'python3 /opt/aimee/scripts/curator-extract.py'") == 0);

   /* (b) the regression: shell metacharacters must survive as LITERALS. Inside
    *     single quotes nothing is special, so $VAR cannot expand and the double
    *     quote cannot terminate anything. */
   assert(kb_curator_shell_quote("python3 -c \"print($HOME)\"", out, sizeof(out)) == 0);
   assert(strcmp(out, "'python3 -c \"print($HOME)\"'") == 0);

   /* (c) an embedded single quote is the one case single-quoting can't nest:
    *     close, escape, reopen. */
   assert(kb_curator_shell_quote("echo 'hi'", out, sizeof(out)) == 0);
   assert(strcmp(out, "'echo '\\''hi'\\'''") == 0);

   /* (d) backslashes and backticks are literal too. */
   assert(kb_curator_shell_quote("a\\b`id`", out, sizeof(out)) == 0);
   assert(strcmp(out, "'a\\b`id`'") == 0);

   /* (e) empty string still yields a valid empty shell word. */
   assert(kb_curator_shell_quote("", out, sizeof(out)) == 0);
   assert(strcmp(out, "''") == 0);

   /* (f) overflow is a hard error, never a truncated (and thus mis-quoted)
    *     string the caller might still run. */
   char tiny[8];
   assert(kb_curator_shell_quote("aaaaaaaaaaaaaaaa", tiny, sizeof(tiny)) == -1);
   assert(kb_curator_shell_quote("abc", NULL, 16) == -1);
   assert(kb_curator_shell_quote(NULL, out, sizeof(out)) == -1);

   printf("  PASS: test_shell_quote\n");
}

/* The quoting must survive a REAL shell round-trip, not just a string compare.
 *
 * The invariant is NOT "nothing expands" — the command still runs under `sh -c`,
 * so the INNER shell applies normal semantics, exactly as the bare popen did.
 * The invariant is that wrapping in timeout(1) changes NOTHING about how the
 * command is interpreted. This command is chosen so the two disagree:
 *
 *   bare popen (correct):        lit::"q"
 *   new, quoted wrapper:         lit::"q"   <- identical, semantics preserved
 *   old, raw interpolation:      lit::q     <- outer shell ate the \" escapes
 */
static void test_sidecar_quoting_end_to_end(void)
{
   char err[256] = "";
   char *out = kb_curator_sidecar_run("printf '%s' \"lit:$NOT_A_REAL_VAR:\\\"q\\\"\"", "{}", 256,
                                      err, sizeof(err));
   assert(out != NULL);
   assert(strcmp(out, "lit::\"q\"") == 0);
   free(out);

   /* A single-quoted argument is the case the '\'' escaping exists for. */
   char *out2 = kb_curator_sidecar_run("printf '%s' 'a b'", "{}", 256, err, sizeof(err));
   assert(out2 != NULL);
   assert(strcmp(out2, "a b") == 0);
   free(out2);

   /* The redirection must stay in the COMMAND's parse context, not the wrapper's.
    * A pipeline is where the two diverge: under the bare popen the line is
    * `printf ABC | wc -c < tmp`, so `< tmp` binds to wc (the LAST command) and
    * wc counts the 2-byte request. Redirecting the timeout wrapper's stdin
    * instead would feed tmp to printf and leave wc counting printf's 3 bytes —
    * a silent change of meaning. Asserting 2 pins the bare-popen semantics.
    *
    *   bare popen:                  2
    *   wrapper stdin redirect:      3   <- the bug
    *   redirect inside, path as $1: 2   <- preserved
    */
   char *out3 = kb_curator_sidecar_run("printf ABC | wc -c", "{}", 256, err, sizeof(err));
   assert(out3 != NULL);
   long counted = strtol(out3, NULL, 10);
   assert(counted == 2);
   free(out3);

   /* The request path is embedded as a quoted literal, not passed as $1. With a
    * positional, a command containing `set --` rebinds $1 before the redirection
    * expands and the sidecar silently reads a DIFFERENT file — measured at 5
    * bytes (/etc/hostname) instead of the 2-byte request. A literal cannot be
    * reassigned, so this still reads the request. */
   char *out4 = kb_curator_sidecar_run("set -- /etc/hostname; wc -c", "{}", 256, err, sizeof(err));
   assert(out4 != NULL);
   assert(strtol(out4, NULL, 10) == 2);
   free(out4);

   printf("  PASS: test_sidecar_quoting_end_to_end\n");
}

/* kb_curator_append_sidecar_error: curator-extract.py's emit_error() prints
 * {"status":"error","error":<why>} to stdout and exits 1. That output was being
 * freed unread, so a job that failed for a knowable reason recorded only
 * "sidecar exited 1". */
static void test_append_sidecar_error(void)
{
   char err[256];

   /* (a) the real shape: the sidecar's reason is appended to the exit code. */
   snprintf(err, sizeof(err), "%s", "sidecar exited 1");
   kb_curator_append_sidecar_error(
       "{\"version\":1,\"status\":\"error\",\"error\":\"LLM returned non-JSON: boom\"}", err,
       sizeof(err));
   assert(strcmp(err, "sidecar exited 1: LLM returned non-JSON: boom") == 0);

   /* (b) a SUCCESSFUL payload is not an error — must not be appended. */
   snprintf(err, sizeof(err), "%s", "sidecar exited 1");
   kb_curator_append_sidecar_error("{\"status\":\"ok\",\"artifacts\":[]}", err, sizeof(err));
   assert(strcmp(err, "sidecar exited 1") == 0);

   /* (c) non-JSON / empty / NULL output leaves the wait-status description
    *     intact rather than inheriting a misleading fragment. */
   snprintf(err, sizeof(err), "%s", "sidecar exited 1");
   kb_curator_append_sidecar_error("Traceback (most recent call last):", err, sizeof(err));
   assert(strcmp(err, "sidecar exited 1") == 0);
   kb_curator_append_sidecar_error("", err, sizeof(err));
   assert(strcmp(err, "sidecar exited 1") == 0);
   kb_curator_append_sidecar_error(NULL, err, sizeof(err));
   assert(strcmp(err, "sidecar exited 1") == 0);

   /* (d) an error field that is not a string is ignored, not printed as junk. */
   snprintf(err, sizeof(err), "%s", "sidecar exited 1");
   kb_curator_append_sidecar_error("{\"status\":\"error\",\"error\":42}", err, sizeof(err));
   assert(strcmp(err, "sidecar exited 1") == 0);

   /* (e) a full errbuf must not overflow. */
   char tiny[20];
   snprintf(tiny, sizeof(tiny), "%s", "sidecar exited 1");
   kb_curator_append_sidecar_error("{\"status\":\"error\",\"error\":\"a very long reason here\"}",
                                   tiny, sizeof(tiny));
   assert(strlen(tiny) < sizeof(tiny));

   printf("  PASS: test_append_sidecar_error\n");
}

static void test_pick_sidecar_command_resolution(void)
{
   char out[768];

   /* (a) explicit command wins verbatim, candidates ignored. */
   const char *none[] = {"/should/not/matter.py"};
   kb_curator_pick_sidecar_command(
       "env SYNTHESIS_ENDPOINT=x python3 /opt/aimee/scripts/curator-extract.py", none, 1, out,
       sizeof(out));
   assert(strcmp(out, "env SYNTHESIS_ENDPOINT=x python3 /opt/aimee/scripts/curator-extract.py") ==
          0);

   /* (b) first READABLE candidate is chosen; a missing one ahead of it is skipped,
    *     and a 0644 (non-executable) file still qualifies. */
   char tmpl[256];
   snprintf(tmpl, sizeof tmpl, "%s/aimee_sidecar_XXXXXX", platform_tmpdir());
   int fd = mkstemp(tmpl);
   assert(fd >= 0);
   close(fd);
   assert(chmod(tmpl, 0644) == 0); /* readable, NOT executable */
   const char *cands[] = {"/nonexistent/curator-extract.py", tmpl};
   kb_curator_pick_sidecar_command("", cands, 2, out, sizeof(out));
   char want[800];
   snprintf(want, sizeof(want), "python3 %s", tmpl);
   assert(strcmp(out, want) == 0);
   unlink(tmpl);

   /* (c) no readable candidate -> cwd-relative fallback. */
   const char *missing[] = {"/nonexistent/a.py", NULL};
   kb_curator_pick_sidecar_command("", missing, 2, out, sizeof(out));
   assert(strcmp(out, "python3 scripts/curator-extract.py") == 0);

   printf("  PASS: test_pick_sidecar_command_resolution\n");
}

/* A job whose attempts are exhausted goes to status='failed' — which is neither
 * 'pending' nor 'done'. The queue counters used to report only those two, so a
 * curator that failed EVERY job read as pending=0 done=0: identical to an idle,
 * healthy queue. Health said "ok" while indexing was dead.
 *
 * Assert the failing jobs are counted AND that a sample diagnostic comes back,
 * because the count alone does not tell an operator what to fix. */
static void test_queue_counts_surface_failures(void)
{
   db2_test_shim_open();
   sqlite3 *db = (sqlite3 *)db2_test_shim_handle();
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

   db2_test_shim_close();
   printf("  PASS: test_queue_counts_surface_failures (failed jobs counted, not silently dropped "
          "between pending and done)\n");
}

static void test_stale_generation_job_is_not_claimed(void)
{
   db2_test_shim_open();
   sqlite3 *db = (sqlite3 *)db2_test_shim_handle();
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

   db2_test_shim_close();
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
   db2_test_shim_open();
   sqlite3 *db = (sqlite3 *)db2_test_shim_handle();
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
   db2_test_shim_close();
   printf("  PASS: code-unit enqueue skipped when no synthesis endpoint can consume the rows\n");
}

int main(void)
{
   printf("curator_code_unit:\n");

   test_force_curator_gate_off();
   /* MUST precede any other kb_curator_extract_code_unit_one caller: the
    * reclaim's throttle is a process-wide static that a successful earlier
    * reclaim would arm, silently skipping this one. */
   test_reclaim_stale_running_code_unit();
   test_queue_code_unit_gate_off();
   test_queue_code_units_for_project_gate_off();
   test_queue_null_args();
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
   test_extract_reads_body_from_db2_when_file_absent();
   test_pick_sidecar_command_resolution();
   test_describe_wait_status();
   test_shell_quote();
   test_sidecar_quoting_end_to_end();
   test_append_sidecar_error();

   test_queue_counts_surface_failures();
   test_stale_generation_job_is_not_claimed();
   test_queue_code_units_skipped_without_synthesis_endpoint();
   printf("ok\n");
   return 0;
}
