/* test_kb_reflection.c: idle-reflection synthesis write-gate.
 *
 * Covers the proposal §3/§4 changes to kb_reflection.c::run_synthesis_pass:
 *   - the LLM step routes through the shared curator path (kb_curator_llm_run),
 *     driven here deterministically via the sidecar command seam (a `printf`
 *     fallback command, no provider configured) — no network, no GPU;
 *   - fail-closed durable writes (the sharpest risk): a garbage response, a
 *     failed command, and shadow mode must each write ZERO session_synthesis
 *     artifacts; only a valid response in normal mode writes exactly one.
 *
 * run_synthesis_pass is static, so the unit is reached by including the .c
 * directly (same pattern as the other curator unit tests). db2 writes land in
 * the in-memory sqlite shim and are counted via SQL.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE /* strptime, used by the included kb_reflection.c */
#endif

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sqlite3.h>

#include "db2_test_shim.h"

/* The unit under test (pulls its own headers). */
#include "../kb/kb_reflection.c"

/* ── Stubs for deps the reflection TU references but this test does not link ── */

/* Graph enrichment: report "no citations" so run_synthesis_pass takes the plain
 * evidence path (the graph seam is exercised elsewhere). */
int kb_reasoning_query(const config_t *cfg, const char *query, const char *bindings_json,
                       const char *program, const char *facts_json,
                       kb_reasoning_result_t *result_out)
{
   (void)cfg;
   (void)query;
   (void)bindings_json;
   (void)program;
   (void)facts_json;
   if (result_out)
      memset(result_out, 0, sizeof(*result_out));
   return -1;
}
void kb_reasoning_result_free(kb_reasoning_result_t *r)
{
   (void)r;
}

/* Feature upsert is a side effect on a real write; no-op under the shim. */
int kb_features_upsert_synthesis_mdl(const char *id, const kb_mdl_score_t *score)
{
   (void)id;
   (void)score;
   return 0;
}

/* Background status heartbeats — not under test. */
void kb_background_set(const char *name, const char *descriptor_fmt, ...)
{
   (void)name;
   (void)descriptor_fmt;
}
void kb_background_clear(const char *name)
{
   (void)name;
}

/* Referenced by the scheduler thread (never started here). */
kb_service_ctx_t *g_kb_ctx = NULL;

/* ── Helpers ── */

static int count_session_synthesis(sqlite3 *db)
{
   sqlite3_stmt *st = NULL;
   assert(sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM artifacts WHERE kind='session_synthesis'",
                             -1, &st, NULL) == SQLITE_OK);
   assert(sqlite3_step(st) == SQLITE_ROW);
   int n = sqlite3_column_int(st, 0);
   sqlite3_finalize(st);
   return n;
}

/* A config wired to run synthesis via the command seam (no provider ⇒ the
 * reflection Tier-B stage falls back to kb_synthesize_command). */
static void base_cfg(config_t *cfg, const char *cmd)
{
   memset(cfg, 0, sizeof(*cfg));
   cfg->kb_mdl_tiebreak_enabled = 1;
   cfg->kb_synthesize_n_attempts = 2;
   cfg->kb_reflection_synthesis_shadow = 0;
   snprintf(cfg->kb_synthesize_command, sizeof(cfg->kb_synthesize_command), "%s", cmd);
}

static void mk_row(db2_artifact_proposed_t *row)
{
   memset(row, 0, sizeof(*row));
   snprintf(row->id, sizeof(row->id), "sess-1");
   snprintf(row->kind, sizeof(row->kind), "session_summary");
   snprintf(row->payload_json, sizeof(row->payload_json), "{\"summary\":\"did X and Y\"}");
}

#define VALID_JSON "{\"candidate\":\"insight\",\"cluster\":\"topicA\",\"confidence\":0.9}"

/* Valid response, normal mode ⇒ exactly one durable candidate written. */
static void test_valid_writes_one(void)
{
   db2_test_shim_open();
   sqlite3 *db = (sqlite3 *)db2_test_shim_handle();
   config_t cfg;
   base_cfg(&cfg, "printf '%s' '" VALID_JSON "'");
   db2_artifact_proposed_t row;
   mk_row(&row);

   int rc = run_synthesis_pass(&cfg, &row);
   assert(rc == 0);
   assert(count_session_synthesis(db) == 1);
   db2_test_shim_close();
   printf("  valid response, normal mode → 1 candidate written OK\n");
}

/* Valid response, SHADOW mode ⇒ scored but NO durable write (fail-closed). */
static void test_shadow_writes_none(void)
{
   db2_test_shim_open();
   sqlite3 *db = (sqlite3 *)db2_test_shim_handle();
   config_t cfg;
   base_cfg(&cfg, "printf '%s' '" VALID_JSON "'");
   cfg.kb_reflection_synthesis_shadow = 1;
   db2_artifact_proposed_t row;
   mk_row(&row);

   int rc = run_synthesis_pass(&cfg, &row);
   assert(rc == 0); /* shadow is a clean no-write success */
   assert(count_session_synthesis(db) == 0);
   db2_test_shim_close();
   printf("  valid response, shadow mode → 0 candidates written OK\n");
}

/* Non-JSON response ⇒ defer (no valid candidate), NO durable write. */
static void test_garbage_writes_none(void)
{
   db2_test_shim_open();
   sqlite3 *db = (sqlite3 *)db2_test_shim_handle();
   config_t cfg;
   base_cfg(&cfg, "printf '%s' 'not json at all'");
   db2_artifact_proposed_t row;
   mk_row(&row);

   int rc = run_synthesis_pass(&cfg, &row);
   assert(rc == -1); /* no valid candidates */
   assert(count_session_synthesis(db) == 0);
   db2_test_shim_close();
   printf("  garbage response → defer, 0 candidates written OK\n");
}

/* Command failure (non-zero exit ⇒ kb_curator_llm_run returns NULL) ⇒ no write. */
static void test_command_failure_writes_none(void)
{
   db2_test_shim_open();
   sqlite3 *db = (sqlite3 *)db2_test_shim_handle();
   config_t cfg;
   base_cfg(&cfg, "false");
   db2_artifact_proposed_t row;
   mk_row(&row);

   int rc = run_synthesis_pass(&cfg, &row);
   assert(rc == -1);
   assert(count_session_synthesis(db) == 0);
   db2_test_shim_close();
   printf("  command failure → 0 candidates written OK\n");
}

int main(void)
{
   printf("test_kb_reflection: reflection synthesis write-gate\n");
   test_valid_writes_one();
   test_shadow_writes_none();
   test_garbage_writes_none();
   test_command_failure_writes_none();
   printf("test_kb_reflection: all passed\n");
   return 0;
}
