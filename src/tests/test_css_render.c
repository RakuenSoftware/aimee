/* test_css_render.c: #4-full storage + evaluation — snapshot upsert/round-trip,
 * verdict folding into css_migration_units.oracle_equivalent, conservative
 * unknown when a snapshot is missing, phase validation, and the config gate. */
#include "aimee.h"
#include "config.h"
#include "css_analyze.h"
#include "db2_test_shim.h"
#include "platform_path.h"
#include "platform_test_util.h"
#include "../db2/code_index.h"
#include "../db2/css_graph.h"
#include "../db2/css_migration.h"
#include "../db2/css_render.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

int main(void)
{
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
