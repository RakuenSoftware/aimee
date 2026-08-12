/* client_session_worktree.c: thin-client per-session worktree bootstrap.
 *
 * See client_session_worktree.h for the policy this implements and why it is a
 * separate implementation from the server-side one in modules/workspace.
 *
 * Every git invocation here is shell-free (fork/execvp): session ids and repo
 * paths reach argv directly, so there is nothing to quote or inject. */
#include "client_session_worktree.h"
#include "cli_attention_guard.h" /* attn_require_session_worktree, attn_session_isolation_blocked */
#include "session_worktree_key.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#ifndef _WIN32
#include <fcntl.h>
#include <sys/wait.h>
#endif

void client_session_worktree_key(const char *sid, char *out, size_t cap)
{
   /* One derivation, shared with the server (session_worktree_key.c). This used
    * to be a second, independent copy here — which is how the client and server
    * came to disagree about where a session's worktree lived. */
   session_worktree_key(sid, out, cap);
}

#ifndef _WIN32
/* --- Session-id rendezvous -------------------------------------------------
 *
 * Every process of one agent session must agree on the session id, because the
 * worktree is keyed on it: disagree and the session gets TWO worktrees, and
 * whichever process holds the wrong one operates on an empty checkout. That is
 * not hypothetical -- a Claude Code session was landing its edits in the
 * hook's worktree while `aimee git` and every delegate were bound to the
 * proxy's, which refused the real one as "outside the session checkout".
 *
 * The rendezvous is a file named for a process both sides can name. `aimee mcp
 * serve` reads session-ppid-<its own ppid>, and its parent IS the host process
 * (verified: the proxy is a direct child of `claude`). The hook cannot use its
 * own getppid() for this: its command carries an environment assignment, so the
 * host must run it through a shell, and the hook is therefore a GRANDchild --
 * publishing under its immediate parent would name a shell that exits
 * immediately and that the proxy never asks about.
 *
 * So the hook walks up to the host and publishes there as well. Only as far as
 * the host: publishing under every ancestor would eventually name something
 * shared (a terminal, a service manager) and hand one session's id to an
 * unrelated one -- the precise collision the ppid key exists to avoid. */
#if defined(__linux__)
/* The parent of `pid`, or 0 when it cannot be read. Parsed from the END of
 * /proc/<pid>/stat: comm sits in field 2 wrapped in parentheses and may itself
 * contain spaces or ')', so everything before the LAST ") " is skipped rather
 * than tokenising from the front. */
static pid_t csw_parent_of(pid_t pid)
{
   char path[64];
   snprintf(path, sizeof(path), "/proc/%d/stat", (int)pid);
   FILE *f = fopen(path, "r");
   if (!f)
      return 0;
   char buf[512];
   size_t n = fread(buf, 1, sizeof(buf) - 1, f);
   fclose(f);
   buf[n] = '\0';
   char *tail = strrchr(buf, ')');
   if (!tail || !tail[1])
      return 0;
   int ppid = 0;
   char state = 0;
   if (sscanf(tail + 1, " %c %d", &state, &ppid) != 2 || ppid <= 0)
      return 0;
   return (pid_t)ppid;
}

static int csw_comm_is(pid_t pid, const char *name)
{
   char path[64];
   snprintf(path, sizeof(path), "/proc/%d/comm", (int)pid);
   FILE *f = fopen(path, "r");
   if (!f)
      return 0;
   char buf[64] = "";
   if (!fgets(buf, sizeof(buf), f))
   {
      fclose(f);
      return 0;
   }
   fclose(f);
   buf[strcspn(buf, "\r\n")] = '\0';
   return strcmp(buf, name) == 0;
}
#endif /* __linux__ */

/* Write `sid` to <aimee_home>/session-ppid-<pid>. Authoritative: the caller
 * holds the id the HOST assigned, which outranks anything a peer minted for
 * itself, so this truncates rather than failing on an existing file. */
static void csw_publish_at(const char *home, pid_t pid, const char *sid)
{
   char path[4200];
   if (snprintf(path, sizeof(path), "%s/session-ppid-%d", home, (int)pid) >= (int)sizeof(path))
      return;
   int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
   if (fd < 0)
      return;
   size_t len = strlen(sid);
   ssize_t wrote = write(fd, sid, len);
   (void)wrote;
   close(fd);
}

int client_session_id_publish(const char *sid, const char *home)
{
   if (!sid || !sid[0] || !home || !home[0])
      return -1;
   /* Reject anything that could escape the filename or the file's one-line
    * contract; a session id is an opaque token from the host, not a path. */
   for (const char *p = sid; *p; p++)
      if (*p == '/' || *p == '\n' || *p == '\r' || (unsigned char)*p < 0x20)
         return -1;

   int published = 0;
   pid_t parent = getppid();
   if (parent > 1)
   {
      csw_publish_at(home, parent, sid);
      published++;
   }
#if defined(__linux__)
   /* Up to the host process, and no further. */
   pid_t pid = parent;
   for (int depth = 0; depth < 8 && pid > 1; depth++)
   {
      if (csw_comm_is(pid, "claude"))
      {
         if (pid != parent)
         {
            csw_publish_at(home, pid, sid);
            published++;
         }
         break;
      }
      pid = csw_parent_of(pid);
   }
#endif
   return published > 0 ? 0 : -1;
}

/* Run `git <argv...>` with NO shell (fork/execvp), discarding stderr. Captures
 * the first trimmed stdout line into out[cap] (out may be NULL — status only).
 * Returns the child's exit code (0 = success), or -1 if it could not be spawned
 * or did not exit cleanly. */
static int csw_git(const char *const argv[], char *out, size_t cap)
{
   if (out && cap)
      out[0] = '\0';
   int pfd[2];
   if (pipe(pfd) != 0)
      return -1;
   pid_t pid = fork();
   if (pid < 0)
   {
      close(pfd[0]);
      close(pfd[1]);
      return -1;
   }
   if (pid == 0)
   {
      dup2(pfd[1], STDOUT_FILENO);
      int devnull = open("/dev/null", O_WRONLY);
      if (devnull >= 0)
         dup2(devnull, STDERR_FILENO);
      close(pfd[0]);
      close(pfd[1]);
      execvp("git", (char *const *)argv);
      _exit(127);
   }
   close(pfd[1]);
   char buf[4096];
   size_t got = 0;
   /* Read the first bufful; keep draining any excess into scratch so a chatty git
    * never gets SIGPIPE (which would look like a failure). Retry on EINTR so a
    * signal cannot truncate a capture that then reports exit 0. */
   for (;;)
   {
      char scratch[4096];
      int have_room = got < sizeof buf - 1;
      char *dst = have_room ? buf + got : scratch;
      size_t room = have_room ? sizeof buf - 1 - got : sizeof scratch;
      ssize_t r = read(pfd[0], dst, room);
      if (r < 0)
      {
         if (errno == EINTR)
            continue;
         break;
      }
      if (r == 0)
         break;
      if (have_room)
         got += (size_t)r;
   }
   buf[got] = '\0';
   close(pfd[0]);
   int status = 0;
   if (waitpid(pid, &status, 0) < 0 || !WIFEXITED(status))
      return -1;
   int code = WEXITSTATUS(status);
   if (out && cap && code == 0)
   {
      size_t n = 0;
      while (buf[n] && buf[n] != '\n' && buf[n] != '\r')
         n++;
      buf[n] = '\0';
      while (n > 0 && (buf[n - 1] == ' ' || buf[n - 1] == '\t'))
         buf[--n] = '\0';
      snprintf(out, cap, "%s", buf);
   }
   return code;
}

/* True when `ref` names an existing commit-ish in git_root. */
static int csw_ref_exists(const char *git_root, const char *ref)
{
   if (!ref || !ref[0])
      return 0;
   char spec[192];
   snprintf(spec, sizeof spec, "%s^{commit}", ref);
   const char *const argv[] = {"git",      "-C",      git_root, "rev-parse",
                               "--verify", "--quiet", spec,     NULL};
   return csw_git(argv, NULL, 0) == 0;
}

/* Prefer the remote-tracking ref for `name`, else the local branch. Mirrors the
 * server's wt_resolve_candidate so both pick the same base for a given repo. */
static int csw_resolve_candidate(const char *git_root, const char *name, char *out, size_t cap)
{
   if (!name || !name[0])
      return 0;
   char remote_ref[176];
   snprintf(remote_ref, sizeof remote_ref, "origin/%s", name);
   if (csw_ref_exists(git_root, remote_ref))
   {
      snprintf(out, cap, "%s", remote_ref);
      return 1;
   }
   if (csw_ref_exists(git_root, name))
   {
      snprintf(out, cap, "%s", name);
      return 1;
   }
   return 0;
}

/* origin/HEAD -> the default branch's short name (without the "origin/" prefix),
 * or "" when the remote advertises none. */
static void csw_remote_default(const char *git_root, char *out, size_t cap)
{
   out[0] = '\0';
   char sym[192];
   const char *const argv[] = {
       "git", "-C", git_root, "symbolic-ref", "--short", "refs/remotes/origin/HEAD", NULL};
   if (csw_git(argv, sym, sizeof sym) != 0 || !sym[0])
      return;
   const char *name = sym;
   if (strncmp(name, "origin/", 7) == 0)
      name += 7;
   if (name[0])
      snprintf(out, cap, "%s", name);
}

int client_session_worktree_base(const char *git_root, char *buf, size_t cap)
{
   if (!git_root || !buf || !cap)
      return -1;
   buf[0] = '\0';

   /* ---- 1. configured ----
    * The client reads only the env var: aimee.yaml parsing lives in config.o,
    * which this binary does not link. An operator who sets session_worktree_base
    * in aimee.yaml is served by the server-side path; AIMEE_SESSION_WORKTREE_BASE
    * is the client-visible knob and is documented as such. */
   const char *mode = getenv("AIMEE_SESSION_WORKTREE_BASE");
   if (!mode || !mode[0])
      mode = "remote_default";

   if (strcmp(mode, "current") == 0)
   {
      /* Explicit opt-in only, for offline/detached workflows that accept
       * inheriting the source checkout's branch. Never reached as a fallback. */
      char cur[192];
      const char *const argv[] = {"git", "-C", git_root, "rev-parse", "--abbrev-ref", "HEAD", NULL};
      if (csw_git(argv, cur, sizeof cur) == 0 && cur[0] && strcmp(cur, "HEAD") != 0)
      {
         snprintf(buf, cap, "%s", cur);
         return 0;
      }
      return -1;
   }
   if (strcmp(mode, "remote_default") != 0 && strcmp(mode, "local_default") != 0)
   {
      /* An explicit ref that does not exist is an operator error, not a hint. */
      if (!csw_ref_exists(git_root, mode))
         return -1;
      snprintf(buf, cap, "%s", mode);
      return 0;
   }

   /* ---- 2. the remote's advertised default ---- */
   char def[128];
   csw_remote_default(git_root, def, sizeof def);
   if (!def[0])
   {
      /* origin/HEAD is unset on repos whose remote was added after clone. Repair
       * once, then re-read. Best-effort: offline, this simply stays unset. */
      const char *const set_argv[] = {"git",      "-C",     git_root, "remote",
                                      "set-head", "origin", "-a",     NULL};
      (void)csw_git(set_argv, NULL, 0);
      csw_remote_default(git_root, def, sizeof def);
   }
   if (def[0])
   {
      if (strcmp(mode, "local_default") == 0)
      {
         if (csw_ref_exists(git_root, def))
         {
            snprintf(buf, cap, "%s", def);
            return 0;
         }
      }
      else if (csw_resolve_candidate(git_root, def, buf, cap))
         return 0;
   }

   /* ---- 3. main, then 4. master ---- */
   if (csw_resolve_candidate(git_root, "main", buf, cap))
      return 0;
   if (csw_resolve_candidate(git_root, "master", buf, cap))
      return 0;

   buf[0] = '\0';
   return -1;
}

/* Give back the worktree this session owned under the PREVIOUS (truncating)
 * key. The derivation changed to stop two sessions colliding on one worktree; a
 * session that spans the change would otherwise strand its old worktree and
 * branch on disk. Only a CLEAN one is removed — `git worktree remove` without
 * --force refuses a dirty tree, so stranded work is kept, not destroyed. The
 * branch goes only if git accepts `branch -d` (merged), never -D. */
static void csw_reclaim_legacy(const char *git_root, const char *sid, const char *live_key)
{
   char old_key[SESSION_WORKTREE_KEY_MAX];
   session_worktree_key_legacy(sid, old_key, sizeof old_key);
   if (!old_key[0] || strcmp(old_key, live_key) == 0)
      return; /* both derivations agree -> that IS the live worktree */

   char old_path[4200];
   if (snprintf(old_path, sizeof old_path, "%s/.aimee/worktrees/%s/main", git_root, old_key) >=
       (int)sizeof old_path)
      return;
   struct stat st;
   if (stat(old_path, &st) != 0 || !S_ISDIR(st.st_mode))
      return; /* nothing stranded */

   const char *const rm_argv[] = {"git", "-C", git_root, "worktree", "remove", old_path, NULL};
   if (csw_git(rm_argv, NULL, 0) != 0)
   {
      fprintf(stderr, "aimee: kept pre-rekey worktree %s — it has uncommitted or unpushed work.\n",
              old_path);
      return;
   }
   fprintf(stderr, "aimee: reclaimed pre-rekey worktree %s\n", old_path);

   char old_branch[160];
   if (snprintf(old_branch, sizeof old_branch, "aimee/session/%s", old_key) <
       (int)sizeof old_branch)
   {
      const char *const br_argv[] = {"git", "-C", git_root, "branch", "-d", old_branch, NULL};
      (void)csw_git(br_argv, NULL, 0); /* -d, not -D: keeps an unmerged branch */
   }
   char parent[4200];
   if (snprintf(parent, sizeof parent, "%s/.aimee/worktrees/%s", git_root, old_key) <
       (int)sizeof parent)
      (void)rmdir(parent);
}

int client_session_worktree_ensure(const char *sid, char *out, size_t cap)
{
   if (!out || !cap)
      return -1;
   out[0] = '\0';

   if (!attn_require_session_worktree())
      return -1; /* isolation not enforced -> nothing to prepare */

   char cwd[4096];
   if (!getcwd(cwd, sizeof cwd))
      return -1;
   /* Already inside a managed (.aimee/.claude/.codex) worktree -> this session is
    * isolated already; don't create a redundant one. Mirrors the guard's own
    * decision so we prepare a worktree exactly when it would otherwise block. */
   if (!attn_session_isolation_blocked(ATTN_OP_SOFT, NULL, cwd, sid))
      return -1;

   /* Need a stable session id to name the worktree. */
   if (!sid || !sid[0])
      return -1;
   char key[80];
   client_session_worktree_key(sid, key, sizeof key);
   if (!key[0])
      return -1;

   const char *const rp_argv[] = {"git", "-C", cwd, "rev-parse", "--show-toplevel", NULL};
   char git_root[4096];
   if (csw_git(rp_argv, git_root, sizeof git_root) != 0 || !git_root[0])
      return -1; /* not a git repo -> nothing to prepare */

   char wt[4200];
   if (snprintf(wt, sizeof wt, "%s/.aimee/worktrees/%s/main", git_root, key) >= (int)sizeof wt)
      return -2;
   char branch[128];
   if (snprintf(branch, sizeof branch, "aimee/session/%s", key) >= (int)sizeof branch)
      return -2;

   struct stat st;
   if (stat(wt, &st) != 0)
   {
      char base_ref[192];
      if (client_session_worktree_base(git_root, base_ref, sizeof base_ref) != 0)
      {
         /* Deliberately NOT falling back to the checkout's current branch: that
          * is how a session inherits another session's work as its base. */
         fprintf(stderr,
                 "aimee: cannot resolve the session worktree base for '%s'. The default is the "
                 "REMOTE default branch (origin/HEAD); it is unset or unreachable here. Fix the "
                 "remote (git remote set-head origin -a) or set AIMEE_SESSION_WORKTREE_BASE to an "
                 "explicit ref.\n",
                 git_root);
         return -2;
      }
      /* A base ref that begins with '-' would be parsed as a git option. */
      if (base_ref[0] == '-')
         return -2;

      /* Prune stale registrations (a worktree dir removed out-of-band leaves an
       * entry under .git/worktrees that makes `worktree add` fail), then create
       * the worktree + session branch. git creates intermediate dirs. */
      const char *const prune_argv[] = {"git", "-C", git_root, "worktree", "prune", NULL};
      (void)csw_git(prune_argv, NULL, 0);
      const char *const add_argv[] = {"git", "-C", git_root, "worktree", "add",
                                      wt,    "-b", branch,   base_ref,   NULL};
      if (csw_git(add_argv, NULL, 0) != 0)
      {
         /* The session branch may already exist with no worktree attached (a
          * prior worktree was force-removed while still ahead). Reattach it so
          * the session still gets isolation instead of sharing the checkout. */
         const char *const attach_argv[] = {"git", "-C", git_root, "worktree",
                                            "add", wt,   branch,   NULL};
         (void)csw_git(attach_argv, NULL, 0);
      }
   }
   if (stat(wt, &st) != 0 || !S_ISDIR(st.st_mode))
   {
      fprintf(stderr, "aimee: failed to create the session worktree at %s\n", wt);
      return -2; /* don't hand the caller a path that isn't there */
   }

   csw_reclaim_legacy(git_root, sid, key);

   snprintf(out, cap, "%s", wt);
   return 0;
}
#else  /* _WIN32 */
/* Session-worktree isolation and the .aimee/worktrees layout are a POSIX/Linux-
 * server feature; the Windows build ships only the thin client, and preparing a
 * worktree needs POSIX process primitives. No-op there. */
int client_session_worktree_base(const char *git_root, char *buf, size_t cap)
{
   (void)git_root;
   if (buf && cap)
      buf[0] = '\0';
   return -1;
}

int client_session_id_publish(const char *sid, const char *home)
{
   (void)sid;
   (void)home;
   return -1;
}

int client_session_worktree_ensure(const char *sid, char *out, size_t cap)
{
   (void)sid;
   if (out && cap)
      out[0] = '\0';
   return -1;
}
#endif /* _WIN32 */
