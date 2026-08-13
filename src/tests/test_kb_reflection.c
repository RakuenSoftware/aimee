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
#include <errno.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>
#include <string.h>

static int g_idle_release_calls;
void reflection_test_release_idle(void);
#define db2_lease_release_idle reflection_test_release_idle
#include <sqlite3.h>

#include "db2_test_shim.h"

/* The unit under test (pulls its own headers). */
#include "../kb/kb_reflection.c"
#include "platform_test_util.h" /* platform_tmpdir: honour TMPDIR, do not leak into /tmp */
#undef db2_lease_release_idle

void reflection_test_release_idle(void)
{
   g_idle_release_calls++;
}

/* ── Stubs for deps the reflection TU references but this test does not link ── */

/* Graph enrichment: report "no citations" so run_synthesis_pass takes the plain
 * evidence path (the graph seam is exercised elsewhere). */
int kb_reasoning_query(const char *query, const char *bindings_json, const char *program,
                       const char *facts_json, kb_reasoning_result_t *result_out)
{
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
/* run_synthesis_pass reads config through accessors now instead of taking a
 * config_t. This suite links the REAL config module (TEST_CORE_OBJS), so the
 * settings come from a real aimee.yaml under an isolated HOME rather than from
 * stubs -- stubbing them here would collide with the linked accessors. Same
 * values base_cfg used to write into the struct; each case still overrides the
 * same field it always did. AIMEE_NO_CACHE=1 makes each rewrite take effect. */
static char g_home[256];
static int g_mdl_tiebreak = 1;
static int g_n_attempts = 2;
static int g_shadow = 0;
static char g_synth_cmd[512] = "";

static void write_cfg(void)
{
   char path[512];
   snprintf(path, sizeof(path), "%s/aimee.yaml", config_default_dir());
   FILE *fp = fopen(path, "w");
   assert(fp != NULL);
   fprintf(fp, "intelligence:\n  synthesize:\n");
   fprintf(fp, "    mdl_tiebreak_enabled: %s\n", g_mdl_tiebreak ? "true" : "false");
   fprintf(fp, "    synthesize_n_attempts: %d\n", g_n_attempts);
   fprintf(fp, "    reflection_shadow: %s\n", g_shadow ? "true" : "false");
   fprintf(fp, "    synthesize_command: \"%s\"\n", g_synth_cmd);
   fclose(fp);
   /* Prove the file round-tripped rather than trusting it: a silently unparsed
    * key would turn every assertion below into a test of the defaults. */
   assert(config_kb_synthesize_n_attempts() == g_n_attempts);
   assert(config_kb_reflection_synthesis_shadow() == g_shadow);
   assert(strcmp(config_kb_synthesize_command(), g_synth_cmd) == 0);
}

static void isolate_home(void)
{
   snprintf(g_home, sizeof(g_home), "%s/aimee-test-kb-reflection-XXXXXX", platform_tmpdir());
   assert(mkdtemp(g_home) != NULL);
   /* AIMEE_HOME rather than HOME: it IS the config dir, where the default is
    * $HOME/.config/aimee -- two levels that would need creating. */
   assert(setenv("AIMEE_HOME", g_home, 1) == 0);
   assert(setenv("AIMEE_NO_CACHE", "1", 1) == 0);
}

static void base_cfg(const char *cmd)
{
   g_mdl_tiebreak = 1;
   g_n_attempts = 2;
   g_shadow = 0;
   snprintf(g_synth_cmd, sizeof(g_synth_cmd), "%s", cmd);
   write_cfg();
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
   g_idle_release_calls = 0;
   db2_test_shim_open();
   sqlite3 *db = (sqlite3 *)db2_test_shim_handle();
   base_cfg("printf '%s' '" VALID_JSON "'");
   db2_artifact_proposed_t row;
   mk_row(&row);

   int rc = run_synthesis_pass(&row);
   assert(rc == 0);
   assert(count_session_synthesis(db) == 1);
   assert(g_idle_release_calls == g_n_attempts);
   db2_test_shim_close();
   printf("  valid response, normal mode → 1 candidate written OK\n");
}

/* Valid response, SHADOW mode ⇒ scored but NO durable write (fail-closed). */
static void test_shadow_writes_none(void)
{
   g_idle_release_calls = 0;
   db2_test_shim_open();
   sqlite3 *db = (sqlite3 *)db2_test_shim_handle();
   base_cfg("printf '%s' '" VALID_JSON "'");
   g_shadow = 1;
   write_cfg();
   db2_artifact_proposed_t row;
   mk_row(&row);

   int rc = run_synthesis_pass(&row);
   assert(rc == 0); /* shadow is a clean no-write success */
   assert(count_session_synthesis(db) == 0);
   assert(g_idle_release_calls == g_n_attempts);
   db2_test_shim_close();
   printf("  valid response, shadow mode → 0 candidates written OK\n");
}

/* Non-JSON response ⇒ defer (no valid candidate), NO durable write. */
static void test_garbage_writes_none(void)
{
   g_idle_release_calls = 0;
   db2_test_shim_open();
   sqlite3 *db = (sqlite3 *)db2_test_shim_handle();
   base_cfg("printf '%s' 'not json at all'");
   db2_artifact_proposed_t row;
   mk_row(&row);

   int rc = run_synthesis_pass(&row);
   assert(rc == -1); /* no valid candidates */
   assert(count_session_synthesis(db) == 0);
   assert(g_idle_release_calls == g_n_attempts);
   db2_test_shim_close();
   printf("  garbage response → defer, 0 candidates written OK\n");
}

/* Command failure (non-zero exit ⇒ kb_curator_llm_run returns NULL) ⇒ no write. */
static void test_command_failure_writes_none(void)
{
   g_idle_release_calls = 0;
   db2_test_shim_open();
   sqlite3 *db = (sqlite3 *)db2_test_shim_handle();
   base_cfg("false");
   db2_artifact_proposed_t row;
   mk_row(&row);

   int rc = run_synthesis_pass(&row);
   assert(rc == -1);
   assert(count_session_synthesis(db) == 0);
   assert(g_idle_release_calls == g_n_attempts);
   db2_test_shim_close();
   printf("  command failure → 0 candidates written OK\n");
}

static void test_empty_pass_releases_before_backoff(void)
{
   g_idle_release_calls = 0;
   db2_test_shim_open();
   base_cfg("false");
   run_reflection_pass_releasing_lease();
   assert(g_idle_release_calls == 1);
   db2_test_shim_close();
   printf("  empty pass releases its DB lease before scheduler backoff OK\n");
}

static void test_backoff_is_interruptible(void)
{
   kb_reflection_ctx_t ctx = {0};
   ctx.stop = 1;
   time_t started = time(NULL);
   reflection_sleep_interruptible(&ctx, 900);
   assert(time(NULL) - started < 1);
   printf("  scheduler backoff observes shutdown without a long sleep OK\n");
}

int main(void)
{
   printf("test_kb_reflection: reflection synthesis write-gate\n");
   isolate_home();
   test_valid_writes_one();
   test_shadow_writes_none();
   test_garbage_writes_none();
   test_command_failure_writes_none();
   test_empty_pass_releases_before_backoff();
   test_backoff_is_interruptible();
   printf("test_kb_reflection: all passed\n");
   return 0;
}
