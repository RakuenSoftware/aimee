/* memory_redirect.c — see memory_redirect.h.
 *
 * v1 scope: the Claude Code file-memory surface — paths HOME-anchored under
 * ~/.claude/projects/<slug>/memory/<name>.md. A Write of such a file is stored
 * centrally and re-materialized by aimee (confined to the real memory dir); an
 * Edit/MultiEdit or a MEMORY.md write is rejected with guidance. Other clients
 * and memory surfaces are a documented v1 limitation.
 */

#include "memory_redirect.h"

#include "cli_client.h" /* cli_http_request, cli_v1_client_endpoint/bearer */
#include "harness_memory_audit.h"
#include "harness_memory_common.h"
#include "harness_memory_scope.h"
#include "harness_memory_spill.h"
#include "platform_path.h" /* platform_mkdir_p */

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h> /* strcasecmp */

#ifndef _WIN32
#include <fcntl.h>
#include <unistd.h>
#endif

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

static int is_write_family(const char *t)
{
   return t && (strcmp(t, "Write") == 0 || strcmp(t, "Edit") == 0 || strcmp(t, "MultiEdit") == 0);
}

/* Does abspath start with "<home>/<projects_root>/"? */
static int under_projects_root(const char *abspath, const char *home, const char *projects_root)
{
   char prefix[PATH_MAX];
   int n = snprintf(prefix, sizeof(prefix), "%s/%s/", home, projects_root);
   if (n < 0 || (size_t)n >= sizeof(prefix) || !abspath)
      return 0;
   return strncmp(abspath, prefix, (size_t)n) == 0;
}

static int ends_md_ci(const char *path, size_t plen)
{
   return plen >= 3 && path[plen - 3] == '.' && tolower((unsigned char)path[plen - 2]) == 'm' &&
          tolower((unsigned char)path[plen - 1]) == 'd';
}

mr_verdict_t memory_redirect_classify(const char *client, const char *tool, const char *path,
                                      const char *home, char *out_name, size_t name_cap,
                                      const char **out_reason)
{
   if (out_reason)
      *out_reason = NULL;
   const hmem_scope_t *scope = hmem_scope_for_client(client);
   if (!scope) /* unregistered client — not a memory surface we manage */
      return MR_ALLOW;
   if (!is_write_family(tool) || !path || !home || !home[0])
      return MR_ALLOW;

   /* HOME-anchored to the client's projects root — an unanchored substring would
    * false-positive on repo paths. */
   char prefix[PATH_MAX];
   int pn = snprintf(prefix, sizeof(prefix), "%s/%s/", home, scope->projects_root);
   if (pn < 0 || (size_t)pn >= sizeof(prefix) || strncmp(path, prefix, (size_t)pn) != 0)
      return MR_ALLOW;
   char memseg[80];
   int mn = snprintf(memseg, sizeof(memseg), "/%s/", scope->memory_seg);
   if (mn < 0 || (size_t)mn >= sizeof(memseg))
      return MR_ALLOW;
   /* Anchor the memseg search AFTER the confirmed prefix so a "/memory/" inside
    * HOME/projects_root can't be mistaken for the project's memory dir. */
   const char *mem = strstr(path + pn, memseg);
   if (!mem)
      return MR_ALLOW;
   size_t plen = strlen(path);
   if (!ends_md_ci(path, plen))
      return MR_ALLOW;

   const char *base = strrchr(path, '/');
   base = base ? base + 1 : path;
   if (strcasecmp(base, "MEMORY.md") == 0)
   {
      if (out_reason)
         *out_reason = "MEMORY.md is auto-rendered from your memory entries; to add or change "
                       "one, Write a file under memory/<name>.md.";
      return MR_REJECT;
   }
   if (strcmp(tool, "Write") != 0)
   {
      if (out_reason)
         *out_reason = "Memory files are managed by aimee; use Write to replace the whole file "
                       "rather than Edit.";
      return MR_REJECT;
   }

   const char *after = mem + strlen(memseg);
   size_t alen = strlen(after);
   if (alen <= 3 || after[0] == '/') /* just ".md", or an odd leading slash */
      return MR_ALLOW;
   /* Reject traversal in the derived name before it reaches the store. */
   if (strstr(after, "../") || strstr(after, "/.."))
   {
      if (out_reason)
         *out_reason = "Invalid memory path (no '..' segments).";
      return MR_REJECT;
   }
   snprintf(out_name, name_cap, "%.*s", (int)(alen - 3), after); /* strip ".md" */
   return MR_REDIRECT;
}

static const char *json_str(cJSON *o, const char *key)
{
   cJSON *i = cJSON_GetObjectItemCaseSensitive(o, key);
   return (i && cJSON_IsString(i)) ? i->valuestring : NULL;
}

/* Re-materialize the file with aimee's own I/O (never an agent tool — so it
 * cannot re-enter the PreToolUse hook). The parent dir is realpath-resolved and
 * confined under ~/.claude/projects/, so a symlinked component can't redirect
 * the write outside the memory tree. Atomic (temp + rename) on POSIX. */
int memory_redirect_rematerialize(const char *path, const char *content, const char *home,
                                  const char *projects_root)
{
   const char *base = strrchr(path, '/');
   if (!base)
      return -1;
   base++;
   char dir[PATH_MAX];
   int dn = (int)(base - 1 - path);
   snprintf(dir, sizeof(dir), "%.*s", dn, path);
   size_t len = strlen(content);

   /* Confine BEFORE any on-disk side effect: the parent must be syntactically
    * under <home>/<projects_root>/ with no ".." segment. This gates the mkdir
    * itself (so a bad path can't create dirs outside the memory tree) and is the
    * ONLY confinement on Windows, where the realpath gate below is compiled out.
    * On POSIX the realpath()+under_projects_root check additionally defeats a
    * symlinked component. */
   if (!under_projects_root(dir, home, projects_root) || strstr(dir, ".."))
      return -1;
   /* Create the memory dir tree if absent so a first write to a fresh project
    * materializes. */
   platform_mkdir_p(dir, 0700);

#ifndef _WIN32
   char rp[PATH_MAX];
   if (!realpath(dir, rp)) /* parent must exist + resolve */
      return -1;
   if (!under_projects_root(rp, home, projects_root)) /* symlink escape — refuse */
      return -1;
   char target[PATH_MAX], tmpl[PATH_MAX];
   if ((size_t)snprintf(target, sizeof(target), "%s/%s", rp, base) >= sizeof(target) ||
       (size_t)snprintf(tmpl, sizeof(tmpl), "%s/.hmem_tmp_XXXXXX", rp) >= sizeof(tmpl))
      return -1;
   int fd = mkstemp(tmpl);
   if (fd < 0)
      return -1;
   ssize_t w = write(fd, content, len);
   fsync(fd);
   close(fd);
   if (w < 0 || (size_t)w != len || rename(tmpl, target) != 0)
   {
      unlink(tmpl);
      return -1;
   }
   return 0;
#else
   (void)home;
   (void)projects_root;
   FILE *f = fopen(path, "wb");
   if (!f)
      return -1;
   size_t w = fwrite(content, 1, len, f);
   fclose(f);
   return (w == len) ? 0 : -1;
#endif
}

/* Any write operator in [a,b)? '>' (covers >, >>, 2>, &>) is only counted
 * OUTSIDE single/double quotes and escapes, so quoted data like grep 'a>b' is
 * not mistaken for a redirection. Plus a best-effort write-tool keyword list. */
static int region_has_write_op(const char *a, const char *b)
{
   int sq = 0, dq = 0;
   for (const char *p = a; p < b; p++)
   {
      char c = *p;
      if (c == '\\' && p + 1 < b)
      {
         p++;
         continue;
      }
      if (!dq && c == '\'')
         sq = !sq;
      else if (!sq && c == '"')
         dq = !dq;
      else if (!sq && !dq && c == '>')
         return 1;
   }
   static const char *const kw[] = {"tee", "sed -i",   "dd of=",  "truncate ", "cp ",
                                    "mv ", "install ", "perl -i", "perl -pi",  "patch "};
   for (size_t i = 0; i < sizeof(kw) / sizeof(kw[0]); i++)
   {
      size_t kl = strlen(kw[i]);
      for (const char *p = a; p + kl <= b; p++)
         if (strncmp(p, kw[i], kl) == 0)
            return 1;
   }
   return 0;
}

int memory_redirect_bash_targets_memory(const char *client, const char *command, const char *home)
{
   const hmem_scope_t *scope = hmem_scope_for_client(client);
   if (!scope || !command || !home || !home[0])
      return 0;
   char prefix[PATH_MAX], memseg[80];
   if ((size_t)snprintf(prefix, sizeof(prefix), "%s/%s/", home, scope->projects_root) >=
       sizeof(prefix))
      return 0;
   if ((size_t)snprintf(memseg, sizeof(memseg), "/%s/", scope->memory_seg) >= sizeof(memseg))
      return 0;
   size_t mslen = strlen(memseg);

   const char *p = command;
   while ((p = strstr(p, prefix)))
   {
      const char *ts = p; /* path-token start */
      const char *end = ts;
      while (*end && !strchr(" \t;|&\n\"'`)<>", *end))
         end++;
      size_t tlen = (size_t)(end - ts);
      int has_mem = 0;
      for (const char *q = ts; q + mslen <= end; q++)
         if (strncmp(q, memseg, mslen) == 0)
         {
            has_mem = 1;
            break;
         }
      int ends_md = tlen >= 3 && end[-3] == '.' && (end[-2] == 'm' || end[-2] == 'M') &&
                    (end[-1] == 'd' || end[-1] == 'D');
      if (has_mem && ends_md)
      {
         /* start of this simple command (after the last separator before the path) */
         const char *cs = ts;
         while (cs > command && !strchr(";|&\n", cs[-1]))
            cs--;
         if (region_has_write_op(cs, ts))
            return 1;
      }
      p = (end > p) ? end : p + 1;
   }
   return 0;
}

int memory_redirect_check(const char *tool, cJSON *root, const char *cwd, const char *project_hint,
                          int md_retire, char *msg, size_t msg_len)
{
   if (!root)
      return 0;
   const char *client = getenv("AIMEE_HOOK_CLIENT");
   const char *home = getenv("HOME");
   if (!home)
      return 0;

   /* Bash bypass: a shell command that writes a memory file is reject-denied —
    * we can't capture the command's output, so steer the agent to the Write
    * tool (which we intercept + store). Reads of memory files are unaffected. */
   if (tool && strcmp(tool, "Bash") == 0)
   {
      const char *cmd = json_str(root, "command");
      if (cmd && memory_redirect_bash_targets_memory(client, cmd, home))
      {
         snprintf(msg, msg_len,
                  "Memory files are managed by aimee — use the Write tool to set "
                  "memory/<name>.md, not shell redirection.");
         hmem_audit("reject", NULL, NULL, "bash-write-memory");
         return 2;
      }
      return 0;
   }

   const char *path = json_str(root, "file_path");
   if (!path)
      path = json_str(root, "path");
   if (!path)
      return 0;

   char name[512];
   const char *reason = NULL;
   mr_verdict_t v = memory_redirect_classify(client, tool, path, home, name, sizeof(name), &reason);
   if (v == MR_ALLOW)
      return 0;
   if (v == MR_REJECT)
   {
      snprintf(msg, msg_len, "%s", reason ? reason : "memory write rejected");
      hmem_audit("reject", NULL, NULL, reason);
      return 2;
   }

   /* MR_REDIRECT. A Write with no string `content` is invalid input — never
    * upsert an empty body over an existing entry; reject it. */
   const char *content = json_str(root, "content");
   if (!content)
   {
      snprintf(msg, msg_len, "Memory Write needs a string 'content' field.");
      return 2;
   }
   /* Prefer the client-resolved project key (correct for a remote server, whose
    * filesystem lacks the client's cwd/git repo); validate it first since it
    * arrives over the wire, then fall back to resolving from cwd for a local
    * server or an older client that doesn't send the hint. */
   char project[HMEM_PROJECT_KEY_MAX], rootdir[PATH_MAX];
   if (project_hint && hmem_project_key_ok(project_hint))
   {
      snprintf(project, sizeof(project), "%s", project_hint);
   }
   else if (hmem_resolve_project(cwd, project, sizeof(project), rootdir, sizeof(rootdir)) != 0)
   {
      return 0; /* can't identify the project — fail open */
   }

   /* .md retirement (config flag memory_md_retire, default-on): push the
    * intercepted write into aimee's db1 memory as a private, non-recallable
    * archive row and do NOT re-materialize the .md — the file never exists,
    * content lives only in aimee. The agent retrieves via `aimee memory search`
    * and is steered to `aimee memory` by the session brief. Fail-open (spill) on
    * store outage. */
   {
      if (md_retire)
      {
         /* Project-qualified: identically-named memories from different projects
          * must not collide under this user's UNIQUE(kind,key). */
         char key[HMEM_PROJECT_KEY_MAX + 600];
         snprintf(key, sizeof(key), "archive:%s/%s", project, name);
         cJSON *ub = cJSON_CreateObject();
         cJSON_AddStringToObject(ub, "kind", "archive");
         cJSON_AddStringToObject(ub, "tier", "L1");
         cJSON_AddStringToObject(ub, "key", key);
         cJSON_AddStringToObject(ub, "content", content);
         char *ub_s = cJSON_PrintUnformatted(ub);
         cJSON_Delete(ub);
         char *ep = cli_v1_client_endpoint();
         char *br = cli_v1_client_bearer();
         int st = 0;
         cJSON *r = (ep && ub_s) ? cli_http_request(ep, "POST", "/v1/memory/user_capture", ub_s, br,
                                                    5000, &st)
                                 : NULL;
         free(ub_s);
         free(ep);
         free(br);
         if (!r || st < 200 || st >= 300)
         {
            if (r)
               cJSON_Delete(r);
            int sp = hmem_spill_write(project, name, "archive", content);
            /* Storage-neutral wording: this file links into the DB-free client,
             * whose build-integrity boundary forbids db1/db2 string leaks. */
            hmem_audit(sp == 0 ? "spill" : "spill-failed", project, name, "store unreachable");
            fprintf(stderr, "aimee: memory store unavailable (status %d); %s\n", st,
                    sp == 0 ? "spilled for reconcile" : "spill FAILED — allowing local write");
            return 0; /* fail-open: allow the local write this once */
         }
         cJSON_Delete(r);
         hmem_audit("redirect-store", project, name, NULL);
         /* No re-materialize: the .md is retired and never created. */
         snprintf(msg, msg_len,
                  "Saved to aimee memory. Memory files are retired — retrieve with "
                  "`aimee memory search` and use `aimee memory store` going forward.");
         return 2;
      }
   }

   cJSON *body = cJSON_CreateObject();
   cJSON_AddStringToObject(body, "project", project);
   cJSON_AddStringToObject(body, "name", name);
   cJSON_AddStringToObject(body, "type", "fact");
   cJSON_AddStringToObject(body, "body", content);
   cJSON_AddStringToObject(body, "client", (client && client[0]) ? client : "claude");
   char *body_s = cJSON_PrintUnformatted(body);
   cJSON_Delete(body);

   char *endpoint = cli_v1_client_endpoint();
   char *bearer = cli_v1_client_bearer();
   int status = 0;
   cJSON *resp = (endpoint && body_s)
                     ? cli_http_request(endpoint, "POST", "/v1/harness_memory/upsert", body_s,
                                        bearer, 5000, &status)
                     : NULL;
   free(body_s);
   free(endpoint);
   free(bearer);

   if (!resp || status < 200 || status >= 300)
   {
      /* Fail-open: spill the intended content so the next session-start reconcile
       * replays it into the store, and let the agent write its own file this once.
       * Never block the agent on our outage. */
      if (resp)
         cJSON_Delete(resp);
      int sp = hmem_spill_write(project, name, "fact", content);
      hmem_audit(sp == 0 ? "spill" : "spill-failed", project, name, "store unreachable");
      fprintf(stderr, "aimee: harness-memory store unavailable (status %d); %s\n", status,
              sp == 0 ? "spilled for reconcile" : "spill FAILED — allowing local write");
      return 0;
   }

   cJSON *jid = cJSON_GetObjectItemCaseSensitive(resp, "id");
   long id = cJSON_IsNumber(jid) ? (long)jid->valuedouble : 0;
   cJSON_Delete(resp);

   const hmem_scope_t *scope = hmem_scope_for_client(client); /* non-NULL: classify redirected */
   memory_redirect_rematerialize(path, content, home,
                                 scope ? scope->projects_root : ".claude/projects");
   hmem_audit("redirect", project, name, NULL);
   snprintf(msg, msg_len,
            "Saved to aimee memory (id=%ld). The file now reflects your content — do not "
            "re-write it.",
            id);
   return 2;
}
