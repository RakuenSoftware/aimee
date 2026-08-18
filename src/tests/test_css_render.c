/* test_css_render.c: #4-full storage + evaluation — snapshot upsert/round-trip,
 * verdict folding into css_migration_units.oracle_equivalent, conservative
 * unknown when a snapshot is missing, phase validation, and the config gate. */
#include "aimee.h"
#include "config.h"
#include "css_analyze.h"
#include "css_render_oracle.h"
#include "modules/db2/c/db2_test_shim.h"
#include "platform_path.h"
#include "platform_test_util.h"
#include "../modules/db2/c/code_index.h"
#include "../modules/db2/c/css_graph.h"
#include "../modules/db2/c/css_migration.h"
#include "../modules/db2/c/css_render.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sqlite3.h>

/* Point config_load at an isolated HOME holding an aimee.yaml with the given
 * css_style_graph_enabled value (AIMEE_NO_CACHE bypasses the mtime cache). */
static void set_config(int css_enabled)
{
   char home[512];
   snprintf(home, sizeof(home), "%s/aimee-rend-XXXXXX", platform_tmpdir());
   assert(platform_mkdtemp(home) != NULL);
   platform_setenv("HOME", home);
   platform_unsetenv("AIMEE_HOME");
   platform_setenv("AIMEE_NO_CACHE", "1");
   char dir[640];
   snprintf(dir, sizeof(dir), "%s/.config/aimee", home);
   assert(platform_mkdir_p(dir, 0700) == 0);
   char path[768];
   snprintf(path, sizeof(path), "%s/aimee.yaml", dir);
   FILE *f = fopen(path, "w");
   assert(f);
   fprintf(f, "css_style_graph_enabled: %s\n", css_enabled ? "true" : "false");
   fclose(f);
}

static const char *SNAP_A =
    "{\"nodes\":[{\"ref\":\".btn\",\"computed\":{\"color\":\"rgb(0, 0, 0)\"}}]}";
static const char *SNAP_B =
    "{\"nodes\":[{\"ref\":\".btn\",\"computed\":{\"color\":\"rgb(255, 0, 0)\"}}]}";

static int compare_result;
static int compare_invalid;

static int compare_provider(const char *before_json, const char *after_json, int *before_valid,
                            int *after_valid, int *available, int *equivalent, int *diff_count)
{
   if (compare_result != 0)
      return compare_result;
   css_render_snapshot_t *before = before_json ? css_render_snapshot_parse(before_json) : NULL;
   css_render_snapshot_t *after = after_json ? css_render_snapshot_parse(after_json) : NULL;
   css_render_result_t *result = css_render_oracle_compare(before, after);
   assert(result);
   *before_valid = compare_invalid == 1 ? 2 : before != NULL;
   *after_valid = after != NULL;
   *available = result->available;
   *equivalent = result->equivalent;
   *diff_count = result->diff_count;
   if (compare_invalid == 2)
      *diff_count = 1;
   else if (compare_invalid == 3)
      *diff_count = -1;
   css_render_result_free(result);
   css_render_snapshot_free(before);
   css_render_snapshot_free(after);
   return 0;
}

static void test_injected_compare_provider(void)
{
   int before_valid = 7, after_valid = 7, available = 7, equivalent = 7, diff_count = 7;
   aimee_db2_register_css_render_compare_provider(NULL);
   assert(db2_css_render_compare(SNAP_A, SNAP_A, &before_valid, &after_valid, &available,
                                 &equivalent, &diff_count) == -1);
   assert(before_valid == 0 && after_valid == 0 && available == 0 && equivalent == 0 &&
          diff_count == 0);

   aimee_db2_register_css_render_compare_provider(compare_provider);
   compare_result = 1;
   assert(db2_css_render_compare(SNAP_A, SNAP_A, &before_valid, &after_valid, &available,
                                 &equivalent, &diff_count) == -1);
   compare_result = 0;
   compare_invalid = 1;
   assert(db2_css_render_compare(SNAP_A, SNAP_A, &before_valid, &after_valid, &available,
                                 &equivalent, &diff_count) == -1);
   compare_invalid = 2;
   assert(db2_css_render_compare(SNAP_A, SNAP_A, &before_valid, &after_valid, &available,
                                 &equivalent, &diff_count) == -1);
   compare_invalid = 3;
   assert(db2_css_render_compare(SNAP_A, SNAP_A, &before_valid, &after_valid, &available,
                                 &equivalent, &diff_count) == -1);
   compare_invalid = 0;
   assert(db2_css_render_compare(SNAP_A, SNAP_A, &before_valid, &after_valid, &available,
                                 &equivalent, &diff_count) == 0);
   assert(before_valid == 1 && after_valid == 1 && available == 1 && equivalent == 1 &&
          diff_count == 0);
}

int main(void)
{
   test_injected_compare_provider();

   db2_test_shim_open();
   set_config(1);

   /* Build one migration unit (Button.tsx) via the enumerate path. */
   int64_t pid = db2_code_index_project_upsert("rend", "/rend");
   int64_t css_fid = db2_code_index_file_upsert(pid, "styles.css", "2026-01-01T00:00:00Z");
   css_stylesheet_t *ss = css_analyze(".btn { color: black; }", 22);
   assert(ss && db2_css_graph_replace(css_fid, ss->rules, ss->rule_count) == 0);
   css_stylesheet_free(ss);
   int64_t comp = db2_code_index_file_upsert(pid, "Button.tsx", "2026-01-01T00:00:00Z");
   const char *tsx = "<button className=\"btn\" />";
   char toks[16][CSS_CLASS_TOKEN_MAX];
   int nt = css_extract_class_tokens(tsx, strlen(tsx), toks, 16);
   assert(db2_css_component_resolve(comp, toks, nt) == 0);
   assert(db2_css_migration_enumerate("rend") == 1);

   /* phase validation: only before/after are accepted. */
   assert(db2_css_render_snapshot_store("rend", "Button.tsx", "sideways", SNAP_A, "t") == -1);

   /* store + round-trip get. */
   assert(db2_css_render_snapshot_store("rend", "Button.tsx", "before", SNAP_A, "t1") == 1);
   char *got = NULL;
   assert(db2_css_render_snapshot_get("rend", "Button.tsx", "before", &got) == 1);
   assert(got && strcmp(got, SNAP_A) == 0);
   free(got);

   /* equivalent before/after -> verdict equivalent, unit oracle_equivalent=1. */
   assert(db2_css_render_snapshot_store("rend", "Button.tsx", "after", SNAP_A, "t2") == 1);
   css_render_verdict_t v;
   assert(db2_css_render_oracle_evaluate("rend", "Button.tsx", "t3", &v) == 0);
   assert(v.available == 1 && v.equivalent == 1 && v.diff_count == 0);
   css_migration_unit_t units[8];
   assert(db2_css_migration_list("rend", NULL, units, 8) == 1);
   assert(units[0].oracle_equivalent == 1);

   /* upsert after with a changed value -> not equivalent, oracle_equivalent=0. */
   assert(db2_css_render_snapshot_store("rend", "Button.tsx", "after", SNAP_B, "t4") == 1);
   assert(db2_css_render_oracle_evaluate("rend", "Button.tsx", "t5", &v) == 0);
   assert(v.available == 1 && v.equivalent == 0 && v.diff_count == 1);
   assert(db2_css_migration_list("rend", NULL, units, 8) == 1 && units[0].oracle_equivalent == 0);

   /* A re-added checkout cannot read the old generation's rendered evidence;
    * the next capture creates a generation-2 row while retaining history. */
   sqlite3 *db = (sqlite3 *)db2_test_shim_handle();
   assert(sqlite3_exec(db, "UPDATE projects SET current_generation=2 WHERE name='rend'", NULL, NULL,
                       NULL) == SQLITE_OK);
   got = NULL;
   assert(db2_css_render_snapshot_get("rend", "Button.tsx", "before", &got) == 0);
   assert(got == NULL);
   assert(db2_css_render_snapshot_store("rend", "Button.tsx", "before", SNAP_B, "t5b") == 1);
   assert(db2_css_render_snapshot_get("rend", "Button.tsx", "before", &got) == 1);
   assert(got && strcmp(got, SNAP_B) == 0);
   free(got);
   sqlite3_stmt *count = NULL;
   assert(sqlite3_prepare_v2(db,
                             "SELECT COUNT(*) FROM css_render_snapshots"
                             " WHERE project='rend' AND unit_path='Button.tsx' AND phase='before'",
                             -1, &count, NULL) == SQLITE_OK);
   assert(sqlite3_step(count) == SQLITE_ROW && sqlite3_column_int(count, 0) == 2);
   sqlite3_finalize(count);

   /* a unit with no snapshots -> conservative unknown (available=0). */
   assert(db2_css_render_oracle_evaluate("rend", "Ghost.tsx", "t6", &v) == 0);
   assert(v.available == 0 && v.equivalent == 0 && strstr(v.summary, "unknown"));

   /* gate off: store + evaluate become no-ops. */
   set_config(0);
   assert(db2_css_render_snapshot_store("rend", "Button.tsx", "before", SNAP_A, "t7") == 0);
   assert(db2_css_render_oracle_evaluate("rend", "Button.tsx", "t8", &v) == 0);
   assert(strstr(v.summary, "disabled"));

   db2_test_shim_close();
   printf("css_render: all tests passed\n");
   return 0;
}
