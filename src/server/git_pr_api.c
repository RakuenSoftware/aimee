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

/* ---- shared REST context for the PR ops (create/info/ci/merge) ---- */

typedef struct
{
   char owner[128];
   char repo[128];
   char token[PR_TOKEN_MAX];
} gh_ctx_t;

/* Resolve repo_dir's github slug + the credential (vault-first, the ONE policy).
 * The raw token is held ONLY for Authorization headers built in-process; call
 * gh_ctx_done to wipe it before returning. */
static int gh_ctx_resolve(const char *principal, const char *repo_dir, gh_ctx_t *cx, char *err,
                          size_t errlen)
{
   memset(cx, 0, sizeof(*cx));
   char origin[1024];
   if (git_cap(repo_dir, "config --get remote.origin.url", origin, sizeof(origin)) != 0)
   {
      snprintf(err, errlen, "no origin remote");
      return -1;
   }
   if (parse_github_slug(origin, cx->owner, sizeof(cx->owner), cx->repo, sizeof(cx->repo)) != 0)
   {
      snprintf(err, errlen, "requires a github.com origin");
      return -1;
   }
   if (git_cred_inject_resolve_token(principal, NULL, repo_dir, NULL, cx->token,
                                     sizeof(cx->token)) != 1 ||
       !cx->token[0])
   {
      wipe(cx->token, sizeof(cx->token));
      snprintf(err, errlen, "no github credential — connect one in the Git panel");
      return -1;
   }
   return 0;
}

static void gh_ctx_done(gh_ctx_t *cx)
{
   wipe(cx->token, sizeof(cx->token));
}

#define GH_ACCEPT "Accept: application/vnd.github+json"

/* GET api.github.com/repos/<owner>/<repo>/<path>. The Authorization header is
 * assembled and wiped here; the token never leaves this process. Returns the
 * HTTP status (or <0), with *resp the malloc'd body (caller frees). */
static int gh_get(const gh_ctx_t *cx, const char *path, char **resp)
{
   *resp = NULL;
   char url[512];
   if ((size_t)snprintf(url, sizeof(url), "https://api.github.com/repos/%s/%s/%s", cx->owner,
                        cx->repo, path) >= sizeof(url))
      return -1;
   /* agent_http_get carries everything in extra_headers (bounded); a token too
    * long for the line budget fails clean here rather than truncating. */
   char hdrs[480];
   if ((size_t)snprintf(hdrs, sizeof(hdrs), "Authorization: Bearer %s\n" GH_ACCEPT, cx->token) >=
       sizeof(hdrs))
      return -1;
   int st = agent_http_get(url, hdrs, resp, 20000);
   wipe(hdrs, sizeof(hdrs));
   return st;
}

/* PUT with a JSON body; same containment as gh_get. */
static int gh_put(const gh_ctx_t *cx, const char *path, const char *body, char **resp)
{
   *resp = NULL;
   char url[512];
   if ((size_t)snprintf(url, sizeof(url), "https://api.github.com/repos/%s/%s/%s", cx->owner,
                        cx->repo, path) >= sizeof(url))
      return -1;
   char auth[PR_TOKEN_MAX + 32];
   snprintf(auth, sizeof(auth), "Authorization: Bearer %s", cx->token);
   int st = agent_http_put(url, auth, body, resp, 20000, GH_ACCEPT);
   wipe(auth, sizeof(auth));
   return st;
}

/* Surface GitHub's own "message" field into err when a call fails. */
static void gh_err(const char *resp, int status, const char *what, char *err, size_t errlen)
{
   const char *msg = NULL;
   cJSON *je = resp ? cJSON_Parse(resp) : NULL;
   cJSON *m = je ? cJSON_GetObjectItem(je, "message") : NULL;
   if (cJSON_IsString(m) && m->valuestring)
      msg = m->valuestring;
   snprintf(err, errlen, "github API (%s, HTTP %d): %s", what, status, msg ? msg : "failed");
   cJSON_Delete(je);
}

int git_pr_create_via_api(const char *principal, const char *repo_dir, const char *title,
                          const char *body, char *out, size_t out_cap, char *err, size_t errlen)
{
   return git_pr_create_via_api_ex(principal, repo_dir, NULL, NULL, title, body, out, out_cap, err,
                                   errlen);
}

int git_pr_create_via_api_ex(const char *principal, const char *repo_dir, const char *head_in,
                             const char *base_in, const char *title, const char *body, char *out,
                             size_t out_cap, char *err, size_t errlen)
{
   if (out && out_cap)
      out[0] = '\0';
   if (err && errlen)
      err[0] = '\0';

   gh_ctx_t cx;
   if (gh_ctx_resolve(principal, repo_dir, &cx, err, errlen) != 0)
      return -1;
   char head[256];
   if (head_in && head_in[0])
      snprintf(head, sizeof(head), "%s", head_in);
   else if (git_cap(repo_dir, "rev-parse --abbrev-ref HEAD", head, sizeof(head)) != 0 ||
            strcmp(head, "HEAD") == 0)
   {
      gh_ctx_done(&cx);
      snprintf(err, errlen, "not on a branch");
      return -1;
   }
   char base[256];
   if (base_in && base_in[0])
      snprintf(base, sizeof(base), "%s", base_in);
   else if (git_cap(repo_dir, "rev-parse --abbrev-ref origin/HEAD", base, sizeof(base)) != 0)
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

   /* Standing directive: no AI co-authorship / "Generated with" attribution in
    * PR bodies — strip a copy before it reaches GitHub. */
   char *bclean = strdup(body ? body : "");
   if (bclean)
      strip_ai_attribution(bclean);

   cJSON *j = cJSON_CreateObject();
   cJSON_AddStringToObject(j, "title", title);
   cJSON_AddStringToObject(j, "head", head);
   cJSON_AddStringToObject(j, "base", base);
   cJSON_AddStringToObject(j, "body", bclean ? bclean : "");
   free(bclean);
   char *jbody = cJSON_PrintUnformatted(j);
   cJSON_Delete(j);
   if (!jbody)
   {
      gh_ctx_done(&cx);
      snprintf(err, errlen, "internal error");
      return -1;
   }

   char url[512];
   snprintf(url, sizeof(url), "https://api.github.com/repos/%s/%s/pulls", cx.owner, cx.repo);
   char auth[PR_TOKEN_MAX + 32];
   snprintf(auth, sizeof(auth), "Authorization: Bearer %s", cx.token);

   char *resp = NULL;
   int status =
       agent_http_post_content_type(url, auth, "application/json", jbody, &resp, 20000, GH_ACCEPT);
   /* The token only ever lived in aimee-server's memory; wipe both copies now. */
   wipe(auth, sizeof(auth));
   gh_ctx_done(&cx);
   free(jbody);

   if (status < 200 || status >= 300 || !resp)
   {
      gh_err(resp, status, "pr create", err, errlen);
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

int git_pr_info_via_api(const char *principal, const char *repo_dir, int number, git_pr_info_t *out,
                        char *err, size_t errlen)
{
   if (err && errlen)
      err[0] = '\0';
   if (!out || number <= 0)
      return -1;
   memset(out, 0, sizeof(*out));
   out->mergeable = -1;

   gh_ctx_t cx;
   if (gh_ctx_resolve(principal, repo_dir, &cx, err, errlen) != 0)
      return -1;
   char path[64];
   snprintf(path, sizeof(path), "pulls/%d", number);
   char *resp = NULL;
   int st = gh_get(&cx, path, &resp);
   gh_ctx_done(&cx);
   if (st < 200 || st >= 300 || !resp)
   {
      gh_err(resp, st, "pr info", err, errlen);
      free(resp);
      return -1;
   }
   cJSON *j = cJSON_Parse(resp);
   free(resp);
   if (!j)
   {
      snprintf(err, errlen, "github API: unparseable pr info");
      return -1;
   }
   const cJSON *state = cJSON_GetObjectItem(j, "state");
   const cJSON *merged = cJSON_GetObjectItem(j, "merged");
   const cJSON *mergeable = cJSON_GetObjectItem(j, "mergeable");
   const cJSON *headj = cJSON_GetObjectItem(j, "head");
   const cJSON *sha = headj ? cJSON_GetObjectItem(headj, "sha") : NULL;
   out->open =
       cJSON_IsString(state) && state->valuestring && strcmp(state->valuestring, "open") == 0;
   out->merged = cJSON_IsTrue(merged) ? 1 : 0;
   if (cJSON_IsBool(mergeable))
      out->mergeable = cJSON_IsTrue(mergeable) ? 1 : 0; /* null stays -1 (computing) */
   if (cJSON_IsString(sha) && sha->valuestring)
      snprintf(out->head_sha, sizeof(out->head_sha), "%s", sha->valuestring);
   cJSON_Delete(j);
   return 0;
}

git_pr_ci_t git_pr_ci_via_api(const char *principal, const char *repo_dir, int number, char *err,
                              size_t errlen)
{
   if (err && errlen)
      err[0] = '\0';
   git_pr_info_t info;
   if (git_pr_info_via_api(principal, repo_dir, number, &info, err, errlen) != 0 ||
       !info.head_sha[0])
      return GIT_PR_CI_ERROR;

   gh_ctx_t cx;
   if (gh_ctx_resolve(principal, repo_dir, &cx, err, errlen) != 0)
      return GIT_PR_CI_ERROR;
   char path[160];
   snprintf(path, sizeof(path), "commits/%s/check-runs?per_page=100", info.head_sha);
   char *runs = NULL;
   int st = gh_get(&cx, path, &runs);
   if (st < 200 || st >= 300)
   {
      gh_ctx_done(&cx);
      gh_err(runs, st, "check-runs", err, errlen);
      free(runs);
      return GIT_PR_CI_ERROR;
   }
   /* Fetch the legacy combined status only when there are no check runs. */
   char *combined = NULL;
   git_pr_ci_t g = git_pr_ci_grade_json(runs, NULL);
   if (g == GIT_PR_CI_NONE)
   {
      snprintf(path, sizeof(path), "commits/%s/status", info.head_sha);
      st = gh_get(&cx, path, &combined);
      if (st >= 200 && st < 300)
         g = git_pr_ci_grade_json(runs, combined);
   }
   gh_ctx_done(&cx);
   free(runs);
   free(combined);
   return g;
}

int git_pr_merge_via_api(const char *principal, const char *repo_dir, int number, char *err,
                         size_t errlen)
{
   if (err && errlen)
      err[0] = '\0';
   if (number <= 0)
      return -1;
   gh_ctx_t cx;
   if (gh_ctx_resolve(principal, repo_dir, &cx, err, errlen) != 0)
      return -1;
   char path[64];
   snprintf(path, sizeof(path), "pulls/%d/merge", number);
   char *resp = NULL;
   int st = gh_put(&cx, path, "{\"merge_method\":\"squash\"}", &resp);
   gh_ctx_done(&cx);
   int res;
   if (st >= 200 && st < 300)
      res = 0; /* merged */
   else if (st == 405 && resp && strstr(resp, "already merged"))
      res = 1;
   else if (st == 405 || st == 409)
   {
      gh_err(resp, st, "pr merge", err, errlen);
      res = 2; /* not mergeable / head moved */
   }
   else
   {
      gh_err(resp, st, "pr merge", err, errlen);
      res = -1;
   }
   free(resp);
   return res;
}
