/* git_project.c — clone a repo into a webuser's scoped workspace. See header.
 *
 * Org-scoped layout (webchat project lifecycle proposal, slice 1): clones land
 * at <user root>/<org>/<repo> where the org derives from the clone URL's owner
 * path (or an explicit override), the project ref is "<org>/<repo>", and the
 * clone is registered in the deployment-global key registry under the
 * first-component lifecycle lock before it is published (temp dir + rename,
 * fd-pinned via openat2 — no string-path window). */
#include "git_project.h"
#include "cJSON.h"           /* kb purge reply parsing (delete path) */
#include "config.h"          /* kb_purge_fence_ttl_s (heartbeat cadence) */
#include "git_cred_inject.h" /* git_cred_inject_build_env_for_repo / _free_env */
#include "git_host_cred.h"   /* per-host token store (single-user, many hosts) */
#include "kb_client.h"       /* kb purge/finalize/cancel wrappers (slice 2) */
#include "log.h"
#include "util.h"            /* safe_exec_capture_env */
#include "util_url.h"        /* util_url_normalize (ssh/scp/git -> https) */
#include "workspace_scope.h" /* ws_scope_* */
#include "ws_registry.h"     /* lifecycle lock + key registry */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "db2/code_index.h" /* db2_code_index_project_delete (slice-2 purge fan-out) */

extern char **environ;

#define GP_PATH_MAX 4096

/* Derive a valid single-component repo name from `name_in` (if non-empty) or
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

/* Sanitize a candidate org component to the ws_scope_name_valid alphabet:
 * invalid bytes -> '-', runs collapsed, leading/trailing '-'/'.' trimmed.
 * Returns 0 iff the FULL sanitized result fits WS_REF_COMP_MAX and passes
 * ws_scope_name_valid — an over-long org is REJECTED, never truncated (two
 * distinct overrides must never collapse onto one identity). */
static int sanitize_org(const char *in, char *out, size_t cap)
{
   if (!in || !in[0] || cap == 0)
      return -1;
   size_t o = 0;
   int last_dash = 0;
   for (const char *p = in; *p; p++)
   {
      unsigned char c = (unsigned char)*p;
      int ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
               c == '.' || c == '_';
      if (ok)
      {
         if (o >= WS_REF_COMP_MAX || o + 1 >= cap)
            return -1; /* sanitized form overflows the component cap */
         out[o++] = (char)c;
         last_dash = 0;
      }
      else if (!last_dash && o > 0) /* collapse runs; no leading '-' */
      {
         if (o >= WS_REF_COMP_MAX || o + 1 >= cap)
            return -1;
         out[o++] = '-';
         last_dash = 1;
      }
   }
   while (o > 0 && (out[o - 1] == '-' || out[o - 1] == '.'))
      o--;
   out[o] = '\0';
   return (o > 0 && ws_scope_name_valid(out)) ? 0 : -1;
}

int git_project_canonical_remote(const char *url, char *out, size_t cap)
{
   if (!url || !url[0] || !out || cap == 0)
      return -1;
   char *norm = util_url_normalize(url);
   const char *u = norm ? norm : url;
   /* Strip userinfo: scheme://user:pass@host/... -> scheme://host/... — the
    * canonical remote is CREDENTIAL-FREE by contract; it is stored in sidecars
    * and echoed in responses/logs. */
   const char *scheme_end = strstr(u, "://");
   char buf[GP_PATH_MAX];
   if (scheme_end)
   {
      const char *host = scheme_end + 3;
      const char *slash = strchr(host, '/');
      const char *at = memchr(host, '@', slash ? (size_t)(slash - host) : strlen(host));
      if (at)
         snprintf(buf, sizeof(buf), "%.*s%s", (int)(scheme_end + 3 - u), u, at + 1);
      else
         snprintf(buf, sizeof(buf), "%s", u);
   }
   else
      snprintf(buf, sizeof(buf), "%s", u);
   free(norm);
   /* Strip query/fragment and a trailing ".git" / trailing slashes. */
   buf[strcspn(buf, "?#")] = '\0';
   size_t bl = strlen(buf);
   while (bl > 0 && buf[bl - 1] == '/')
      buf[--bl] = '\0';
   if (bl > 4 && strcmp(buf + bl - 4, ".git") == 0)
      buf[bl - 4] = '\0';
   if (!buf[0] || strchr(buf, ' ') || strchr(buf, '\n'))
      return -1;
   snprintf(out, cap, "%s", buf);
   return 0;
}

int git_project_derive_org(const char *url, char *out, size_t cap, int *multi_segment)
{
   if (multi_segment)
      *multi_segment = 0;
   if (!url || !out || cap == 0)
      return -1;
   out[0] = '\0';
   char remote[GP_PATH_MAX];
   if (git_project_canonical_remote(url, remote, sizeof(remote)) != 0)
      return -1;
   const char *scheme_end = strstr(remote, "://");
   if (!scheme_end)
      return -1; /* local path etc. — no derivable owner */
   const char *path = strchr(scheme_end + 3, '/');
   if (!path)
      return -1;
   path++; /* first owner segment */
   const char *last_slash = strrchr(path, '/');
   if (!last_slash || last_slash == path)
      return -1; /* no owner segment at all: https://host/repo */
   /* owner path = [path, last_slash); multi-segment (subgroups) bails out —
    * a joined org is lossy; the caller may pass an explicit override. */
   char owner[GP_PATH_MAX];
   size_t ol = (size_t)(last_slash - path);
   if (ol >= sizeof(owner))
      return -1;
   memcpy(owner, path, ol);
   owner[ol] = '\0';
   if (strchr(owner, '/'))
   {
      if (multi_segment)
         *multi_segment = 1;
      return -1;
   }
   return sanitize_org(owner, out, cap) == 0 ? 0 : -1;
}

int git_project_org_candidates(const char *url, char *out, size_t cap)
{
   if (!url || !out || cap == 0)
      return -1;
   out[0] = '\0';
   char remote[GP_PATH_MAX];
   if (git_project_canonical_remote(url, remote, sizeof(remote)) != 0)
      return -1;
   const char *scheme_end = strstr(remote, "://");
   if (!scheme_end)
      return -1;
   const char *path = strchr(scheme_end + 3, '/');
   if (!path)
      return -1;
   path++;
   const char *last_slash = strrchr(path, '/');
   if (!last_slash || last_slash == path)
      return -1;
   /* Sanitize each owner segment independently and join with ", ". */
   size_t o = 0;
   const char *seg = path;
   while (seg < last_slash)
   {
      const char *end = memchr(seg, '/', (size_t)(last_slash - seg));
      if (!end)
         end = last_slash;
      char raw[WS_REF_COMP_MAX * 2], clean[WS_REF_COMP_MAX + 1];
      size_t sl = (size_t)(end - seg);
      if (sl > 0 && sl < sizeof(raw))
      {
         memcpy(raw, seg, sl);
         raw[sl] = '\0';
         if (sanitize_org(raw, clean, sizeof(clean)) == 0)
         {
            int n = snprintf(out + o, cap - o, "%s%s", o ? ", " : "", clean);
            if (n < 0 || (size_t)n >= cap - o)
               break;
            o += (size_t)n;
         }
      }
      seg = end + 1;
   }
   return o > 0 ? 0 : -1;
}

/* Recursively remove `name` (a directory tree) under `parentfd`. Every descent
 * uses O_NOFOLLOW — directory symlinks are unlinked as entries, never
 * followed. Best-effort; returns 0 when the entry is gone. `tick` (optional)
 * is a progress hook invoked per entry: a non-zero return STOPS the walk
 * immediately (the delete path uses it to heartbeat the purge fence and to
 * bail when fence ownership is lost — the partial tree is retried by
 * re-running the idempotent delete). */
static int rm_rf_at_tick(int parentfd, const char *name, int (*tick)(void *), void *tick_ctx)
{
   if (tick && tick(tick_ctx) != 0)
      return -1;
   int fd = openat(parentfd, name, O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
   if (fd < 0)
      return unlinkat(parentfd, name, 0) == 0 || errno == ENOENT ? 0 : -1;
   DIR *d = fdopendir(fd);
   if (!d)
   {
      close(fd);
      return -1;
   }
   int stopped = 0;
   struct dirent *e;
   while ((e = readdir(d)) != NULL)
   {
      if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
         continue;
      if (tick && tick(tick_ctx) != 0)
      {
         stopped = 1;
         break;
      }
      struct stat st;
      if (fstatat(fd, e->d_name, &st, AT_SYMLINK_NOFOLLOW) != 0)
         continue;
      if (S_ISDIR(st.st_mode))
      {
         if (rm_rf_at_tick(fd, e->d_name, tick, tick_ctx) != 0 && tick && tick(tick_ctx) != 0)
         {
            stopped = 1;
            break;
         }
      }
      else
         unlinkat(fd, e->d_name, 0);
   }
   closedir(d); /* closes fd */
   if (stopped)
      return -1;
   return unlinkat(parentfd, name, AT_REMOVEDIR) == 0 ? 0 : -1;
}

static int rm_rf_at(int parentfd, const char *name)
{
   return rm_rf_at_tick(parentfd, name, NULL, NULL);
}

/* Does `name` under `dirfd` exist as a dir containing a .git entry (i.e. a
 * published flat project)? 0/1. */
static int is_published_project_at(int dirfd, const char *name)
{
   int fd = openat(dirfd, name, O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
   if (fd < 0)
      return 0;
   struct stat st;
   int has_git = fstatat(fd, ".git", &st, AT_SYMLINK_NOFOLLOW) == 0;
   close(fd);
   return has_git;
}

int git_project_clone(const char *principal, const char *url, const char *name, const char *org_in,
                      const char *token, char *out_path, size_t path_cap, char *out_ref,
                      size_t ref_cap, char *err, size_t errlen)
{
   if (err && errlen)
      err[0] = '\0';
   if (out_path && path_cap)
      out_path[0] = '\0';
   if (out_ref && ref_cap)
      out_ref[0] = '\0';

   if (!principal || strncmp(principal, "webuser:", 8) != 0)
   {
      snprintf(err, errlen, "git projects require a webchat user");
      return -1;
   }
   if (!ws_scope_openat2_available())
   {
      snprintf(err, errlen,
               "the webuser project surface requires openat2 (Linux >= 5.6); it is disabled on "
               "this kernel");
      return -1;
   }
   if (!ws_reg_ready())
   {
      snprintf(err, errlen, "the project registry is unavailable; try again shortly");
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

   char repo[WS_REF_COMP_MAX + 1];
   if (derive_name(url, name, repo, sizeof(repo)) != 0)
   {
      snprintf(err, errlen, "could not derive a valid project name");
      return -1;
   }

   /* Org: explicit override wins; else derived ONCE from the URL owner path
    * (multi-segment owners bail to flat); else flat fallback. The single
    * sanitized value is used for the conflict checks, the registry, the
    * layout, and the ref — nothing re-derives it. */
   char org[WS_REF_COMP_MAX + 1] = "";
   if (org_in && org_in[0])
   {
      if (sanitize_org(org_in, org, sizeof(org)) != 0)
      {
         snprintf(err, errlen, "invalid org override");
         return -1;
      }
   }
   else
      (void)git_project_derive_org(url, org, sizeof(org), NULL); /* flat on failure */

   char ref[WS_REF_MAX + 1];
   if (org[0])
      snprintf(ref, sizeof(ref), "%s/%s", org, repo);
   else
      snprintf(ref, sizeof(ref), "%s", repo);

   char remote[GP_PATH_MAX];
   if (git_project_canonical_remote(url, remote, sizeof(remote)) != 0)
   {
      snprintf(err, errlen, "could not derive a canonical remote from the URL");
      return -1;
   }

   /* ---- lifecycle-locked section: conflict checks -> register -> create ----
    * The first-component lock serializes flat/org namespace conflicts,
    * registration, publication, deletion, and org pruning deployment-wide. */
   int lockfd = ws_reg_lock(ref);
   if (lockfd < 0)
   {
      snprintf(err, errlen, "could not acquire the project lifecycle lock");
      return -1;
   }
   int registered = 0, rootfd = -1, orgfd = -1, tmpfd = -1;
   char tmpname[WS_REF_COMP_MAX + 32];
   tmpname[0] = '\0';
   int rc_final = -1;

   rootfd = ws_scope_open_user_root(principal);
   if (rootfd < 0)
   {
      snprintf(err, errlen, "could not open the workspace root");
      goto out;
   }

   /* Caller's own tree: the target must not exist (as anything). */
   {
      struct stat st;
      const char *probe = org[0] ? org : repo;
      if (org[0])
      {
         int ofd = ws_scope_openat2_dir(rootfd, org);
         if (ofd >= 0)
         {
            int exists = fstatat(ofd, repo, &st, AT_SYMLINK_NOFOLLOW) == 0;
            close(ofd);
            if (exists)
            {
               rc_final = GP_ERR_CONFLICT;
               snprintf(err, errlen, "project '%s' already exists", ref);
               goto out;
            }
         }
         /* flat/org namespace conflict: org name taken by a flat project */
         if (is_published_project_at(rootfd, org))
         {
            rc_final = GP_ERR_CONFLICT;
            snprintf(err, errlen, "a project named '%s' already exists; the org name is taken",
                     org);
            goto out;
         }
      }
      else
      {
         if (fstatat(rootfd, probe, &st, AT_SYMLINK_NOFOLLOW) == 0)
         {
            /* flat target exists — as a project (409) or as an org dir (the
             * reverse namespace conflict). */
            rc_final = GP_ERR_CONFLICT;
            if (is_published_project_at(rootfd, probe))
               snprintf(err, errlen, "project '%s' already exists", ref);
            else
               snprintf(err, errlen, "an org named '%s' already exists; pass an explicit org",
                        repo);
            goto out;
         }
      }
   }

   /* Registry: same key bound to a different remote anywhere -> generic 409
    * (no cross-principal disclosure: neither the other remote nor its holder
    * is echoed). */
   {
      char cur_remote[GP_PATH_MAX];
      int holders = 0;
      int found = ws_reg_lookup(ref, cur_remote, sizeof(cur_remote), &holders);
      if (found < 0)
      {
         snprintf(err, errlen, "project registry unavailable");
         goto out;
      }
      if (found == 1)
      {
         /* The registry entry may be stale after a holder's `git remote
          * set-url` — in EITHER direction: a stale mismatch would spuriously
          * 409 a legitimate clone, and a stale MATCH would silently join a
          * ref whose holders have since diverged. Resync this ref from the
          * holders' git configs (authoritative, under the held lock) before
          * any comparison or increment. */
         if (ws_reg_resync(ref) != 0 ||
             (found = ws_reg_lookup(ref, cur_remote, sizeof(cur_remote), &holders)) < 0)
         {
            snprintf(err, errlen, "project registry unavailable");
            goto out;
         }
         if (found == 1 && strcmp(cur_remote, remote) != 0)
         {
            rc_final = GP_ERR_CONFLICT;
            snprintf(err, errlen,
                     "ref '%s' is already bound to a different remote; pass a distinct org", ref);
            goto out;
         }
      }
   }
   {
      int reg_rc = ws_reg_register(ref, remote);
      if (reg_rc == 1)
      {
         rc_final = GP_ERR_CONFLICT;
         snprintf(err, errlen,
                  "ref '%s' is already bound to a different remote; pass a distinct org", ref);
         goto out;
      }
      if (reg_rc != 0)
      {
         snprintf(err, errlen, "could not register the project key");
         goto out;
      }
   }
   registered = 1;

   /* Create: org dir (mkdirat + openat2 re-open), temp clone dir, clone with
    * the destination pinned by fd, sidecar, then atomic rename-publish. */
   if (org[0])
   {
      if (mkdirat(rootfd, org, 0700) != 0 && errno != EEXIST)
      {
         snprintf(err, errlen, "could not create the org directory");
         goto out;
      }
      orgfd = ws_scope_openat2_dir(rootfd, org);
      if (orgfd < 0)
      {
         snprintf(err, errlen, "could not open the org directory");
         goto out;
      }
      struct stat ost;
      if (fstat(orgfd, &ost) != 0 || !S_ISDIR(ost.st_mode) || ost.st_uid != geteuid())
      {
         snprintf(err, errlen, "org directory failed the ownership check");
         goto out;
      }
   }
   int destfd_parent = org[0] ? orgfd : rootfd;
   snprintf(tmpname, sizeof(tmpname), ".tmp-%s-%d", repo, (int)getpid());
   (void)rm_rf_at(destfd_parent, tmpname); /* stale leftover from a crash */
   if (mkdirat(destfd_parent, tmpname, 0700) != 0)
   {
      snprintf(err, errlen, "could not create the clone staging directory");
      tmpname[0] = '\0';
      goto out;
   }
   tmpfd = ws_scope_openat2_dir(destfd_parent, tmpname);
   if (tmpfd < 0)
   {
      snprintf(err, errlen, "could not open the clone staging directory");
      goto out;
   }

   /* Resolve the git credential vault-first for THIS repo's host (single-user
    * server, many hosts/providers), via the one shared policy. Precedence: an
    * inline token from the request (then stored for the host) > the host's
    * already-stored vault token > the principal's own vaulted token > the
    * server's own identity (App token / AIMEE_FORGE_TOKEN); plus the principal's
    * vaulted SSH agent. The token crosses only via GIT_ASKPASS, never argv. */
   /* FD mode (#3A.2): the HTTPS token rides an inherited memfd (token_fd), so the
    * clone child's /proc/<pid>/environ never carries it (parity with run_git). */
   {
      int token_fd = -1;
      char **envp =
          git_cred_inject_build_env_for_repo(principal, url, NULL, token, environ, &token_fd);
      /* Clone over HTTPS, never SSH: our credential model is per-host HTTPS
       * tokens (+ OAuth), and a non-interactive server has no seeded
       * known_hosts. Normalize ssh://, git@host:path, git:// to https. */
      char *clone_url_norm = util_url_normalize(url);
      const char *clone_url = clone_url_norm ? clone_url_norm : url;
      /* Destination pinned by fd: the child chdirs to /proc/self/fd/<tmpfd>
       * (pre-exec, while the fd is still open) and clones into ".", so git
       * never resolves an attacker-influencable path string. */
      char cwd[64];
      snprintf(cwd, sizeof(cwd), "/proc/self/fd/%d", tmpfd);
      const char *argv[] = {"git", "clone", "--", clone_url, ".", NULL};
      char *out = NULL;
      int rc = safe_exec_capture_cwd_env_fd_timeout(argv, cwd, envp ? envp : environ, &out, 1 << 16,
                                                    300000, token_fd,
                                                    token_fd >= 0 ? GIT_CRED_TOKEN_TARGET_FD : -1);
      if (token_fd >= 0)
         close(token_fd);
      free(clone_url_norm);
      if (envp)
         git_cred_inject_free_env(envp);

      /* A working inline token is worth keeping: persist it for the host so
       * future pulls/pushes on any repo there reuse it (no re-entry). */
      if (rc == 0 && token && token[0])
      {
         char host[GIT_HOST_MAX];
         if (git_host_from_url(url, host, sizeof(host)))
            git_host_cred_set(host, token);
      }

      struct stat hst;
      if (rc != 0 || fstatat(tmpfd, ".git/HEAD", &hst, 0) != 0)
      {
         /* Publish is conditional on git exit 0 AND a present .git/HEAD — a
          * partially-initialized repo never publishes. */
         snprintf(err, errlen, "git clone failed (rc=%d)%s%.200s", rc, out && out[0] ? ": " : "",
                  out ? out : "");
         free(out);
         goto out;
      }
      free(out);
   }

   /* Sidecar: the credential-free canonical remote, written + fsynced before
    * publish so a published project always carries its disambiguator. */
   {
      if (mkdirat(tmpfd, ".aimee", 0700) != 0 && errno != EEXIST)
      {
         snprintf(err, errlen, "could not write the project sidecar");
         goto out;
      }
      int sfd = openat(tmpfd, ".aimee/remote", O_CREAT | O_TRUNC | O_WRONLY | O_NOFOLLOW, 0600);
      if (sfd < 0)
      {
         snprintf(err, errlen, "could not write the project sidecar");
         goto out;
      }
      size_t rl = strlen(remote);
      int ok = write(sfd, remote, rl) == (ssize_t)rl && write(sfd, "\n", 1) == 1 && fsync(sfd) == 0;
      close(sfd);
      if (!ok)
      {
         snprintf(err, errlen, "could not write the project sidecar");
         goto out;
      }
   }

   /* Publish: atomic rename. A project is either fully present with its
    * sidecar or invisible (the lister skips dot-entries). */
   if (renameat(destfd_parent, tmpname, destfd_parent, repo) != 0)
   {
      snprintf(err, errlen, "could not publish the cloned project");
      goto out;
   }
   tmpname[0] = '\0'; /* published — nothing to roll back */
   rc_final = 0;

   if (out_ref && ref_cap)
      snprintf(out_ref, ref_cap, "%s", ref);
   if (out_path && path_cap)
   {
      char root[GP_PATH_MAX];
      if (ws_scope_user_root(principal, 0, root, sizeof(root)) == 0)
      {
         if (org[0])
            snprintf(out_path, path_cap, "%s/%s/%s", root, org, repo);
         else
            snprintf(out_path, path_cap, "%s/%s", root, repo);
      }
   }

out:
   /* Registration/publication is ONE transaction under the still-held lock:
    * any failure before a successful publish rolls the increment back, so
    * phantom holders cannot exist. */
   if (rc_final != 0 && tmpname[0] && destfd_parent >= 0)
      (void)rm_rf_at(destfd_parent, tmpname);
   if (rc_final != 0 && registered)
      (void)ws_reg_unregister(ref);
   if (tmpfd >= 0)
      close(tmpfd);
   if (orgfd >= 0)
      close(orgfd);
   if (rootfd >= 0)
      close(rootfd);
   close(lockfd); /* releases the flock */
   return rc_final;
}

/* ---- delete (slice 2) ---------------------------------------------------- */

/* One webuser_project_delete_audit_v1 line. Every phase of one operation
 * shares the same purge_id; `extra_fmt` (may be "") formats phase-specific
 * key=value pairs (reason=…, kb_status=…, fence_generation=…, kb=…). The
 * extra tail is built in a heap buffer when needed so the per-store kb detail
 * is NEVER truncated in the audit record. */
__attribute__((format(printf, 5, 6))) static void delete_audit(const char *purge_id,
                                                               const char *principal,
                                                               const char *ref, const char *phase,
                                                               const char *extra_fmt, ...)
{
   char stackbuf[512];
   char *extra = stackbuf;
   va_list ap;
   va_start(ap, extra_fmt);
   int need = vsnprintf(stackbuf, sizeof(stackbuf), extra_fmt, ap);
   va_end(ap);
   if (need >= (int)sizeof(stackbuf))
   {
      char *heap = malloc((size_t)need + 1);
      if (heap)
      {
         va_start(ap, extra_fmt);
         vsnprintf(heap, (size_t)need + 1, extra_fmt, ap);
         va_end(ap);
         extra = heap;
      } /* OOM: fall back to the truncated stack copy */
   }
   aimee_log(LOG_INFO, "webuser.project.delete",
             "webuser_project_delete_audit_v1 schema_version=1 purge_id=%s principal=%s ref=%s "
             "phase=%s%s%s",
             purge_id, principal, ref, phase, extra[0] ? " " : "", extra);
   if (extra != stackbuf)
      free(extra);
}

/* Mint a random hex purge id (16 bytes of /dev/urandom -> 32 hex chars;
 * time^pid fallback so the id is never empty). */
static void mint_purge_id(char *out, size_t cap)
{
   unsigned char raw[16];
   size_t got = 0;
   int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
   if (fd >= 0)
   {
      ssize_t n = read(fd, raw, sizeof(raw));
      if (n > 0)
         got = (size_t)n;
      close(fd);
   }
   if (got < sizeof(raw))
   {
      unsigned long long seed =
          (unsigned long long)time(NULL) ^ ((unsigned long long)getpid() << 32);
      for (size_t i = got; i < sizeof(raw); i++)
         raw[i] = (unsigned char)(seed >> ((i % 8) * 8));
   }
   size_t o = 0;
   for (size_t i = 0; i < sizeof(raw) && o + 2 < cap; i++)
      o += (size_t)snprintf(out + o, cap - o, "%02x", raw[i]);
   out[o] = '\0';
}

/* Parse a kb purge-wrapper reply. Returns 1 on full success ({"status":"ok",
 * "ok":true}), 0 when the kb was REACHED but a store failed ("ok":false — the
 * fence was written), -1 on transport failure ({"status":"error"} / no reply —
 * the kb was never reached, so NO fence exists). Fills *detail (malloc'd JSON:
 * the per-store map, or {"error": …}) when detail is non-NULL. */
static int purge_reply_parse(const char *json, char **detail)
{
   if (detail)
      *detail = NULL;
   cJSON *j = json ? cJSON_Parse(json) : NULL;
   if (!j)
   {
      if (detail)
         *detail = strdup("{\"error\":\"no response from the knowledge service\"}");
      return -1;
   }
   const cJSON *jstatus = cJSON_GetObjectItemCaseSensitive(j, "status");
   const char *status = cJSON_IsString(jstatus) ? jstatus->valuestring : "";
   if (strcmp(status, "ok") != 0)
   {
      const cJSON *jmsg = cJSON_GetObjectItemCaseSensitive(j, "message");
      if (detail)
      {
         cJSON *d = cJSON_CreateObject();
         cJSON_AddStringToObject(d, "error",
                                 cJSON_IsString(jmsg) ? jmsg->valuestring : "unreachable");
         *detail = cJSON_PrintUnformatted(d);
         cJSON_Delete(d);
      }
      cJSON_Delete(j);
      return -1;
   }
   const cJSON *jok = cJSON_GetObjectItemCaseSensitive(j, "ok");
   int ok = cJSON_IsBool(jok) && cJSON_IsTrue(jok);
   const cJSON *jstores = cJSON_GetObjectItemCaseSensitive(j, "stores");
   if (detail && cJSON_IsObject(jstores))
      *detail = cJSON_PrintUnformatted(jstores);
   cJSON_Delete(j);
   return ok ? 1 : 0;
}

/* Was a finalize/cancel CONFIRMED? Requires {"status":"ok","cleared":true} —
 * cleared:false is the kb's generation/purge_id-mismatch no-op (someone else
 * owns the fence now) and an absent/malformed `cleared` is NOT confirmation.
 * 0/1. */
static int purge_reply_cleared(const char *json)
{
   cJSON *j = json ? cJSON_Parse(json) : NULL;
   if (!j)
      return 0;
   const cJSON *jstatus = cJSON_GetObjectItemCaseSensitive(j, "status");
   const cJSON *jcleared = cJSON_GetObjectItemCaseSensitive(j, "cleared");
   int ok = cJSON_IsString(jstatus) && strcmp(jstatus->valuestring, "ok") == 0 &&
            cJSON_IsBool(jcleared) && cJSON_IsTrue(jcleared);
   cJSON_Delete(j);
   return ok;
}

/* The fence TTL, mirroring the kb side (JSON key kb.purge_fence_ttl_s,
 * default 900s) so both ends agree on the staleness bound. */
static int gp_fence_ttl_s(void)
{
   config_t cfg;
   if (config_load(&cfg) == 0 && cfg.kb_purge_fence_ttl_s > 0)
      return cfg.kb_purge_fence_ttl_s;
   return 900;
}

/* Heartbeat state for one delete's fence: refreshed at ~TTL/6 cadence during
 * the filesystem walk so a tree bigger than the TTL cannot let the fence
 * expire (and writers resume) mid-delete. */
typedef struct
{
   const char *ref, *generation, *purge_id;
   time_t last;    /* last heartbeat attempt */
   int interval_s; /* ~TTL/6 (150s at the 900s default) */
   int fails;      /* consecutive transport failures */
   int lost;       /* sticky: ownership lost / kb gone — stop the walk */
} gp_hb_ctx_t;

/* Send one heartbeat now. 0 = the fence is still ours; -1 = ownership lost
 * (refreshed:false — a takeover displaced this operation) or the transport
 * failed repeatedly (the fence may expire under us). Sticky via hb->lost. */
static int gp_hb_beat(gp_hb_ctx_t *hb)
{
   if (hb->lost)
      return -1;
   char *j = kb_client_purge_heartbeat_json(hb->ref, hb->generation, hb->purge_id);
   cJSON *r = j ? cJSON_Parse(j) : NULL;
   int reached = 0, refreshed = 1;
   if (r)
   {
      const cJSON *js = cJSON_GetObjectItemCaseSensitive(r, "status");
      reached = cJSON_IsString(js) && strcmp(js->valuestring, "ok") == 0;
      const cJSON *jr = cJSON_GetObjectItemCaseSensitive(r, "refreshed");
      if (cJSON_IsBool(jr))
         refreshed = cJSON_IsTrue(jr);
   }
   cJSON_Delete(r);
   free(j);
   hb->last = time(NULL);
   if (reached && refreshed)
   {
      hb->fails = 0;
      return 0;
   }
   if (reached) /* refreshed:false — a takeover owns the fence now */
   {
      hb->lost = 1;
      return -1;
   }
   if (++hb->fails >= 3) /* 3 consecutive misses at TTL/6 is still < TTL/2 */
   {
      hb->lost = 1;
      return -1;
   }
   return 0;
}

/* rm_rf_at_tick progress hook: heartbeat when the cadence elapsed; a non-zero
 * return stops the walk (fence lost). */
static int gp_hb_tick(void *ctx)
{
   gp_hb_ctx_t *hb = ctx;
   if (!hb)
      return 0;
   if (hb->lost)
      return -1;
   if (time(NULL) - hb->last < hb->interval_s)
      return 0;
   return gp_hb_beat(hb);
}

int git_project_delete(const char *principal, const char *ref, int force,
                       git_project_delete_result_t *res, char *err, size_t errlen)
{
   if (err && errlen)
      err[0] = '\0';
   if (!res)
      return -1;
   memset(res, 0, sizeof(*res));

   if (!principal || strncmp(principal, "webuser:", 8) != 0)
   {
      snprintf(err, errlen, "git projects require a webchat user");
      return -1;
   }
   if (!ws_scope_openat2_available())
   {
      snprintf(err, errlen,
               "the webuser project surface requires openat2 (Linux >= 5.6); it is disabled on "
               "this kernel");
      return -1;
   }
   if (!ws_reg_ready())
   {
      snprintf(err, errlen, "the project registry is unavailable; try again shortly");
      return -1;
   }
   /* Step 1: validate the ref, mint the purge id + a monotonic generation. */
   size_t reflen = ref ? strlen(ref) : 0;
   if (!ws_scope_project_ref_valid(ref, reflen))
   {
      snprintf(err, errlen, "invalid project ref");
      return -1;
   }
   mint_purge_id(res->purge_id, sizeof(res->purge_id));
   {
      struct timespec ts;
      clock_gettime(CLOCK_REALTIME, &ts);
      snprintf(res->generation, sizeof(res->generation), "%lld",
               (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
   }

   /* Step 2: audit intent FIRST — before any existence resolution — so a
    * cross-principal or nonexistent ref still leaves a record. */
   delete_audit(res->purge_id, principal, ref, "intent", "%s", "");

   int lockfd = ws_reg_lock(ref);
   if (lockfd < 0)
   {
      delete_audit(res->purge_id, principal, ref, "aborted", "reason=lock-unavailable");
      snprintf(err, errlen, "could not acquire the project lifecycle lock");
      return -1;
   }

   int rc_final = -1, rootfd = -1, orgfd = -1;
   int purge_ran = 0; /* a fence was written — heartbeat + finalize apply */

   /* Step 3: resolve strictly under the caller's tree; any failure is a plain
    * not-found (no existence disclosure about other principals' trees). */
   {
      int pfd = ws_scope_open_project(principal, ref, 0);
      if (pfd < 0)
      {
         delete_audit(res->purge_id, principal, ref, "aborted", "reason=not-found");
         snprintf(err, errlen, "not found");
         rc_final = GP_ERR_NOT_FOUND;
         goto out;
      }
      close(pfd);
   }

   /* Step 4: holder decision (registry-based, git config authoritative).
    * Capture the recorded remote BEFORE the decrement so an abort can roll it
    * back with ws_reg_register. */
   char reg_remote[1024];
   reg_remote[0] = '\0';
   {
      int holders = 0;
      if (ws_reg_resync(ref) != 0 ||
          ws_reg_lookup(ref, reg_remote, sizeof(reg_remote), &holders) != 1)
      {
         delete_audit(res->purge_id, principal, ref, "aborted", "reason=registry-unavailable");
         snprintf(err, errlen, "project registry unavailable");
         goto out;
      }
   }
   int remaining = ws_reg_unregister(ref);
   if (remaining < 0)
   {
      delete_audit(res->purge_id, principal, ref, "aborted", "reason=registry-unavailable");
      snprintf(err, errlen, "project registry unavailable");
      goto out;
   }

   if (remaining > 0)
   {
      /* Other holders remain: the shared knowledge and shared lexical rows
       * stay for them. An UNKNOWN-remote holder is conservatively a
       * same-remote holder (stale kb rows are recoverable; a wrong purge is
       * not) — flag the possible dangling state loudly. */
      snprintf(res->kb_status, sizeof(res->kb_status), "retained");
      if (strncmp(reg_remote, "unknown://", 10) == 0)
         aimee_log(LOG_WARN, "webuser.project.delete",
                   "ref '%s' retained for a holder with an UNKNOWN remote — kb rows may dangle; "
                   "operator can re-run purge-project after verifying (purge_id=%s)",
                   ref, res->purge_id);
   }
   else
   {
      /* Last holder: fenced kb purge. */
      char *pj = kb_client_purge_project_json(ref, res->generation, res->purge_id, 0);
      int prc = purge_reply_parse(pj, &res->kb_detail);
      free(pj);
      if (prc == 1)
      {
         snprintf(res->kb_status, sizeof(res->kb_status), "purged");
         purge_ran = 1;
      }
      else if (force)
      {
         /* Proceed anyway; the response and audit carry the FULL per-store
          * detail so the operator can re-run purge-project to convergence.
          * A transport failure means no fence exists (the kb is down, so no
          * writer is committing either) — skip heartbeat/finalize then. */
         snprintf(res->kb_status, sizeof(res->kb_status), "forced");
         purge_ran = (prc == 0);
      }
      else if (prc == -1)
      {
         /* Transport failure: the kb was never reached, so NO fence was
          * written — roll the registry decrement back and abort. Nothing
          * filesystem has been destroyed. */
         if (ws_reg_register(ref, reg_remote) != 0)
            aimee_log(LOG_ERROR, "webuser.project.delete",
                      "ref '%s': could not roll back the registry decrement after a kb transport "
                      "failure (purge_id=%s); the startup rebuild self-heals",
                      ref, res->purge_id);
         delete_audit(res->purge_id, principal, ref, "aborted", "reason=kb-unreachable kb=%s",
                      res->kb_detail ? res->kb_detail : "");
         snprintf(err, errlen, "knowledge service unavailable");
         rc_final = GP_ERR_KB_UNAVAILABLE;
         goto out;
      }
      else
      {
         /* The kb was reached but a store failed: a fence exists. The
          * decrement is reinstated ONLY if purge-cancel CONFIRMS the fence
          * rollback (cleared:true — a cleared:false mismatch no-op or a
          * failed cancel keeps the fence AND the decrement: terminal "purge
          * committed but unfinished", re-running the delete converges). */
         char *cj = kb_client_purge_cancel_json(ref, res->generation, res->purge_id);
         int cancelled = purge_reply_cleared(cj);
         free(cj);
         if (cancelled)
         {
            if (ws_reg_register(ref, reg_remote) != 0)
               aimee_log(LOG_ERROR, "webuser.project.delete",
                         "ref '%s': could not roll back the registry decrement after purge-cancel "
                         "(purge_id=%s); the startup rebuild self-heals",
                         ref, res->purge_id);
            delete_audit(res->purge_id, principal, ref, "aborted", "reason=kb-error kb=%s",
                         res->kb_detail ? res->kb_detail : "");
            snprintf(err, errlen, "knowledge service unavailable");
         }
         else
         {
            delete_audit(res->purge_id, principal, ref, "aborted",
                         "reason=purge-committed-unfinished fence_generation=%s kb=%s",
                         res->generation, res->kb_detail ? res->kb_detail : "");
            snprintf(err, errlen,
                     "purge committed but unfinished: the knowledge fence is set and the purge "
                     "must be re-run to convergence");
         }
         rc_final = GP_ERR_KB_UNAVAILABLE;
         goto out;
      }
   }

   /* Step 5: server-local lexical index rows are keyed by project (shared
    * across webusers) and follow the kb decision exactly: deleted on
    * purged/forced, kept on retained. A failure here ABORTS BEFORE any
    * filesystem removal — proceeding would strand shared index rows with no
    * retry path once the clone is gone. The clone still exists, so the
    * holder is re-registered and the fence cancelled; re-running the delete
    * converges (the kb deletes are idempotent, and a re-scan restores any
    * stores this attempt already emptied). */
   if (strcmp(res->kb_status, "retained") != 0 && db2_code_index_project_delete(ref) != 0)
   {
      int cancelled;
      if (purge_ran)
      {
         char *cj = kb_client_purge_cancel_json(ref, res->generation, res->purge_id);
         cancelled = purge_reply_cleared(cj);
         free(cj);
      }
      else
         cancelled = 1; /* forced transport failure: no fence was ever written */
      if (cancelled)
      {
         if (ws_reg_register(ref, reg_remote) != 0)
            aimee_log(LOG_ERROR, "webuser.project.delete",
                      "ref '%s': could not roll back the registry decrement after a local-index "
                      "failure (purge_id=%s); the startup rebuild self-heals",
                      ref, res->purge_id);
         delete_audit(res->purge_id, principal, ref, "aborted", "reason=local-index-failed kb=%s",
                      res->kb_detail ? res->kb_detail : "");
         snprintf(err, errlen,
                  "could not clear the server-local code index; nothing was removed — try again");
      }
      else
      {
         delete_audit(res->purge_id, principal, ref, "aborted",
                      "reason=purge-committed-unfinished fence_generation=%s kb=%s",
                      res->generation, res->kb_detail ? res->kb_detail : "");
         snprintf(err, errlen,
                  "purge committed but unfinished: the knowledge fence is set and the purge "
                  "must be re-run to convergence");
      }
      rc_final = GP_ERR_KB_UNAVAILABLE;
      goto out;
   }

   /* Heartbeat the fence before AND periodically during the filesystem walk
    * (a tree larger than the TTL must not let the fence expire — and writers
    * resume — mid-delete). Losing ownership (a takeover displaced us) or the
    * kb transport stops the walk; the partial tree is retried by re-running
    * the idempotent delete. */
   gp_hb_ctx_t hb;
   memset(&hb, 0, sizeof(hb));
   hb.ref = ref;
   hb.generation = res->generation;
   hb.purge_id = res->purge_id;
   hb.interval_s = gp_fence_ttl_s() / 6;
   if (hb.interval_s < 1)
      hb.interval_s = 1;
   if (purge_ran && gp_hb_beat(&hb) != 0)
   {
      delete_audit(res->purge_id, principal, ref, "aborted",
                   "reason=fence-lost fence_generation=%s", res->generation);
      snprintf(err, errlen,
               "purge fence ownership lost before removal; re-run the delete to convergence");
      rc_final = GP_ERR_KB_UNAVAILABLE;
      goto out;
   }

   /* Step 6: filesystem removal — an unlinkat-based walk from the pinned
    * parent fd (rm_rf_at never follows symlinks), heartbeating via the tick
    * hook. Step 7: prune the org dir when it emptied (best-effort, still
    * under the first-component lock). */
   {
      int (*tick)(void *) = purge_ran ? gp_hb_tick : NULL;
      void *tick_ctx = purge_ran ? &hb : NULL;
      char org[WS_REF_COMP_MAX + 1], repo[WS_REF_COMP_MAX + 1];
      if (ws_scope_ref_split(ref, org, sizeof(org), repo, sizeof(repo)) < 0)
      {
         snprintf(err, errlen, "invalid project ref");
         goto out;
      }
      rootfd = ws_scope_open_user_root(principal);
      if (rootfd < 0)
      {
         delete_audit(res->purge_id, principal, ref, "aborted", "reason=fs-failed");
         snprintf(err, errlen, "could not open the workspace root");
         goto out;
      }
      int rm_rc;
      if (org[0])
      {
         orgfd = ws_scope_openat2_dir(rootfd, org);
         if (orgfd < 0)
         {
            delete_audit(res->purge_id, principal, ref, "aborted", "reason=fs-failed");
            snprintf(err, errlen, "could not open the org directory");
            goto out;
         }
         rm_rc = rm_rf_at_tick(orgfd, repo, tick, tick_ctx);
         close(orgfd);
         orgfd = -1;
         if (rm_rc == 0)
            (void)unlinkat(rootfd, org, AT_REMOVEDIR); /* prune if now empty */
      }
      else
         rm_rc = rm_rf_at_tick(rootfd, repo, tick, tick_ctx);
      if (rm_rc != 0)
      {
         /* The kb purge (when one ran) is committed and its fence stays until
          * finalize or TTL — re-running the delete converges over the partial
          * tree in either case. */
         if (hb.lost)
         {
            delete_audit(res->purge_id, principal, ref, "aborted",
                         "reason=fence-lost fence_generation=%s", res->generation);
            snprintf(err, errlen,
                     "purge fence ownership lost during removal; re-run the delete to convergence");
            rc_final = GP_ERR_KB_UNAVAILABLE;
         }
         else
         {
            delete_audit(res->purge_id, principal, ref, "aborted", "reason=fs-failed");
            snprintf(err, errlen, "could not remove the project directory");
         }
         goto out;
      }
   }

   /* Step 8: clear the fence (only when a purge ran; finalize no-ops on a
    * generation/purge_id mismatch), then audit done. */
   if (purge_ran)
   {
      char *fj = kb_client_purge_finalize_json(ref, res->generation, res->purge_id);
      if (!purge_reply_cleared(fj))
         aimee_log(LOG_WARN, "webuser.project.delete",
                   "ref '%s': purge-finalize did not confirm clearing the fence (purge_id=%s); "
                   "it expires via its TTL",
                   ref, res->purge_id);
      free(fj);
   }
   if (strcmp(res->kb_status, "retained") == 0)
      delete_audit(res->purge_id, principal, ref, "done", "kb_status=retained fs=removed");
   else
      delete_audit(res->purge_id, principal, ref, "done",
                   "kb_status=%s fs=removed fence_generation=%s kb=%s", res->kb_status,
                   res->generation, res->kb_detail ? res->kb_detail : "");
   rc_final = 0;

out:
   if (orgfd >= 0)
      close(orgfd);
   if (rootfd >= 0)
      close(rootfd);
   close(lockfd); /* releases the flock */
   return rc_final;
}

int git_project_remote(const char *principal, const char *ref, char *out, size_t cap)
{
   if (!out || cap == 0)
      return -1;
   out[0] = '\0';
   int pfd = ws_scope_open_project(principal, ref, 0);
   if (pfd < 0)
      return -1;
   /* The project tree is user-writable (editor), so `.aimee` could be a
    * planted symlink: open it with RESOLVE_NO_SYMLINKS relative to the pinned
    * project fd, then the single-component file with O_NOFOLLOW — no
    * component of the sidecar path can escape the project. */
   int afd = ws_scope_openat2_dir(pfd, ".aimee");
   close(pfd);
   if (afd < 0)
      return -1;
   int sfd = openat(afd, "remote", O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
   close(afd);
   if (sfd < 0)
      return -1;
   char line[1024];
   ssize_t n = read(sfd, line, sizeof(line) - 1);
   close(sfd);
   if (n <= 0)
      return -1;
   line[n] = '\0';
   line[strcspn(line, "\n")] = '\0';
   if (!line[0])
      return -1;
   snprintf(out, cap, "%s", line);
   return 0;
}

/* Append one row to the list output. */
static int list_emit(char out[][GIT_PROJECT_NAME_MAX], int n, int max, const char *org,
                     const char *repo)
{
   if (n >= max)
      return n;
   if (org && org[0])
      snprintf(out[n], GIT_PROJECT_NAME_MAX, "%s/%s", org, repo);
   else
      snprintf(out[n], GIT_PROJECT_NAME_MAX, "%s", repo);
   return n + 1;
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
         continue; /* skip . / .. / hidden (incl. unpublished .tmp-* clones) */
      if (!ws_scope_name_valid(e->d_name))
         continue;
      struct stat st;
      if (fstatat(dfd, e->d_name, &st, AT_SYMLINK_NOFOLLOW) != 0 || !S_ISDIR(st.st_mode))
         continue;
      /* Structural rule: a first-level dir WITH a .git entry is a flat
       * project; WITHOUT one it is an org dir whose children are projects.
       * Unambiguous because unpublished clones are dot-prefixed (invisible)
       * and ws_scope_name_valid rejects dot-prefixed names. */
      if (is_published_project_at(dfd, e->d_name))
      {
         n = list_emit(out, n, max, NULL, e->d_name);
         continue;
      }
      int ofd = openat(dfd, e->d_name, O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
      if (ofd < 0)
         continue;
      DIR *od = fdopendir(ofd);
      if (!od)
      {
         close(ofd);
         continue;
      }
      struct dirent *oe;
      while (n < max && (oe = readdir(od)) != NULL)
      {
         if (oe->d_name[0] == '.' || !ws_scope_name_valid(oe->d_name))
            continue;
         if (fstatat(ofd, oe->d_name, &st, AT_SYMLINK_NOFOLLOW) != 0 || !S_ISDIR(st.st_mode))
            continue;
         if (is_published_project_at(ofd, oe->d_name))
            n = list_emit(out, n, max, e->d_name, oe->d_name);
      }
      closedir(od); /* closes ofd */
   }
   closedir(d); /* closes dfd */
   return n;
}
