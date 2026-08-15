/* git_project.c — clone a repo into a webuser's scoped workspace. See header.
 *
 * Org-scoped layout (webchat project lifecycle proposal, slice 1): clones land
 * at <user root>/<org>/<repo> where the org derives from the clone URL's owner
 * path (or an explicit override), the project ref is "<org>/<repo>", and the
 * clone is registered in the deployment-global key registry under the
 * first-component lifecycle lock before it is published (temp dir + rename,
 * fd-pinned via openat2 — no string-path window). */
#include "git_project.h"
#include "git_cred_inject.h" /* git_cred_inject_build_env_for_repo / _free_env */
#include "git_host_cred.h"   /* per-host token store (single-user, many hosts) */
#include "log.h"
#include "util.h"                              /* safe_exec_capture_env */
#include "util_url.h"                          /* util_url_normalize / util_url_is_ssh */
#include "modules/workspace/workspace_scope.h" /* ws_scope_* */
#include "ws_registry.h"                       /* lifecycle lock + key registry */

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

/* The server-local lexical index delete seam. The shipped aimee-server keeps
 * NO local lexical index: index_scan_project is compiled to a stub in this
 * binary (build/obj/server/index.o, -DAIMEE_DB2_DISABLED) and the canonical
 * code index lives in aimee-kb — so there is nothing to delete here and no db2
 * linkage in this TU. Kept as a weak seam so the delete flow's
 * abort-before-filesystem ordering stays unit-testable (the tests override it
 * to inject failures). */
__attribute__((weak)) int gp_local_index_delete(const char *ref)
{
   (void)ref;
   return 0;
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
 * immediately. The delete path passes NULL; the hook is retained for callers
 * that need to bail mid-walk. */
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

/* True if `envp` carries an SSH_AUTH_SOCK entry — i.e. git_cred_inject wired up
 * the user's in-memory ssh-agent because they have a vaulted SSH key. */
static int env_has_ssh_sock(char *const *envp)
{
   for (int i = 0; envp && envp[i]; i++)
      if (strncmp(envp[i], "SSH_AUTH_SOCK=", 14) == 0)
         return 1;
   return 0;
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
   int registered = 0, rootfd = -1, orgfd = -1, tmpfd = -1, destfd_parent = -1;
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

   /* Registry: the key already bound to a different remote -> generic 409
    * (the other remote is never echoed). */
   {
      char cur_remote[GP_PATH_MAX];
      int found = ws_reg_lookup(ref, cur_remote, sizeof(cur_remote));
      if (found < 0)
      {
         snprintf(err, errlen, "project registry unavailable");
         goto out;
      }
      if (found == 1)
      {
         /* The entry may be stale after a `git remote set-url` — in EITHER
          * direction: a stale mismatch would spuriously 409 a legitimate
          * clone, and a stale MATCH would silently accept a ref that has since
          * diverged. Resync from the clone's git config (authoritative, under
          * the held lock) before comparing. */
         if (ws_reg_resync(ref) != 0 ||
             (found = ws_reg_lookup(ref, cur_remote, sizeof(cur_remote))) < 0)
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
   destfd_parent = org[0] ? orgfd : rootfd;
   snprintf(tmpname, sizeof(tmpname), ".tmp-%s-%d", repo, (int)getpid());
   /* Stale leftover from a crashed clone only: the cleanup targets this exact
    * ".tmp-<repo>-<pid>" name and never touches ".deleting-*" markers — those
    * belong to the delete path's resumable tombstones. */
   (void)rm_rf_at(destfd_parent, tmpname);
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
      /* Clone over SSH when the user gave an SSH URL and we loaded their vaulted
       * key (the env then carries SSH_AUTH_SOCK + a TOFU GIT_SSH_COMMAND): honor
       * the SSH URL so key auth is actually used — the only way in for a host
       * that offers SSH-key access only (e.g. a corporate Bitbucket). Otherwise
       * clone over HTTPS: our default credential model is per-host HTTPS tokens
       * (+ OAuth), and normalizing ssh://, git@host:path, git:// to https lets a
       * public or token-authed repo still clone. */
      char *clone_url_norm = NULL;
      const char *clone_url;
      int ssh_clone = 0;
      if (util_url_is_ssh(url) && env_has_ssh_sock(envp))
      {
         clone_url = url;
         ssh_clone = 1;
      }
      else
      {
         clone_url_norm = util_url_normalize(url);
         clone_url = clone_url_norm ? clone_url_norm : url;
      }
      /* Destination pinned by fd: the child chdirs to /proc/self/fd/<tmpfd>
       * (pre-exec, while the fd is still open) and clones into ".", so git
       * never resolves an attacker-influencable path string. */
      char cwd[64];
      snprintf(cwd, sizeof(cwd), "/proc/self/fd/%d", tmpfd);
      const char *argv[] = {"git", "clone", "--", clone_url, ".", NULL};
      char *out = NULL;
      /* Defense in depth: an SSH clone authenticates via the agent, so the HTTPS
       * token memfd has no business in that child — git only consults GIT_ASKPASS
       * on the HTTP(S) transport. Don't inherit the token fd on the SSH path (the
       * real fd is still closed below); the HTTPS path passes it as before. */
      int child_token_fd = ssh_clone ? -1 : token_fd;
      int rc = safe_exec_capture_cwd_env_fd_timeout(
          argv, cwd, envp ? envp : environ, &out, 1 << 16, 300000, child_token_fd,
          child_token_fd >= 0 ? GIT_CRED_TOKEN_TARGET_FD : -1);
      if (token_fd >= 0)
         close(token_fd);
      free(clone_url_norm);
      if (envp)
         git_cred_inject_free_env(envp);

      /* A working inline token is worth keeping: persist it for the host so
       * future pulls/pushes on any repo there reuse it (no re-entry).
       *
       * The clone itself already succeeded, so a failure here must not fail the
       * call — the caller has their repository. But it must not pass silently
       * either: this is the only moment the token exists to be kept, and losing
       * it here is indistinguishable afterwards from never having supplied one.
       * Every later forge call then reports "no github credential" and points at
       * a sign-in flow that appears to have worked. Say so where an operator
       * will find it. */
      if (rc == 0 && token && token[0])
      {
         char host[GIT_HOST_MAX];
         if (!git_host_from_url(url, host, sizeof(host)))
            aimee_log(LOG_WARN, "git.project.clone",
                      "clone succeeded but the host could not be derived from the remote, so the "
                      "supplied token was not kept; re-enter it to avoid re-authenticating");
         else if (git_host_cred_set(host, token) != 0)
            aimee_log(LOG_WARN, "git.project.clone",
                      "clone succeeded but storing the token for %s failed, so it was not kept; "
                      "forge operations on that host will report no credential until it is "
                      "supplied again",
                      host);
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
    * any failure before a successful publish removes the entry again, so a
    * registered ref always has a published clone behind it. */
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
 * shares the same delete_id; `extra_fmt` (may be "") formats phase-specific
 * key=value pairs (reason=…, fs=…). The extra tail is built in a heap buffer
 * when needed so a long reason is NEVER truncated in the audit record.
 * schema_version 2 renamed purge_id -> delete_id and dropped the kb fields
 * when the delete path stopped calling aimee-kb. */
__attribute__((format(printf, 5, 6))) static void delete_audit(const char *delete_id,
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
             "webuser_project_delete_audit_v1 schema_version=2 delete_id=%s principal=%s ref=%s "
             "phase=%s%s%s",
             delete_id, principal, ref, phase, extra[0] ? " " : "", extra);
   if (extra != stackbuf)
      free(extra);
}

/* Mint a random hex delete id (16 bytes of /dev/urandom -> 32 hex chars;
 * time^pid fallback so the id is never empty). */
static void mint_delete_id(char *out, size_t cap)
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

int git_project_delete(const char *principal, const char *ref, char *err, size_t errlen)
{
   if (err && errlen)
      err[0] = '\0';

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
   /* Step 1: validate the ref and mint the id shared by this run's audit lines. */
   size_t reflen = ref ? strlen(ref) : 0;
   if (!ws_scope_project_ref_valid(ref, reflen))
   {
      snprintf(err, errlen, "invalid project ref");
      return -1;
   }
   char delete_id[40];
   mint_delete_id(delete_id, sizeof(delete_id));

   /* Step 2: audit intent FIRST — before any existence resolution — so a
    * nonexistent ref still leaves a record. */
   delete_audit(delete_id, principal, ref, "intent", "%s", "");

   int lockfd = ws_reg_lock(ref);
   if (lockfd < 0)
   {
      delete_audit(delete_id, principal, ref, "aborted", "reason=lock-unavailable");
      snprintf(err, errlen, "could not acquire the project lifecycle lock");
      return -1;
   }

   int rc_final = -1, rootfd = -1, orgfd = -1;
   int resumed = 0; /* a ".deleting-<repo>" tombstone from an interrupted walk */

   char org[WS_REF_COMP_MAX + 1], repo[WS_REF_COMP_MAX + 1];
   char marker[WS_REF_COMP_MAX + 32]; /* ".deleting-<repo>" rename-first tombstone */
   if (ws_scope_ref_split(ref, org, sizeof(org), repo, sizeof(repo)) < 0)
   {
      snprintf(err, errlen, "invalid project ref");
      goto out;
   }
   snprintf(marker, sizeof(marker), ".deleting-%s", repo);

   /* Step 3: resolve the ref. When it does not resolve, a matching
    * ".deleting-<repo>" tombstone at the expected level marks a RESUMABLE
    * partial delete (an earlier walk renamed the project to the tombstone, then
    * was interrupted); only when neither exists is it a plain not-found. */
   {
      int pfd = ws_scope_open_project(principal, ref, 0);
      if (pfd >= 0)
         close(pfd);
      else
      {
         int rfd = ws_scope_open_user_root(principal);
         if (rfd >= 0)
         {
            int parent = org[0] ? ws_scope_openat2_dir(rfd, org) : rfd;
            if (parent >= 0)
            {
               struct stat mst;
               if (fstatat(parent, marker, &mst, AT_SYMLINK_NOFOLLOW) == 0 && S_ISDIR(mst.st_mode))
                  resumed = 1;
               if (parent != rfd)
                  close(parent);
            }
            close(rfd);
         }
         if (!resumed)
         {
            delete_audit(delete_id, principal, ref, "aborted", "reason=not-found");
            snprintf(err, errlen, "not found");
            rc_final = GP_ERR_NOT_FOUND;
            goto out;
         }
      }
   }

   /* Step 4: drop the registry entry. A resumed delete already unregistered on
    * its earlier attempt, so unregister is idempotent by ref. */
   if (ws_reg_unregister(ref) < 0)
   {
      delete_audit(delete_id, principal, ref, "aborted", "reason=registry-unavailable");
      snprintf(err, errlen, "project registry unavailable");
      goto out;
   }

   /* Step 5: server-local lexical index. gp_local_index_delete returns a
    * deleted row count (>= 0 success, only < 0 fails). A FAILURE must ABORT
    * BEFORE any filesystem removal — proceeding would strand index rows with no
    * retry path once the clone is gone. The registry entry is restored so
    * re-running the delete converges. */
   if (gp_local_index_delete(ref) < 0)
   {
      char reg_remote[1024] = "";
      if (git_project_remote(principal, ref, reg_remote, sizeof(reg_remote)) != 0)
         snprintf(reg_remote, sizeof(reg_remote), "unknown://%s", ref);
      if (ws_reg_register(ref, reg_remote) != 0)
         aimee_log(LOG_ERROR, "webuser.project.delete",
                   "ref '%s': could not restore the registry entry after a local-index failure "
                   "(delete_id=%s); the startup rebuild self-heals",
                   ref, delete_id);
      delete_audit(delete_id, principal, ref, "aborted", "reason=local-index-failed");
      snprintf(err, errlen,
               "could not clear the server-local code index; nothing was removed — try again");
      goto out;
   }

   /* Step 6: filesystem removal — rename-first tombstone, then an unlinkat-based
    * walk from the pinned parent fd (rm_rf_at never follows symlinks). The
    * project is atomically renamed to the dot-prefixed ".deleting-<repo>"
    * sibling BEFORE the walk: dot names are invisible to the lister/structural
    * rules and cannot collide with valid refs, so an interrupted walk leaves a
    * RESUMABLE marker instead of a half-removed tree that no longer resolves.
    * Step 7: prune the org dir when it emptied (best-effort, still under the
    * first-component lock). */
   {
      rootfd = ws_scope_open_user_root(principal);
      if (rootfd < 0)
      {
         delete_audit(delete_id, principal, ref, "aborted", "reason=fs-failed");
         snprintf(err, errlen, "could not open the workspace root");
         goto out;
      }
      int parentfd = rootfd;
      if (org[0])
      {
         orgfd = ws_scope_openat2_dir(rootfd, org);
         if (orgfd < 0)
         {
            delete_audit(delete_id, principal, ref, "aborted", "reason=fs-failed");
            snprintf(err, errlen, "could not open the org directory");
            goto out;
         }
         parentfd = orgfd;
      }
      int rm_rc = 0;
      if (!resumed)
      {
         /* Fold in any stale marker from an even older crash (renameat onto a
          * non-empty dir would fail), then tombstone the project. */
         (void)rm_rf_at_tick(parentfd, marker, NULL, NULL);
         if (renameat(parentfd, repo, parentfd, marker) != 0)
            rm_rc = -1;
      }
      if (rm_rc == 0)
         rm_rc = rm_rf_at_tick(parentfd, marker, NULL, NULL);
      if (orgfd >= 0)
      {
         close(orgfd);
         orgfd = -1;
      }
      if (rm_rc == 0 && org[0])
         (void)unlinkat(rootfd, org, AT_REMOVEDIR); /* prune if now empty */
      if (rm_rc != 0)
      {
         delete_audit(delete_id, principal, ref, "aborted", "reason=fs-failed");
         snprintf(err, errlen, "could not remove the project directory");
         goto out;
      }
   }

   /* The 'done' record only emits here — after the marker tree is fully gone. */
   delete_audit(delete_id, principal, ref, "done", "%sfs=removed", resumed ? "resumed=1 " : "");
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
