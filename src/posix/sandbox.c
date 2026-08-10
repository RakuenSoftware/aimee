/*
 * posix/sandbox.c: Linux namespace sandbox for tool execution.
 *
 * Provides optional OS-level isolation around agent tool execution using
 * Linux user+mount namespaces.  Falls back safely with a warning when
 * namespaces are unavailable (nested containers, restricted kernels).
 *
 * Supported modes:
 *   off            — plain fork/exec, no isolation
 *   workspace_only — new mount namespace; bind-mount workspace + essentials only
 *   allowlist      — new mount namespace; bind-mount configured paths + essentials
 *
 * Network isolation (optional):
 *   Creates a new network namespace so the child cannot reach the network.
 *   Falls back silently if CLONE_NEWNET is unavailable.
 *
 * Container detection:
 *   Checks /.dockerenv, /run/.containerenv, and /proc/1/cgroup for container
 *   markers.  Detection is DIAGNOSTIC, not a verdict: nested user namespaces work
 *   inside many containers, so availability is decided by actually attempting
 *   unshare(CLONE_NEWUSER).  Container-ness only shapes the reason string when that
 *   real probe fails, so the message points at the container rather than the kernel.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "sandbox.h"
#include "aimee_home.h"
#include "log.h"

#define log_warn(...) aimee_log(LOG_WARN, "sandbox", __VA_ARGS__)
#define log_info(...) aimee_log(LOG_INFO, "sandbox", __VA_ARGS__)

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

/* -------------------------------------------------------------------------
 * Audit hook + test seam (see sandbox.h)
 * ---------------------------------------------------------------------- */

/* Installed once at startup by the server-only bridge; NULL = no audit. */
static sandbox_audit_hook_fn g_sbx_audit_hook = NULL;
void sandbox_set_audit_hook(sandbox_audit_hook_fn fn)
{
   g_sbx_audit_hook = fn;
}

/* Test-only availability override (NULL in production). */
static int (*g_sbx_avail_override)(const char **) = NULL;
void sandbox_set_available_override_for_test(int (*fn)(const char **reason))
{
   g_sbx_avail_override = fn;
}

/* Test-only effective-mode override; <0 means "no override" (production). */
static int g_sbx_mode_override = -1;
void sandbox_set_mode_override_for_test(int mode)
{
   g_sbx_mode_override = mode;
}

int sandbox_effective_mode(const sandbox_config_t *cfg)
{
   if (g_sbx_mode_override >= 0)
      return g_sbx_mode_override;
   return cfg ? (int)cfg->mode : SANDBOX_MODE_OFF;
}

/* Characters allowed in a bare program path we are willing to surface verbatim.
 * A real program token is a plain path; anything else is refused (see below). */
static int sbx_prog_char(char c)
{
   return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' ||
          c == '/' || c == '.' || c == '-' || c == '+';
}

/* True if the whitespace-delimited token [tok, end) is a SIMPLE `NAME=value`
 * environment assignment whose whole token is safe to skip: the value must
 * contain no shell byte that could span the whitespace boundary or start a
 * construct (quote, '$', backtick, backslash) or otherwise smuggle secret bytes
 * into the next token. Anything fancier and we refuse to keep parsing. */
static int sbx_is_simple_assignment(const char *tok, const char *end)
{
   const char *p = tok;
   if (!((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') || *p == '_'))
      return 0;
   while (p < end && ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') ||
                      (*p >= '0' && *p <= '9') || *p == '_'))
      p++;
   if (p >= end || *p != '=')
      return 0; /* not NAME= */
   for (const char *v = p + 1; v < end; v++)
   {
      char c = *v;
      if (c == '\'' || c == '"' || c == '$' || c == '`' || c == '\\' || c == '(' || c == ')' ||
          c == ';' || c == '&' || c == '|' || c == '<' || c == '>' || c == '{' || c == '}')
         return 0;
   }
   return 1;
}

/* Safe-by-construction: emit the program basename ONLY when the command is a run
 * of simple `NAME=value` assignments followed by a bare program path. Any shell
 * construct in the way — a leading subshell '(', a quote, '$', a value that spans
 * whitespace, an assignment we can't cleanly skip — could carry secret bytes, so
 * we emit the fixed marker "unparsed" instead of raw command text. Emitting NO
 * program bytes is strictly safer than risking a secret in the audit trail. */
void sandbox_command_program(const char *cmd, char *out, size_t out_len)
{
   if (!out || out_len == 0)
      return;
   out[0] = '\0';
   if (!cmd)
      return;
   const char *p = cmd;
   for (;;)
   {
      while (*p == ' ' || *p == '\t')
         p++;
      if (!*p)
         return; /* only assignments / blank — no program to name */
      const char *end = p;
      while (*end && *end != ' ' && *end != '\t')
         end++;
      if (sbx_is_simple_assignment(p, end))
      {
         p = end; /* skip the whole assignment token, value included */
         continue;
      }
      break; /* p..end is the program-token candidate */
   }
   const char *end = p;
   while (*end && *end != ' ' && *end != '\t')
      end++;
   for (const char *s = p; s < end; s++)
   {
      if (!sbx_prog_char(*s))
      {
         snprintf(out, out_len, "unparsed");
         return;
      }
   }
   const char *base = p;
   for (const char *s = p; s < end; s++)
      if (*s == '/')
         base = s + 1;
   size_t blen = (size_t)(end - base);
   if (blen >= out_len)
      blen = out_len - 1;
   memcpy(out, base, blen);
   out[blen] = '\0';
}

/* Fire the audit hook (if installed) for a degraded-isolation outcome. Extracts
 * the NON-SECRET program name; never passes the raw command line. */
static void sbx_audit_degraded(const char *cmd, const sandbox_config_t *cfg, const char *verdict,
                               const char *reason)
{
   sandbox_audit_hook_fn h = g_sbx_audit_hook;
   if (!h)
      return;
   char prog[64];
   sandbox_command_program(cmd, prog, sizeof prog);
   h(prog, cfg->mode, cfg->network_isolated, verdict, reason ? reason : "");
}

/* -------------------------------------------------------------------------
 * Container detection
 * ---------------------------------------------------------------------- */

int sandbox_detect_container(void)
{
   /* Docker leaves a sentinel file */
   if (access("/.dockerenv", F_OK) == 0)
      return 1;

   /* Podman / systemd-nspawn */
   if (access("/run/.containerenv", F_OK) == 0)
      return 1;

   /* Inspect /proc/1/cgroup for container runtime signatures */
   FILE *f = fopen("/proc/1/cgroup", "r");
   if (f)
   {
      char line[256];
      while (fgets(line, sizeof(line), f))
      {
         if (strstr(line, "docker") || strstr(line, "lxc") || strstr(line, "kubepods") ||
             strstr(line, "containerd"))
         {
            fclose(f);
            return 1;
         }
      }
      fclose(f);
   }

   return 0;
}

/* -------------------------------------------------------------------------
 * Availability check
 * ---------------------------------------------------------------------- */

int sandbox_available(const char **reason)
{
   if (g_sbx_avail_override)
      return g_sbx_avail_override(reason);
#ifndef __linux__
   if (reason)
      *reason = "Linux namespaces not available on this platform";
   return 0;
#else
   /* Being in a container is NOT a verdict. Nested user namespaces work inside many
    * containers, and this function already ends with an authoritative test: fork a
    * child and actually call unshare(CLONE_NEWUSER). Returning early on detection made
    * that test dead code in precisely the deployments that need it — aimee-server ships
    * as a container, so every co-located delegate shell was refused ("sandbox fallback
    * could not start") on a capability that was never measured, only inferred from
    * /.dockerenv.
    *
    * Container-ness is kept as DIAGNOSIS: when the real probe below fails, saying so in
    * container terms points at the right fix (grant the container the capability) rather
    * than at the kernel. Fail-closed is unchanged — an environment that genuinely cannot
    * create a user namespace still reports unavailable. */
   const int in_container = sandbox_detect_container();

   /* Probe whether unprivileged user namespaces are permitted.
    * The kernel sysctl /proc/sys/kernel/unprivileged_userns_clone (Debian/Ubuntu)
    * or /proc/sys/user/max_user_namespaces controls this. */
   {
      FILE *f = fopen("/proc/sys/kernel/unprivileged_userns_clone", "r");
      if (f)
      {
         int val = 0;
         int rc = fscanf(f, "%d", &val);
         fclose(f);
         if (rc == 1 && val == 0)
         {
            if (reason)
               *reason = in_container ? "unprivileged user namespaces disabled in this container "
                                        "(kernel.unprivileged_userns_clone=0)"
                                      : "unprivileged user namespaces disabled "
                                        "(kernel.unprivileged_userns_clone=0)";
            return 0;
         }
      }
   }

   /* Quick smoke-test: can we actually call unshare(CLONE_NEWUSER)?
    * We do this in a child so we don't contaminate the current process. */
   pid_t pid = fork();
   if (pid < 0)
   {
      if (reason)
         *reason = "fork failed during sandbox availability check";
      return 0;
   }
   if (pid == 0)
   {
      /* child: attempt user namespace creation and exit with status */
      if (unshare(CLONE_NEWUSER) == 0)
         _exit(0);
      _exit(1);
   }
   int status = 0;
   waitpid(pid, &status, 0);
   if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
   {
      if (reason)
         *reason = in_container ? "unshare(CLONE_NEWUSER) failed inside this container — grant it "
                                  "unprivileged user namespaces (or run delegates in their own "
                                  "container) to enable isolated execution"
                                : "unshare(CLONE_NEWUSER) failed — kernel may not support "
                                  "unprivileged user namespaces";
      return 0;
   }

   return 1;
#endif /* __linux__ */
}

/* -------------------------------------------------------------------------
 * Internal: write uid_map / gid_map to implement a root-in-namespace mapping.
 * Called from the child after unshare(CLONE_NEWUSER) so it can perform mounts.
 * ---------------------------------------------------------------------- */

#ifdef __linux__

static int write_file(const char *path, const char *content)
{
   int fd = open(path, O_WRONLY);
   if (fd < 0)
      return -1;
   size_t len = strlen(content);
   ssize_t n = write(fd, content, len);
   close(fd);
   return (n == (ssize_t)len) ? 0 : -1;
}

/* Map child's uid 0 → parent's real uid, and similarly for gid. */
static int setup_userns_maps(pid_t child_pid)
{
   char path[64];
   char map[64];
   uid_t uid = getuid();
   gid_t gid = getgid();

   snprintf(path, sizeof(path), "/proc/%d/uid_map", (int)child_pid);
   snprintf(map, sizeof(map), "0 %d 1\n", (int)uid);
   if (write_file(path, map) != 0)
      return -1;

   /* Must write "deny" to setgroups before writing gid_map */
   snprintf(path, sizeof(path), "/proc/%d/setgroups", (int)child_pid);
   if (write_file(path, "deny\n") != 0)
      return -1;

   snprintf(path, sizeof(path), "/proc/%d/gid_map", (int)child_pid);
   snprintf(map, sizeof(map), "0 %d 1\n", (int)gid);
   if (write_file(path, map) != 0)
      return -1;

   return 0;
}

/* -------------------------------------------------------------------------
 * Internal: essentials to bind-mount in any sandboxed environment.
 * ---------------------------------------------------------------------- */

static const char *const g_essential_paths[] = {
    "/bin",       "/usr/bin",    "/usr/lib",   "/usr/lib64",
    "/lib",       "/lib64",      "/lib32",     "/etc/resolv.conf",
    "/etc/hosts", "/etc/passwd", "/etc/group", "/etc/ssl",
    "/proc",      "/dev",        "/sys",       "/tmp",
    NULL,
};

/* Bind-mount src → dst (create mountpoint as needed).
 * Returns 0 on success, -1 on error (errno set). */
static int bind_mount(const char *src, const char *dst)
{
   struct stat st;
   if (stat(src, &st) != 0)
      return 0; /* source doesn't exist — skip silently */

   if (S_ISDIR(st.st_mode))
   {
      if (mkdir(dst, 0755) != 0 && errno != EEXIST)
         return -1;
   }
   else
   {
      /* File — create empty target */
      int fd = open(dst, O_WRONLY | O_CREAT, 0644);
      if (fd < 0 && errno != EEXIST)
         return -1;
      if (fd >= 0)
         close(fd);
   }

   if (mount(src, dst, NULL, MS_BIND | MS_REC, NULL) != 0)
      return -1;

   return 0;
}

static int sandbox_path_is_abs_clean(const char *path)
{
   return path && path[0] == '/' && strstr(path, "/../") == NULL && strcmp(path, "/..") != 0 &&
          strstr(path, "/..") == NULL;
}

static void mkdir_sandbox_parents(const char *sandbox_root, char *dst)
{
   size_t root_len = strlen(sandbox_root);
   for (char *p = dst + root_len + 1; *p; p++)
   {
      if (*p == '/')
      {
         *p = '\0';
         mkdir(dst, 0755);
         *p = '/';
      }
   }
}

static void bind_mount_abs_path(const char *sandbox_root, const char *src)
{
   if (!sandbox_path_is_abs_clean(src))
      return;

   char dst[SANDBOX_MAX_PATH_LEN];
   int n = snprintf(dst, sizeof(dst), "%s%s", sandbox_root, src);
   if (n < 0 || (size_t)n >= sizeof(dst))
      return;
   mkdir_sandbox_parents(sandbox_root, dst);
   bind_mount(src, dst);
}

static int bind_mount_abs_path_required(const char *sandbox_root, const char *src, int read_only)
{
   struct stat st;

   if (!sandbox_path_is_abs_clean(src))
      return -1;
   if (stat(src, &st) != 0)
      return -1;

   char dst[SANDBOX_MAX_PATH_LEN];
   int n = snprintf(dst, sizeof(dst), "%s%s", sandbox_root, src);
   if (n < 0 || (size_t)n >= sizeof(dst))
      return -1;
   mkdir_sandbox_parents(sandbox_root, dst);
   if (bind_mount(src, dst) != 0)
      return -1;
   if (read_only && mount(dst, dst, NULL, MS_BIND | MS_REMOUNT | MS_RDONLY | MS_REC, NULL) != 0)
   {
      log_warn("sandbox: failed to remount %s read-only: %s", src, strerror(errno));
      return -1;
   }
   return 0;
}

static void bind_mount_parent_abs_path(const char *sandbox_root, const char *path)
{
   if (!sandbox_path_is_abs_clean(path))
      return;

   char parent[SANDBOX_MAX_PATH_LEN];
   int n = snprintf(parent, sizeof(parent), "%s", path);
   if (n < 0 || (size_t)n >= sizeof(parent))
      return;

   char *slash = strrchr(parent, '/');
   if (!slash || slash == parent)
      return;
   *slash = '\0';
   bind_mount_abs_path(sandbox_root, parent);
}

static void bind_mount_executable_parent(const char *sandbox_root, const char *path)
{
   if (!sandbox_path_is_abs_clean(path))
      return;

   char parent[SANDBOX_MAX_PATH_LEN];
   int n = snprintf(parent, sizeof(parent), "%s", path);
   if (n < 0 || (size_t)n >= sizeof(parent))
      return;

   char *slash = strrchr(parent, '/');
   if (!slash || slash == parent)
      return;
   *slash = '\0';
   bind_mount_abs_path(sandbox_root, parent);
}

static int sandbox_path_has_dir(const char *path_env, const char *dir)
{
   if (!path_env || !dir || !dir[0])
      return 0;
   size_t dir_len = strlen(dir);
   const char *p = path_env;
   while (*p)
   {
      const char *colon = strchr(p, ':');
      size_t len = colon ? (size_t)(colon - p) : strlen(p);
      if (len == dir_len && strncmp(p, dir, dir_len) == 0)
         return 1;
      if (!colon)
         break;
      p = colon + 1;
   }
   return 0;
}

static int sandbox_dir_has_aimee(const char *dir)
{
   if (!dir || !dir[0])
      return 0;
   char candidate[SANDBOX_MAX_PATH_LEN];
   int n = snprintf(candidate, sizeof(candidate), "%s/aimee", dir);
   return n > 0 && (size_t)n < sizeof(candidate) && access(candidate, X_OK) == 0;
}

static void sandbox_prepend_path_dir(char *buf, size_t buf_len, const char *dir)
{
   if (!buf || buf_len == 0 || !sandbox_dir_has_aimee(dir) || sandbox_path_has_dir(buf, dir))
      return;

   char old[8192];
   snprintf(old, sizeof(old), "%s", buf);
   if (old[0])
      snprintf(buf, buf_len, "%s:%s", dir, old);
   else
      snprintf(buf, buf_len, "%s", dir);
}

static void sandbox_prepare_child_path(void)
{
   char path_buf[8192];
   const char *old_path = getenv("PATH");
   snprintf(path_buf, sizeof(path_buf), "%s", old_path ? old_path : "");

   char exe[SANDBOX_MAX_PATH_LEN];
   ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
   if (n > 0)
   {
      exe[n] = '\0';
      char *slash = strrchr(exe, '/');
      if (slash && slash != exe)
      {
         *slash = '\0';
         sandbox_prepend_path_dir(path_buf, sizeof(path_buf), exe);
      }
   }

   const char *home = getenv("HOME");
   if (home && home[0])
   {
      char local_bin[SANDBOX_MAX_PATH_LEN];
      int hn = snprintf(local_bin, sizeof(local_bin), "%s/.local/bin", home);
      if (hn > 0 && (size_t)hn < sizeof(local_bin))
         sandbox_prepend_path_dir(path_buf, sizeof(path_buf), local_bin);
   }

   sandbox_prepend_path_dir(path_buf, sizeof(path_buf), "/usr/local/bin");
   if (path_buf[0])
      setenv("PATH", path_buf, 1);
}

static void bind_aimee_cli_paths(const char *sandbox_root)
{
   char exe[SANDBOX_MAX_PATH_LEN];
   ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
   if (n > 0)
   {
      exe[n] = '\0';
      bind_mount_executable_parent(sandbox_root, exe);
   }

   const char *path_env = getenv("PATH");
   if (!path_env || !path_env[0])
      return;

   const char *p = path_env;
   while (*p)
   {
      const char *colon = strchr(p, ':');
      size_t len = colon ? (size_t)(colon - p) : strlen(p);
      if (len > 0 && p[0] == '/' && len < SANDBOX_MAX_PATH_LEN)
      {
         char dir[SANDBOX_MAX_PATH_LEN];
         char candidate[SANDBOX_MAX_PATH_LEN];
         memcpy(dir, p, len);
         dir[len] = '\0';
         int cn = snprintf(candidate, sizeof(candidate), "%s/aimee", dir);
         if (cn > 0 && (size_t)cn < sizeof(candidate) && access(candidate, X_OK) == 0)
            bind_mount_abs_path(sandbox_root, dir);
      }
      if (!colon)
         break;
      p = colon + 1;
   }

   const char *home = getenv("HOME");
   if (home && home[0])
   {
      char local_bin[SANDBOX_MAX_PATH_LEN];
      int hn = snprintf(local_bin, sizeof(local_bin), "%s/.local/bin", home);
      if (hn > 0 && (size_t)hn < sizeof(local_bin) && sandbox_dir_has_aimee(local_bin))
         bind_mount_abs_path(sandbox_root, local_bin);
   }
   if (sandbox_dir_has_aimee("/usr/local/bin"))
      bind_mount_abs_path(sandbox_root, "/usr/local/bin");
}

static void bind_aimee_runtime_paths(const char *sandbox_root)
{
   /* Delegate shell tools need the active aimee runtime sockets even in
    * workspace-only sandboxes; otherwise `aimee index` and `aimee memory`
    * see an empty/unavailable runtime instead of the parent service. */
   bind_mount_abs_path(sandbox_root, aimee_home());
   bind_aimee_cli_paths(sandbox_root);

   const char *sock = getenv("AIMEE_SOCK");
   if (sock && sock[0])
      bind_mount_parent_abs_path(sandbox_root, sock);
}

/* -------------------------------------------------------------------------
 * Internal: set up the mount namespace inside the child.
 *
 * Strategy:
 *   1. Create a tmpfs at /tmp/aimee-sandbox-XXXXXX as the new root.
 *   2. Bind-mount selected paths into it.
 *   3. pivot_root or chroot into it.
 * ---------------------------------------------------------------------- */

/* Remove sandbox roots left behind by earlier runs.
 *
 * setup_mount_ns() mkdtemp()s /tmp/aimee-sandbox-XXXXXX in the HOST namespace,
 * then mounts a tmpfs over it inside the child's new mount namespace. When the
 * sandboxed process exits the namespace goes with it and the tmpfs unmounts —
 * but the host-side directory stays. Nothing removes it: it is created in the
 * child after fork, so the parent never learns the path, and sandbox_spawn()
 * returns the pid without waiting, so there is no point in this module where
 * the child is known to have exited.
 *
 * One empty directory leaks per sandboxed run. They accumulate: a development
 * box had 477, part of a /tmp that had exhausted its inode table (857k inodes
 * across 40k directories, with 45GB of space still free) — at which point
 * nothing on the machine could create a file.
 *
 * Sweeping with rmdir is safe BY CONSTRUCTION and needs no age heuristic: root
 * creation is serialised across processes until the tmpfs is mounted, a root
 * that is then in use cannot be removed (rmdir fails EBUSY), and a root that is
 * merely non-empty fails ENOTEMPTY. Only the leaked ones — unmounted and empty
 * — can be removed, so a sweep can never disturb a running sandbox, including
 * one owned by a different process or user. Errors are ignored for exactly
 * that reason. */
static void reap_stale_sandbox_roots(void)
{
   DIR *d = opendir("/tmp");
   if (!d)
      return;
   struct dirent *e;
   while ((e = readdir(d)) != NULL)
   {
      if (strncmp(e->d_name, "aimee-sandbox-", 14) != 0)
         continue;
      char path[SANDBOX_MAX_PATH_LEN];
      int n = snprintf(path, sizeof(path), "/tmp/%s", e->d_name);
      if (n > 0 && (size_t)n < sizeof(path))
         (void)rmdir(path); /* EBUSY = in use, ENOTEMPTY = not ours to judge */
   }
   closedir(d);
}

static int setup_mount_ns(const sandbox_config_t *cfg, const char *workspace,
                          const char *read_only_path, const char *write_path)
{
   /* A second process must not reap our root between mkdtemp() and mount().
    * Locking the shared /tmp directory inode avoids creating another file that
    * itself needs lifecycle management. Linux flock() accepts directory fds. */
   int root_lock_fd = open("/tmp", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
   if (root_lock_fd < 0)
      return -1;
   if (flock(root_lock_fd, LOCK_EX) != 0)
   {
      close(root_lock_fd);
      return -1;
   }

   /* Clear roots orphaned by earlier runs before adding another. */
   reap_stale_sandbox_roots();

   /* Create a private tmpfs to serve as the sandbox root */
   char sandbox_root[] = "/tmp/aimee-sandbox-XXXXXX";
   if (mkdtemp(sandbox_root) == NULL)
   {
      close(root_lock_fd);
      return -1;
   }

   /* Mount tmpfs at sandbox root */
   if (mount("none", sandbox_root, "tmpfs", 0, "size=64m") != 0)
   {
      rmdir(sandbox_root);
      close(root_lock_fd);
      return -1;
   }
   close(root_lock_fd);

   /* Bind essential system paths */
   for (int i = 0; g_essential_paths[i]; i++)
   {
      char dst[SANDBOX_MAX_PATH_LEN];
      snprintf(dst, sizeof(dst), "%s%s", sandbox_root, g_essential_paths[i]);
      /* Ignore errors for optional paths (e.g., /lib64 may not exist) */
      bind_mount(g_essential_paths[i], dst);
   }

   bind_aimee_runtime_paths(sandbox_root);

   if (read_only_path && read_only_path[0] &&
       bind_mount_abs_path_required(sandbox_root, read_only_path, 1) != 0)
      return -1;
   if (write_path && write_path[0] &&
       bind_mount_abs_path_required(sandbox_root, write_path, 0) != 0)
      return -1;

   /* Bind workspace or allowlist paths */
   if (cfg->mode == SANDBOX_MODE_WORKSPACE_ONLY && workspace && workspace[0])
   {
      char dst[SANDBOX_MAX_PATH_LEN];
      snprintf(dst, sizeof(dst), "%s%s", sandbox_root, workspace);
      mkdir_sandbox_parents(sandbox_root, dst);
      bind_mount(workspace, dst);
   }
   else if (cfg->mode == SANDBOX_MODE_ALLOWLIST)
   {
      for (int i = 0; i < cfg->allow_path_count; i++)
      {
         const char *src = cfg->allow_paths[i];
         char dst[SANDBOX_MAX_PATH_LEN];
         snprintf(dst, sizeof(dst), "%s%s", sandbox_root, src);
         mkdir_sandbox_parents(sandbox_root, dst);
         bind_mount(src, dst);
      }
   }

   /* chroot into the sandbox root */
   if (chroot(sandbox_root) != 0)
      return -1;
   if (chdir("/") != 0)
      return -1;

   /* Change CWD to workspace if it exists inside sandbox */
   if (workspace && workspace[0])
      chdir(workspace); /* best-effort: ignore if not present */

   return 0;
}

/* -------------------------------------------------------------------------
 * Synchronisation pipe: parent signals child once uid/gid maps are written.
 * ---------------------------------------------------------------------- */

static int g_sync_pipe[2] = {-1, -1};

static void sync_parent_signal(void)
{
   char c = 'r';
   (void)write(g_sync_pipe[1], &c, 1);
   close(g_sync_pipe[1]);
   g_sync_pipe[1] = -1;
}

static void sync_child_wait(void)
{
   char c;
   (void)read(g_sync_pipe[0], &c, 1);
   close(g_sync_pipe[0]);
   g_sync_pipe[0] = -1;
}

/* -------------------------------------------------------------------------
 * sandbox_exec: fork the child under the configured namespaces.
 * ---------------------------------------------------------------------- */

static pid_t sandbox_exec_internal(const sandbox_config_t *cfg, const char *cmd, int out_fd,
                                   int err_fd, const char *workspace, const char *read_only_path,
                                   const char *write_path, int require_isolation)
{
   if (!cfg || cfg->mode == SANDBOX_MODE_OFF)
   {
      if (require_isolation)
         return -1;
      /* Plain fork/exec — existing behaviour */
      pid_t pid = fork();
      if (pid == 0)
      {
         dup2(out_fd, STDOUT_FILENO);
         dup2(err_fd, STDERR_FILENO);
         if (workspace && workspace[0])
            chdir(workspace);
         sandbox_prepare_child_path();
         execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
         _exit(127);
      }
      return pid;
   }

   /* Check availability; fall back with a warning if not possible */
   const char *unavail_reason = NULL;
   if (!sandbox_available(&unavail_reason))
   {
      if (require_isolation)
      {
         log_warn("sandbox: unavailable (%s) — refusing unsandboxed guarded execution",
                  unavail_reason ? unavail_reason : "unknown reason");
         sbx_audit_degraded(cmd, cfg, "refused", unavail_reason);
         return -1;
      }
      log_warn("sandbox: unavailable (%s) — running unsandboxed",
               unavail_reason ? unavail_reason : "unknown reason");
      sbx_audit_degraded(cmd, cfg, "unsandboxed_fallback", unavail_reason);
      pid_t pid = fork();
      if (pid == 0)
      {
         dup2(out_fd, STDOUT_FILENO);
         dup2(err_fd, STDERR_FILENO);
         if (workspace && workspace[0])
            chdir(workspace);
         sandbox_prepare_child_path();
         execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
         _exit(127);
      }
      return pid;
   }

   if (pipe(g_sync_pipe) != 0)
      return -1;
   /* Child -> parent readiness. setup_userns_maps() writes /proc/<pid>/uid_map, which
    * is only permitted once the child is actually IN its new user namespace. g_sync_pipe
    * only signals parent -> child ("maps written"), so without this second pipe the
    * parent raced the child's unshare() and — winning that race — wrote uid_map while
    * the child was still in the initial userns. That fails EPERM, so sandbox_exec
    * returned -1 and every sandboxed command reported a bare "fork failed". The bug was
    * invisible while sandbox.mode defaulted to OFF, because this path never ran. */
   int ready_pipe[2] = {-1, -1};
   if (pipe(ready_pipe) != 0)
   {
      close(g_sync_pipe[0]);
      close(g_sync_pipe[1]);
      return -1;
   }
   int setup_pipe[2] = {-1, -1};
   if (require_isolation && pipe(setup_pipe) != 0)
   {
      close(g_sync_pipe[0]);
      close(g_sync_pipe[1]);
      close(ready_pipe[0]);
      close(ready_pipe[1]);
      return -1;
   }

   /* Clone with a new user namespace */
   int clone_flags = CLONE_NEWUSER | SIGCHLD;

   /* Add mount namespace for filesystem isolation */
   if (cfg->mode != SANDBOX_MODE_OFF)
      clone_flags |= CLONE_NEWNS;

   /* Optionally isolate network */
   if (cfg->network_isolated)
      clone_flags |= CLONE_NEWNET;

   pid_t pid = fork();
   if (pid < 0)
   {
      close(g_sync_pipe[0]);
      close(g_sync_pipe[1]);
      close(ready_pipe[0]);
      close(ready_pipe[1]);
      if (setup_pipe[0] >= 0)
         close(setup_pipe[0]);
      if (setup_pipe[1] >= 0)
         close(setup_pipe[1]);
      return -1;
   }

   if (pid == 0)
   {
      /* Child — wait for uid/gid map to be written before doing anything */
      close(g_sync_pipe[1]);
      g_sync_pipe[1] = -1;
      close(ready_pipe[0]);
      if (setup_pipe[0] >= 0)
         close(setup_pipe[0]);

      /* Enter user namespace */
      if (unshare(clone_flags & ~SIGCHLD) != 0)
      {
         /* Report the FAILURE to the parent so it skips the map write (there is no
          * new userns to map) and releases us, instead of blocking on a readiness
          * byte that never comes. */
         char unshared = '0';
         (void)write(ready_pipe[1], &unshared, 1);
         close(ready_pipe[1]);
         sync_child_wait();
         if (require_isolation)
         {
            char failed = '0';
            (void)write(setup_pipe[1], &failed, 1);
            _exit(126);
         }
         /* Fall back: just exec without namespaces */
         dup2(out_fd, STDOUT_FILENO);
         dup2(err_fd, STDERR_FILENO);
         if (workspace && workspace[0])
            chdir(workspace);
         execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
         _exit(127);
      }

      /* We are now IN the new user namespace — tell the parent it is safe to write
       * our uid/gid maps, then wait for it to finish. */
      {
         char unshared = '1';
         (void)write(ready_pipe[1], &unshared, 1);
         close(ready_pipe[1]);
      }

      sync_child_wait();

      /* Set up mount namespace now that we have uid 0 in the new user ns */
      if (cfg->mode != SANDBOX_MODE_OFF)
      {
         if (setup_mount_ns(cfg, workspace, read_only_path, write_path) != 0)
         {
            /* Mount namespace setup failed — exec without FS isolation */
            if (require_isolation)
            {
               char failed = '0';
               (void)write(setup_pipe[1], &failed, 1);
               _exit(126);
            }
         }
      }

      if (require_isolation)
      {
         char ok = '1';
         (void)write(setup_pipe[1], &ok, 1);
         close(setup_pipe[1]);
      }

      dup2(out_fd, STDOUT_FILENO);
      dup2(err_fd, STDERR_FILENO);
      sandbox_prepare_child_path();
      execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
      _exit(127);
   }

   /* Parent: write uid/gid maps then signal child */
   close(g_sync_pipe[0]);
   g_sync_pipe[0] = -1;
   close(ready_pipe[1]);
   if (setup_pipe[1] >= 0)
      close(setup_pipe[1]);

   /* Block until the child reports whether it entered the new user namespace.
    * Writing uid_map before unshare() has taken effect fails EPERM — that race is
    * what made every sandboxed command report "fork failed". A short read (child
    * died) is treated as "did not unshare". */
   char child_unshared = '0';
   {
      ssize_t rn = read(ready_pipe[0], &child_unshared, 1);
      close(ready_pipe[0]);
      if (rn != 1)
         child_unshared = '0';
   }

   /* No new userns means there are no maps to write; skipping the write lets the
    * child's own non-namespaced fallback proceed instead of failing the exec. */
   if (child_unshared == '1' && setup_userns_maps(pid) != 0)
   {
      /* Maps failed — signal child anyway so it doesn't hang, then reap */
      sync_parent_signal();
      waitpid(pid, NULL, 0);
      if (setup_pipe[0] >= 0)
         close(setup_pipe[0]);
      return -1;
   }

   sync_parent_signal();
   if (require_isolation)
   {
      char setup_status = '0';
      ssize_t n = read(setup_pipe[0], &setup_status, 1);
      close(setup_pipe[0]);
      if (n != 1 || setup_status != '1')
      {
         /* Child reported that in-namespace setup (unshare / mount ns) failed;
          * a require-isolation exec is refused rather than downgraded. This is a
          * child-side degradation the parent CAN observe, so it is audited. */
         sbx_audit_degraded(cmd, cfg, "refused", "namespace setup failed in child");
         waitpid(pid, NULL, 0);
         return -1;
      }
   }
   log_info("sandbox: started pid %d mode=%s network_isolated=%d", (int)pid,
            sandbox_mode_to_string(cfg->mode), cfg->network_isolated);
   return pid;
}

pid_t sandbox_exec(const sandbox_config_t *cfg, const char *cmd, int out_fd, int err_fd,
                   const char *workspace)
{
   return sandbox_exec_internal(cfg, cmd, out_fd, err_fd, workspace, NULL, NULL, 0);
}

pid_t sandbox_exec_with_readonly(const sandbox_config_t *cfg, const char *cmd, int out_fd,
                                 int err_fd, const char *workspace, const char *read_only_path,
                                 const char *write_path)
{
   return sandbox_exec_internal(cfg, cmd, out_fd, err_fd, workspace, read_only_path, write_path, 1);
}

#endif /* __linux__ */
