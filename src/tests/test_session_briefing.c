/* test_session_briefing.c: phase-2-step-2 recall helpers.
 * Uses an in-memory DB + the public prospective/directive create
 * APIs so the tests exercise the same path the session-start runner
 * exercises. */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "aimee.h"
#include "db2.h"
#include "db2_test_shim.h"
#include "memory.h"
#include "session_briefing.h"
#include "platform_test_util.h" /* platform_tmpdir: honour TMPDIR, do not leak into /tmp */

static char *make_tmpdir(void)
{
   char *tmp = malloc(64);
   assert(tmp);
   snprintf(tmp, 64, "%s/test_session_briefing_XXXXXX", platform_tmpdir());
   assert(mkdtemp(tmp) != NULL);
   return tmp;
}

static void rm_rf(const char *path)
{
   char cmd[1024];
   snprintf(cmd, sizeof(cmd), "rm -rf '%s'", path);
   (void)system(cmd);
}

static void mkdir_p(const char *path)
{
   char cmd[1024];
   snprintf(cmd, sizeof(cmd), "mkdir -p '%s'", path);
   (void)system(cmd);
}

static void write_file(const char *path, const char *content)
{
   FILE *f = fopen(path, "w");
   assert(f);
   fputs(content, f);
   fclose(f);
}

int main(void)
{
   printf("session_briefing: ");

   {
      char *root = make_tmpdir();
      char *bundled = make_tmpdir();
      setenv("AIMEE_BUNDLED_SKILLS_DIR", bundled, 1);

      char skill_dir[512];
      snprintf(skill_dir, sizeof(skill_dir), "%s/find-symbols", bundled);
      mkdir_p(skill_dir);
      char path[512];
      snprintf(path, sizeof(path), "%s/SKILL.md", skill_dir);
      write_file(path, "---\n"
                       "name: find-symbols\n"
                       "description: Use when locating where a symbol is defined.\n"
                       "---\n"
                       "Body.\n");

      char *idx = session_briefing_render_skill_index(root, 0);
      assert(idx != NULL);
      assert(strstr(idx, "# Skill Dispatch") != NULL);
      assert(strstr(idx, "activate it with `/skill <name>`") != NULL);
      assert(strstr(idx, "- find-symbols: Use when locating where a symbol is defined.") != NULL);

      char *idx2 = session_briefing_render_skill_index(root, 0);
      assert(idx2 != NULL);
      assert(strcmp(idx, idx2) == 0);
      free(idx2);
      free(idx);

      char project_skill_dir[512];
      snprintf(project_skill_dir, sizeof(project_skill_dir), "%s/.aimee/skills/project-first",
               root);
      mkdir_p(project_skill_dir);
      snprintf(path, sizeof(path), "%s/SKILL.md", project_skill_dir);
      write_file(path, "---\n"
                       "name: project-first\n"
                       "description: Use when proving index limit behavior.\n"
                       "---\n"
                       "Body.\n");
      char *limited = session_briefing_render_skill_index(root, 1);
      assert(limited != NULL);
      assert(strstr(limited, "- project-first: Use when proving index limit behavior.") != NULL);
      assert(strstr(limited, "- find-symbols:") == NULL);
      free(limited);

      rm_rf(root);
      rm_rf(bundled);
      free(root);
      free(bundled);
   }

   /* DB2 holds prospective_memories and the directive helpers' tables;
    * the shim helper opens an in-memory backing for the test. */
   db2_test_shim_open();

   /* --- empty DB: both helpers return NULL so the session-start
    *     output stays clean when there's nothing to surface. --- */
   {
      char *c = session_briefing_render_commitments(0);
      char *q = session_briefing_render_directives(0);
      assert(c == NULL);
      assert(q == NULL);
      free(c);
      free(q);
   }

   /* --- one armed reminder → commitments section present --- */
   {
      memory_prospective_t pr;
      assert(memory_prospective_create("when the CI token rotates",
                                       "open the runbook and swap the stored value", "ci-runbook",
                                       "", "once", "2026-05-01 12:00:00", "sess-a", &pr) == 0);

      char *c = session_briefing_render_commitments(0);
      assert(c != NULL);
      assert(strstr(c, "# Open Commitments") != NULL);
      assert(strstr(c, "when the CI token rotates") != NULL);
      assert(strstr(c, "open the runbook") != NULL);
      /* valid_until shows up as a suffix. */
      assert(strstr(c, "until 2026-05-01 12:00:00") != NULL);
      free(c);
   }

   /* --- completed reminder does NOT surface (only armed) --- */
   {
      memory_prospective_t pr;
      assert(memory_prospective_create("when X happens", "do Y", "", "", "once", "", "sess-a",
                                       &pr) == 0);
      assert(memory_prospective_complete(pr.id) == 0);

      memory_prospective_t rows[16];
      int armed = memory_prospective_list("armed", rows, 16);
      int completed = memory_prospective_list("completed", rows, 16);
      assert(armed == 1); /* only the first, unrelated reminder */
      assert(completed == 1);

      char *c = session_briefing_render_commitments(0);
      assert(c != NULL);
      /* The completed reminder text must not appear. */
      assert(strstr(c, "when X happens") == NULL);
      free(c);
   }

   /* --- directives render in priority order (highest first) --- */
   {
      memory_directive_t out;
      int rc =
          memory_directive_create("What framework does the frontend use?", "frontend-framework", "",
                                  "", "missing_config", 30, 0, 0, "", "sess-a", "", &out);
      assert(rc == 0);
      rc = memory_directive_create("Is the nightly cron still expected to run?", "cron-status", "",
                                   "", "retrieval_failure", 80, 0, 0, "", "sess-a", "", &out);
      assert(rc == 0);
      rc =
          memory_directive_create("Which branch is canonical after the rename?", "canonical-branch",
                                  "", "", "contradiction", 55, 0, 0, "", "sess-a", "", &out);
      assert(rc == 0);

      char *q = session_briefing_render_directives(0);
      assert(q != NULL);
      assert(strstr(q, "# Open Questions") != NULL);
      /* Priority-ordered: p80 (retrieval_failure) before p55 before p30. */
      char *hi = strstr(q, "Is the nightly cron");
      char *mid = strstr(q, "Which branch is canonical");
      char *lo = strstr(q, "What framework does the frontend");
      assert(hi && mid && lo);
      assert(hi < mid);
      assert(mid < lo);
      /* Cause tag surfaces so operators can spot contradictions. */
      assert(strstr(q, "contradiction") != NULL);
      assert(strstr(q, "retrieval_failure") != NULL);
      free(q);
   }

   /* --- resolved directives do NOT surface --- */
   {
      memory_directive_t out;
      int rc = memory_directive_create("Should the payment test fixture rotate weekly?",
                                       "rotation-question", "", "", "missing_config", 10, 0, 0, "",
                                       "sess-a", "", &out);
      assert(rc == 0);
      assert(memory_directive_resolve(out.id, 0, "answered in slack") == 0);

      char *q = session_briefing_render_directives(0);
      assert(q != NULL);
      assert(strstr(q, "Should the payment test fixture rotate weekly?") == NULL);
      free(q);
   }

   /* --- limit knob caps the output --- */
   {
      /* Already have 3 open directives from the priority test; cap to 1. */
      char *q = session_briefing_render_directives(1);
      assert(q != NULL);
      int dash_count = 0;
      for (const char *p = q; *p; p++)
         if (*p == '\n' && p > q && p[-1] != '\n' && p[1] == '-')
            dash_count++;
      /* Only one list-item line. */
      int list_items = 0;
      for (const char *line = q; *line;)
      {
         const char *nl = strchr(line, '\n');
         if (!nl)
            break;
         if (line[0] == '-' && line[1] == ' ')
            list_items++;
         line = nl + 1;
      }
      assert(list_items == 1);
      (void)dash_count;
      free(q);
   }

   db2_test_shim_close();
   printf("all tests passed\n");
   return 0;
}
