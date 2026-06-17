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
#include <unistd.h>
#include <sqlite3.h>

#include "db2_test_shim.h"
#include "cJSON.h"
#include "config.h"
#include "kb/kb_curator_grounding.h"

/* The deep-curator code-extract gate is now ON by compiled default, but the
 * gate-off tests below need it OFF. Point AIMEE_HOME at an isolated temp config
 * with the extract gates disabled so the real config_load() the queue functions
 * call reports the gate off deterministically (and the run never touches the
 * developer's real ~/.config/aimee). */
static void test_force_curator_gate_off(void)
{
   static char dir[] = "/tmp/aimee-curtest-XXXXXX";
   static int done = 0;
   if (done)
      return;
   done = 1;
   if (mkdtemp(dir))
      setenv("AIMEE_HOME", dir, 1);
   config_t cfg;
   config_load(&cfg);
   cfg.kb_curator_extract_code_enabled = 0;
   cfg.kb_curator_extract_docs_enabled = 0;
   config_save(&cfg);
}

/* Forward declarations (headers live in src/, not src/headers/). */
typedef struct
{
   char extract_command[512];
   int max_tokens;
   int max_attempts;
} kb_curator_extract_opts_t;
int kb_curator_queue_code_unit(const char *project, const char *file_path, const char *symbol,
                               int line);
int kb_curator_queue_code_units_for_project(const char *project, const char *root_path);
int kb_curator_extract_code_unit_one(const kb_curator_extract_opts_t *opts);
int db2_artifact_count(const char *kind, const char *state);

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

/* ── pure grounding-gate tests (no DB) ──────────────────────────────────── */

static void test_grounding_side_effecting_predicate(void)
{
   /* Names that previously broke a mis-sorted bsearch table must still match. */
   assert(kb_curator_callee_is_side_effecting("write") == 1);
   assert(kb_curator_callee_is_side_effecting("fopen") == 1);
   assert(kb_curator_callee_is_side_effecting("execl") == 1); /* execl < execle */
   assert(kb_curator_callee_is_side_effecting("fdatasync") == 1);
   assert(kb_curator_callee_is_side_effecting("PQexec") == 1); /* uppercase */
   assert(kb_curator_callee_is_side_effecting("sqlite3_step") == 1);
   assert(kb_curator_callee_is_side_effecting("strlen") == 0);
   assert(kb_curator_callee_is_side_effecting("memcpy") == 0);
   assert(kb_curator_callee_is_side_effecting("") == 0);
   assert(kb_curator_callee_is_side_effecting(NULL) == 0);
   printf("  PASS: test_grounding_side_effecting_predicate\n");
}

static void test_grounding_claims_no_side_effects(void)
{
   cJSON *p1 = cJSON_Parse("{\"side_effects\":[]}");
   assert(kb_curator_payload_claims_no_side_effects(p1) == 1);
   cJSON_Delete(p1);

   cJSON *p2 = cJSON_Parse("{\"intent\":\"x\"}"); /* key absent */
   assert(kb_curator_payload_claims_no_side_effects(p2) == 1);
   cJSON_Delete(p2);

   cJSON *p3 = cJSON_Parse("{\"side_effects\":[\"none\"]}");
   assert(kb_curator_payload_claims_no_side_effects(p3) == 1);
   cJSON_Delete(p3);

   cJSON *p4 = cJSON_Parse("{\"side_effects\":[\"writes to disk\"]}");
   assert(kb_curator_payload_claims_no_side_effects(p4) == 0);
   cJSON_Delete(p4);

   assert(kb_curator_payload_claims_no_side_effects(NULL) == 1);
   printf("  PASS: test_grounding_claims_no_side_effects\n");
}

static void test_grounding_contradicts_pure(void)
{
   const char *se_callees[] = {"strlen", "write"};
   const char *clean_callees[] = {"strlen", "memcpy"};
   char reason[64];

   cJSON *claims_none = cJSON_Parse("{\"side_effects\":[]}");
   assert(kb_curator_grounding_contradicts(claims_none, se_callees, 2, reason, sizeof(reason)) ==
          1);
   assert(strcmp(reason, "write") == 0);
   assert(kb_curator_grounding_contradicts(claims_none, clean_callees, 2, reason, sizeof(reason)) ==
          0);
   assert(reason[0] == '\0');
   cJSON_Delete(claims_none);

   /* Honest non-empty claim never contradicts, even with a side-effecting edge. */
   cJSON *honest = cJSON_Parse("{\"side_effects\":[\"writes\"]}");
   assert(kb_curator_grounding_contradicts(honest, se_callees, 2, reason, sizeof(reason)) == 0);
   cJSON_Delete(honest);

   printf("  PASS: test_grounding_contradicts_pure\n");
}

/* ── DB-backed full-path gate tests ─────────────────────────────────────── */

/* Run the code-unit extract path once with a stubbed sidecar that returns a
 * single code_unit whose side_effects field is |side_effects_json|. When
 * |callee| is non-NULL, seed a code_calls edge target_fn -> callee so the
 * structural grounding gate has something to check. Reports the resulting
 * artifact/audit counts. */
static void run_extract_scenario(const char *side_effects_json, const char *callee,
                                 int *proposed_out, int *rejected_out, int *audit_out)
{
   db2_test_shim_open();
   sqlite3 *db = (sqlite3 *)db2_test_shim_handle();
   assert(db != NULL);

   /* A real source file on disk: ccu_read_body() fopen()s job.file_path. */
   char src_path[] = "/tmp/aimee_ccu_src_XXXXXX";
   int src_fd = mkstemp(src_path);
   assert(src_fd >= 0);
   const char *src_body = "int target_fn(void) { return 0; }\n";
   assert(write(src_fd, src_body, strlen(src_body)) == (ssize_t)strlen(src_body));
   close(src_fd);

   /* The stubbed sidecar response. `cat <file>` ignores the redirected stdin
    * the C harness supplies and just prints this canned JSON. */
   char resp_path[] = "/tmp/aimee_ccu_resp_XXXXXX";
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
            "INSERT INTO files (id, project_id, path, scanned_at) VALUES (1,1,'%s','t');",
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

   db2_test_shim_close();
   unlink(src_path);
   unlink(resp_path);
}

static void test_extract_rejects_false_no_side_effects(void)
{
   /* Claims no side effects, but the call graph shows a write() edge. */
   int proposed = -1, rejected = -1, audit = -1;
   run_extract_scenario("[]", "write", &proposed, &rejected, &audit);
   assert(proposed == 0);
   assert(rejected == 1);
   assert(audit == 1); /* rejection recorded in the audit_events log */
   printf("  PASS: test_extract_rejects_false_no_side_effects\n");
}

static void test_extract_accepts_honest_claim(void)
{
   /* Honest non-empty claim with the same edge: committed, not rejected. */
   int proposed = -1, rejected = -1, audit = -1;
   run_extract_scenario("[\"writes to disk\"]", "write", &proposed, &rejected, &audit);
   assert(proposed == 1);
   assert(rejected == 0);
   assert(audit == 0);
   printf("  PASS: test_extract_accepts_honest_claim\n");
}

static void test_extract_accepts_pure_function(void)
{
   /* Claims no side effects and only calls a pure function: committed. */
   int proposed = -1, rejected = -1, audit = -1;
   run_extract_scenario("[]", "strlen", &proposed, &rejected, &audit);
   assert(proposed == 1);
   assert(rejected == 0);
   assert(audit == 0);
   printf("  PASS: test_extract_accepts_pure_function\n");
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
   char resp_path[] = "/tmp/aimee_ccu_resp_XXXXXX";
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

int main(void)
{
   printf("curator_code_unit:\n");

   test_force_curator_gate_off();
   test_queue_code_unit_gate_off();
   test_queue_code_units_for_project_gate_off();
   test_queue_null_args();
   test_extract_code_unit_one_empty_queue();
   test_queue_dedup_via_conflict();

   test_grounding_side_effecting_predicate();
   test_grounding_claims_no_side_effects();
   test_grounding_contradicts_pure();

   test_extract_rejects_false_no_side_effects();
   test_extract_accepts_honest_claim();
   test_extract_accepts_pure_function();
   test_extract_reads_body_from_db2_when_file_absent();

   printf("ok\n");
   return 0;
}
