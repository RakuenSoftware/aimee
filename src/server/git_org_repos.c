/* git_org_repos.c — enumerate the repositories under an owner/org on a git host,
 * provider-agnostic (GitHub, GitLab, Gitea/Forgejo, Bitbucket). See header. */

#include "git_org_repos.h"

#include "git_host_cred.h" /* git_host_cred_get — per-host token */

#include "cJSON.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Declared in agent_exec.h; forward-declared here to keep this small module free
 * of the heavy agent header chain (which pulls generated headers). Signature must
 * stay in lockstep with agent_exec.h. */
int agent_http_get(const char *url, const char *extra_headers, char **response_buf, int timeout_ms);

#define ORG_PER_PAGE    100 /* GitHub / GitLab / Bitbucket page size */
#define GITEA_LIMIT     50  /* Gitea caps `limit` at 50 */
#define MAX_PAGES       20
#define MAX_REPOS       500
#define HTTP_TIMEOUT_MS 20000

git_org_provider_t git_org_detect(const char *host)
{
   if (!host || !host[0])
      return GIT_ORG_UNKNOWN;
   /* Case-insensitive substring hints; default self-hosted to the Gitea shape. */
   char h[256];
   size_t i = 0;
   for (; host[i] && i < sizeof(h) - 1; i++)
      h[i] = (char)tolower((unsigned char)host[i]);
   h[i] = '\0';
   if (strstr(h, "github"))
      return GIT_ORG_GITHUB;
   if (strstr(h, "gitlab"))
      return GIT_ORG_GITLAB;
   if (strstr(h, "bitbucket"))
      return GIT_ORG_BITBUCKET;
   return GIT_ORG_GITEA;
}

const char *git_org_provider_name(git_org_provider_t p)
{
   switch (p)
   {
   case GIT_ORG_GITHUB:
      return "github";
   case GIT_ORG_GITLAB:
      return "gitlab";
   case GIT_ORG_GITEA:
      return "gitea";
   case GIT_ORG_BITBUCKET:
      return "bitbucket";
   default:
      return "unknown";
   }
}

/* Append {name, clone_url, ssh_url, private} to `out`, skipping an entry missing a
 * name or an HTTPS clone URL. Returns 1 if appended, else 0. */
static int append_repo(cJSON *out, const char *name, const char *clone_url, const char *ssh_url,
                       int is_private)
{
   if (!name || !name[0] || !clone_url || !clone_url[0])
      return 0;
   cJSON *o = cJSON_CreateObject();
   if (!o)
      return 0;
   cJSON_AddStringToObject(o, "name", name);
   cJSON_AddStringToObject(o, "clone_url", clone_url);
   if (ssh_url && ssh_url[0])
      cJSON_AddStringToObject(o, "ssh_url", ssh_url);
   cJSON_AddBoolToObject(o, "private", is_private ? 1 : 0);
   cJSON_AddItemToArray(out, o);
   return 1;
}

static const char *jstr(const cJSON *obj, const char *key)
{
   const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
   return (cJSON_IsString(v) && v->valuestring) ? v->valuestring : NULL;
}

/* GitHub + Gitea share the same repo object shape (name/clone_url/ssh_url/private). */
static int parse_github_like(const cJSON *arr, cJSON *out)
{
   if (!cJSON_IsArray(arr))
      return -1;
   int n = 0;
   const cJSON *it = NULL;
   cJSON_ArrayForEach(it, arr)
   {
      const cJSON *priv = cJSON_GetObjectItemCaseSensitive(it, "private");
      n += append_repo(out, jstr(it, "name"), jstr(it, "clone_url"), jstr(it, "ssh_url"),
                       cJSON_IsTrue(priv));
   }
   return n;
}

/* GitLab projects: http_url_to_repo / ssh_url_to_repo, visibility != "public". */
static int parse_gitlab(const cJSON *arr, cJSON *out)
{
   if (!cJSON_IsArray(arr))
      return -1;
   int n = 0;
   const cJSON *it = NULL;
   cJSON_ArrayForEach(it, arr)
   {
      const char *vis = jstr(it, "visibility");
      const char *name = jstr(it, "path");
      if (!name)
         name = jstr(it, "name");
      n += append_repo(out, name, jstr(it, "http_url_to_repo"), jstr(it, "ssh_url_to_repo"),
                       vis ? strcmp(vis, "public") != 0 : 0);
   }
   return n;
}

/* Bitbucket: { values: [ { name, is_private, links: { clone: [ {name,href} ] } } ] }. */
static int parse_bitbucket(const cJSON *root, cJSON *out)
{
   const cJSON *values = cJSON_GetObjectItemCaseSensitive(root, "values");
   if (!cJSON_IsArray(values))
      return -1;
   int n = 0;
   const cJSON *it = NULL;
   cJSON_ArrayForEach(it, values)
   {
      const cJSON *links = cJSON_GetObjectItemCaseSensitive(it, "links");
      const cJSON *clone = links ? cJSON_GetObjectItemCaseSensitive(links, "clone") : NULL;
      const char *https = NULL, *ssh = NULL;
      const cJSON *ln = NULL;
      cJSON_ArrayForEach(ln, clone)
      {
         const char *nm = jstr(ln, "name");
         const char *href = jstr(ln, "href");
         if (nm && href && strcmp(nm, "https") == 0)
            https = href;
         else if (nm && href && strcmp(nm, "ssh") == 0)
            ssh = href;
      }
      const cJSON *priv = cJSON_GetObjectItemCaseSensitive(it, "is_private");
      n += append_repo(out, jstr(it, "name"), https, ssh, cJSON_IsTrue(priv));
   }
   return n;
}

int git_org_parse(git_org_provider_t p, const char *json, cJSON *out)
{
   if (!json || !out)
      return -1;
   cJSON *root = cJSON_Parse(json);
   if (!root)
      return -1;
   int n;
   switch (p)
   {
   case GIT_ORG_GITLAB:
      n = parse_gitlab(root, out);
      break;
   case GIT_ORG_BITBUCKET:
      n = parse_bitbucket(root, out);
      break;
   case GIT_ORG_GITHUB:
   case GIT_ORG_GITEA:
   default:
      n = parse_github_like(root, out);
      break;
   }
   cJSON_Delete(root);
   return n;
}

/* An owner/host slug is interpolated into a URL + headers, so validate strictly:
 * host allows alnum, '.', '-', ':'; owner allows alnum, '.', '_', '-'. */
static int host_ok(const char *s)
{
   if (!s || !s[0] || strlen(s) > 200)
      return 0;
   for (const char *p = s; *p; p++)
      if (!(isalnum((unsigned char)*p) || *p == '.' || *p == '-' || *p == ':'))
         return 0;
   return 1;
}
static int owner_ok(const char *s)
{
   if (!s || !s[0] || strlen(s) > 128)
      return 0;
   for (const char *p = s; *p; p++)
      if (!(isalnum((unsigned char)*p) || *p == '.' || *p == '_' || *p == '-'))
         return 0;
   return 1;
}

/* Fetch + parse every page for one endpoint whose URL up to (but not including)
 * the page number is `url_base` (already carrying a '?...' query with the page-size
 * param). Appends to `out`. Returns 200 (ok, appended), 404 (endpoint absent and
 * nothing appended), 401 (unauthorized), or 502 (upstream/parse failure). */
static int fetch_pages(git_org_provider_t p, const char *url_base, const char *pageparam,
                       int page_size, const char *headers, cJSON *out, char *err, size_t errlen)
{
   int appended_total = 0;
   for (int page = 1; page <= MAX_PAGES; page++)
   {
      char url[1200];
      if (snprintf(url, sizeof(url), "%s&%s=%d", url_base, pageparam, page) >= (int)sizeof(url))
         return appended_total > 0 ? 200 : 502;
      char *resp = NULL;
      int st = agent_http_get(url, headers, &resp, HTTP_TIMEOUT_MS);
      if (st == 404)
      {
         free(resp);
         return appended_total > 0 ? 200 : 404;
      }
      if (st == 401 || st == 403)
      {
         free(resp);
         snprintf(err, errlen, "not authorized for that owner — add a token for this host");
         return 401;
      }
      if (st != 200 || !resp)
      {
         free(resp);
         if (appended_total > 0)
            return 200;
         snprintf(err, errlen, "could not reach the git host (status %d)", st);
         return 502;
      }
      int n = git_org_parse(p, resp, out);
      free(resp);
      if (n < 0)
      {
         if (appended_total > 0)
            return 200;
         snprintf(err, errlen, "unexpected response from the git host");
         return 502;
      }
      appended_total += n;
      if (n < page_size || cJSON_GetArraySize(out) >= MAX_REPOS)
         break; /* short page ⇒ last page */
   }
   return 200;
}

int git_org_repos_list(const char *host, const char *owner, cJSON **out, char *provider,
                       size_t provider_cap, char *err, size_t errlen)
{
   if (out)
      *out = NULL;
   if (err && errlen)
      err[0] = '\0';
   if (!out || !host_ok(host) || !owner_ok(owner))
   {
      if (err)
         snprintf(err, errlen, "host and owner are required");
      return 400;
   }
   git_org_provider_t p = git_org_detect(host);

   /* Resolve the per-host token (write-only vault); enumerate unauthenticated when
    * none is stored (works for public orgs). */
   char tok[1024];
   int have_tok = git_host_cred_get(host, tok, sizeof(tok)) == 1 && tok[0];

   /* Per-provider request headers (newline-separated; agent_bridge splits on \n). */
   char headers[1400];
   switch (p)
   {
   case GIT_ORG_GITHUB:
      snprintf(headers, sizeof(headers),
               "Accept: application/vnd.github+json\nUser-Agent: aimee-server\n"
               "X-GitHub-Api-Version: 2022-11-28%s%s",
               have_tok ? "\nAuthorization: Bearer " : "", have_tok ? tok : "");
      break;
   case GIT_ORG_GITLAB:
      if (have_tok)
         snprintf(headers, sizeof(headers), "PRIVATE-TOKEN: %s", tok);
      else
         headers[0] = '\0';
      break;
   case GIT_ORG_BITBUCKET:
      snprintf(headers, sizeof(headers), "User-Agent: aimee-server%s%s",
               have_tok ? "\nAuthorization: Bearer " : "", have_tok ? tok : "");
      break;
   case GIT_ORG_GITEA:
   default:
      if (have_tok)
         snprintf(headers, sizeof(headers), "Authorization: token %s", tok);
      else
         headers[0] = '\0';
      break;
   }

   /* Build the org (primary) + user (fallback) endpoint bases + page param. */
   char org_base[900] = "", user_base[900] = "";
   const char *pageparam = "page";
   int page_size = ORG_PER_PAGE;
   switch (p)
   {
   case GIT_ORG_GITHUB:
      snprintf(org_base, sizeof(org_base), "https://api.github.com/orgs/%s/repos?per_page=%d",
               owner, ORG_PER_PAGE);
      snprintf(user_base, sizeof(user_base), "https://api.github.com/users/%s/repos?per_page=%d",
               owner, ORG_PER_PAGE);
      break;
   case GIT_ORG_GITLAB:
   {
      /* gitlab.com or a self-hosted host; both serve /api/v4. */
      snprintf(org_base, sizeof(org_base), "https://%s/api/v4/groups/%s/projects?per_page=%d", host,
               owner, ORG_PER_PAGE);
      snprintf(user_base, sizeof(user_base), "https://%s/api/v4/users/%s/projects?per_page=%d",
               host, owner, ORG_PER_PAGE);
      break;
   }
   case GIT_ORG_BITBUCKET:
      snprintf(org_base, sizeof(org_base),
               "https://api.bitbucket.org/2.0/repositories/%s?pagelen=%d", owner, ORG_PER_PAGE);
      /* Bitbucket has no org/user split. */
      break;
   case GIT_ORG_GITEA:
   default:
      page_size = GITEA_LIMIT;
      snprintf(org_base, sizeof(org_base), "https://%s/api/v1/orgs/%s/repos?limit=%d", host, owner,
               GITEA_LIMIT);
      snprintf(user_base, sizeof(user_base), "https://%s/api/v1/users/%s/repos?limit=%d", host,
               owner, GITEA_LIMIT);
      break;
   }

   cJSON *arr = cJSON_CreateArray();
   if (!arr)
   {
      snprintf(err, errlen, "out of memory");
      return 502;
   }

   int st = fetch_pages(p, org_base, pageparam, page_size, headers, arr, err, errlen);
   /* An org-endpoint 404 means the owner is likely a user (or a self-hosted user
    * account) — retry the user endpoint where the provider has one. */
   if (st == 404 && user_base[0])
      st = fetch_pages(p, user_base, pageparam, page_size, headers, arr, err, errlen);

   if (st != 200)
   {
      cJSON_Delete(arr);
      if (st == 404 && err && errlen)
         snprintf(err, errlen, "no repositories found for that owner");
      return st;
   }

   if (provider && provider_cap)
      snprintf(provider, provider_cap, "%s", git_org_provider_name(p));
   *out = arr;
   return 0;
}
