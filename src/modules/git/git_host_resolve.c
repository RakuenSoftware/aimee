/* git_host_resolve.c — per-host vault token resolution via a registered seam.
 * See git_host_resolve.h. Depends only on util (local git exec); the actual vault
 * read is reached through the registered git_host_cred_for_url pointer. */
#include "git_host_resolve.h"
#include "util.h" /* safe_exec_capture_cwd_env_timeout */

#include <stdlib.h>
#include <string.h>

extern char **environ;

#define GIT_HOST_RESOLVE_ORIGIN_TIMEOUT_MS 5000

/* Registered per-host vault lookup (git_host_cred_for_url), or NULL when the
 * server has not registered it (thin client / unit tests). */
static int (*g_host_cred_for_url)(const char *url, char *out, size_t out_len);

void git_host_resolve_register(int (*host_cred_for_url)(const char *url, char *out, size_t out_len))
{
   g_host_cred_for_url = host_cred_for_url;
}

/* Resolve the `origin` remote URL of the repo at `dir` into out (a local
 * `git config --get` — no network, no creds). Returns 1 + out, else 0. */
static int origin_url(const char *dir, char *out, size_t cap)
{
   if (out && cap)
      out[0] = '\0';
   if (!dir || !dir[0] || !out || cap == 0)
      return 0;
   const char *argv[] = {"git", "-C", dir, "config", "--get", "remote.origin.url", NULL};
   char *cap_out = NULL;
   int rc = safe_exec_capture_cwd_env_timeout(argv, dir, environ, &cap_out, 4096,
                                              GIT_HOST_RESOLVE_ORIGIN_TIMEOUT_MS);
   if (rc != 0 || !cap_out)
   {
      free(cap_out);
      return 0;
   }
   size_t n = strlen(cap_out);
   while (n > 0 && (cap_out[n - 1] == '\n' || cap_out[n - 1] == '\r' || cap_out[n - 1] == ' ' ||
                    cap_out[n - 1] == '\t'))
      cap_out[--n] = '\0';
   int ok = (n > 0 && (size_t)snprintf(out, cap, "%s", cap_out) < cap);
   free(cap_out);
   if (!ok && out && cap)
      out[0] = '\0';
   return ok ? 1 : 0;
}

int git_host_resolve_token(const char *remote_url, const char *repo_dir, char *out, size_t out_len)
{
   if (out && out_len)
      out[0] = '\0';
   if (!out || out_len == 0 || !g_host_cred_for_url)
      return 0;

   char urlbuf[2048];
   const char *url = (remote_url && remote_url[0]) ? remote_url : NULL;
   if (!url && origin_url(repo_dir, urlbuf, sizeof(urlbuf)) == 1)
      url = urlbuf;
   if (url && g_host_cred_for_url(url, out, out_len) == 1 && out[0])
      return 1;
   out[0] = '\0';
   return 0;
}
