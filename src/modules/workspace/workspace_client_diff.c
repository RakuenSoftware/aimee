/* workspace_client_diff.c: the client's working-tree patch for the mirror tier.
 * See include/aimee/workspace/client_diff.h. Moved here verbatim from the
 * `workspace mirror-sync` marshaller so the reverse channel can ship the same
 * patch on attach without computing it a second, divergent way. */
#include <aimee/workspace/client_diff.h>

#include "util.h" /* safe_exec_capture_env */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#if !defined(_WIN32) && !defined(_WIN64)
extern char **environ;
char *workspace_client_diff_compute(const char *root)
{
   char idx[] = "/tmp/aimee-msync-idx-XXXXXX";
   int fd = mkstemp(idx);
   if (fd < 0)
      return NULL;
   close(fd); /* git read-tree overwrites it */

   int n = 0;
   while (environ[n])
      n++;
   char **envp = calloc((size_t)n + 2, sizeof(char *));
   if (!envp)
   {
      unlink(idx);
      return NULL;
   }
   char giv[300];
   snprintf(giv, sizeof(giv), "GIT_INDEX_FILE=%s", idx);
   for (int i = 0; i < n; i++)
      envp[i] = environ[i];
   envp[n] = giv;
   envp[n + 1] = NULL;

   char *out = NULL;
   const char *rt[] = {"git", "-C", root, "read-tree", "HEAD", NULL};
   int rc = safe_exec_capture_env(rt, envp, &out, 4096);
   free(out);
   out = NULL;
   char *patch = NULL;
   if (rc == 0)
   {
      const char *add[] = {"git", "-C", root, "add", "-A", NULL};
      safe_exec_capture_env(add, envp, &out, 4096);
      free(out);
      out = NULL;
      const char *df[] = {"git", "-C", root, "diff", "--cached", "--binary", "HEAD", NULL};
      safe_exec_capture_env(df, envp, &patch, 16 * 1024 * 1024);
   }
   free(envp);
   unlink(idx);
   return patch; /* may be "" (clean tree) */
}
#else
char *workspace_client_diff_compute(const char *root)
{
   (void)root;
   return NULL;
}
#endif
