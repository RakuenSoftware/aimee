/* Unit test for the spill producer (PR-B): write an envelope and read it back. */

#include "cJSON.h"
#include "harness_memory_spill.h"

#include <assert.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "platform_test_util.h" /* platform_tmpdir: honour TMPDIR, do not leak into /tmp */

int main(void)
{
   char tmpl[256];
   snprintf(tmpl, sizeof tmpl, "%s/hmem_spill_test_XXXXXX", platform_tmpdir());
   char *home = mkdtemp(tmpl);
   assert(home);
   setenv("AIMEE_HOME", home, 1);

   char dir[4096];
   assert(hmem_spill_dir("proj/x", dir, sizeof(dir)) == 0);

   assert(hmem_spill_write("proj/x", "topics/auth", "fact", "hello body") == 0);

   /* find the .json spill (skip the .spill_ temp + . / ..) */
   DIR *d = opendir(dir);
   assert(d);
   char found[4096] = "";
   struct dirent *e;
   while ((e = readdir(d)))
   {
      if (e->d_name[0] == '.')
         continue;
      size_t l = strlen(e->d_name);
      if (l > 5 && strcmp(e->d_name + l - 5, ".json") == 0)
      {
         snprintf(found, sizeof(found), "%s/%s", dir, e->d_name);
         break;
      }
   }
   closedir(d);
   assert(found[0]);

   FILE *f = fopen(found, "rb");
   assert(f);
   char buf[4096];
   size_t n = fread(buf, 1, sizeof(buf) - 1, f);
   buf[n] = '\0';
   fclose(f);

   cJSON *o = cJSON_Parse(buf);
   assert(o);
   assert(strcmp(cJSON_GetObjectItem(o, "op")->valuestring, "upsert") == 0);
   assert(strcmp(cJSON_GetObjectItem(o, "project")->valuestring, "proj/x") == 0);
   assert(strcmp(cJSON_GetObjectItem(o, "name")->valuestring, "topics/auth") == 0);
   assert(strcmp(cJSON_GetObjectItem(o, "type")->valuestring, "fact") == 0);
   assert(strcmp(cJSON_GetObjectItem(o, "body")->valuestring, "hello body") == 0);
   cJSON_Delete(o);

   printf("test_harness_memory_spill: OK\n");
   return 0;
}
