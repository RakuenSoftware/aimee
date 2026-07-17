/* delegate_backend_docker.c: see delegate_backend_docker.h.
 *
 * acquire() runs `docker create --name <container> -v <workspace>:
 * /workspace -w /workspace <image> sleep infinity`, then `docker
 * start <container>`. exec() rides `docker exec -i <container> bash
 * -c '<b64-wrapped>'`, captures stdout/stderr/exit_code through pipes.
 * release() does `docker stop` (hibernate=1) or `docker rm -f`
 * (hibernate=0).
 *
 * File ops (read_file/write_file/list_dir) all route through
 * docker_exec — read does `cat <path>` then C-side slices for
 * offset/limit, write b64-encodes content and decodes inside the
 * container, list does `ls -1A` and splits lines. All paths are
 * workspace-relative (anchored at /workspace); absolute paths and
 * '..' segments are rejected.
 *
 * Cwd persistence is partial: set_cwd writes to the local state
 * struct and exec() prefixes `cd <cwd> &&` to subsequent commands
 * when set. */

#include "delegate_backend_docker.h"
#include "util.h"

#include "aimee.h"      /* MAX_PATH_LEN */
#include "aimee_home.h" /* aimee_home() — resolves the server's UDS path */
#include "config.h"     /* delegate_sandbox_package_access mode */

#include <assert.h>

/* Fixed in-container path for the bind-mounted aimee-server UDS. The delegate's
 * only outward channel: `--network none` cuts every IP route, and AIMEE_API_ENDPOINT
 * points the baked-in `aimee` CLI at this socket, so `aimee git_commit`,
 * `aimee web_search`, `aimee memory ...` reach the server (and nothing else). */
#define DELEGATE_SOCK_PATH "/run/aimee/aimee-http.sock"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include "log.h"
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* RFC 4648 base64 — same as the SSH backend uses. Kept local so this
 * source file stays standalone (no shared b64 header yet). */
int delegate_backend_docker_container_name(const char *task_id, char *out, size_t outsz)
{
   if (!task_id || !task_id[0] || !out || outsz == 0)
      return -1;
   const char *prefix = "aimee-delegate-";
   size_t plen = strlen(prefix);
   if (plen + strlen(task_id) + 1 > outsz)
      return -1;
   memcpy(out, prefix, plen);
   /* Sanitise: docker container names allow [a-zA-Z0-9_.-]+. Anything
    * else gets replaced with '_'. The leading char must be alnum;
    * "aimee-delegate-" satisfies that already. */
   size_t i = 0;
   for (const char *p = task_id; *p; p++)
   {
      char c = *p;
      if (isalnum((unsigned char)c) || c == '_' || c == '.' || c == '-')
         out[plen + i] = c;
      else
         out[plen + i] = '_';
      i++;
   }
   out[plen + i] = '\0';
   return 0;
}

int delegate_backend_docker_build_exec_command(const char *container_name,
                                               const char *wrapped_script, char **out_cmd)
{
   if (out_cmd)
      *out_cmd = NULL;
   if (!container_name || !container_name[0] || !wrapped_script || !out_cmd)
      return -1;

   size_t script_len = strlen(wrapped_script);
   size_t b64_cap = ((script_len + 2) / 3) * 4 + 1;
   char *b64 = malloc(b64_cap);
   if (!b64)
      return -1;
   if (util_b64_encode(wrapped_script, script_len, b64, b64_cap) < 0)
   {
      free(b64);
      return -1;
   }

   /* `docker exec -i <name> bash -c "echo '<b64>' | base64 -d | bash"` */
   size_t need = strlen(container_name) + b64_cap + 128;
   char *cmd = malloc(need);
   if (!cmd)
   {
      free(b64);
      return -1;
   }
   int n = snprintf(cmd, need, "docker exec -i %s bash -c \"echo '%s' | base64 -d | bash\"",
                    container_name, b64);
   free(b64);
   if (n < 0 || (size_t)n >= need)
   {
      free(cmd);
      return -1;
   }
   *out_cmd = cmd;
   return 0;
}

/* --- container lifecycle --- */

typedef struct
{
   char container_name[128];
   char image[256];
   char workspace_host[MAX_PATH_LEN]; /* host-side mount point */
   char cwd[MAX_PATH_LEN];            /* in-container cwd, defaults to /workspace */
   /* 1 when workspace_host is the CALLER's real tree rather than our own scratch
    * dir. It changes who the container must run as: files written into a scratch
    * dir we own are ours to clean up, but files written into the user's checkout
    * must not land root-owned. */
   int mount_host_tree;
   int mount_read_only; /* :ro — the tree is not this delegate's to change */
   /* The in-container working directory, and what relative tool paths resolve
    * against. Per-state, not the global resolve_docker_workdir(): a caller-provided
    * tree is mounted at its OWN absolute host path (see docker_build_mounts), so
    * /workspace is not where its files live. Anchoring resolution on a global while
    * the mount moved is how a tool would read the wrong tree and report it as
    * missing. */
   char workdir[MAX_PATH_LEN];
   /* Bind mounts for `docker create`, already in "<src>:<dst>[:ro]" form. */
   char mounts[3][MAX_PATH_LEN * 2 + 8];
   int mount_count;
} docker_state_t;

#define DOCKER_DEFAULT_IMAGE   "ubuntu:22.04"
#define DOCKER_WORKDIR_DEFAULT "/workspace"

/* Test seam: production workdir is /workspace inside the container.
 * Unit tests substitute AIMEE_DOCKER_WORKDIR so file ops + mkdir
 * happen under a writable temp dir on the host (the fake-docker
 * fixture runs commands locally, not in a real container). */
static const char *resolve_docker_workdir(void)
{
   const char *override = getenv("AIMEE_DOCKER_WORKDIR");
   if (override && override[0])
      return override;
   return DOCKER_WORKDIR_DEFAULT;
}

/* Pick the docker binary. Tests substitute a fake-docker fixture
 * via AIMEE_DOCKER_BIN. Production runs default to "docker". */
static const char *resolve_docker_bin(void)
{
   const char *override = getenv("AIMEE_DOCKER_BIN");
   if (override && override[0])
      return override;
   return "docker";
}

/* Build the host-side workspace path: $XDG_CACHE_HOME/aimee/delegate/
 * <task_id>/ (falls back to $HOME/.cache/...). The dir is mounted
 * read/write into the container at DOCKER_WORKDIR so files survive
 * `docker rm` on hibernate=0. */
static int compute_workspace_host(const char *task_id, char *out, size_t outsz)
{
   const char *xdg = getenv("XDG_CACHE_HOME");
   int n;
   if (xdg && xdg[0])
      n = snprintf(out, outsz, "%s/aimee/delegate/%s", xdg, task_id);
   else
   {
      const char *home = getenv("HOME");
      if (!home || !home[0])
         return -1;
      n = snprintf(out, outsz, "%s/.cache/aimee/delegate/%s", home, task_id);
   }
   return (n < 0 || (size_t)n >= outsz) ? -1 : 0;
}

/* mkdir -p — same shape as the local backend. */
static int docker_mkdir_p(const char *path)
{
   char buf[MAX_PATH_LEN];
   snprintf(buf, sizeof(buf), "%s", path);
   for (char *p = buf + 1; *p; p++)
   {
      if (*p == '/')
      {
         *p = '\0';
         if (mkdir(buf, 0700) != 0 && errno != EEXIST)
            return -1;
         *p = '/';
      }
   }
   if (mkdir(buf, 0700) != 0 && errno != EEXIST)
      return -1;
   return 0;
}

/* Run `docker <args>` synchronously, returning the child's exit code.
 * argv must be NULL-terminated. -1 on fork/wait failure. */
static int run_docker(const char *const argv[])
{
   pid_t pid = fork();
   if (pid < 0)
      return -1;
   if (pid == 0)
   {
      /* Replace argv[0] with the resolved docker binary. */
      const char *bin = resolve_docker_bin();
      /* Build a copy of argv with argv[0] set to bin so the test
       * fixture (which checks argv[0] for "docker") still routes
       * through the override. execvp uses the bin path for lookup
       * but argv[0] for the process name. */
      execvp(bin, (char *const *)argv);
      _exit(127);
   }
   int status = 0;
   if (waitpid(pid, &status, 0) != pid)
      return -1;
   return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

/* Read a linked worktree's `.git` pointer file: "gitdir: <absolute path>".
 * Returns 0 and fills `out` on success. */
static int docker_read_gitlink(const char *gitfile, char *out, size_t outsz)
{
   FILE *f = fopen(gitfile, "r");
   if (!f)
      return -1;
   char line[MAX_PATH_LEN + 16];
   if (!fgets(line, sizeof(line), f))
   {
      fclose(f);
      return -1;
   }
   fclose(f);
   const char *p = strstr(line, "gitdir:");
   if (!p)
      return -1;
   p += 7;
   while (*p == ' ' || *p == '\t')
      p++;
   size_t n = strlen(p);
   while (n && (p[n - 1] == '\n' || p[n - 1] == '\r' || p[n - 1] == ' '))
      n--;
   if (!n || p[0] != '/' || n >= outsz)
      return -1; /* must be absolute: a relative gitdir is not one we can mount */
   memcpy(out, p, n);
   out[n] = '\0';
   return 0;
}

/* Add "<src>:<dst>[:ro]" to the state's mount list. */
static int docker_add_mount(docker_state_t *st, const char *src, const char *dst, int ro)
{
   if (st->mount_count >= (int)(sizeof(st->mounts) / sizeof(st->mounts[0])))
      return -1;
   if ((size_t)snprintf(st->mounts[st->mount_count], sizeof(st->mounts[0]), "%s:%s%s", src, dst,
                        ro ? ":ro" : "") >= sizeof(st->mounts[0]))
      return -1;
   st->mount_count++;
   return 0;
}

/* Decide what to mount for a caller-provided tree `real` (already canonical).
 *
 * A LINKED WORKTREE's .git is a FILE holding `gitdir: <absolute host path>` into
 * the main repo, so mounting the worktree alone leaves that path absent inside the
 * container and every git command fails — after the delegate has started work,
 * looking like a corrupt repo rather than a bad mount. Mounting the repo at its OWN
 * absolute path makes the pointer resolve verbatim, with no rewriting.
 *
 * Isolation decides the modes, and they are measured, not assumed (validated on
 * docker 26.1.5):
 *   <repo>:ro     the whole tree is readable; a write outside the worktree fails
 *                 with "Read-only file system"
 *   <worktree>    this delegate's files, writable — a nested mount correctly
 *                 overlays the read-only repo
 *   <gitdir>      this delegate's git metadata, writable — `git status` refreshes
 *                 its index here, so a read-only .git does not break it
 * `git commit` inside then fails (blobs cannot be written to the :ro object store),
 * which is intended: git_commit runs server-side and require_aimee_git forbids the
 * shell route anyway.
 *
 * A PLAIN checkout needs none of that: one mount at its own absolute path.
 * Returns 0 on success. */
static int docker_build_mounts(docker_state_t *st, const char *real, int read_only)
{
   char gitmark[MAX_PATH_LEN];
   if ((size_t)snprintf(gitmark, sizeof(gitmark), "%s/.git", real) >= sizeof(gitmark))
   {
      aimee_log(LOG_ERROR, "delegate-backend-docker",
                "workspace path '%s' is too long to check for .git; refusing", real);
      return -1;
   }
   struct stat gst;
   if (lstat(gitmark, &gst) != 0)
   {
      aimee_log(LOG_ERROR, "delegate-backend-docker",
                "workspace '%s' is not a git checkout (no .git); refusing to bind-mount an "
                "arbitrary host directory into a delegate container",
                real);
      return -1;
   }
   if (S_ISLNK(gst.st_mode))
   {
      aimee_log(LOG_ERROR, "delegate-backend-docker",
                "workspace '%s' has a symlinked .git; refusing to treat it as a checkout", real);
      return -1;
   }

   if (S_ISDIR(gst.st_mode))
   {
      /* Plain checkout: the tree carries its own .git. Mount it at its own absolute
       * path so paths inside the container match the host's. */
      snprintf(st->workdir, sizeof(st->workdir), "%s", real);
      return docker_add_mount(st, real, real, read_only);
   }

   /* Linked worktree. */
   char gitdir[MAX_PATH_LEN];
   if (docker_read_gitlink(gitmark, gitdir, sizeof(gitdir)) != 0)
   {
      aimee_log(LOG_ERROR, "delegate-backend-docker",
                "workspace '%s': .git is a file but carries no absolute `gitdir:` pointer; "
                "refusing rather than mounting a worktree whose git would be broken",
                real);
      return -1;
   }
   struct stat gdst;
   if (stat(gitdir, &gdst) != 0 || !S_ISDIR(gdst.st_mode))
   {
      aimee_log(LOG_ERROR, "delegate-backend-docker",
                "workspace '%s': gitdir '%s' does not exist on the host; refusing", real, gitdir);
      return -1;
   }
   /* The repo root is what contains that gitdir: <repo>/.git/worktrees/<name>. */
   const char *marker = strstr(gitdir, "/.git/");
   if (!marker)
   {
      aimee_log(LOG_ERROR, "delegate-backend-docker",
                "workspace '%s': gitdir '%s' is not under a <repo>/.git/ — cannot derive the repo "
                "root to mount; refusing",
                real, gitdir);
      return -1;
   }
   char repo[MAX_PATH_LEN];
   size_t rlen = (size_t)(marker - gitdir);
   if (rlen == 0 || rlen >= sizeof(repo))
      return -1;
   memcpy(repo, gitdir, rlen);
   repo[rlen] = '\0';

   /* The worktree and its gitdir must live under that repo root, or the nested
    * overlay does not apply and we would be mounting unrelated trees. */
   size_t plen = strlen(repo);
   if (strncmp(real, repo, plen) != 0 || (real[plen] != '/' && real[plen] != '\0'))
   {
      aimee_log(LOG_ERROR, "delegate-backend-docker",
                "workspace '%s' is not inside its repo root '%s'; refusing (the nested "
                "read-only overlay would not apply)",
                real, repo);
      return -1;
   }

   snprintf(st->workdir, sizeof(st->workdir), "%s", real);
   /* Order matters: the repo first, then the writable parts nested over it. */
   if (docker_add_mount(st, repo, repo, 1) != 0 ||
       docker_add_mount(st, real, real, read_only) != 0 ||
       docker_add_mount(st, gitdir, gitdir, read_only) != 0)
      return -1;
   aimee_log(LOG_INFO, "delegate-backend-docker",
             "linked worktree '%s': mounting repo '%s' read-only with the worktree and its gitdir "
             "%s over it",
             real, repo, read_only ? "read-only" : "writable");
   return 0;
}

/* 32-bit FNV-1a over the mount specs. The container is resumed by NAME
 * (`docker start` first, create only on failure), so a task id reused with a
 * DIFFERENT workspace would silently resume a container carrying the OLD mounts —
 * the delegate would then work in the previous tree and nothing would say so. I hit
 * exactly this while testing: containers created against one image were resumed and
 * the new image ignored. Folding the mounts into the name makes a changed mount a
 * different container by construction. */
static unsigned docker_mounts_fingerprint(const docker_state_t *st)
{
   unsigned h = 2166136261u;
   for (int i = 0; i < st->mount_count; i++)
      for (const char *p = st->mounts[i]; *p; p++)
      {
         h ^= (unsigned char)*p;
         h *= 16777619u;
      }
   return h;
}

/* package_access=proxy: copy the static aimee-forwarder into the (running) delegate
 * container and start it detached, so 127.0.0.1:3129 inside the sandbox bridges to the
 * bound aimee UDS where the package forward proxy runs. Best-effort: on failure the
 * delegate still runs; package installs then fail fast (http_proxy -> a dead port is
 * refused immediately, no hang). The forwarder is re-copied/started after every start
 * because a `docker exec -d` process does not survive a container stop. */
/* Remove the forwarder binary from the container so a half-configured proxy delegate
 * can't be left with an unusable-but-present forwarder it could relaunch pointed at an
 * arbitrary UDS path. Best-effort. */
static void docker_pkg_forwarder_strip(const char *container)
{
   const char *rm_argv[] = {
       "docker", "exec", container, "rm", "-f", "/usr/local/bin/aimee-forwarder", NULL};
   (void)run_docker(rm_argv);
}

static void docker_pkg_forwarder_setup(const char *container)
{
   /* Fixed, canonical ship path — never operator/env-overridable, so nothing can
    * point the copy at an attacker-controlled binary. */
   static const char *const bin = "/usr/local/bin/aimee-forwarder";
   struct stat bst;
   if (stat(bin, &bst) != 0)
   {
      aimee_log(LOG_ERROR, "delegate-sandbox",
                "package-access=proxy but forwarder binary '%s' is missing — sandbox package "
                "installs will fail (ship aimee-forwarder in the aimee-server image)",
                bin);
      return;
   }
   char dst[MAX_PATH_LEN + 64];
   snprintf(dst, sizeof(dst), "%s:/usr/local/bin/aimee-forwarder", container);
   const char *cp_argv[] = {"docker", "cp", bin, dst, NULL};
   if (run_docker(cp_argv) != 0)
   {
      aimee_log(LOG_ERROR, "delegate-sandbox", "forwarder `docker cp` into %s failed", container);
      return;
   }
   const char *ex_argv[] = {"docker",
                            "exec",
                            "-d",
                            container,
                            "env",
                            "AIMEE_FORWARDER_SOCK=" DELEGATE_SOCK_PATH,
                            "AIMEE_FORWARDER_PORT=3129",
                            "/usr/local/bin/aimee-forwarder",
                            NULL};
   if (run_docker(ex_argv) != 0)
   {
      /* Copied but not running: strip it rather than leave a relaunchable binary. */
      aimee_log(LOG_ERROR, "delegate-sandbox",
                "starting package forwarder in %s failed — stripping the binary", container);
      docker_pkg_forwarder_strip(container);
      return;
   }
   /* apt does not honour http_proxy through every transport — drop in an explicit
    * proxy config too. Best-effort (needs a root-writable /etc/apt; a non-root
    * delegate still gets the env vars, which cover apt's http/https transports). */
   const char *apt_argv[] = {"docker",
                             "exec",
                             container,
                             "sh",
                             "-c",
                             "mkdir -p /etc/apt/apt.conf.d 2>/dev/null && "
                             "printf 'Acquire::http::Proxy \"http://127.0.0.1:3129\";\\n"
                             "Acquire::https::Proxy \"http://127.0.0.1:3129\";\\n' "
                             "> /etc/apt/apt.conf.d/99aimee-proxy 2>/dev/null || true",
                             NULL};
   (void)run_docker(apt_argv);
   aimee_log(LOG_INFO, "delegate-sandbox",
             "package proxy armed in %s (127.0.0.1:3129 -> aimee UDS; egress via aimee)",
             container);
}

static int docker_acquire(delegate_backend_t *self, const char *task_id,
                          const delegate_backend_config_t *cfg, void **state_out)
{
   (void)self;
   if (state_out)
      *state_out = NULL;
   if (!task_id || !task_id[0] || !state_out)
      return -1;

   docker_state_t *st = calloc(1, sizeof(*st));
   if (!st)
      return -1;
   if (delegate_backend_docker_container_name(task_id, st->container_name,
                                              sizeof(st->container_name)) != 0)
   {
      free(st);
      return -1;
   }
   const char *image = (cfg && cfg->image && cfg->image[0]) ? cfg->image : DOCKER_DEFAULT_IMAGE;
   snprintf(st->image, sizeof(st->image), "%s", image);
   if (cfg && cfg->workspace && cfg->workspace[0])
   {
      /* Caller-provided tree: mount it so the delegate gets the entire current
       * source tree — by bind-mount, so it IS the tree, not a copy that can drift.
       *
       * Canonicalize FIRST: stat() follows symlinks and so does the docker daemon,
       * so validating the path as given and mounting the same string would let a
       * symlinked component point the mount somewhere the checks never saw. */
      char real[MAX_PATH_LEN];
      if (!realpath(cfg->workspace, real))
      {
         aimee_log(LOG_ERROR, "delegate-backend-docker",
                   "workspace '%s' does not resolve (%s); refusing to mount it — a delegate "
                   "would see an empty tree and conclude the code is missing",
                   cfg->workspace, strerror(errno));
         free(st);
         return -1;
      }
      struct stat wst;
      if (stat(real, &wst) != 0 || !S_ISDIR(wst.st_mode))
      {
         aimee_log(LOG_ERROR, "delegate-backend-docker",
                   "workspace '%s' is not an existing directory; refusing to mount it", real);
         free(st);
         return -1;
      }
      /* Refuse truncation: a path that passed the checks but does not fit would
       * mount a different directory than the one that was validated. */
      if ((size_t)snprintf(st->workspace_host, sizeof(st->workspace_host), "%s", real) >=
          sizeof(st->workspace_host))
      {
         aimee_log(LOG_ERROR, "delegate-backend-docker",
                   "workspace path '%s' is too long for the mount buffer; refusing rather than "
                   "mounting a truncated path",
                   real);
         free(st);
         return -1;
      }
      st->mount_read_only = (cfg->workspace_read_only != 0);
      if (docker_build_mounts(st, real, st->mount_read_only) != 0)
      {
         free(st);
         return -1;
      }
      st->mount_host_tree = 1;
   }
   else if (compute_workspace_host(task_id, st->workspace_host, sizeof(st->workspace_host)) != 0 ||
            docker_mkdir_p(st->workspace_host) != 0)
   {
      free(st);
      return -1;
   }
   else
   {
      /* Our own scratch dir: keep the historical /workspace contract. */
      snprintf(st->workdir, sizeof(st->workdir), "%s", resolve_docker_workdir());
      if (docker_add_mount(st, st->workspace_host, st->workdir, 0) != 0)
      {
         free(st);
         return -1;
      }
   }
   snprintf(st->cwd, sizeof(st->cwd), "%s", st->workdir);

   /* Fold the mounts into the container's identity. `docker start` resumes by name,
    * so without this a task id reused with a different workspace resumes a container
    * carrying the OLD mounts, and the delegate works in the previous tree with
    * nothing to say so. Only for caller-provided trees: the scratch dir is derived
    * from the task id already, and hibernate/resume there is the point. */
   if (st->mount_host_tree)
   {
      char suffix[16];
      snprintf(suffix, sizeof(suffix), "-%08x", docker_mounts_fingerprint(st));
      size_t have = strlen(st->container_name);
      if (have + strlen(suffix) < sizeof(st->container_name))
         memcpy(st->container_name + have, suffix, strlen(suffix) + 1);
      else /* truncating would collide two different mount sets onto one name */
         snprintf(st->container_name + sizeof(st->container_name) - sizeof(suffix), sizeof(suffix),
                  "%s", suffix);
   }

   /* Runtime package-access policy: "proxy" (default) arms the in-container forwarder
    * + http_proxy so a --network none delegate can install via aimee's egress. */
   config_t sbx_pkg_cfg;
   int pkg_proxy = (config_load(&sbx_pkg_cfg) == 0) &&
                   strcmp(sbx_pkg_cfg.delegate_sandbox_package_access, "proxy") == 0;

   /* Try `docker start` first — if a container with this name already
    * exists (operator opted hibernate=1 last release), starting it
    * resumes the same workspace. If start fails, create + start. */
   const char *start_argv[] = {"docker", "start", st->container_name, NULL};
   if (run_docker(start_argv) != 0)
   {
      /* Run as the server's uid:gid when the mount is the caller's real tree.
       * Containers run as root by default, so every file the delegate creates in
       * the user's checkout would land root-owned — the user could not then edit
       * or delete their own files, and git would report "dubious ownership" and
       * refuse to operate on the tree at all. Our own scratch dir keeps the
       * historical behaviour: nobody else owns it. */
      char userflag[64] = "";
      if (st->mount_host_tree)
         snprintf(userflag, sizeof(userflag), "%u:%u", (unsigned)getuid(), (unsigned)getgid());

      /* The aimee-server UDS is the delegate's only outward channel (see the
       * DELEGATE_SOCK_PATH note). Resolve + bind it; if the server has no socket
       * yet (should not happen for a running server) we still create the container
       * so file/exec isolation holds — the delegate just cannot call `aimee`. */
      char host_sock[MAX_PATH_LEN + 32] = "";
      char sock_bind[MAX_PATH_LEN + 96] = "";
      const char *aimee_h = aimee_home();
      if (aimee_h && aimee_h[0])
         snprintf(host_sock, sizeof(host_sock), "%s/aimee-http.sock", aimee_h);
      struct stat sock_st;
      const int have_sock = host_sock[0] && stat(host_sock, &sock_st) == 0;
      if (have_sock)
         snprintf(sock_bind, sizeof(sock_bind), "%s:%s", host_sock, DELEGATE_SOCK_PATH);

      /* Sized from the mount array itself: a hand-counted bound silently overflows
       * the day a fourth mount is added. Fixed slots cover docker/create/--name/
       * --network/--user/-w/image/sleep + the aimee-socket -v and -e. */
      const char *create_argv[40 + 2 * (sizeof(st->mounts) / sizeof(st->mounts[0]))];
      int n = 0;
      create_argv[n++] = "docker";
      create_argv[n++] = "create";
      create_argv[n++] = "--name";
      create_argv[n++] = st->container_name;
      /* No IP network: the container's only reachable peer is aimee-server, over the
       * bound UDS below. Removes lateral movement and data-exfil in one flag.
       * INVARIANT: never bind the docker socket (/var/run/docker.sock) — that would
       * hand the delegate root-equivalent control of the host daemon. */
      create_argv[n++] = "--network";
      create_argv[n++] = "none";
      if (have_sock)
      {
         /* The one permitted channel out: aimee-server's UDS, with the in-container
          * `aimee` CLI pointed at it. Auth is filesystem-permission on the socket. */
         create_argv[n++] = "-v";
         create_argv[n++] = sock_bind;
         create_argv[n++] = "-e";
         create_argv[n++] = "AIMEE_API_ENDPOINT=unix:" DELEGATE_SOCK_PATH;
         /* package-access=proxy: point package managers at the in-container forwarder
          * (started after boot), which bridges to the bound UDS. docker exec inherits
          * these, so every delegate tool run sees them. Only with the UDS present. */
         if (pkg_proxy)
         {
            create_argv[n++] = "-e";
            create_argv[n++] = "http_proxy=http://127.0.0.1:3129";
            create_argv[n++] = "-e";
            create_argv[n++] = "https_proxy=http://127.0.0.1:3129";
            create_argv[n++] = "-e";
            create_argv[n++] = "HTTP_PROXY=http://127.0.0.1:3129";
            create_argv[n++] = "-e";
            create_argv[n++] = "HTTPS_PROXY=http://127.0.0.1:3129";
            create_argv[n++] = "-e";
            create_argv[n++] = "no_proxy=localhost,127.0.0.1";
            create_argv[n++] = "-e";
            create_argv[n++] = "NO_PROXY=localhost,127.0.0.1";
         }
      }
      if (userflag[0])
      {
         create_argv[n++] = "--user";
         create_argv[n++] = userflag;
      }
      for (int m = 0; m < st->mount_count; m++)
      {
         create_argv[n++] = "-v";
         create_argv[n++] = st->mounts[m];
      }
      create_argv[n++] = "-w";
      create_argv[n++] = st->workdir;
      create_argv[n++] = st->image;
      create_argv[n++] = "sleep";
      create_argv[n++] = "infinity";
      /* Guard the hand-sized slot count against a future knob overflowing it. */
      assert(n < (int)(sizeof(create_argv) / sizeof(create_argv[0])));
      create_argv[n] = NULL;
      if (run_docker(create_argv) != 0)
      {
         free(st);
         return -1;
      }
      const char *start2_argv[] = {"docker", "start", st->container_name, NULL};
      if (run_docker(start2_argv) != 0)
      {
         free(st);
         return -1;
      }
   }

   /* Container is running (resumed or freshly created): (re)arm the package forwarder.
    * A docker exec -d process does not survive a stop, so this runs on every acquire. */
   if (pkg_proxy)
      docker_pkg_forwarder_setup(st->container_name);

   *state_out = st;
   return 0;
}

static void docker_release(delegate_backend_t *self, void *state, int hibernate)
{
   (void)self;
   if (!state)
      return;
   docker_state_t *st = state;
   if (st->container_name[0])
   {
      if (hibernate)
      {
         /* Stop only; container persists for next acquire. */
         const char *argv[] = {"docker", "stop", st->container_name, NULL};
         (void)run_docker(argv);
      }
      else
      {
         /* Force-remove (kills if running, removes container). */
         const char *argv[] = {"docker", "rm", "-f", st->container_name, NULL};
         (void)run_docker(argv);
      }
   }
   free(st);
}

static int docker_exec(delegate_backend_t *self, void *state, const char *command, int timeout_ms,
                       delegate_exec_result_t *result_out)
{
   (void)self;
   if (!state || !command || !result_out)
      return -1;
   docker_state_t *st = state;
   long long start = util_now_ms();

   /* Prefix `cd <cwd> &&` when set_cwd has been called. The container
    * default cwd is /workspace (see -w flag at create); set_cwd lets
    * the operator land elsewhere across exec calls. */
   char *prefixed = NULL;
   const char *to_run = command;
   if (st->cwd[0] && strcmp(st->cwd, resolve_docker_workdir()) != 0)
   {
      size_t need = strlen(st->cwd) + strlen(command) + 16;
      prefixed = malloc(need);
      if (prefixed)
      {
         snprintf(prefixed, need, "cd %s && %s", st->cwd, command);
         to_run = prefixed;
      }
   }

   /* Build the docker shell command via the iter-37 b64 helper. */
   char *docker_cmd = NULL;
   if (delegate_backend_docker_build_exec_command(st->container_name, to_run, &docker_cmd) != 0 ||
       !docker_cmd)
   {
      free(prefixed);
      return -1;
   }
   free(prefixed);

   /* If AIMEE_DOCKER_BIN is set, swap the leading "docker" in the
    * built command for the override binary. Production runs leave
    * the env unset and the leading "docker" wins via PATH. */
   const char *bin_override = getenv("AIMEE_DOCKER_BIN");
   if (bin_override && bin_override[0] && strncmp(docker_cmd, "docker ", 7) == 0)
   {
      size_t bin_len = strlen(bin_override);
      size_t tail_len = strlen(docker_cmd) - 6; /* "docker" is 6 chars */
      char *swapped = malloc(bin_len + tail_len + 1);
      if (swapped)
      {
         memcpy(swapped, bin_override, bin_len);
         memcpy(swapped + bin_len, docker_cmd + 6, tail_len + 1);
         free(docker_cmd);
         docker_cmd = swapped;
      }
   }

   int out_pipe[2] = {-1, -1};
   int err_pipe[2] = {-1, -1};
   if (pipe(out_pipe) != 0 || pipe(err_pipe) != 0)
   {
      if (out_pipe[0] >= 0)
         close(out_pipe[0]);
      if (out_pipe[1] >= 0)
         close(out_pipe[1]);
      if (err_pipe[0] >= 0)
         close(err_pipe[0]);
      if (err_pipe[1] >= 0)
         close(err_pipe[1]);
      free(docker_cmd);
      return -1;
   }

   pid_t pid = fork();
   if (pid < 0)
   {
      close(out_pipe[0]);
      close(out_pipe[1]);
      close(err_pipe[0]);
      close(err_pipe[1]);
      free(docker_cmd);
      return -1;
   }
   if (pid == 0)
   {
      dup2(out_pipe[1], STDOUT_FILENO);
      dup2(err_pipe[1], STDERR_FILENO);
      close(out_pipe[0]);
      close(out_pipe[1]);
      close(err_pipe[0]);
      close(err_pipe[1]);
      execlp("sh", "sh", "-c", docker_cmd, (char *)NULL);
      _exit(127);
   }
   free(docker_cmd);
   close(out_pipe[1]);
   close(err_pipe[1]);

   long long deadline = timeout_ms > 0 ? start + timeout_ms : 0;
   size_t out_off = 0;
   size_t err_off = 0;
   int out_open = 1, err_open = 1;
   while (out_open || err_open)
   {
      fd_set rfds;
      FD_ZERO(&rfds);
      int maxfd = -1;
      if (out_open)
      {
         FD_SET(out_pipe[0], &rfds);
         if (out_pipe[0] > maxfd)
            maxfd = out_pipe[0];
      }
      if (err_open)
      {
         FD_SET(err_pipe[0], &rfds);
         if (err_pipe[0] > maxfd)
            maxfd = err_pipe[0];
      }
      struct timeval tv = {1, 0};
      if (deadline > 0)
      {
         long long remaining = deadline - util_now_ms();
         if (remaining <= 0)
         {
            kill(pid, SIGTERM);
            break;
         }
         tv.tv_sec = remaining / 1000;
         tv.tv_usec = (remaining % 1000) * 1000;
      }
      int sel = select(maxfd + 1, &rfds, NULL, NULL, &tv);
      if (sel < 0)
      {
         if (errno == EINTR)
            continue;
         break;
      }
      if (sel == 0)
         continue;
      if (out_open && FD_ISSET(out_pipe[0], &rfds))
      {
         if (result_out->stdout_buf && out_off + 1 < result_out->stdout_cap)
         {
            ssize_t r = read(out_pipe[0], result_out->stdout_buf + out_off,
                             result_out->stdout_cap - 1 - out_off);
            if (r <= 0)
               out_open = 0;
            else
               out_off += (size_t)r;
         }
         else
         {
            char throwaway[4096];
            if (read(out_pipe[0], throwaway, sizeof(throwaway)) <= 0)
               out_open = 0;
         }
      }
      if (err_open && FD_ISSET(err_pipe[0], &rfds))
      {
         if (result_out->stderr_buf && err_off + 1 < result_out->stderr_cap)
         {
            ssize_t r = read(err_pipe[0], result_out->stderr_buf + err_off,
                             result_out->stderr_cap - 1 - err_off);
            if (r <= 0)
               err_open = 0;
            else
               err_off += (size_t)r;
         }
         else
         {
            char throwaway[4096];
            if (read(err_pipe[0], throwaway, sizeof(throwaway)) <= 0)
               err_open = 0;
         }
      }
   }
   close(out_pipe[0]);
   close(err_pipe[0]);

   if (result_out->stdout_buf && result_out->stdout_cap > 0)
      result_out->stdout_buf[result_out->stdout_cap - 1] = '\0';
   if (result_out->stderr_buf && result_out->stderr_cap > 0)
      result_out->stderr_buf[result_out->stderr_cap - 1] = '\0';

   int status = 0;
   waitpid(pid, &status, 0);
   result_out->exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
   result_out->latency_ms = (int)(util_now_ms() - start);
   return 0;
}

/* Workspace path validation: reject absolute paths and any '..' segment
 * so the in-container workspace sandbox is honest. Resolves `rel`
 * against the container's WORKDIR (/workspace by default) so the same
 * path the caller hands in lands in the same place across exec calls. */
static int docker_resolve_in_workspace(const docker_state_t *st, const char *rel, char *out,
                                       size_t outsz)
{
   if (!st || !rel || !out || outsz == 0)
      return -1;
   if (rel[0] == '/')
      return -1;
   const char *p = rel;
   while (*p)
   {
      const char *seg = p;
      while (*p && *p != '/')
         p++;
      size_t len = (size_t)(p - seg);
      if (len == 2 && seg[0] == '.' && seg[1] == '.')
         return -1;
      if (*p == '/')
         p++;
   }
   if (snprintf(out, outsz, "%s/%s", st->workdir[0] ? st->workdir : resolve_docker_workdir(),
                rel) >= (int)outsz)
      return -1;
   return 0;
}

static int docker_read_file(delegate_backend_t *self, void *state, const char *p, int offset,
                            int limit, char **out)
{
   if (out)
      *out = NULL;
   if (!state || !p || !out || offset < 0 || limit < 0)
      return -1;
   docker_state_t *st = state;
   char abs[MAX_PATH_LEN];
   if (docker_resolve_in_workspace(st, p, abs, sizeof(abs)) != 0)
      return -1;
   /* `cat <abs>` via docker_exec; slice in C for offset/limit. limit=0
    * means "to EOF" capped at 16 MiB to bound malloc. */
   size_t cap = limit > 0 ? (size_t)limit : 16 * 1024 * 1024;
   size_t buf_cap = cap + (size_t)offset + 1;
   char *buf = malloc(buf_cap);
   char *err = malloc(4096);
   if (!buf || !err)
   {
      free(buf);
      free(err);
      return -1;
   }
   delegate_exec_result_t r = {0, 0, buf, buf_cap, err, 4096};
   char cmd[MAX_PATH_LEN + 16];
   snprintf(cmd, sizeof(cmd), "cat %s", abs);
   if (docker_exec(self, state, cmd, 30000, &r) != 0 || r.exit_code != 0)
   {
      free(buf);
      free(err);
      return -1;
   }
   free(err);
   size_t n = strlen(buf);
   if ((size_t)offset > n)
      offset = (int)n;
   size_t avail = n - (size_t)offset;
   size_t take = avail < cap ? avail : cap;
   char *slice = malloc(take + 1);
   if (!slice)
   {
      free(buf);
      return -1;
   }
   memcpy(slice, buf + offset, take);
   slice[take] = '\0';
   free(buf);
   *out = slice;
   return 0;
}

static int docker_write_file(delegate_backend_t *self, void *state, const char *p,
                             const char *content)
{
   if (!state || !p || !content)
      return -1;
   docker_state_t *st = state;
   char abs[MAX_PATH_LEN];
   if (docker_resolve_in_workspace(st, p, abs, sizeof(abs)) != 0)
      return -1;

   size_t clen = strlen(content);
   size_t b64_cap = ((clen + 2) / 3) * 4 + 1;
   char *b64 = malloc(b64_cap);
   if (!b64)
      return -1;
   if (util_b64_encode(content, clen, b64, b64_cap) < 0)
   {
      free(b64);
      return -1;
   }
   /* mkdir -p parent then b64-decode into the file. */
   size_t cmd_cap = strlen(abs) * 2 + b64_cap + 128;
   char *cmd = malloc(cmd_cap);
   if (!cmd)
   {
      free(b64);
      return -1;
   }
   snprintf(cmd, cmd_cap, "mkdir -p \"$(dirname %s)\" && echo '%s' | base64 -d > %s", abs, b64,
            abs);
   free(b64);

   char out[256] = {0}, err[256] = {0};
   delegate_exec_result_t r = {0, 0, out, sizeof(out), err, sizeof(err)};
   int rc = docker_exec(self, state, cmd, 30000, &r);
   free(cmd);
   if (rc != 0 || r.exit_code != 0)
      return -1;
   return 0;
}

static int docker_list_dir(delegate_backend_t *self, void *state, const char *p,
                           char ***entries_out)
{
   if (entries_out)
      *entries_out = NULL;
   if (!state || !p || !entries_out)
      return -1;
   docker_state_t *st = state;
   char abs[MAX_PATH_LEN];
   if (docker_resolve_in_workspace(st, p, abs, sizeof(abs)) != 0)
      return -1;

   char buf[64 * 1024] = {0};
   char err[4096] = {0};
   delegate_exec_result_t r = {0, 0, buf, sizeof(buf), err, sizeof(err)};
   char cmd[MAX_PATH_LEN + 16];
   snprintf(cmd, sizeof(cmd), "ls -1A %s", abs);
   if (docker_exec(self, state, cmd, 30000, &r) != 0 || r.exit_code != 0)
      return -1;

   int n = 0;
   for (const char *q = buf; *q; q++)
      if (*q == '\n')
         n++;
   if (buf[0] && buf[strlen(buf) - 1] != '\n')
      n++;

   char **entries = calloc((size_t)n + 1, sizeof(*entries));
   if (!entries)
      return -1;
   int i = 0;
   const char *q = buf;
   while (*q && i < n)
   {
      const char *line_start = q;
      while (*q && *q != '\n')
         q++;
      size_t len = (size_t)(q - line_start);
      if (len > 0)
      {
         entries[i] = malloc(len + 1);
         if (!entries[i])
         {
            for (int j = 0; j < i; j++)
               free(entries[j]);
            free(entries);
            return -1;
         }
         memcpy(entries[i], line_start, len);
         entries[i][len] = '\0';
         i++;
      }
      if (*q == '\n')
         q++;
   }
   entries[i] = NULL;
   *entries_out = entries;
   return i;
}

static int docker_get_cwd(delegate_backend_t *self, void *state, char **out)
{
   (void)self;
   if (!state || !out)
      return -1;
   docker_state_t *st = state;
   *out = strdup(st->cwd[0] ? st->cwd : resolve_docker_workdir());
   return *out ? 0 : -1;
}

static int docker_set_cwd(delegate_backend_t *self, void *state, const char *path)
{
   (void)self;
   if (!state || !path || !path[0])
      return -1;
   docker_state_t *st = state;
   snprintf(st->cwd, sizeof(st->cwd), "%s", path);
   return 0;
}

static delegate_backend_t g_docker = {
    .name = "docker",
    .description = "long-lived container per task_id",
    .acquire = docker_acquire,
    .release = docker_release,
    .exec = docker_exec,
    .read_file = docker_read_file,
    .write_file = docker_write_file,
    .list_dir = docker_list_dir,
    .get_cwd = docker_get_cwd,
    .set_cwd = docker_set_cwd,
};

int delegate_backend_register_docker(void)
{
   return delegate_backend_register(&g_docker);
}

delegate_backend_t *delegate_backend_docker_get(void)
{
   return &g_docker;
}
