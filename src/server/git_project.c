/* git_project.c — clone a repo into a webuser's scoped workspace. See header. */
#include "git_project.h"
#include "git_cred_inject.h" /* git_cred_inject_build_env / _free_env */
#include "util.h"            /* safe_exec_capture_env */
#include "workspace_scope.h" /* ws_scope_project_path, ws_scope_name_valid */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

int git_project_clone(const char *principal, const char *url, const char *name, char *out_path,
                      size_t path_cap, char *out_name, size_t name_cap, char *err, size_t errlen)
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

   /* Inject the user's vaulted git credentials into the child env only (NULL =
    * no vaulted token -> git uses ambient creds for a public/file:// clone). */
   char **envp = git_cred_inject_build_env(principal, environ);
   const char *argv[] = {"git", "clone", "--", url, dest, NULL};
   char *out = NULL;
   int rc = safe_exec_capture_env(argv, envp ? envp : environ, &out, 1 << 16);
   if (envp)
      git_cred_inject_free_env(envp);

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
