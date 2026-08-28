#include "artifact_trust.h"

#include <assert.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "platform_test_util.h"

static char g_home[512];

const char *aimee_home(void)
{
   return g_home;
}

int main(void)
{
   snprintf(g_home, sizeof(g_home), "%s/aimee-artifact-trust-XXXXXX", platform_tmpdir());
   assert(mkdtemp(g_home) != NULL);
   unsetenv("AIMEE_ARTIFACT_TRUST_MODE");

   char digest[65], err[256];
   const char *identity = "config:mcp/fixture";
   assert(artifact_trust_verify_bytes("plugin", "fixture", identity, "argv=node\n", 10, digest, err,
                                      sizeof(err)) == 0);
   assert(strlen(digest) == 64);
   assert(artifact_trust_verify_bytes("plugin", "fixture", identity, "argv=node\n", 10, NULL, err,
                                      sizeof(err)) == 0);
   assert(artifact_trust_verify_bytes("plugin", "fixture", identity, "argv=evil\n", 10, NULL, err,
                                      sizeof(err)) == -1);
   assert(strstr(err, "unapproved or changed") != NULL);

   setenv("AIMEE_ARTIFACT_TRUST_MODE", "hardened", 1);
   assert(artifact_trust_verify_bytes("plugin", "other", "config:mcp/other", "argv=node\n", 10,
                                      NULL, err, sizeof(err)) == -1);
   unsetenv("AIMEE_ARTIFACT_TRUST_MODE");

   char dir[1024];
   snprintf(dir, sizeof(dir), "%s/artifact-trust", g_home);
   /* One standard-mode identity produced exactly one private digest pin. */
   struct stat st;
   assert(stat(dir, &st) == 0 && S_ISDIR(st.st_mode) && !(st.st_mode & 0077));
   DIR *d = opendir(dir);
   assert(d != NULL);
   int pins = 0;
   struct dirent *entry;
   while ((entry = readdir(d)) != NULL)
   {
      if (entry->d_name[0] == '.')
         continue;
      char path[1200];
      snprintf(path, sizeof(path), "%s/%s", dir, entry->d_name);
      assert(unlink(path) == 0);
      pins++;
   }
   closedir(d);
   assert(pins == 1);
   assert(rmdir(dir) == 0);
   assert(rmdir(g_home) == 0);

   puts("artifact_trust: all tests passed");
   return 0;
}
