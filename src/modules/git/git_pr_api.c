/* git_pr_api.c — open a GitHub PR via the REST API, in-process. See git_pr_api.h. */
#define _GNU_SOURCE 1
#include "git_pr_api.h"

#include "aimee.h"           /* MAX_PATH_LEN (needed by agent_types.h) */
#include "agent_exec.h"      /* agent_http_get/put/patch — the ops still on HTTP */
#include "cJSON.h"           /* request/response JSON */
#include "git_cred_inject.h" /* git_cred_inject_resolve_token — the ONE cred policy */
#include "egress_credential_envelope.h"
#include "util.h" /* run_cmd */

#include "headers/module_json_call.h" /* forge operations run in the module */
#include <aimee/git/module_api.h>

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

/* Run one fixed local git query and capture the trimmed first line of stdout.
 * A minimal environment prevents inherited GIT_DIR/WORK_TREE/config overrides
 * from changing the trusted repository context. No shell is involved. */
static int git_cap(const char *repo_dir, const char *args, char *out, size_t cap)
{
   if (out && cap)
      out[0] = '\0';
   if (!repo_dir)
      return -1;
   const char *const *argv = NULL;
   const char *origin[] = {"git", "-C", repo_dir, "config", "--get", "remote.origin.url", NULL};
   const char *branch[] = {"git", "-C", repo_dir, "rev-parse", "--abbrev-ref", "HEAD", NULL};
   const char *origin_head[] = {"git",          "-C",          repo_dir, "rev-parse",
                                "--abbrev-ref", "origin/HEAD", NULL};
   const char *subject[] = {"git", "-C", repo_dir, "log", "-1", "--format=%s", NULL};
   if (strcmp(args, "config --get remote.origin.url") == 0)
      argv = origin;
   else if (strcmp(args, "rev-parse --abbrev-ref HEAD") == 0)
      argv = branch;
   else if (strcmp(args, "rev-parse --abbrev-ref origin/HEAD") == 0)
      argv = origin_head;
   else if (strcmp(args, "log -1 --format=%s") == 0)
      argv = subject;
   else
      return -1;
   char *const envp[] = {"PATH=/usr/local/bin:/usr/bin:/bin", "GIT_CONFIG_NOSYSTEM=1",
                         "GIT_CONFIG_SYSTEM=/dev/null", "GIT_CONFIG_GLOBAL=/dev/null", NULL};
   char *r = NULL;
   int rc = safe_exec_capture_cwd_env_timeout(argv, repo_dir, envp, &r, 4096, 5000);
   if (rc != 0 || !r)
   {
      free(r);
      return -1;
   }
   char *nl = strchr(r, '\n');
   if (nl)
      *nl = '\0';
   size_t n = strlen(r);
   while (n && (r[n - 1] == ' ' || r[n - 1] == '\r' || r[n - 1] == '\t'))
      r[--n] = '\0';
   snprintf(out, cap, "%s", r);
   free(r);
   return out[0] ? 0 : -1;
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
   /* Pass the origin we just read, not NULL. THE MIRROR CONDITION applies here
    * exactly as it does to push (see test_git_host_resolve.c): for a mirror or
    * served workspace the checkout the client named does not exist on this host,
    * so making the per-host step re-derive the origin from repo_dir finds no
    * remote and returns "no host". Upstream that is indistinguishable from "no
    * credential", so the per-host token — the one the setup wizard stores and
    * the one `git clone` authenticates with, since clone is handed its URL and
    * never derives one — was skipped entirely, and this reported a missing
    * credential while that credential was present and working. We already have
    * the origin in hand for the slug; hand it over. */
   if (git_cred_inject_resolve_token(principal, origin, repo_dir, NULL, cx->token,
                                     sizeof(cx->token)) != 1 ||
       !cx->token[0])
   {
      wipe(cx->token, sizeof(cx->token));
      snprintf(err, errlen, "no github credential — connect one in the Git panel");
      return -1;
   }
   return 0;
}

/* As gh_ctx_resolve, but from an "owner/repo" slug the CALLER resolved, for the
 * callers with no server-visible checkout to run git in. The credential is looked
 * up against the slug's canonical https remote so per-host resolution still works;
 * repo_dir is NULL because there is no such directory here. */
static int gh_ctx_resolve_slug(const char *principal, const char *slug, gh_ctx_t *cx, char *err,
                               size_t errlen)
{
   memset(cx, 0, sizeof(*cx));
   if (!slug || !slug[0])
   {
      snprintf(err, errlen, "no repository");
      return -1;
   }
   const char *sl = strchr(slug, '/');
   if (!sl || sl == slug || !sl[1] || strchr(sl + 1, '/'))
   {
      snprintf(err, errlen, "requires an owner/repo slug");
      return -1;
   }
   size_t ol = (size_t)(sl - slug);
   if (ol >= sizeof(cx->owner) || strlen(sl + 1) >= sizeof(cx->repo))
   {
      snprintf(err, errlen, "repository name too long");
      return -1;
   }
   memcpy(cx->owner, slug, ol);
   cx->owner[ol] = '\0';
   snprintf(cx->repo, sizeof(cx->repo), "%s", sl + 1);

   char remote[512];
   snprintf(remote, sizeof(remote), "https://github.com/%s/%s.git", cx->owner, cx->repo);
   if (git_cred_inject_resolve_token(principal, remote, NULL, NULL, cx->token, sizeof(cx->token)) !=
           1 ||
       !cx->token[0])
   {
      wipe(cx->token, sizeof(cx->token));
      snprintf(err, errlen, "no github credential — connect one in the Git panel");
      return -1;
   }
   return 0;
}

/* "owner/repo" for a checkout THIS PROCESS can read. The repo_dir entry points use
 * it to reach their _slug siblings, which hold the actual request. */
static int gh_slug_from_repo_dir(const char *repo_dir, char *slug, size_t cap, char *err,
                                 size_t errlen)
{
   char origin[1024];
   if (git_cap(repo_dir, "config --get remote.origin.url", origin, sizeof(origin)) != 0)
   {
      snprintf(err, errlen, "no origin remote");
      return -1;
   }
   char owner[128], repo[128];
   if (parse_github_slug(origin, owner, sizeof(owner), repo, sizeof(repo)) != 0)
   {
      snprintf(err, errlen, "requires a github.com origin");
      return -1;
   }
   if ((size_t)snprintf(slug, cap, "%s/%s", owner, repo) >= cap)
   {
      snprintf(err, errlen, "repository name too long");
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

/* Authoritative default branch via GET /repos/<owner>/<repo>. Used when no base
 * was passed and the local origin/HEAD cache is unset/stale. Returns 0 with buf
 * filled, or -1 — callers must NOT fall back to guessing "main": a repo whose
 * default is e.g. "testing" would get its PR opened against the wrong branch. */
#define FORGE_STAGE_MAX_BODY   (1024u * 1024u)
#define FORGE_STAGE_TIMEOUT_MS 25000

/* Run one forge operation in the git module (bus stage 4,
 * server-go/modules/git/forge_request.go). Which endpoint, which method and
 * what the answer means are decisions and live there; this carries the facts and
 * applies the ruling.
 *
 * `extra` is merged into the request and consumed. Returns the reply (caller
 * frees) or NULL when the module could not be reached. */
static cJSON *forge_stage(const gh_ctx_t *cx, const char *op, cJSON *extra)
{
   char resource[sizeof(cx->owner) + sizeof(cx->repo) + 1];
   if (!cx || !op ||
       (size_t)snprintf(resource, sizeof(resource), "%s/%s", cx->owner, cx->repo) >=
           sizeof(resource))
   {
      cJSON_Delete(extra);
      return NULL;
   }
   cJSON *credential = aimee_egress_wrap_forge_credential(cx->token, op, resource);
   if (!credential)
   {
      cJSON_Delete(extra);
      return NULL;
   }
   cJSON *request = cJSON_CreateObject();
   if (!request)
   {
      cJSON_Delete(credential);
      cJSON_Delete(extra);
      return NULL;
   }
   cJSON_AddStringToObject(request, "op", op);
   cJSON_AddStringToObject(request, "owner", cx->owner);
   cJSON_AddStringToObject(request, "repo", cx->repo);
   cJSON_AddItemToObject(request, "credential", credential);
   if (extra)
   {
      cJSON *field = extra->child;
      while (field)
      {
         cJSON *next = field->next;
         cJSON_DetachItemViaPointer(extra, field);
         cJSON_AddItemToObject(request, field->string, field);
         field = next;
      }
      cJSON_Delete(extra);
   }
   return aimee_module_json_call(AIMEE_GIT_EVENT_FORGE_REQUEST, AIMEE_GIT_STAGE_FORGE_REQUEST,
                                 request, FORGE_STAGE_MAX_BODY, FORGE_STAGE_TIMEOUT_MS, NULL);
}

static int gh_default_branch(const gh_ctx_t *cx, char *buf, size_t n)
{
   if (!buf || n == 0)
      return -1;
   buf[0] = '\0';
   cJSON *reply = forge_stage(cx, "default_branch", NULL);
   if (!reply)
      return -1;
   const cJSON *branch = cJSON_GetObjectItemCaseSensitive(reply, "default_branch");
   int rc = -1;
   /* The answer is authoritative: a caller must NOT fall back to guessing
    * "main", because a repo whose default is "testing" would get its PR opened
    * against the wrong branch. An empty answer stays a failure. */
   if (cJSON_IsString(branch) && branch->valuestring[0] && strlen(branch->valuestring) < n)
   {
      snprintf(buf, n, "%s", branch->valuestring);
      rc = 0;
   }
   cJSON_Delete(reply);
   if (rc != 0)
      buf[0] = '\0';
   return rc;
}

int git_pr_default_branch_via_api(const char *principal, const char *repo_dir, char *out,
                                  size_t out_cap, char *err, size_t errlen)
{
   if (out && out_cap)
      out[0] = '\0';
   if (err && errlen)
      err[0] = '\0';
   gh_ctx_t cx;
   if (!out || out_cap == 0 || gh_ctx_resolve(principal, repo_dir, &cx, err, errlen) != 0)
      return -1;
   int rc = gh_default_branch(&cx, out, out_cap);
   gh_ctx_done(&cx);
   if (rc != 0)
   {
      out[0] = '\0';
      snprintf(err, errlen, "cannot resolve authoritative default branch");
   }
   return rc;
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

static int gh_patch(const gh_ctx_t *cx, const char *path, const char *body, char **resp)
{
   *resp = NULL;
   char url[512];
   if ((size_t)snprintf(url, sizeof(url), "https://api.github.com/repos/%s/%s/%s", cx->owner,
                        cx->repo, path) >= sizeof(url))
      return -1;
   char auth[PR_TOKEN_MAX + 32];
   snprintf(auth, sizeof(auth), "Authorization: Bearer %s", cx->token);
   int st = agent_http_patch(url, auth, body, resp, 20000, GH_ACCEPT);
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

int git_pr_https_origin_url(const char *repo_dir, char *out, size_t out_cap, char *err,
                            size_t errlen)
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
      snprintf(err, errlen, "requires a github.com origin");
      return -1;
   }
   if ((size_t)snprintf(out, out_cap, "https://github.com/%s/%s.git", owner, repo) >= out_cap)
   {
      snprintf(err, errlen, "url too long");
      out[0] = '\0';
      return -1;
   }
   return 0;
}

int git_pr_create_via_api_ex(const char *principal, const char *repo_dir, const char *head_in,
                             const char *base_in, const char *title, const char *body, char *out,
                             size_t out_cap, char *err, size_t errlen)
{
   return git_pr_create_via_api_ex_draft(principal, repo_dir, head_in, base_in, title, body, 0, out,
                                         out_cap, err, errlen);
}

int git_pr_create_via_api_ex_draft(const char *principal, const char *repo_dir, const char *head_in,
                                   const char *base_in, const char *title, const char *body,
                                   int draft, char *out, size_t out_cap, char *err, size_t errlen)
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
   else if (git_cap(repo_dir, "rev-parse --abbrev-ref origin/HEAD", base, sizeof(base)) == 0 &&
            base[0] && strcmp(base, "origin/HEAD") != 0)
   {
      char *sl = strchr(base, '/'); /* "origin/main" → "main" */
      if (sl)
         memmove(base, sl + 1, strlen(sl + 1) + 1);
   }
   else if (gh_default_branch(&cx, base, sizeof(base)) != 0)
   {
      /* No base given, local origin/HEAD unset/stale, and the authoritative API
       * lookup failed. Refuse rather than guess "main": opening the PR against
       * the wrong default branch is worse than surfacing the failure. */
      gh_ctx_done(&cx);
      snprintf(err, errlen, "cannot resolve default branch (pass an explicit base)");
      return -1;
   }
   char tbuf[512];
   if (!title || !title[0]) /* default the title to the last commit subject */
   {
      if (git_cap(repo_dir, "log -1 --format=%s", tbuf, sizeof(tbuf)) == 0 && tbuf[0])
         title = tbuf;
      else
         title = head;
   }

   /* Everything above needed the checkout; the request itself does not. Hand the
    * resolved slug to the no-checkout path so there is one copy of the POST. The
    * token is resolved again there, which costs one vault read and keeps this
    * function from having to hand a live credential across the boundary. */
   char slug[264];
   snprintf(slug, sizeof(slug), "%s/%s", cx.owner, cx.repo);
   gh_ctx_done(&cx);
   return git_pr_create_via_api_slug(principal, slug, head, base, title, body, draft, out, out_cap,
                                     err, errlen);
}

int git_pr_create_via_api_slug(const char *principal, const char *slug, const char *head,
                               const char *base, const char *title, const char *body, int draft,
                               char *out, size_t out_cap, char *err, size_t errlen)
{
   if (out && out_cap)
      out[0] = '\0';
   if (err && errlen)
      err[0] = '\0';

   /* No checkout to infer these from, and guessing is how a PR lands on the wrong
    * base. Refuse instead. */
   if (!head || !head[0] || !base || !base[0] || !title || !title[0])
   {
      snprintf(err, errlen, "head, base and title are required without a checkout");
      return -1;
   }

   gh_ctx_t cx;
   if (gh_ctx_resolve_slug(principal, slug, &cx, err, errlen) != 0)
      return -1;

   /* Standing directive: no AI co-authorship / "Generated with" attribution in
    * PR bodies — strip a copy before it reaches GitHub. */
   char *bclean = strdup(body ? body : "");
   if (bclean)
      strip_ai_attribution(bclean);

   cJSON *extra = cJSON_CreateObject();
   if (!extra)
   {
      free(bclean);
      gh_ctx_done(&cx);
      snprintf(err, errlen, "internal error");
      return -1;
   }
   cJSON_AddStringToObject(extra, "title", title);
   cJSON_AddStringToObject(extra, "head", head);
   cJSON_AddStringToObject(extra, "base", base);
   cJSON_AddStringToObject(extra, "body", bclean ? bclean : "");
   if (draft)
      cJSON_AddBoolToObject(extra, "draft", 1);
   free(bclean);

   cJSON *reply = forge_stage(&cx, "pr_create", extra);
   gh_ctx_done(&cx);
   if (!reply)
   {
      /* Not a refusal: say the module was unreachable, so a caller does not
       * record "the forge rejected this PR" for a request never sent. */
      snprintf(err, errlen, "pr create: the git module could not be reached");
      return -1;
   }

   const cJSON *pull = cJSON_GetObjectItemCaseSensitive(reply, "pull");
   const cJSON *url = pull ? cJSON_GetObjectItemCaseSensitive(pull, "url") : NULL;
   const cJSON *message = cJSON_GetObjectItemCaseSensitive(reply, "error");

   int ok = -1;
   if (cJSON_IsString(url) && url->valuestring && url->valuestring[0])
   {
      snprintf(out, out_cap, "%s", url->valuestring);
      ok = 0;
   }
   else if (cJSON_IsString(message) && message->valuestring)
   {
      /* The forge's own words -- "A pull request already exists" sends an
       * operator somewhere useful; "HTTP 422" does not. */
      snprintf(err, errlen, "%s", message->valuestring);
   }
   else
   {
      snprintf(err, errlen, "github API: unexpected response");
   }
   cJSON_Delete(reply);
   return ok;
}

int git_pr_find_open_via_api(const char *principal, const char *repo_dir, const char *head,
                             const char *base, char *out, size_t out_cap, int *number_out,
                             char *err, size_t errlen)
{
   if (out && out_cap)
      out[0] = '\0';
   if (number_out)
      *number_out = 0;
   char slug[264];
   if (gh_slug_from_repo_dir(repo_dir, slug, sizeof(slug), err, errlen) != 0)
      return -1;
   return git_pr_find_open_via_api_slug(principal, slug, head, base, out, out_cap, number_out, err,
                                        errlen);
}

int git_pr_find_open_via_api_slug(const char *principal, const char *slug, const char *head,
                                  const char *base, char *out, size_t out_cap, int *number_out,
                                  char *err, size_t errlen)
{
   if (out && out_cap)
      out[0] = '\0';
   /* Clear err like every sibling does. This one returns 0 for "no open PR",
    * which is a SUCCESS, and a caller reusing the buffer would otherwise read a
    * stale message from an earlier call and report a failure that never
    * happened. */
   if (err && errlen)
      err[0] = '\0';
   if (number_out)
      *number_out = 0;
   if (!head || !head[0] || !base || !base[0] || strlen(head) > 200 || strlen(base) > 200 ||
       strchr(head, '&') || strchr(head, '?') || strchr(base, '&') || strchr(base, '?'))
   {
      snprintf(err, errlen, "invalid PR head/base");
      return -1;
   }
   gh_ctx_t cx;
   if (gh_ctx_resolve_slug(principal, slug, &cx, err, errlen) != 0)
      return -1;
   cJSON *extra = cJSON_CreateObject();
   if (!extra)
   {
      gh_ctx_done(&cx);
      snprintf(err, errlen, "internal error");
      return -1;
   }
   /* Head AND base: the same head can have an open PR into one base and none
    * into another, so filtering by head alone answers the question for the
    * wrong target branch. */
   cJSON_AddStringToObject(extra, "head", head);
   cJSON_AddStringToObject(extra, "base", base);
   cJSON_AddNumberToObject(extra, "limit", 1);
   cJSON *reply = forge_stage(&cx, "pr_find_open", extra);
   gh_ctx_done(&cx);
   if (!reply)
   {
      snprintf(err, errlen, "find PR: the git module could not be reached");
      return -1;
   }
   const cJSON *message = cJSON_GetObjectItemCaseSensitive(reply, "error");
   if (cJSON_IsString(message) && message->valuestring)
   {
      snprintf(err, errlen, "%s", message->valuestring);
      cJSON_Delete(reply);
      return -1;
   }
   /* NO MATCH IS NOT AN ERROR: 0 means "no open PR", which is the answer the
    * caller acts on by opening one. Reporting -1 here would make it give up. */
   int found = 0;
   const cJSON *pull = cJSON_GetObjectItemCaseSensitive(reply, "pull");
   const cJSON *url = pull ? cJSON_GetObjectItemCaseSensitive(pull, "url") : NULL;
   const cJSON *number = pull ? cJSON_GetObjectItemCaseSensitive(pull, "number") : NULL;
   if (cJSON_IsString(url) && url->valuestring && url->valuestring[0])
   {
      snprintf(out, out_cap, "%s", url->valuestring);
      if (number_out && cJSON_IsNumber(number) && number->valueint > 0)
         *number_out = number->valueint;
      found = 1;
   }
   cJSON_Delete(reply);
   return found;
}

int git_pr_update_via_api(const char *principal, const char *repo_dir, int number,
                          const char *title, const char *body, char *err, size_t errlen)
{
   if (err && errlen)
      err[0] = '\0';
   char slug[264];
   if (gh_slug_from_repo_dir(repo_dir, slug, sizeof(slug), err, errlen) != 0)
      return -1;
   return git_pr_update_via_api_slug(principal, slug, number, title, body, err, errlen);
}

int git_pr_update_via_api_slug(const char *principal, const char *slug, int number,
                               const char *title, const char *body, char *err, size_t errlen)
{
   if (err && errlen)
      err[0] = '\0';
   if (!title || !title[0] || !body || !body[0])
   {
      snprintf(err, errlen, "invalid PR update");
      return -1;
   }
   return git_pr_edit_via_api_slug(principal, slug, number, title, body, NULL, err, errlen);
}

int git_pr_edit_via_api_slug(const char *principal, const char *slug, int number, const char *title,
                             const char *body, const char *base, char *err, size_t errlen)
{
   if (err && errlen)
      err[0] = '\0';
   int has_title = title && title[0];
   int has_body = body && body[0];
   int has_base = base && base[0];
   if (number <= 0 || (!has_title && !has_body && !has_base))
   {
      snprintf(err, errlen, "invalid PR update");
      return -1;
   }
   gh_ctx_t cx;
   if (gh_ctx_resolve_slug(principal, slug, &cx, err, errlen) != 0)
      return -1;

   /* Only the fields the caller supplied go in the payload: sending an empty
    * string would blank the PR's title or body rather than leave it alone. */
   cJSON *json = cJSON_CreateObject();
   if (has_title)
      cJSON_AddStringToObject(json, "title", title);
   if (has_body)
   {
      char *clean = strdup(body);
      if (clean)
         strip_ai_attribution(clean);
      cJSON_AddStringToObject(json, "body", clean ? clean : body);
      free(clean);
   }
   if (has_base)
      cJSON_AddStringToObject(json, "base", base);
   char *payload = cJSON_PrintUnformatted(json);
   cJSON_Delete(json);
   if (!payload)
   {
      gh_ctx_done(&cx);
      snprintf(err, errlen, "internal error");
      return -1;
   }

   char path[64];
   snprintf(path, sizeof(path), "pulls/%d", number);
   char *response = NULL;
   int status = gh_patch(&cx, path, payload, &response);
   free(payload);
   gh_ctx_done(&cx);
   if (status < 200 || status >= 300)
   {
      gh_err(response, status, "pr update", err, errlen);
      free(response);
      return -1;
   }
   free(response);
   return 0;
}

/* Seconds since the epoch for "YYYY-MM-DDTHH:MM:SSZ", or -1 if it does not parse.
 *
 * Done by arithmetic rather than strptime/timegm: neither is available on every
 * target this builds for (there is a Windows build), and the input is a fixed UTC
 * format, so a civil-days conversion is both portable and exact. */
static long long gh_iso8601_secs(const char *s)
{
   int y, mo, d, h, mi, se;
   if (!s || sscanf(s, "%4d-%2d-%2dT%2d:%2d:%2dZ", &y, &mo, &d, &h, &mi, &se) != 6)
      return -1;
   /* days_from_civil (Howard Hinnant): shift the year so March starts the era,
    * which removes the leap-day special case entirely. */
   y -= mo <= 2;
   long long era = (y >= 0 ? y : y - 399) / 400;
   unsigned yoe = (unsigned)(y - era * 400);
   unsigned doy = (unsigned)((153 * (mo + (mo > 2 ? -3 : 9)) + 2) / 5 + d - 1);
   unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
   long long days = era * 146097 + (long long)doe - 719468;
   return days * 86400 + h * 3600 + mi * 60 + se;
}

/* gh printed a bare "0" for anything not completed or of zero length, "<n>s"
 * under a minute, and "<m>m<s>s" above it. */
static void gh_elapsed(const char *started, const char *completed, int completed_state, char *out,
                       size_t cap)
{
   long long a = gh_iso8601_secs(started), b = gh_iso8601_secs(completed);
   if (!completed_state || a < 0 || b < 0 || b <= a)
   {
      snprintf(out, cap, "0");
      return;
   }
   long long d = b - a;
   if (d >= 60)
      snprintf(out, cap, "%lldm%llds", d / 60, d % 60);
   else
      snprintf(out, cap, "%llds", d);
}

/* GitHub's conclusion vocabulary mapped to the four words gh printed. Anything
 * unrecognised reports "pending" rather than inventing a word: a caller polling
 * for completion keeps polling, which is the safe direction to be wrong in. */
static const char *gh_check_word(const char *status, const char *conclusion)
{
   if (!status || strcmp(status, "completed") != 0)
      return "pending";
   if (!conclusion)
      return "pending";
   if (strcmp(conclusion, "success") == 0)
      return "pass";
   if (strcmp(conclusion, "failure") == 0 || strcmp(conclusion, "timed_out") == 0 ||
       strcmp(conclusion, "action_required") == 0 || strcmp(conclusion, "startup_failure") == 0 ||
       strcmp(conclusion, "cancelled") == 0)
      return "fail";
   if (strcmp(conclusion, "neutral") == 0 || strcmp(conclusion, "skipped") == 0 ||
       strcmp(conclusion, "stale") == 0)
      return "skipping";
   return "pending";
}

/* By name, then by url.
 *
 * This is OUR order, not a reproduction of gh's. gh was observed grouping skipped
 * checks ahead of passing ones on one PR, sorting plain alphabetically on another,
 * and doing neither on a third whose CI was still running -- so there is no stable
 * order to copy. Callers read these rows by field, and two calls on unchanged data
 * returning the same order matters more than matching something gh does not hold
 * still. The url tiebreak is what makes that true: a repository can run two checks
 * under ONE name (observed: `pins` twice on the same commit) and qsort is not
 * stable, so without it the duplicate pair could swap between calls. */
static int gh_check_cmp(const void *a, const void *b)
{
   const git_pr_check_t *x = a, *y = b;
   int c = strcmp(x->name, y->name);
   return c ? c : strcmp(x->url, y->url);
}

int git_pr_checks_via_api_slug(const char *principal, const char *slug, int number, int max,
                               git_pr_check_t *out, int *count, char *err, size_t errlen)
{
   if (err && errlen)
      err[0] = '\0';
   if (count)
      *count = 0;
   if (!out || !count || max <= 0 || number <= 0)
   {
      snprintf(err, errlen, "invalid PR checks request");
      return -1;
   }

   /* The checks belong to the head COMMIT, so the PR has to be read first. */
   git_pr_info_t info;
   if (git_pr_info_via_api_slug(principal, slug, number, &info, err, errlen) != 0)
      return -1;
   if (!info.head_sha[0])
   {
      snprintf(err, errlen, "pull request has no head commit");
      return -1;
   }

   gh_ctx_t cx;
   if (gh_ctx_resolve_slug(principal, slug, &cx, err, errlen) != 0)
      return -1;
   char path[160];
   snprintf(path, sizeof(path), "commits/%s/check-runs?per_page=100", info.head_sha);
   char *resp = NULL;
   int st = gh_get(&cx, path, &resp);
   gh_ctx_done(&cx);
   if (st < 200 || st >= 300 || !resp)
   {
      gh_err(resp, st, "pr checks", err, errlen);
      free(resp);
      return -1;
   }
   cJSON *j = cJSON_Parse(resp);
   free(resp);
   const cJSON *runs = j ? cJSON_GetObjectItem(j, "check_runs") : NULL;
   if (!cJSON_IsArray(runs))
   {
      cJSON_Delete(j);
      snprintf(err, errlen, "github API: unparseable check runs");
      return -1;
   }
   int n = 0;
   const cJSON *r = NULL;
   cJSON_ArrayForEach(r, runs)
   {
      if (n >= max)
         break;
      const cJSON *name = cJSON_GetObjectItem(r, "name");
      if (!cJSON_IsString(name) || !name->valuestring)
         continue; /* an unnamed check cannot be reported usefully */
      const cJSON *status = cJSON_GetObjectItem(r, "status");
      const cJSON *concl = cJSON_GetObjectItem(r, "conclusion");
      const cJSON *started = cJSON_GetObjectItem(r, "started_at");
      const cJSON *done = cJSON_GetObjectItem(r, "completed_at");
      const cJSON *url = cJSON_GetObjectItem(r, "details_url");
      const char *status_s = cJSON_IsString(status) ? status->valuestring : NULL;
      git_pr_check_t *row = &out[n];
      memset(row, 0, sizeof(*row));
      snprintf(row->name, sizeof(row->name), "%s", name->valuestring);
      snprintf(row->status, sizeof(row->status), "%s",
               gh_check_word(status_s, cJSON_IsString(concl) ? concl->valuestring : NULL));
      gh_elapsed(cJSON_IsString(started) ? started->valuestring : NULL,
                 cJSON_IsString(done) ? done->valuestring : NULL,
                 status_s && strcmp(status_s, "completed") == 0, row->elapsed,
                 sizeof(row->elapsed));
      if (cJSON_IsString(url) && url->valuestring)
         snprintf(row->url, sizeof(row->url), "%s", url->valuestring);
      n++;
   }
   cJSON_Delete(j);
   if (n > 1)
      qsort(out, (size_t)n, sizeof(out[0]), gh_check_cmp); /* gh listed them by name */
   *count = n;
   return 0;
}

/* --- Why a check failed --- */

/* A check run's details_url ends ".../actions/runs/<run>/job/<job>"; the job id is
 * the only handle the steps and log endpoints take, and reading it from the URL
 * we already have avoids listing every job in the run to match by name. */
static long gh_job_id_from_details_url(const char *url)
{
   if (!url || !url[0])
      return 0;
   const char *seg = strstr(url, "/job/");
   if (!seg)
      return 0;
   seg += 5;
   char *end = NULL;
   long id = strtol(seg, &end, 10);
   if (end == seg || id <= 0)
      return 0;
   return id;
}

/* First failed step of a job: the name, and the 1-based position. For a step the
 * workflow did not name, the forge reports the command line, which is exactly what
 * a caller needs in order to run the same gate locally. */
static void gh_failed_step(const gh_ctx_t *cx, long job_id, char *name, size_t name_cap,
                           int *number)
{
   name[0] = '\0';
   *number = 0;
   char path[96];
   snprintf(path, sizeof(path), "actions/jobs/%ld", job_id);
   char *resp = NULL;
   int st = gh_get(cx, path, &resp);
   if (st < 200 || st >= 300 || !resp)
   {
      free(resp);
      return;
   }
   cJSON *j = cJSON_Parse(resp);
   free(resp);
   const cJSON *steps = j ? cJSON_GetObjectItem(j, "steps") : NULL;
   const cJSON *s = NULL;
   cJSON_ArrayForEach(s, steps)
   {
      const cJSON *concl = cJSON_GetObjectItem(s, "conclusion");
      if (!cJSON_IsString(concl) || !concl->valuestring)
         continue;
      if (strcmp(concl->valuestring, "failure") != 0 &&
          strcmp(concl->valuestring, "timed_out") != 0)
         continue;
      const cJSON *nm = cJSON_GetObjectItem(s, "name");
      const cJSON *num = cJSON_GetObjectItem(s, "number");
      if (cJSON_IsString(nm) && nm->valuestring)
         snprintf(name, name_cap, "%s", nm->valuestring);
      if (cJSON_IsNumber(num))
         *number = num->valueint;
      break; /* the first failure is the cause; later ones are usually fallout */
   }
   cJSON_Delete(j);
}

/* The last `tail_bytes` of a job's log.
 *
 * Two requests, deliberately. The forge's log endpoint answers 302 to pre-signed
 * blob storage, so the redirect is followed HERE rather than in the HTTP layer:
 * the second request must carry the Range header but must NOT carry the
 * Authorization header, because that would hand the forge token to a third-party
 * host. Range asks for the tail so a multi-megabyte log costs one small read, and
 * the tail is where the error is. Returns malloc'd text, or NULL. */
static char *gh_job_log_tail(const gh_ctx_t *cx, long job_id, long tail_bytes)
{
   if (tail_bytes <= 0)
      return NULL;

   char url[512];
   if ((size_t)snprintf(url, sizeof(url),
                        "https://api.github.com/repos/%s/%s/actions/jobs/%ld/logs", cx->owner,
                        cx->repo, job_id) >= sizeof(url))
      return NULL;
   char hdrs[480];
   if ((size_t)snprintf(hdrs, sizeof(hdrs), "Authorization: Bearer %s\n" GH_ACCEPT, cx->token) >=
       sizeof(hdrs))
      return NULL;

   char location[1024] = "";
   char *body = NULL;
   int st = agent_http_get_location(url, hdrs, location, sizeof(location), &body, 20000);
   wipe(hdrs, sizeof(hdrs));

   /* Some deployments answer the log inline instead of redirecting. */
   if (st >= 200 && st < 300 && body && body[0])
      return body;
   free(body);
   body = NULL;
   if (st < 300 || st >= 400 || !location[0])
      return NULL;

   /* Signed URL: Range only. No Authorization — see above. */
   char range[64];
   snprintf(range, sizeof(range), "Range: bytes=-%ld", tail_bytes);
   int st2 = agent_http_get(location, range, &body, 20000);
   wipe(location, sizeof(location));
   if ((st2 != 200 && st2 != 206) || !body)
   {
      free(body);
      return NULL;
   }
   return body;
}

int git_pr_failures_via_api_slug(const char *principal, const char *slug, int number, int max,
                                 int logs_for, long tail_bytes, git_pr_failure_t *out, int *count,
                                 char *err, size_t errlen)
{
   if (err && errlen)
      err[0] = '\0';
   if (count)
      *count = 0;
   if (!out || !count || max <= 0 || number <= 0)
   {
      snprintf(err, errlen, "invalid PR failures request");
      return -1;
   }

   git_pr_info_t info;
   if (git_pr_info_via_api_slug(principal, slug, number, &info, err, errlen) != 0)
      return -1;
   if (!info.head_sha[0])
   {
      snprintf(err, errlen, "pull request has no head commit");
      return -1;
   }

   gh_ctx_t cx;
   if (gh_ctx_resolve_slug(principal, slug, &cx, err, errlen) != 0)
      return -1;

   char path[160];
   snprintf(path, sizeof(path), "commits/%s/check-runs?per_page=100", info.head_sha);
   char *resp = NULL;
   int st = gh_get(&cx, path, &resp);
   if (st < 200 || st >= 300 || !resp)
   {
      gh_err(resp, st, "pr failures", err, errlen);
      free(resp);
      gh_ctx_done(&cx);
      return -1;
   }
   cJSON *j = cJSON_Parse(resp);
   free(resp);
   const cJSON *runs = j ? cJSON_GetObjectItem(j, "check_runs") : NULL;
   if (!cJSON_IsArray(runs))
   {
      cJSON_Delete(j);
      gh_ctx_done(&cx);
      snprintf(err, errlen, "github API: unparseable check runs");
      return -1;
   }

   int n = 0;
   const cJSON *r = NULL;
   cJSON_ArrayForEach(r, runs)
   {
      if (n >= max)
         break;
      const cJSON *concl = cJSON_GetObjectItem(r, "conclusion");
      if (!cJSON_IsString(concl) || !concl->valuestring)
         continue;
      const char *c = concl->valuestring;
      /* Only states a caller can act on. `cancelled` and `skipped` usually mean
       * another job failed first, and `neutral` is not a failure. */
      if (strcmp(c, "failure") != 0 && strcmp(c, "timed_out") != 0 &&
          strcmp(c, "action_required") != 0)
         continue;

      const cJSON *name = cJSON_GetObjectItem(r, "name");
      const cJSON *url = cJSON_GetObjectItem(r, "details_url");
      git_pr_failure_t *row = &out[n];
      memset(row, 0, sizeof(*row));
      snprintf(row->conclusion, sizeof(row->conclusion), "%s", c);
      if (cJSON_IsString(name) && name->valuestring)
         snprintf(row->name, sizeof(row->name), "%s", name->valuestring);
      if (cJSON_IsString(url) && url->valuestring)
         snprintf(row->url, sizeof(row->url), "%s", url->valuestring);

      long job_id = gh_job_id_from_details_url(row->url);
      if (job_id > 0)
      {
         gh_failed_step(&cx, job_id, row->failed_step, sizeof(row->failed_step),
                        &row->failed_step_number);
         if (n < logs_for)
            row->log_tail = gh_job_log_tail(&cx, job_id, tail_bytes);
      }
      n++;
   }
   cJSON_Delete(j);
   gh_ctx_done(&cx);
   *count = n;
   return 0;
}

void git_pr_failures_free(git_pr_failure_t *rows, int count)
{
   if (!rows)
      return;
   for (int i = 0; i < count; i++)
   {
      free(rows[i].log_tail);
      rows[i].log_tail = NULL;
   }
}

int git_pr_list_open_via_api_slug(const char *principal, const char *slug, int limit,
                                  git_pr_list_item_t *out, int *count, char *err, size_t errlen)
{
   if (err && errlen)
      err[0] = '\0';
   if (count)
      *count = 0;
   if (!out || !count || limit <= 0)
   {
      snprintf(err, errlen, "invalid PR list request");
      return -1;
   }
   if (limit > 100) /* one page; GitHub caps per_page at 100 */
      limit = 100;

   gh_ctx_t cx;
   if (gh_ctx_resolve_slug(principal, slug, &cx, err, errlen) != 0)
      return -1;
   cJSON *extra = cJSON_CreateObject();
   if (!extra)
   {
      gh_ctx_done(&cx);
      snprintf(err, errlen, "internal error");
      return -1;
   }
   cJSON_AddNumberToObject(extra, "limit", limit);
   cJSON *reply = forge_stage(&cx, "pr_list_open", extra);
   gh_ctx_done(&cx);
   if (!reply)
   {
      snprintf(err, errlen, "pr list: the git module could not be reached");
      return -1;
   }
   const cJSON *message = cJSON_GetObjectItemCaseSensitive(reply, "error");
   if (cJSON_IsString(message) && message->valuestring)
   {
      snprintf(err, errlen, "%s", message->valuestring);
      cJSON_Delete(reply);
      return -1;
   }
   const cJSON *statusj = cJSON_GetObjectItemCaseSensitive(reply, "status");
   int status = cJSON_IsNumber(statusj) ? statusj->valueint : 0;
   cJSON *arr = cJSON_DetachItemFromObjectCaseSensitive(reply, "pulls");
   cJSON_Delete(reply);
   if (!cJSON_IsArray(arr))
   {
      /* NO OPEN PRs IS A RESULT, NOT A FAILURE. The stage omits an empty list
       * entirely, so an absent key on a 2xx is zero rows -- reporting an error
       * here would turn "nothing to list" into "the listing is broken". */
      cJSON_Delete(arr);
      if (status >= 200 && status < 300)
      {
         *count = 0;
         return 0;
      }
      snprintf(err, errlen, "github API: unparseable pr list");
      return -1;
   }
   int n = 0;
   const cJSON *item = NULL;
   cJSON_ArrayForEach(item, arr)
   {
      if (n >= limit)
         break;
      const cJSON *num = cJSON_GetObjectItem(item, "number");
      const cJSON *state = cJSON_GetObjectItem(item, "state");
      const cJSON *title = cJSON_GetObjectItem(item, "title");
      const cJSON *headref = cJSON_GetObjectItem(item, "head");
      const cJSON *mat = cJSON_GetObjectItem(item, "merged_at");
      if (!cJSON_IsNumber(num) || num->valueint <= 0)
         continue; /* a row without a number is not addressable; skip it */
      git_pr_list_item_t *row = &out[n];
      memset(row, 0, sizeof(*row));
      row->number = num->valueint;
      /* gh spelled the state upper-case, and reported MERGED in place of the
       * closed state a merged PR actually carries. */
      if (cJSON_IsString(mat) && mat->valuestring)
         snprintf(row->state, sizeof(row->state), "MERGED");
      else if (cJSON_IsString(state) && state->valuestring)
      {
         snprintf(row->state, sizeof(row->state), "%s", state->valuestring);
         for (char *p = row->state; *p; p++)
            *p = (char)toupper((unsigned char)*p);
      }
      if (cJSON_IsString(headref) && headref->valuestring)
         snprintf(row->head, sizeof(row->head), "%s", headref->valuestring);
      /* base.ref: "which branch would this merge into". Feature-branch promotion asks
       * whether any open PR still targets a feature branch, which head alone cannot
       * answer. The stage already emits it; this only stopped being dropped here. */
      const cJSON *baseref = cJSON_GetObjectItem(item, "base");
      if (cJSON_IsString(baseref) && baseref->valuestring)
         snprintf(row->base, sizeof(row->base), "%s", baseref->valuestring);
      if (cJSON_IsString(title) && title->valuestring)
         snprintf(row->title, sizeof(row->title), "%s", title->valuestring);
      n++;
   }
   cJSON_Delete(arr);
   *count = n;
   return 0;
}

int git_pr_info_via_api(const char *principal, const char *repo_dir, int number, git_pr_info_t *out,
                        char *err, size_t errlen)
{
   if (err && errlen)
      err[0] = '\0';
   char slug[264];
   if (gh_slug_from_repo_dir(repo_dir, slug, sizeof(slug), err, errlen) != 0)
      return -1;
   return git_pr_info_via_api_slug(principal, slug, number, out, err, errlen);
}

int git_pr_info_via_api_slug(const char *principal, const char *slug, int number,
                             git_pr_info_t *out, char *err, size_t errlen)
{
   if (err && errlen)
      err[0] = '\0';
   if (!out || number <= 0)
      return -1;
   memset(out, 0, sizeof(*out));
   out->mergeable = -1;

   gh_ctx_t cx;
   if (gh_ctx_resolve_slug(principal, slug, &cx, err, errlen) != 0)
      return -1;
   cJSON *extra = cJSON_CreateObject();
   if (!extra)
   {
      gh_ctx_done(&cx);
      snprintf(err, errlen, "internal error");
      return -1;
   }
   cJSON_AddNumberToObject(extra, "number", number);
   cJSON *reply = forge_stage(&cx, "pr_info", extra);
   gh_ctx_done(&cx);
   if (!reply)
   {
      snprintf(err, errlen, "pr info: the git module could not be reached");
      return -1;
   }
   const cJSON *message = cJSON_GetObjectItemCaseSensitive(reply, "error");
   cJSON *j = cJSON_DetachItemFromObjectCaseSensitive(reply, "pull");
   if (!j)
   {
      if (cJSON_IsString(message) && message->valuestring)
         snprintf(err, errlen, "%s", message->valuestring);
      else
         snprintf(err, errlen, "github API: unparseable pr info");
      cJSON_Delete(reply);
      return -1;
   }
   cJSON_Delete(reply);

   const cJSON *state = cJSON_GetObjectItem(j, "state");
   const cJSON *merged = cJSON_GetObjectItem(j, "merged");
   /* ABSENT means the forge is still computing the merge, which is not the same
    * as "cannot merge": out->mergeable stays -1 in that case, and only an
    * explicit boolean moves it to 1/0. */
   const cJSON *mergeable = cJSON_GetObjectItem(j, "mergeable");
   const cJSON *shaj = cJSON_GetObjectItem(j, "head_sha");
   const cJSON *headref = cJSON_GetObjectItem(j, "head");
   const cJSON *baseref = cJSON_GetObjectItem(j, "base");
   const char *sha_s = cJSON_IsString(shaj) ? shaj->valuestring : NULL;
   const char *head_s = cJSON_IsString(headref) ? headref->valuestring : NULL;
   const char *base_s = cJSON_IsString(baseref) ? baseref->valuestring : NULL;
   if (!cJSON_IsString(state) || !state->valuestring || !sha_s || !sha_s[0] || !head_s ||
       !head_s[0] || !base_s || !base_s[0])
   {
      cJSON_Delete(j);
      snprintf(err, errlen, "github API: pull request response is missing required refs");
      return -1;
   }
   out->open =
       cJSON_IsString(state) && state->valuestring && strcmp(state->valuestring, "open") == 0;
   out->merged = cJSON_IsTrue(merged) ? 1 : 0;

   /* Reviewer-facing fields, best-effort: a PR is still usable without them, so a
    * missing or over-long value is left empty rather than failing the whole call
    * the way a missing ref does above. */
   const cJSON *title = cJSON_GetObjectItem(j, "title");
   const cJSON *hurl = cJSON_GetObjectItem(j, "url");
   const cJSON *mat = cJSON_GetObjectItem(j, "merged_at");
   if (cJSON_IsString(title) && title->valuestring)
      snprintf(out->title, sizeof(out->title), "%s", title->valuestring);
   if (cJSON_IsString(hurl) && hurl->valuestring)
      snprintf(out->html_url, sizeof(out->html_url), "%s", hurl->valuestring);
   if (cJSON_IsString(mat) && mat->valuestring) /* absent when never merged */
      snprintf(out->merged_at, sizeof(out->merged_at), "%s", mat->valuestring);
   /* Already upper-cased by the module: REST spells mergeable_state lowercase
    * while callers render the gh mergeStateStatus spelling, and normalising it
    * in one place beats every caller remembering to. */
   const cJSON *mstate = cJSON_GetObjectItem(j, "merge_state");
   if (cJSON_IsString(mstate) && mstate->valuestring)
      snprintf(out->merge_state, sizeof(out->merge_state), "%s", mstate->valuestring);

   if (cJSON_IsBool(mergeable))
      out->mergeable = cJSON_IsTrue(mergeable) ? 1 : 0; /* null stays -1 (computing) */
   if (strlen(sha_s) >= sizeof(out->head_sha) || strlen(head_s) >= sizeof(out->head) ||
       strlen(base_s) >= sizeof(out->base))
   {
      cJSON_Delete(j);
      snprintf(err, errlen, "github API: pull request ref is too long");
      return -1;
   }
   snprintf(out->head_sha, sizeof(out->head_sha), "%s", sha_s);
   snprintf(out->head, sizeof(out->head), "%s", head_s);
   snprintf(out->base, sizeof(out->base), "%s", base_s);
   cJSON_Delete(j);
   return 0;
}

git_pr_ci_t git_pr_ci_via_api(const char *principal, const char *repo_dir, int number, char *err,
                              size_t errlen)
{
   if (err && errlen)
      err[0] = '\0';
   char slug[264];
   if (gh_slug_from_repo_dir(repo_dir, slug, sizeof(slug), err, errlen) != 0)
      return GIT_PR_CI_ERROR;
   return git_pr_ci_via_api_slug(principal, slug, number, err, errlen);
}

git_pr_ci_t git_pr_ci_via_api_slug(const char *principal, const char *slug, int number, char *err,
                                   size_t errlen)
{
   if (err && errlen)
      err[0] = '\0';
   git_pr_info_t info;
   if (git_pr_info_via_api_slug(principal, slug, number, &info, err, errlen) != 0 ||
       !info.head_sha[0])
      return GIT_PR_CI_ERROR;

   gh_ctx_t cx;
   if (gh_ctx_resolve_slug(principal, slug, &cx, err, errlen) != 0)
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
   char slug[264];
   if (gh_slug_from_repo_dir(repo_dir, slug, sizeof(slug), err, errlen) != 0)
      return -1;
   return git_pr_merge_via_api_slug(principal, slug, number, err, errlen);
}

int git_pr_merge_via_api_slug(const char *principal, const char *slug, int number, char *err,
                              size_t errlen)
{
   /* The squash-with-empty-body form the workflow forge and webchat rely on. */
   return git_pr_merge_via_api_slug_ex(principal, slug, number, "squash", NULL, NULL, 0, err,
                                       errlen);
}

/* Merge the base branch INTO the PR's head branch -- the REST equivalent of the
 * "Update branch" button. A protected base that requires branches to be up to
 * date reports its required checks as merely "expected" while the head is
 * BEHIND, so the PR cannot merge however green those checks already are; this is
 * the only call that clears that state. GitHub answers 202 (queued), so a 2xx
 * means accepted, NOT that the new head has been built yet -- poll merge_status
 * before merging. 422 is the benign "already up to date" case. */
int git_pr_update_branch_via_api_slug(const char *principal, const char *slug, int number,
                                      const char *expected_head_sha, char *err, size_t errlen)
{
   if (err && errlen)
      err[0] = '\0';
   if (number <= 0)
      return -1;

   gh_ctx_t cx;
   if (gh_ctx_resolve_slug(principal, slug, &cx, err, errlen) != 0)
      return -1;

   /* Drift safety, same contract as the merge op: refuse if the head moved. */
   char *payload = NULL;
   if (expected_head_sha && expected_head_sha[0])
   {
      cJSON *j = cJSON_CreateObject();
      cJSON_AddStringToObject(j, "expected_head_sha", expected_head_sha);
      payload = cJSON_PrintUnformatted(j);
      cJSON_Delete(j);
      if (!payload)
      {
         gh_ctx_done(&cx);
         snprintf(err, errlen, "internal error");
         return -1;
      }
   }

   char path[64];
   snprintf(path, sizeof(path), "pulls/%d/update-branch", number);
   char *resp = NULL;
   int st = gh_put(&cx, path, payload ? payload : "{}", &resp);
   free(payload);
   gh_ctx_done(&cx);

   int res;
   if (st >= 200 && st < 300)
      res = 0; /* accepted/queued */
   else if (st == 422)
      res = 1; /* nothing to do: head already contains base */
   else
   {
      gh_err(resp, st, "pr update_branch", err, errlen);
      res = -1;
   }
   free(resp);
   return res;
}

int git_pr_merge_via_api_slug_ex(const char *principal, const char *slug, int number,
                                 const char *merge_method, const char *expected_head_sha,
                                 char *out_sha, size_t out_sha_cap, char *err, size_t errlen)
{
   if (err && errlen)
      err[0] = '\0';
   if (out_sha && out_sha_cap)
      out_sha[0] = '\0';
   if (number <= 0)
      return -1;

   /* Default to a real merge commit. Silently squashing would rewrite history the
    * caller did not ask to rewrite, so an unrecognised method is refused rather
    * than coerced. */
   const char *method = (merge_method && merge_method[0]) ? merge_method : "merge";
   if (strcmp(method, "merge") != 0 && strcmp(method, "squash") != 0 &&
       strcmp(method, "rebase") != 0)
   {
      snprintf(err, errlen, "unsupported merge_method '%s' (merge|squash|rebase)", method);
      return -1;
   }

   gh_ctx_t cx;
   if (gh_ctx_resolve_slug(principal, slug, &cx, err, errlen) != 0)
      return -1;

   /* The merge itself, its payload shaping (a squash's synthesized body, the
    * drift-safety sha) and the 405/409 classification all live in the module
    * now. What stays here is the translation into this function's long-standing
    * return codes, which callers switch on. */
   cJSON *extra = cJSON_CreateObject();
   if (!extra)
   {
      gh_ctx_done(&cx);
      snprintf(err, errlen, "internal error");
      return -1;
   }
   cJSON_AddNumberToObject(extra, "number", number);
   cJSON_AddStringToObject(extra, "merge_method", method);
   if (expected_head_sha && expected_head_sha[0])
      cJSON_AddStringToObject(extra, "expected_head_sha", expected_head_sha);

   cJSON *reply = forge_stage(&cx, "pr_merge", extra);
   gh_ctx_done(&cx);
   if (!reply)
   {
      /* Unreachable module is NOT a refused merge: say so, rather than letting a
       * caller read it as "the forge said no" and stop retrying. */
      snprintf(err, errlen, "pr merge: the git module could not be reached");
      return -1;
   }

   const cJSON *merged = cJSON_GetObjectItemCaseSensitive(reply, "merged");
   const cJSON *already = cJSON_GetObjectItemCaseSensitive(reply, "already_merged");
   const cJSON *conflict = cJSON_GetObjectItemCaseSensitive(reply, "conflict");
   const cJSON *retryable = cJSON_GetObjectItemCaseSensitive(reply, "retryable");
   const cJSON *message = cJSON_GetObjectItemCaseSensitive(reply, "error");
   const cJSON *sha = cJSON_GetObjectItemCaseSensitive(reply, "merge_sha");

   int res;
   if (cJSON_IsTrue(merged))
   {
      /* The 2xx body carried the merge commit, so no second lookup. */
      if (out_sha && out_sha_cap && cJSON_IsString(sha) && sha->valuestring)
         snprintf(out_sha, out_sha_cap, "%s", sha->valuestring);
      res = 0;
   }
   else if (cJSON_IsTrue(already))
   {
      res = 1;
   }
   else
   {
      if (cJSON_IsString(message) && message->valuestring)
         snprintf(err, errlen, "%s", message->valuestring);
      else
         snprintf(err, errlen, "pr merge: refused");
      /* Terminal vs retryable, decided in the module: a content conflict is a
       * property of the two trees and a retry reproduces it exactly, while a
       * moved head/base is a lost race a retry wins. */
      if (cJSON_IsTrue(conflict))
         res = 3;
      else if (cJSON_IsTrue(retryable))
         res = 2;
      else
         res = -1;
   }
   cJSON_Delete(reply);
   return res;
}
