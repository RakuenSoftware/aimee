/* Unit tests for the per-client memory-surface registry (PR-A) and its
 * config-file override (PR-E). */

#include "harness_memory_scope.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "platform_test_util.h" /* platform_tmpdir: honour TMPDIR, do not leak into /tmp */

int main(void)
{
   char c[64], r[4096], g[128];

   /* --- pure line parser --- */
   assert(hmem_scope_parse_line("gemini:.gemini/projects:memory", c, sizeof(c), r, sizeof(r), g,
                                sizeof(g)) == 0);
   assert(!strcmp(c, "gemini") && !strcmp(r, ".gemini/projects") && !strcmp(g, "memory"));

   /* surrounding whitespace on each field is trimmed */
   assert(hmem_scope_parse_line("  codex : .codex/p : notes ", c, sizeof(c), r, sizeof(r), g,
                                sizeof(g)) == 0);
   assert(!strcmp(c, "codex") && !strcmp(r, ".codex/p") && !strcmp(g, "notes"));

   /* blank and comment lines are skipped (1) */
   assert(hmem_scope_parse_line("", c, sizeof(c), r, sizeof(r), g, sizeof(g)) == 1);
   assert(hmem_scope_parse_line("   \t ", c, sizeof(c), r, sizeof(r), g, sizeof(g)) == 1);
   assert(hmem_scope_parse_line("# a comment", c, sizeof(c), r, sizeof(r), g, sizeof(g)) == 1);

   /* malformed: too few fields */
   assert(hmem_scope_parse_line("only:two", c, sizeof(c), r, sizeof(r), g, sizeof(g)) == -1);
   assert(hmem_scope_parse_line("noseparators", c, sizeof(c), r, sizeof(r), g, sizeof(g)) == -1);

   /* empty client or path field rejected */
   assert(hmem_scope_parse_line(":root:seg", c, sizeof(c), r, sizeof(r), g, sizeof(g)) == -1);
   assert(hmem_scope_parse_line("cl::seg", c, sizeof(c), r, sizeof(r), g, sizeof(g)) == -1);
   assert(hmem_scope_parse_line("cl:root:", c, sizeof(c), r, sizeof(r), g, sizeof(g)) == -1);

   /* path safety: a path field may not be absolute or climb out of $HOME */
   assert(hmem_scope_parse_line("cl:/abs:seg", c, sizeof(c), r, sizeof(r), g, sizeof(g)) == -1);
   assert(hmem_scope_parse_line("cl:../x:seg", c, sizeof(c), r, sizeof(r), g, sizeof(g)) == -1);
   assert(hmem_scope_parse_line("cl:root:../s", c, sizeof(c), r, sizeof(r), g, sizeof(g)) == -1);
   assert(hmem_scope_parse_line("cl:a/../b:seg", c, sizeof(c), r, sizeof(r), g, sizeof(g)) == -1);
   assert(hmem_scope_parse_line("cl:./x:seg", c, sizeof(c), r, sizeof(r), g, sizeof(g)) == -1);

   /* a Windows UNC/root path (leading backslash) is rejected. NB: drive paths
    * like "C:\..." can't even be expressed here — ':' is the field separator. */
   assert(hmem_scope_parse_line("cl:\\\\server\\share:seg", c, sizeof(c), r, sizeof(r), g,
                                sizeof(g)) == -1);
   assert(hmem_scope_parse_line("cl:\\rooted:seg", c, sizeof(c), r, sizeof(r), g, sizeof(g)) == -1);

   /* empty path segments (consecutive or trailing separators) are rejected */
   assert(hmem_scope_parse_line("cl:a//b:seg", c, sizeof(c), r, sizeof(r), g, sizeof(g)) == -1);
   assert(hmem_scope_parse_line("cl:a/:seg", c, sizeof(c), r, sizeof(r), g, sizeof(g)) == -1);

   /* a mid-token ".." (not a path component) is allowed */
   assert(hmem_scope_parse_line("cl:my..backup:seg", c, sizeof(c), r, sizeof(r), g, sizeof(g)) ==
          0);
   assert(!strcmp(r, "my..backup"));

   /* the client id must be a bare token (no '/', '\\', or "..") */
   assert(hmem_scope_parse_line("a/b:root:seg", c, sizeof(c), r, sizeof(r), g, sizeof(g)) == -1);
   assert(hmem_scope_parse_line("a\\b:root:seg", c, sizeof(c), r, sizeof(r), g, sizeof(g)) == -1);

   /* a field that overflows its buffer is rejected, never truncated */
   {
      char tiny[3];
      assert(hmem_scope_parse_line("gemini:.g:m", tiny, sizeof(tiny), r, sizeof(r), g, sizeof(g)) ==
             -1);
   }

   /* --- live registry merged with a config override file --- */
   char tmpl[256];
   snprintf(tmpl, sizeof tmpl, "%s/hmem_scopes_XXXXXX", platform_tmpdir());
   int fd = mkstemp(tmpl);
   assert(fd >= 0);
   const char *cfg = "# test scopes\n"
                     "gemini:.gemini/projects:memory\n"
                     "claude:.claude/projects:memory2\n"; /* overrides the claude built-in seg */
   assert(write(fd, cfg, strlen(cfg)) == (ssize_t)strlen(cfg));
   close(fd);
   setenv("AIMEE_HARNESS_MEMORY_SCOPES", tmpl, 1);

   /* the built-in claude row exists and its seg was overridden by the config */
   const hmem_scope_t *s = hmem_scope_for_client("claude");
   assert(s && !strcmp(s->client, "claude"));
   assert(!strcmp(s->projects_root, ".claude/projects"));
   assert(!strcmp(s->memory_seg, "memory2"));

   /* a config-only client is registered */
   const hmem_scope_t *gm = hmem_scope_for_client("gemini");
   assert(gm && !strcmp(gm->projects_root, ".gemini/projects") &&
          !strcmp(gm->memory_seg, "memory"));

   /* a missing/empty client never silently defaults; unknown stays unknown */
   assert(hmem_scope_for_client(NULL) == NULL);
   assert(hmem_scope_for_client("") == NULL);
   assert(hmem_scope_for_client("nope") == NULL);

   unlink(tmpl);
   printf("test_harness_memory_scope: OK\n");
   return 0;
}
