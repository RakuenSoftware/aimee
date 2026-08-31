/* workspace_client_diff.c: the client's mirror base + working-tree patch.
 * See include/aimee/workspace/client_diff.h. */
#include <aimee/workspace/client_diff.h>

#include "util.h" /* safe_exec_capture, safe_exec_capture_env */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* A commit id as git prints it. */
#define GIT_OID_HEX 40

static int is_hex_oid(const char *s, size_t n)
{
   if (n != GIT_OID_HEX)
      return 0;
   for (size_t i = 0; i < n; i++)
   {
      char c = s[i];
      if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
         return 0;
   }
   return 1;
}

int workspace_client_mirror_base_select(const char *revlist_out, const char *head, char *out,
                                        size_t out_cap)
{
   if (!out || out_cap <= GIT_OID_HEX)
      return -1;
   out[0] = '\0';

   /* Scan for a boundary line ("-<sha>"): the newest ancestor of HEAD that a
    * remote already has. Boundary lines can appear anywhere in the output, so
    * the whole thing is scanned rather than just the tail. */
   int saw_line = 0;
   for (const char *p = revlist_out ? revlist_out : ""; *p;)
   {
      const char *nl = strchr(p, '\n');
      size_t len = nl ? (size_t)(nl - p) : strlen(p);
      /* Tolerate CRLF and stray padding. */
      while (len > 0 && (p[len - 1] == '\r' || p[len - 1] == ' ' || p[len - 1] == '\t'))
         len--;
      if (len > 0)
      {
         if (p[0] == '-' && is_hex_oid(p + 1, len - 1))
         {
            memcpy(out, p + 1, GIT_OID_HEX);
            out[GIT_OID_HEX] = '\0';
            return 0;
         }
         saw_line = 1;
      }
      if (!nl)
         break;
      p = nl + 1;
   }

   /* Output we could not resolve to a boundary. Either unpushed commits with
    * nothing beneath them on any remote, or something we do not understand --
    * and the two are indistinguishable from here. Both mean we cannot name a
    * commit the server is able to fetch, so refuse. Falling back to HEAD would
    * register exactly the unfetchable commit this function exists to avoid. */
   if (saw_line)
      return -1;

   /* Nothing unpushed at all — HEAD itself is on a remote. */
   if (!head || !is_hex_oid(head, strlen(head)))
      return -1;
   memcpy(out, head, GIT_OID_HEX);
   out[GIT_OID_HEX] = '\0';
   return 0;
}

#if !defined(_WIN32) && !defined(_WIN64)
extern char **environ;

/* Trim one captured git line in place. */
static void chomp(char *s)
{
   size_t n = strlen(s);
   while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r' || s[n - 1] == ' ' || s[n - 1] == '\t'))
      s[--n] = '\0';
}

int workspace_client_mirror_base(const char *root, char *out, size_t out_cap)
{
   if (!root || !root[0] || !out || out_cap <= GIT_OID_HEX)
      return -1;
   out[0] = '\0';

   char *head_buf = NULL;
   const char *rp[] = {"git", "-C", root, "rev-parse", "HEAD", NULL};
   if (safe_exec_capture(rp, &head_buf, 128) != 0 || !head_buf)
   {
      free(head_buf);
      return -1; /* not a repo, or no commit yet */
   }
   chomp(head_buf);

   /* Every ancestor of HEAD that no remote has, with the boundary beneath them.
    * Generous cap: a long-lived unpushed branch is ~41 bytes per commit, and
    * truncating would hide the boundary and silently pick the wrong base. */
   char *rl = NULL;
   const char *bl[] = {"git",  "-C",    root,        "rev-list", "--boundary",
                       "HEAD", "--not", "--remotes", NULL};
   int rc = safe_exec_capture(bl, &rl, 16 * 1024 * 1024);
   if (rc != 0)
   {
      /* A repository with no remotes at all makes this fail on some git
       * versions; either way we cannot prove a fetchable ancestor. */
      free(head_buf);
      free(rl);
      return -1;
   }
   int sel = workspace_client_mirror_base_select(rl ? rl : "", head_buf, out, out_cap);
   free(head_buf);
   free(rl);
   return sel;
}

char *workspace_client_diff_compute(const char *root, const char *base)
{
   if (!root || !root[0] || !base || !base[0])
      return NULL;
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
   const char *rt[] = {"git", "-C", root, "read-tree", base, NULL};
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
      const char *df[] = {"git", "-C", root, "diff", "--cached", "--binary", base, NULL};
      safe_exec_capture_env(df, envp, &patch, 16 * 1024 * 1024);
   }
   free(envp);
   unlink(idx);
   return patch; /* may be "" (tree matches base exactly) */
}
#else
int workspace_client_mirror_base(const char *root, char *out, size_t out_cap)
{
   (void)root;
   if (out && out_cap)
      out[0] = '\0';
   return -1;
}

char *workspace_client_diff_compute(const char *root, const char *base)
{
   (void)root;
   (void)base;
   return NULL;
}
#endif
