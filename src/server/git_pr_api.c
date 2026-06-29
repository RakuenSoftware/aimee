/* git_pr_api.c — open a GitHub PR via the REST API, in-process. See git_pr_api.h. */
#define _GNU_SOURCE 1
#include "git_pr_api.h"

#include "aimee.h"           /* MAX_PATH_LEN (needed by agent_types.h) */
#include "agent_exec.h"      /* agent_http_post_content_type */
#include "cJSON.h"           /* request/response JSON */
#include "git_cred_inject.h" /* git_cred_inject_resolve_token — the ONE cred policy */
#include "util.h"            /* run_cmd */

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PR_TOKEN_MAX 4096

static void wipe(void *p, size_t n)
{
   volatile unsigned char *v = (volatile unsigned char *)p;
   while (n--)
      *v++ = 0;
}

/* Run `git -C <repo_dir> <args>` and capture the trimmed first line of stdout.
 * Returns 0 + out on success (non-empty, rc==0). repo_dir is a server-resolved
 * workspace path; a stray single quote is rejected (no shell injection). */
static int git_cap(const char *repo_dir, const char *args, char *out, size_t cap)
{
   if (out && cap)
      out[0] = '\0';
   if (!repo_dir || strchr(repo_dir, '\''))
      return -1;
   char cmd[1024];
   if ((size_t)snprintf(cmd, sizeof(cmd), "git -C '%s' %s 2>/dev/null", repo_dir, args) >=
       sizeof(cmd))
      return -1;
   int rc = 0;
   char *r = run_cmd(cmd, &rc);
   if (!r)
      return -1;
   char *nl = strchr(r, '\n');
   if (nl)
      *nl = '\0';
   size_t n = strlen(r);
   while (n && (r[n - 1] == ' ' || r[n - 1] == '\r' || r[n - 1] == '\t'))
      r[--n] = '\0';
   snprintf(out, cap, "%s", r);
   free(r);
   return (rc == 0 && out[0]) ? 0 : -1;
}

/* The host (case-insensitive, between [s, e)) is exactly github.com / www.github.com. */
static int host_is_github(const char *s, const char *e)
{
   size_t n = (size_t)(e - s);
   char h[64];
   if (n == 0 || n >= sizeof(h))
      return 0;
   for (size_t i = 0; i < n; i++)
      h[i] = (char)tolower((unsigned char)s[i]);
   h[n] = '\0';
   return strcmp(h, "github.com") == 0 || strcmp(h, "www.github.com") == 0;
}

/* owner/repo are interpolated into the API URL: a GitHub name starts
 * alphanumeric and is otherwise [A-Za-z0-9._-]. */
static int gh_name_ok(const char *s, size_t n)
{
   if (n == 0 || !isalnum((unsigned char)s[0]))
      return 0;
   for (size_t i = 0; i < n; i++)
      if (!isalnum((unsigned char)s[i]) && s[i] != '-' && s[i] != '_' && s[i] != '.')
         return 0;
   return 1;
}

/* Parse github.com owner/repo from an origin URL by EXACT host match (no
 * substring trickery like evilgithub.com / github.com.evil.com). Handles
 * scheme://[user@]host[:port]/owner/repo[.git] (https/http/ssh/git) and the SCP
 * form [user@]host:owner/repo[.git]. github.com only. */
static int parse_github_slug(const char *url, char *owner, size_t ocap, char *repo, size_t rcap)
{
   const char *host, *path = NULL;
   const char *scheme = strstr(url, "://");
   if (scheme)
   {
      host = scheme + 3;
      const char *slash = strchr(host, '/');
      const char *at = strchr(host, '@');
      if (at && (!slash || at < slash)) /* skip user[:pass]@ */
         host = at + 1;
      const char *hend = host;
      while (*hend && *hend != '/' && *hend != ':')
         hend++;
      if (!host_is_github(host, hend))
         return -1;
      const char *q = hend;
      if (*q == ':') /* skip :port */
         while (*q && *q != '/')
            q++;
      if (*q != '/')
         return -1;
      path = q + 1;
   }
   else /* SCP form: [user@]host:owner/repo */
   {
      const char *colon = strchr(url, ':');
      if (!colon)
         return -1;
      const char *at = strchr(url, '@');
      host = (at && at < colon) ? at + 1 : url;
      if (!host_is_github(host, colon))
         return -1;
      path = colon + 1;
   }

   while (*path == '/') /* tolerate a leading slash in the path */
      path++;
   const char *slash = strchr(path, '/');
   if (!slash)
      return -1;
   size_t ol = (size_t)(slash - path);
   const char *r = slash + 1;
   size_t rl = strlen(r);
   if (rl > 4 && strcmp(r + rl - 4, ".git") == 0) /* strip trailing .git */
      rl -= 4;
   if (ol >= ocap || rl >= rcap || !gh_name_ok(path, ol) || !gh_name_ok(r, rl))
      return -1;
   memcpy(owner, path, ol);
   owner[ol] = '\0';
   memcpy(repo, r, rl);
   repo[rl] = '\0';
   return 0;
}

int git_pr_create_via_api(const char *principal, const char *repo_dir, const char *title,
                          const char *body, char *out, size_t out_cap, char *err, size_t errlen)
{
   if (out && out_cap)
      out[0] = '\0';
   if (err && errlen)
      err[0] = '\0';

   char origin[1024];
   if (git_cap(repo_dir, "config --get remote.origin.url", origin, sizeof(origin)) != 0)
   {
      snprintf(err, errlen, "no origin remote");
      return -1;
   }
   char owner[128], repo[128];
   if (parse_github_slug(origin, owner, sizeof(owner), repo, sizeof(repo)) != 0)
   {
      snprintf(err, errlen, "open-PR requires a github.com origin");
      return -1;
   }
   char head[256];
   if (git_cap(repo_dir, "rev-parse --abbrev-ref HEAD", head, sizeof(head)) != 0 ||
       strcmp(head, "HEAD") == 0)
   {
      snprintf(err, errlen, "not on a branch");
      return -1;
   }
   char base[256];
   if (git_cap(repo_dir, "rev-parse --abbrev-ref origin/HEAD", base, sizeof(base)) != 0)
      snprintf(base, sizeof(base), "main"); /* no origin/HEAD ref → assume main */
   else
   {
      char *sl = strchr(base, '/'); /* "origin/main" → "main" */
      if (sl)
         memmove(base, sl + 1, strlen(sl + 1) + 1);
   }
   char tbuf[512];
   if (!title || !title[0]) /* default the title to the last commit subject */
   {
      if (git_cap(repo_dir, "log -1 --format=%s", tbuf, sizeof(tbuf)) == 0 && tbuf[0])
         title = tbuf;
      else
         title = head;
   }

   /* Resolve the token through the ONE credential policy (same precedence as the
    * git exec path) so the ladder never drifts; we use the raw token here only
    * because it goes into an Authorization header, not an exec env. */
   char token[PR_TOKEN_MAX] = {0};
   int have =
       (git_cred_inject_resolve_token(principal, NULL, repo_dir, NULL, token, sizeof(token)) == 1 &&
        token[0]);
   if (!have)
   {
      wipe(token, sizeof(token));
      snprintf(err, errlen, "no github credential — connect one in the Git panel");
      return -1;
   }

   cJSON *j = cJSON_CreateObject();
   cJSON_AddStringToObject(j, "title", title);
   cJSON_AddStringToObject(j, "head", head);
   cJSON_AddStringToObject(j, "base", base);
   cJSON_AddStringToObject(j, "body", body ? body : "");
   char *jbody = cJSON_PrintUnformatted(j);
   cJSON_Delete(j);
   if (!jbody)
   {
      wipe(token, sizeof(token));
      snprintf(err, errlen, "internal error");
      return -1;
   }

   char url[512];
   snprintf(url, sizeof(url), "https://api.github.com/repos/%s/%s/pulls", owner, repo);
   char auth[PR_TOKEN_MAX + 32];
   snprintf(auth, sizeof(auth), "Authorization: Bearer %s", token);

   char *resp = NULL;
   int status = agent_http_post_content_type(url, auth, "application/json", jbody, &resp, 20000,
                                             "Accept: application/vnd.github+json");
   /* The token only ever lived in aimee-server's memory; wipe both copies now. */
   wipe(auth, sizeof(auth));
   wipe(token, sizeof(token));
   free(jbody);

   if (status < 200 || status >= 300 || !resp)
   {
      /* Surface GitHub's own error message when present. */
      const char *msg = NULL;
      cJSON *je = resp ? cJSON_Parse(resp) : NULL;
      cJSON *m = je ? cJSON_GetObjectItem(je, "message") : NULL;
      if (cJSON_IsString(m) && m->valuestring)
         msg = m->valuestring;
      snprintf(err, errlen, "github API: %s", msg ? msg : "PR not created");
      cJSON_Delete(je);
      free(resp);
      return -1;
   }

   cJSON *jr = cJSON_Parse(resp);
   int ok = -1;
   cJSON *hu = jr ? cJSON_GetObjectItem(jr, "html_url") : NULL;
   if (cJSON_IsString(hu) && hu->valuestring)
   {
      snprintf(out, out_cap, "%s", hu->valuestring);
      ok = 0;
   }
   else
   {
      snprintf(err, errlen, "github API: unexpected response");
   }
   cJSON_Delete(jr);
   free(resp);
   return ok;
}
