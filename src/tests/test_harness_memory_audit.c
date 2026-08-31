/* Unit test for the interception audit log (PR-B). */

#include "harness_memory_audit.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "platform_test_util.h" /* platform_tmpdir: honour TMPDIR, do not leak into /tmp */

int main(void)
{
   char tmpl[256];
   snprintf(tmpl, sizeof tmpl, "%s/hmem_audit_test_XXXXXX", platform_tmpdir());
   char *home = mkdtemp(tmpl);
   assert(home);
   setenv("AIMEE_HOME", home, 1);

   hmem_audit("redirect", "proj/x", "topics/auth", "saved");
   hmem_audit("reject", NULL, NULL, "MEMORY.md");

   char path[4096];
   snprintf(path, sizeof(path), "%s/logs/interception.jsonl", home);
   FILE *f = fopen(path, "rb");
   assert(f);
   char buf[8192];
   size_t n = fread(buf, 1, sizeof(buf) - 1, f);
   buf[n] = '\0';
   fclose(f);

   assert(strstr(buf, "\"action\":\"redirect\""));
   assert(strstr(buf, "\"name\":\"topics/auth\""));
   assert(strstr(buf, "\"action\":\"reject\""));
   /* two records => two newlines */
   int lines = 0;
   for (const char *p = buf; *p; p++)
      if (*p == '\n')
         lines++;
   assert(lines == 2);

   printf("test_harness_memory_audit: OK\n");
   return 0;
}
