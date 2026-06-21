/* git_project.c — clone a repo into a webuser's scoped workspace. See header. */
#include "git_project.h"
#include "git_cred_inject.h" /* git_cred_inject_build_env_for_repo / _free_env */
#include "git_host_cred.h"   /* per-host token store (single-user, many hosts) */
#include "util.h"            /* safe_exec_capture_env */
#include "util_url.h"        /* util_url_normalize (ssh/scp/git -> https) */
#include "workspace_scope.h" /* ws_scope_project_path, ws_scope_name_valid */

#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

extern char **environ;

#define GP_PATH_MAX 4096

/* Derive a valid single-component project name from `name_in` (if non-empty) or
 * the basename of `url` (stripping a trailing ".git" and any trailing '/').
 * Returns 0 + out, or -1 if no valid name can be formed. */
static int derive_name(const char *url, const char *name_in, char *out, size_t cap)
{
   if (name_in && name_in[0])
   {
      if (!ws_scope_name_valid(name_in) || strlen(name_in) >= cap)
         return -1;
      snprintf(out, cap, "%s", name_in);
      return 0;
   }
   if (!url || !url[0])
      return -1;
   /* basename: last segment after '/' or ':'. */
   const char *base = url + strlen(url);
   while (base > url && base[-1] == '/') /* ignore trailing slashes */
      base--;
   const char *end = base;
   while (base > url && base[-1] != '/' && base[-1] != ':')
      base--;
   size_t len = (size_t)(end - base);
   if (len == 0 || len >= cap)
      return -1;
   char tmp[256];
   if (len >= sizeof(tmp))
      return -1;
   memcpy(tmp, base, len);
   tmp[len] = '\0';
   /* strip a trailing ".git". */
   size_t tl = strlen(tmp);
   if (tl > 4 && strcmp(tmp + tl - 4, ".git") == 0)
      tmp[tl - 4] = '\0';
   if (!ws_scope_name_valid(tmp) || strlen(tmp) >= cap)
      return -1;
   snprintf(out, cap, "%s", tmp);
   return 0;
}

int git_project_clone(const char *principal, const char *url, const char *name, const char *token,
                      char *out_path, size_t path_cap, char *out_name, size_t name_cap, char *err,
                      size_t errlen)
{
   if (err && errlen)
      err[0] = '\0';
   if (out_path && path_cap)
      out_path[0] = '\0';
   if (out_name && name_cap)
      out_name[0] = '\0';

   if (!principal || strncmp(principal, "webuser:", 8) != 0)
   {
      snprintf(err, errlen, "git projects require a webchat user");
      return -1;
   }
   /* Reject an empty or flag-like URL. The '--' separator below also stops git
    * from treating the URL as an option, but reject the obvious case early. */
   if (!url || !url[0] || url[0] == '-')
   {
      snprintf(err, errlen, "invalid repository URL");
      return -1;
   }
   /* No control chars / whitespace in the URL (defensive; argv already avoids a
    * shell, but keep the value clean for logs and git). */
   for (const char *p = url; *p; p++)
      if ((unsigned char)*p < 0x20 || *p == ' ')
      {
         snprintf(err, errlen, "invalid repository URL");
         return -1;
      }

   char pname[128];
   if (derive_name(url, name, pname, sizeof(pname)) != 0)
   {
      snprintf(err, errlen, "could not derive a valid project name");
      return -1;
   }

   char dest[GP_PATH_MAX];
   if (ws_scope_project_path(principal, pname, 0, dest, sizeof(dest)) != 0)
   {
      snprintf(err, errlen, "project already exists or invalid name");
      return -1;
   }

   /* Resolve the git credential vault-first for THIS repo's host (single-user
    * server, many hosts/providers), via the one shared policy. Precedence: an
    * inline token from the request (then stored for the host) > the host's
    * already-stored vault token > the principal's own vaulted token > the
    * server's own identity (App token / AIMEE_FORGE_TOKEN); plus the principal's
    * vaulted SSH agent. The token crosses only via GIT_ASKPASS, never argv. */
   char **envp = git_cred_inject_build_env_for_repo(principal, url, NULL, token, environ);
   /* Clone over HTTPS, never SSH: our credential model is per-host HTTPS tokens
    * (+ OAuth), and a non-interactive server has no seeded known_hosts, so an
    * ssh://, git@host:path, or git:// URL would die on "Host key verification
    * failed". Normalize any such form to https://host/path; fall back to the raw
    * URL only for forms util_url_normalize can't parse (e.g. a local file path). */
   char *clone_url_norm = util_url_normalize(url);
   const char *clone_url = clone_url_norm ? clone_url_norm : url;
   const char *argv[] = {"git", "clone", "--", clone_url, dest, NULL};
   char *out = NULL;
   int rc = safe_exec_capture_env(argv, envp ? envp : environ, &out, 1 << 16);
   free(clone_url_norm);
   if (envp)
      git_cred_inject_free_env(envp);

   /* A working inline token is worth keeping: persist it for the host so future
    * pulls/pushes on any repo there reuse it (no re-entry). */
   if (rc == 0 && token && token[0])
   {
      char host[GIT_HOST_MAX];
      if (git_host_from_url(url, host, sizeof(host)))
         git_host_cred_set(host, token);
   }

   if (rc != 0)
   {
      /* Surface a short, non-sensitive tail of git's stderr (no creds are ever
       * in it — they cross via GIT_ASKPASS, not the command line/output). */
      snprintf(err, errlen, "git clone failed (rc=%d)%s%.200s", rc, out && out[0] ? ": " : "",
               out ? out : "");
      free(out);
      return -1;
   }
   free(out);

   if (out_path && path_cap)
      snprintf(out_path, path_cap, "%s", dest);
   if (out_name && name_cap)
      snprintf(out_name, name_cap, "%s", pname);
   return 0;
}

int git_project_list(const char *principal, char out[][GIT_PROJECT_NAME_MAX], int max)
{
   if (!principal || strncmp(principal, "webuser:", 8) != 0)
      return -1;
   /* Open the scope root via the O_NOFOLLOW base fd (TOCTOU-safe); a missing
    * root just means no projects yet. */
   int dfd = ws_scope_open_user_root(principal);
   if (dfd < 0)
      return 0;
   DIR *d = fdopendir(dfd);
   if (!d)
   {
      close(dfd);
      return 0;
   }
   int n = 0;
   struct dirent *e;
   while (n < max && (e = readdir(d)) != NULL)
   {
      if (e->d_name[0] == '.')
         continue; /* skip . / .. / hidden */
      if (strlen(e->d_name) >= GIT_PROJECT_NAME_MAX)
         continue;
      /* directories only (project dirs); openat avoids re-resolving a path */
      struct stat st;
      if (fstatat(dfd, e->d_name, &st, AT_SYMLINK_NOFOLLOW) != 0 || !S_ISDIR(st.st_mode))
         continue;
      snprintf(out[n], GIT_PROJECT_NAME_MAX, "%s", e->d_name);
      n++;
   }
   closedir(d); /* closes dfd */
   return n;
}
