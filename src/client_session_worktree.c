/* client_session_worktree.c: thin-client per-session worktree bootstrap.
 *
 * See client_session_worktree.h for the policy this implements and why it is a
 * separate implementation from the server-side one in modules/workspace.
 *
 * Every git invocation here is shell-free (fork/execvp): session ids and repo
 * paths reach argv directly, so there is nothing to quote or inject. */
#include "client_session_worktree.h"
#include "session_worktree_key.h"
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#ifndef _WIN32
#include <fcntl.h>
#include <sys/file.h>
#include <sys/wait.h>
#include <time.h>
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
 * not hypothetical -- an older hook/MCP split landed host edits in one checkout
 * while `aimee git` and delegates were bound to another.
 *
 * The rendezvous is a file named for a process both sides can name. `aimee mcp
 * serve` reads session-ppid-<its own ppid>, and its parent IS the host process
 * (verified: the proxy is a direct child of `claude`). The hook cannot use its
 * own getppid() for this: its command carries an environment assignment, so the
 * host must run it through a shell, and the hook is therefore a GRANDchild --
 * publishing under its immediate parent would name a shell that exits
 * immediately and that the proxy never asks about.
 *
 * A host hook fallback therefore walks up to the host and publishes there as
 * well. The universal launcher normally avoids this race entirely by exporting
 * AIMEE_SESSION_ID before either child exists. Only walk as far as
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

/* Resolve the shared Git administration directory.  A linked worktree has a
 * .git FILE, so placing the provisioning lock beneath <worktree>/.git fails
 * with ENOTDIR.  rev-parse points every linked checkout at the same common
 * directory, which is also exactly the scope the lock needs to serialize. */
static int csw_git_common_dir(const char *git_root, char *out, size_t cap)
{
   if (!git_root || !git_root[0] || !out || cap == 0)
      return -1;
   char common[4096];
   const char *const argv[] = {"git", "-C", git_root, "rev-parse", "--git-common-dir", NULL};
   if (csw_git(argv, common, sizeof common) != 0 || !common[0])
      return -1;
   if (common[0] == '/')
      return snprintf(out, cap, "%s", common) < (int)cap ? 0 : -1;
   return snprintf(out, cap, "%s/%s", git_root, common) < (int)cap ? 0 : -1;
}

/* Network git for session start is deliberately separate from csw_git.  A
 * provider client must never sit forever before its first prompt because git
 * is waiting for a password or an unreachable SSH host.  Output is discarded;
 * callers need only the status and resolve exact commits in a second local
 * command. */
static int csw_git_network(const char *const argv[])
{
   pid_t pid = fork();
   if (pid < 0)
      return -1;
   if (pid == 0)
   {
      int devnull = open("/dev/null", O_RDWR);
      if (devnull >= 0)
      {
         dup2(devnull, STDOUT_FILENO);
         dup2(devnull, STDERR_FILENO);
      }
      setenv("GIT_TERMINAL_PROMPT", "0", 1);
      setenv("GIT_SSH_COMMAND", "ssh -o BatchMode=yes -o ConnectTimeout=5", 1);
      execvp("git", (char *const *)argv);
      _exit(127);
   }

   const long timeout_ms = 30000;
   struct timespec pause = {.tv_sec = 0, .tv_nsec = 10000000};
   long waited = 0;
   int status = 0;
   while (waited < timeout_ms)
   {
      pid_t done = waitpid(pid, &status, WNOHANG);
      if (done == pid)
         return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
      if (done < 0 && errno != EINTR)
         return -1;
      nanosleep(&pause, NULL);
      waited += 10;
   }
   kill(pid, SIGKILL);
   (void)waitpid(pid, &status, 0);
   return -1;
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

/* Resolve both halves of the start contract. selected is the branch/ref the
 * user asked to work from. default_oid is the exact default-branch tip observed
 * at this session start. When enforce_default is true, creation must prove that
 * default_oid is an ancestor of the new session branch (and merge it there if
 * necessary). `current` and `local_default` are the two explicit offline/stale
 * overrides; merely naming a feature ref is not an override of freshness. */
static int csw_session_bases(const char *git_root, char *selected, size_t selected_cap,
                             char *default_oid, size_t default_cap, int *enforce_default)
{
   if (!git_root || !selected || !selected_cap || !default_oid || !default_cap || !enforce_default)
      return -1;
   selected[0] = '\0';
   default_oid[0] = '\0';
   *enforce_default = 0;

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
         snprintf(selected, selected_cap, "%s", cur);
         return 0;
      }
      return -1;
   }
   if (strcmp(mode, "remote_default") != 0 && strcmp(mode, "local_default") != 0)
   {
      /* An explicit ref that does not exist is an operator error, not a hint. */
      if (!csw_ref_exists(git_root, mode))
         return -1;
      snprintf(selected, selected_cap, "%s", mode);
   }

   /* A configured origin makes remote freshness mandatory. Fetch every
    * tracking ref first (so an explicit origin/feature base is current), then
    * fetch HEAD once more so FETCH_HEAD is the exact current default tip even
    * when origin/HEAD is missing or stale after a server-side rename. */
   char origin[512];
   const char *const origin_argv[] = {"git", "-C", git_root, "remote", "get-url", "origin", NULL};
   int have_origin = csw_git(origin_argv, origin, sizeof origin) == 0 && origin[0];
   if (have_origin)
   {
      const char *const fetch_all[] = {"git",     "-C",      git_root, "fetch",
                                       "--quiet", "--prune", "origin", NULL};
      const char *const fetch_head[] = {"git",     "-C",     git_root, "fetch",
                                        "--quiet", "origin", "HEAD",   NULL};
      if (csw_git_network(fetch_all) != 0 || csw_git_network(fetch_head) != 0)
      {
         fprintf(stderr,
                 "aimee: cannot verify the latest default branch for '%s'; fetch from origin "
                 "failed. Use AIMEE_SESSION_WORKTREE_BASE=current or local_default only for "
                 "an explicit offline/stale start.\n",
                 git_root);
         return -1;
      }
      const char *const head_argv[] = {
          "git", "-C", git_root, "rev-parse", "--verify", "FETCH_HEAD^{commit}", NULL};
      if (csw_git(head_argv, default_oid, default_cap) != 0 || !default_oid[0])
         return -1;
      *enforce_default = 1;
      if (strcmp(mode, "remote_default") == 0)
      {
         snprintf(selected, selected_cap, "%s", default_oid);
         return 0;
      }
      /* An explicit ref was already copied above. It must be verified after
       * the fetch, then creation will incorporate default_oid if needed. */
      if (strcmp(mode, "local_default") != 0 && selected[0])
         return csw_ref_exists(git_root, selected) ? 0 : -1;
   }

   /* No origin: the repository's local default is the only freshness authority
    * available. local_default explicitly selects this path even with origin. */
   char def[128];
   csw_remote_default(git_root, def, sizeof def);
   if (strcmp(mode, "local_default") == 0)
   {
      if (def[0] && csw_ref_exists(git_root, def))
      {
         snprintf(selected, selected_cap, "%s", def);
         return 0;
      }
      if (csw_ref_exists(git_root, "main"))
         snprintf(selected, selected_cap, "%s", "main");
      else if (csw_ref_exists(git_root, "master"))
         snprintf(selected, selected_cap, "%s", "master");
      return selected[0] ? 0 : -1;
   }

   if (selected[0])
   {
      if (!csw_ref_exists(git_root, selected))
         return -1;
   }
   else if (csw_resolve_candidate(git_root, "main", selected, selected_cap) ||
            csw_resolve_candidate(git_root, "master", selected, selected_cap))
   {
      /* selected below */
   }
   else
      return -1;

   if (!default_oid[0])
   {
      const char *const oid_argv[] = {"git",      "-C",     git_root, "rev-parse",
                                      "--verify", selected, NULL};
      if (csw_git(oid_argv, default_oid, default_cap) != 0)
         return -1;
   }
   *enforce_default = 1;
   return 0;
}

int client_session_worktree_base(const char *git_root, char *buf, size_t cap)
{
   char default_oid[96];
   int enforce_default = 0;
   return csw_session_bases(git_root, buf, cap, default_oid, sizeof default_oid, &enforce_default);
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

static int csw_path_has_prefix(const char *path, const char *prefix)
{
   if (!path || !prefix)
      return 0;
   size_t n = strlen(prefix);
   return strncmp(path, prefix, n) == 0 && (path[n] == '\0' || path[n] == '/');
}

/* Keep Aimee's checkout store out of the source checkout's untracked-file
 * view without modifying the project's committed .gitignore. The exclude file
 * belongs to Git's common directory, so one entry covers the primary checkout
 * and every linked worktree. Provisioning holds the common-dir lock while this
 * runs, making the idempotent read/append safe across simultaneous starts. */
static int csw_exclude_worktree_store(const char *git_root)
{
   char exclude_path[4096];
   const char *const argv[] = {
       "git",        "-C",           git_root, "rev-parse", "--path-format=absolute",
       "--git-path", "info/exclude", NULL};
   if (csw_git(argv, exclude_path, sizeof exclude_path) != 0 || !exclude_path[0])
      return -1;

   static const char entry[] = "/.aimee/worktrees/";
   FILE *f = fopen(exclude_path, "r");
   if (f)
   {
      char line[4096];
      while (fgets(line, sizeof line, f))
      {
         line[strcspn(line, "\r\n")] = '\0';
         if (strcmp(line, entry) == 0)
         {
            fclose(f);
            return 0;
         }
      }
      fclose(f);
   }

   f = fopen(exclude_path, "a");
   if (!f)
      return -1;
   int ok = fprintf(f, "\n%s\n", entry) > 0;
   if (fclose(f) != 0)
      ok = 0;
   return ok ? 0 : -1;
}

/* Lexical absolute normalisation is intentional. Tool paths need not exist yet,
 * while realpath() would reject the most common Write case. */
static int csw_normalize(const char *cwd, const char *path, char *out, size_t cap)
{
   char joined[8192];
   if (!path || !path[0])
      path = cwd;
   if (!path || !path[0] || !out || cap < 2)
      return -1;
   if (path[0] == '/')
      snprintf(joined, sizeof joined, "%s", path);
   else
      snprintf(joined, sizeof joined, "%s/%s", cwd ? cwd : "", path);

   const int absolute = joined[0] == '/';
   size_t marks[1024], depth = 0, used = 0;
   const char *p = joined;
   if (absolute)
   {
      out[used++] = '/';
      out[used] = '\0';
   }
   while (*p)
   {
      while (*p == '/')
         p++;
      if (!*p)
         break;
      const char *end = strchr(p, '/');
      size_t n = end ? (size_t)(end - p) : strlen(p);
      if (n == 1 && p[0] == '.')
      {
         p += n;
         continue;
      }
      if (n == 2 && p[0] == '.' && p[1] == '.')
      {
         if (depth)
         {
            used = marks[--depth];
            if (used > 1 && out[used - 1] == '/')
               used--;
            out[used] = '\0';
         }
         p += n;
         continue;
      }
      if (used && out[used - 1] != '/')
      {
         if (used + 1 >= cap)
            return -1;
         out[used++] = '/';
      }
      if (depth >= sizeof marks / sizeof marks[0] || used + n >= cap)
         return -1;
      marks[depth++] = used;
      memcpy(out + used, p, n);
      used += n;
      out[used] = '\0';
      p += n;
   }
   if (!used)
      snprintf(out, cap, "%s", absolute ? "/" : ".");
   return 0;
}

/* Resolve the primary checkout behind cwd. `git --show-toplevel` returns the
 * current linked worktree, so an Aimee worktree must be traced through its
 * common git dir to avoid nesting one session beneath another. */
static int csw_repo_context(const char *cwd, const char *sid, char *source, size_t source_cap,
                            char *top, size_t top_cap, int *already_owned)
{
   *already_owned = 0;
   const char *const rp[] = {"git", "-C", cwd, "rev-parse", "--show-toplevel", NULL};
   if (csw_git(rp, top, top_cap) != 0 || !top[0])
      return -1;

   char key[SESSION_WORKTREE_KEY_MAX];
   client_session_worktree_key(sid, key, sizeof key);
   const char *managed = strstr(top, "/.aimee/worktrees/");
   if (managed)
   {
      const char *found = managed + strlen("/.aimee/worktrees/");
      const char *slash = strchr(found, '/');
      size_t found_n = slash ? (size_t)(slash - found) : strlen(found);
      *already_owned = key[0] && strlen(key) == found_n && strncmp(found, key, found_n) == 0;

      char common[4096];
      const char *const cd[] = {
          "git", "-C", top, "rev-parse", "--path-format=absolute", "--git-common-dir", NULL};
      if (csw_git(cd, common, sizeof common) != 0 || !common[0])
         return -1;
      size_t n = strlen(common);
      if (n <= 5 || strcmp(common + n - 5, "/.git") != 0)
         return -1;
      common[n - 5] = '\0';
      snprintf(source, source_cap, "%s", common);
      return 0;
   }

   /* Native client worktrees already provide a per-session process cwd. Keep
    * them instead of creating a redundant Aimee tree inside their repository. */
   if (strstr(top, "/.claude/worktrees/") || strstr(top, "/.codex/worktrees/"))
   {
      *already_owned = 1;
      snprintf(source, source_cap, "%s", top);
      return 0;
   }

   snprintf(source, source_cap, "%s", top);
   return 0;
}

static int csw_ensure_at_unlocked(const char *sid, const char *cwd, char *out, size_t cap)
{
   if (!out || !cap)
      return -1;
   out[0] = '\0';

   /* Need a stable session id to name the worktree. */
   if (!sid || !sid[0] || !cwd || !cwd[0])
      return -1;
   char key[80];
   client_session_worktree_key(sid, key, sizeof key);
   if (!key[0])
      return -1;

   char git_root[4096], current_top[4096];
   int already_owned = 0;
   if (csw_repo_context(cwd, sid, git_root, sizeof git_root, current_top, sizeof current_top,
                        &already_owned) != 0)
      return -1; /* not a git repo -> nothing to prepare */
   if (already_owned)
   {
      snprintf(out, cap, "%s", current_top);
      return 0;
   }

   if (csw_exclude_worktree_store(git_root) != 0)
   {
      fprintf(stderr, "aimee: could not hide the internal session workspace store\n");
      return -2;
   }

   char wt[4200];
   if (snprintf(wt, sizeof wt, "%s/.aimee/worktrees/%s/main", git_root, key) >= (int)sizeof wt)
      return -2;
   char branch[128];
   if (snprintf(branch, sizeof branch, "aimee/session/%s", key) >= (int)sizeof branch)
      return -2;

   struct stat st;
   int created = 0;
   char default_oid[96] = "";
   int enforce_default = 0;
   if (stat(wt, &st) != 0)
   {
      char base_ref[192];
      if (csw_session_bases(git_root, base_ref, sizeof base_ref, default_oid, sizeof default_oid,
                            &enforce_default) != 0)
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
      if (csw_git(add_argv, NULL, 0) == 0)
         created = 1;
      else
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

   /* A user-selected feature/release base may already contain the exact
    * default tip observed above. In that common case this is a no-op. When it
    * does not, merge only into the brand-new Aimee session branch: the source
    * branch and shared checkout remain untouched. A conflict destroys this
    * empty session checkout and fails launch rather than handing the client a
    * half-merged tree. Reattached branches are preserved verbatim because they
    * may contain work from an earlier incarnation of this same session. */
   if (created && enforce_default && default_oid[0])
   {
      const char *const ancestor_argv[] = {"git",           "-C",        wt,     "merge-base",
                                           "--is-ancestor", default_oid, "HEAD", NULL};
      int ancestor = csw_git(ancestor_argv, NULL, 0);
      if (ancestor != 0)
      {
         const char *const merge_argv[] = {"git",
                                           "-C",
                                           wt,
                                           "-c",
                                           "user.name=Aimee Session",
                                           "-c",
                                           "user.email=aimee-session@localhost",
                                           "-c",
                                           "commit.gpgSign=false",
                                           "merge",
                                           "--no-edit",
                                           default_oid,
                                           NULL};
         if (ancestor < 0 || csw_git(merge_argv, NULL, 0) != 0)
         {
            const char *const abort_argv[] = {"git", "-C", wt, "merge", "--abort", NULL};
            const char *const remove_argv[] = {"git",    "-C",      git_root, "worktree",
                                               "remove", "--force", wt,       NULL};
            const char *const branch_argv[] = {"git", "-C", git_root, "branch", "-D", branch, NULL};
            (void)csw_git(abort_argv, NULL, 0);
            (void)csw_git(remove_argv, NULL, 0);
            (void)csw_git(branch_argv, NULL, 0);
            fprintf(stderr, "aimee: the requested session base could not incorporate the latest "
                            "default branch without conflicts; no client was started\n");
            return -2;
         }
      }
   }

   csw_reclaim_legacy(git_root, sid, key);

   snprintf(out, cap, "%s", wt);
   return 0;
}

int client_session_worktree_ensure_at(const char *sid, const char *cwd, char *out, size_t cap)
{
   if (!out || !cap)
      return -1;
   out[0] = '\0';

   char source[4096], top[4096];
   int already_owned = 0;
   if (!sid || !sid[0] || !cwd || !cwd[0] ||
       csw_repo_context(cwd, sid, source, sizeof source, top, sizeof top, &already_owned) != 0 ||
       already_owned)
      return csw_ensure_at_unlocked(sid, cwd, out, cap);

   char common_dir[4096], lock_path[4200];
   if (csw_git_common_dir(source, common_dir, sizeof common_dir) != 0 ||
       snprintf(lock_path, sizeof lock_path, "%s/aimee-session-worktrees.lock", common_dir) >=
           (int)sizeof lock_path)
      return -2;
   int lock_fd = open(lock_path, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
   if (lock_fd < 0 || flock(lock_fd, LOCK_EX) != 0)
   {
      if (lock_fd >= 0)
         close(lock_fd);
      fprintf(stderr, "aimee: could not lock session worktree provisioning for %s\n", source);
      return -2;
   }
   int rc = csw_ensure_at_unlocked(sid, cwd, out, cap);
   (void)flock(lock_fd, LOCK_UN);
   close(lock_fd);
   return rc;
}

int client_session_worktree_ensure(const char *sid, char *out, size_t cap)
{
   char cwd[4096];
   if (!getcwd(cwd, sizeof cwd))
      return -1;
   return client_session_worktree_ensure_at(sid, cwd, out, cap);
}

static int csw_has_native_worktree_marker(const char *path);

int client_session_worktree_release_at(const char *sid, const char *cwd)
{
   if (!sid || !sid[0] || !cwd || !cwd[0])
      return -1;
   char key[SESSION_WORKTREE_KEY_MAX];
   client_session_worktree_key(sid, key, sizeof key);
   if (!key[0])
      return -1;

   char source[4096], top[4096];
   int owned = 0;
   if (csw_repo_context(cwd, sid, source, sizeof source, top, sizeof top, &owned) != 0 ||
       csw_has_native_worktree_marker(top))
      return 1;
   char wt[4200];
   if (snprintf(wt, sizeof wt, "%s/.aimee/worktrees/%s/main", source, key) >= (int)sizeof wt)
      return -1;
   struct stat st;
   if (stat(wt, &st) != 0)
      return 1;

   /* No --force: dirty and untracked state is persistence, not garbage. */
   const char *const rm_argv[] = {"git", "-C", source, "worktree", "remove", wt, NULL};
   if (csw_git(rm_argv, NULL, 0) != 0)
      return 1;

   char branch[128];
   if (snprintf(branch, sizeof branch, "aimee/session/%s", key) < (int)sizeof branch)
   {
      const char *const br_argv[] = {"git", "-C", source, "branch", "-d", branch, NULL};
      (void)csw_git(br_argv, NULL, 0); /* merged only */
   }
   char parent[4200];
   if (snprintf(parent, sizeof parent, "%s/.aimee/worktrees/%s", source, key) < (int)sizeof parent)
      (void)rmdir(parent);
   return 0;
}

static int csw_is_foreign_worktree(const char *path, const char *sid)
{
   const char *m = path ? strstr(path, "/.aimee/worktrees/") : NULL;
   if (!m)
      return 0;
   m += strlen("/.aimee/worktrees/");
   const char *slash = strchr(m, '/');
   size_t n = slash ? (size_t)(slash - m) : strlen(m);
   char key[SESSION_WORKTREE_KEY_MAX];
   client_session_worktree_key(sid, key, sizeof key);
   return !key[0] || strlen(key) != n || strncmp(m, key, n) != 0;
}

static int csw_has_native_worktree_marker(const char *path)
{
   return path && (strstr(path, "/.claude/worktrees/") || strstr(path, "/.codex/worktrees/"));
}

/* Native client worktrees are valid isolation boundaries too, but a session may
 * only address the native tree it is already running in.  Their directory names
 * are client-owned and cannot be derived from Aimee's sid, so bind them to the
 * canonical git top-level instead of guessing at a client-specific key scheme. */
static int csw_is_foreign_native_path(const char *path, const char *cwd, const char *sid)
{
   if (!csw_has_native_worktree_marker(path))
      return 0;
   char source[4096], top[4096];
   int owned = 0;
   if (csw_repo_context(cwd, sid, source, sizeof source, top, sizeof top, &owned) != 0)
      return 1;
   return !csw_has_native_worktree_marker(top) || !csw_path_has_prefix(path, top);
}

int client_session_worktree_route_path(const char *sid, const char *cwd, const char *input,
                                       char *out, size_t cap)
{
   if (!out || !cap)
      return -2;
   out[0] = '\0';
   char target[8192];
   if (csw_normalize(cwd, input, target, sizeof target) != 0)
      return -2;
   if (csw_is_foreign_worktree(target, sid) || csw_is_foreign_native_path(target, cwd, sid))
      return -3;

   /* Decide whether the target belongs to this repository before provisioning
    * anything. External paths (for example a read from a client-owned memory
    * store) are intentionally left alone and must not depend on the repository
    * having a reachable remote/default branch. */
   char source[4096], top[4096];
   int owned = 0;
   if (csw_repo_context(cwd, sid, source, sizeof source, top, sizeof top, &owned) != 0)
      return 1;
   if (owned && csw_path_has_prefix(target, top))
   {
      snprintf(out, cap, "%s", target);
      return 0;
   }

   const char *relative = NULL;
   if (csw_path_has_prefix(target, top))
      relative = target + strlen(top);
   else if (csw_path_has_prefix(target, source))
      relative = target + strlen(source);
   else
   {
      snprintf(out, cap, "%s", target); /* non-repository path: unchanged */
      return 1;
   }

   char wt[4096];
   int prepared = client_session_worktree_ensure_at(sid, cwd, wt, sizeof wt);
   if (prepared != 0)
      return prepared == -1 ? 1 : prepared;
   if (csw_path_has_prefix(target, wt))
   {
      snprintf(out, cap, "%s", target);
      return 0;
   }

   /* cwd may itself be another Aimee worktree. In that case paths are relative
    * to its top-level checkout, but must land at the same repository-relative
    * location in this session's checkout. */
   if (snprintf(out, cap, "%s%s", wt, relative) >= (int)cap)
      return -2;
   return 0;
}

static int csw_shell_quote(const char *raw, char *out, size_t cap)
{
   size_t used = 0;
   if (!cap)
      return -1;
   out[0] = '\0';
#define CSW_PUT(ch)                                                                                \
   do                                                                                              \
   {                                                                                               \
      if (used + 1 >= cap)                                                                         \
         return -1;                                                                                \
      out[used++] = (ch);                                                                          \
   } while (0)
   CSW_PUT('\'');
   for (const char *p = raw; p && *p; p++)
   {
      if (*p == '\'')
      {
         CSW_PUT('\'');
         CSW_PUT('\\');
         CSW_PUT('\'');
         CSW_PUT('\'');
      }
      else
         CSW_PUT(*p);
   }
   CSW_PUT('\'');
   out[used] = '\0';
#undef CSW_PUT
   return 0;
}

int client_session_worktree_route_command(const char *sid, const char *cwd, const char *command,
                                          char *out, size_t cap)
{
   if (!command || !out || !cap)
      return -2;
   if (csw_is_foreign_worktree(command, sid))
      return -3;
   char source[4096], top[4096];
   int owned = 0;
   if (csw_repo_context(cwd, sid, source, sizeof source, top, sizeof top, &owned) != 0)
      return 1;
   if (csw_has_native_worktree_marker(command))
   {
      /* A command is opaque shell text. Conservatively reject a native marker
       * unless the command names this process's exact native worktree root. */
      const char *markers[] = {"/.claude/worktrees/", "/.codex/worktrees/", NULL};
      int foreign = !csw_has_native_worktree_marker(top);
      for (int i = 0; !foreign && markers[i]; i++)
      {
         const char *expected = strstr(top, markers[i]);
         const char *at = command;
         while ((at = strstr(at, markers[i])) != NULL)
         {
            size_t n = expected ? strlen(expected) : 0;
            if (!expected || strncmp(at, expected, n) != 0 ||
                (at[n] != '\0' && at[n] != '/' && at[n] != '\'' && at[n] != '"' && at[n] != ' '))
            {
               foreign = 1;
               break;
            }
            at += n;
         }
      }
      if (foreign)
         return -3;
   }
   char mapped_cwd[8192];
   int rc = client_session_worktree_route_path(sid, cwd, NULL, mapped_cwd, sizeof mapped_cwd);
   if (rc != 0)
      return rc;
   char quoted[16384], prefix[16416];
   if (csw_shell_quote(mapped_cwd, quoted, sizeof quoted) != 0 ||
       snprintf(prefix, sizeof prefix, "cd -- %s && ", quoted) >= (int)sizeof prefix)
      return -2;
   if (strncmp(command, prefix, strlen(prefix)) == 0)
   {
      snprintf(out, cap, "%s", command);
      return 0;
   }

   /* Preserve explicit absolute source paths, but make them name the same
    * repository-relative object in the session checkout. This covers commands
    * generated with an absolute path even though their execution cwd is also
    * redirected. */
   char wt[4096], remapped[32768];
   rc = client_session_worktree_ensure_at(sid, cwd, wt, sizeof wt);
   if (rc != 0)
      return rc == -1 ? 1 : rc;
   size_t used = 0;
   for (const char *p = command; *p;)
   {
      const char *from = NULL;
      size_t from_n = 0;
      if (csw_path_has_prefix(p, wt))
      {
         from = wt;
         from_n = strlen(wt);
      }
      else if (csw_path_has_prefix(p, top))
      {
         from = top;
         from_n = strlen(top);
      }
      else if (strcmp(top, source) != 0 && csw_path_has_prefix(p, source))
      {
         from = source;
         from_n = strlen(source);
      }
      if (from)
      {
         size_t n = strlen(wt);
         if (used + n >= sizeof remapped)
            return -2;
         memcpy(remapped + used, wt, n);
         used += n;
         p += from_n;
      }
      else
      {
         if (used + 1 >= sizeof remapped)
            return -2;
         remapped[used++] = *p++;
      }
   }
   remapped[used] = '\0';

   if (owned)
   {
      snprintf(out, cap, "%s", remapped);
      return 0;
   }

   if (snprintf(out, cap, "cd -- %s && %s", quoted, remapped) >= (int)cap)
      return -2;
   return 0;
}

/* Relative path from an absolute directory to an absolute target. */
static int csw_relative(const char *dir, const char *target, char *out, size_t cap)
{
   char a[8192], b[8192];
   if (csw_normalize("/", dir, a, sizeof a) != 0 || csw_normalize("/", target, b, sizeof b) != 0)
      return -1;
   size_t common = 1, last_slash = 0;
   while (a[common] && b[common] && a[common] == b[common])
   {
      if (a[common] == '/')
         last_slash = common;
      common++;
   }
   if ((!a[common] && (b[common] == '/' || !b[common])) ||
       (!b[common] && (a[common] == '/' || !a[common])))
      last_slash = common;
   const char *tail = b + last_slash;
   while (*tail == '/')
      tail++;
   size_t used = 0;
   const char *rest = a + last_slash;
   while (*rest == '/')
      rest++;
   for (const char *p = rest; *p; p++)
      if (*p == '/' || p == rest)
      {
         if (used + 3 >= cap)
            return -1;
         memcpy(out + used, "../", 3);
         used += 3;
      }
   size_t n = strlen(tail);
   if (used + n + 1 > cap)
      return -1;
   memcpy(out + used, tail, n + 1);
   if (!used && !n)
      snprintf(out, cap, ".");
   return 0;
}

int client_session_worktree_route_patch(const char *sid, const char *cwd, const char *patch,
                                        char *out, size_t cap)
{
   if (!patch || !out || !cap)
      return -2;
   static const char *const leaders[] = {
       "*** Add File: ", "*** Update File: ", "*** Delete File: ", "*** Move to: ", NULL};
   size_t used = 0;
   int changed = 0;
   const char *line = patch;
   while (*line)
   {
      const char *end = strchr(line, '\n');
      size_t line_n = end ? (size_t)(end - line) : strlen(line);
      const char *path = NULL;
      size_t leader_n = 0;
      int line_routed = 0;
      for (int i = 0; leaders[i]; i++)
      {
         size_t n = strlen(leaders[i]);
         if (line_n >= n && strncmp(line, leaders[i], n) == 0)
         {
            path = line + n;
            leader_n = n;
            break;
         }
      }
      if (path)
      {
         char raw[8192], mapped[8192], relative[8192];
         size_t n = line_n - leader_n;
         if (n >= sizeof raw)
            return -2;
         memcpy(raw, path, n);
         raw[n] = '\0';
         int rc = client_session_worktree_route_path(sid, cwd, raw, mapped, sizeof mapped);
         if (rc < 0)
            return rc;
         if (rc == 0)
         {
            if (csw_relative(cwd, mapped, relative, sizeof relative) != 0)
               return -2;
            int wrote = snprintf(out + used, cap - used, "%.*s%s", (int)leader_n, line, relative);
            if (wrote < 0 || (size_t)wrote >= cap - used)
               return -2;
            used += (size_t)wrote;
            changed = 1;
            line_routed = 1;
         }
      }
      if (!line_routed)
      {
         if (used + line_n + 1 >= cap)
            return -2;
         memcpy(out + used, line, line_n);
         used += line_n;
      }
      if (end)
      {
         if (used + 2 > cap)
            return -2;
         out[used++] = '\n';
         line = end + 1;
      }
      else
         line += line_n;
   }
   out[used] = '\0';
   return changed ? 0 : 1;
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
int client_session_worktree_ensure_at(const char *sid, const char *cwd, char *out, size_t cap)
{
   (void)cwd;
   return client_session_worktree_ensure(sid, out, cap);
}
int client_session_worktree_release_at(const char *sid, const char *cwd)
{
   (void)sid;
   (void)cwd;
   return 1;
}
int client_session_worktree_route_path(const char *sid, const char *cwd, const char *input,
                                       char *out, size_t cap)
{
   (void)sid;
   (void)cwd;
   (void)input;
   if (out && cap)
      out[0] = '\0';
   return 1;
}
int client_session_worktree_route_command(const char *sid, const char *cwd, const char *command,
                                          char *out, size_t cap)
{
   (void)sid;
   (void)cwd;
   (void)command;
   if (out && cap)
      out[0] = '\0';
   return 1;
}
int client_session_worktree_route_patch(const char *sid, const char *cwd, const char *patch,
                                        char *out, size_t cap)
{
   (void)sid;
   (void)cwd;
   (void)patch;
   if (out && cap)
      out[0] = '\0';
   return 1;
}
#endif /* _WIN32 */
