/* test_css_render_cmd.c: #4-full slice 3 — the command-driven render backend.
 * Registers css_render_command as the css_render_oracle adapter and drives it
 * through platform_exec_pipe (a /bin/sh -c backend), covering success, failure,
 * and the enable/command gates. */
#include "cJSON.h"
#include "config.h"
#include "css_render_cmd.h"
#include "css_render_oracle.h"
#include "platform_path.h"
#include "platform_test_util.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char g_home[512];

/* Write an aimee.yaml under an isolated HOME with the given flags/command. */
static void set_config(int css_enabled, const char *render_cmd)
{
   if (!g_home[0])
   {
      snprintf(g_home, sizeof(g_home), "%s/aimee-rcmd-XXXXXX", platform_tmpdir());
      assert(platform_mkdtemp(g_home) != NULL);
   }
   platform_setenv("HOME", g_home);
   platform_unsetenv("AIMEE_HOME");
   platform_setenv("AIMEE_NO_CACHE", "1");
   char dir[640];
   snprintf(dir, sizeof(dir), "%s/.config/aimee", g_home);
   assert(platform_mkdir_p(dir, 0700) == 0);
   char path[768];
   snprintf(path, sizeof(path), "%s/aimee.yaml", dir);
   FILE *f = fopen(path, "w");
   assert(f);
   fprintf(f, "css_style_graph_enabled: %s\n", css_enabled ? "true" : "false");
   /* NULL = omit the key (the non-empty default command applies); "" = write an
    * explicit empty value to DISABLE the now-default-on backend; else the command. */
   if (render_cmd && render_cmd[0])
      fprintf(f, "css_render_command: %s\n", render_cmd);
   else if (render_cmd)
      fprintf(f, "css_render_command: \"\"\n");
   fclose(f);
}

/* Write a tiny shell script (run via `sh <path>`, no exec bit needed). */
static void write_script(const char *name, const char *body, char *out, size_t cap)
{
   snprintf(out, cap, "%s/%s", g_home, name);
   FILE *f = fopen(out, "w");
   assert(f);
   fputs(body, f);
   fclose(f);
}

int main(void)
{
   /* set_config needs g_home; prime it with an initial (gate-off) config. */
   set_config(0, NULL);

   /* An OK backend: discard stdin ({html,css}), emit a fixed snapshot. */
   char ok_sh[640];
   write_script("render.sh",
                "cat >/dev/null\n"
                "printf '%s' '{\"nodes\":[{\"ref\":\".btn\",\"computed\":"
                "{\"color\":\"rgb(0, 0, 0)\"}}]}'\n",
                ok_sh, sizeof(ok_sh));
   char ok_cmd[700];
   snprintf(ok_cmd, sizeof(ok_cmd), "sh %s", ok_sh);

   /* Gate: flag on but command explicitly emptied -> no adapter. (css_render_command
    * is default-on, so "no backend" means an explicit empty override, not omission.) */
   css_render_oracle_set_adapter(NULL);
   set_config(1, "");
   assert(css_render_cmd_register() == 0);
   assert(!css_render_oracle_has_adapter());

   /* Gate: command set but flag off -> no adapter. */
   set_config(0, ok_cmd);
   assert(css_render_cmd_register() == 0);
   assert(!css_render_oracle_has_adapter());

   /* Default: flag on, command omitted -> the default sidecar command registers. */
   css_render_oracle_set_adapter(NULL);
   set_config(1, NULL);
   assert(css_render_cmd_register() == 1);
   assert(css_render_oracle_has_adapter());

   /* Both set -> adapter registered, render produces a parseable snapshot. */
   set_config(1, ok_cmd);
   assert(css_render_cmd_register() == 1);
   assert(css_render_oracle_has_adapter());
   char *json = NULL, *err = NULL;
   assert(css_render_oracle_render("<button class=btn/>", ".btn{color:#000}", &json, &err) ==
          CSS_RENDER_OK);
   assert(json && err == NULL);
   css_render_snapshot_t *s = css_render_snapshot_parse(json);
   assert(s && s->nnodes == 1 && strcmp(s->nodes[0].ref, ".btn") == 0);
   css_render_snapshot_free(s);
   free(json);
   json = NULL;

   /* A failing backend (non-zero exit) -> CSS_RENDER_ERROR, *err set. */
   char fail_sh[640];
   write_script("fail.sh", "cat >/dev/null\nexit 7\n", fail_sh, sizeof(fail_sh));
   char fail_cmd[700];
   snprintf(fail_cmd, sizeof(fail_cmd), "sh %s", fail_sh);
   set_config(1, fail_cmd);
   assert(css_render_cmd_register() == 1);
   assert(css_render_oracle_render("<x/>", ".x{}", &json, &err) == CSS_RENDER_ERROR);
   assert(json == NULL && err != NULL);
   free(err);

   printf("css_render_cmd: all tests passed\n");
   return 0;
}
