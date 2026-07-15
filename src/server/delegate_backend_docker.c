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

#include "aimee.h" /* MAX_PATH_LEN */

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
      /* Caller-provided tree: mount it AS the workspace, so the delegate gets the
       * entire current source tree by bind-mount — it IS the tree, not a copy that
       * can drift. Everything below is a refusal, and each one is a way a delegate
       * could otherwise end up reasoning about a tree that is not the one it was
       * told about. */

      /* Canonicalize FIRST. stat() follows symlinks and so does the docker daemon,
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

      /* It must be a git checkout. Not fussiness: it is what bounds the mount. The
       * caller derives this path from a session cwd, so without this an unlucky or
       * hostile cwd — '/', a secrets directory, the server's own checkout — becomes
       * a read-write bind mount into a delegate's container. A repository root is a
       * thing someone deliberately made; '/' is not. */
      char gitmark[MAX_PATH_LEN];
      if ((size_t)snprintf(gitmark, sizeof(gitmark), "%s/.git", real) >= sizeof(gitmark))
      {
         aimee_log(LOG_ERROR, "delegate-backend-docker",
                   "workspace path '%s' is too long to check for .git; refusing", real);
         free(st);
         return -1;
      }
      /* lstat, not stat: a .git that is a SYMLINK is not a marker we should trust —
       * it points outside the tree we just canonicalized, so the thing vouching for
       * this directory lives somewhere the checks never looked. */
      struct stat gst;
      if (lstat(gitmark, &gst) != 0)
      {
         aimee_log(LOG_ERROR, "delegate-backend-docker",
                   "workspace '%s' is not a git checkout (no .git); refusing to bind-mount an "
                   "arbitrary host directory into a delegate container",
                   real);
         free(st);
         return -1;
      }

      /* A LINKED worktree's .git is a FILE holding `gitdir: <absolute host path>`
       * into the main repo. Mounting only the worktree leaves that path absent
       * inside the container, so every git command fails — and it fails AFTER the
       * delegate has started work, looking like a broken repo rather than a bad
       * mount. Refuse until the mount can carry the common gitdir too (mounting the
       * main repo at its own absolute path, so the gitlink resolves).
       *
       * This is the wfe delegate's case, so the sandbox does not cover it yet. Said
       * out loud, at acquire time, rather than discovered by a delegate. */
      if (S_ISLNK(gst.st_mode))
      {
         aimee_log(LOG_ERROR, "delegate-backend-docker",
                   "workspace '%s' has a symlinked .git; refusing to treat it as a checkout", real);
         free(st);
         return -1;
      }
      if (S_ISREG(gst.st_mode))
      {
         aimee_log(LOG_ERROR, "delegate-backend-docker",
                   "workspace '%s' is a linked git worktree (.git is a gitdir pointer, not a "
                   "directory): mounting it alone would leave git broken inside the container. "
                   "Refusing — worktree support needs the common gitdir mounted too",
                   real);
         free(st);
         return -1;
      }

      /* Copy the CANONICAL path, and refuse truncation: a path that passed the
       * checks above but does not fit would mount a different directory than the
       * one that was validated. */
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
      st->mount_host_tree = 1;
      st->mount_read_only = (cfg->workspace_read_only != 0);
   }
   else if (compute_workspace_host(task_id, st->workspace_host, sizeof(st->workspace_host)) != 0 ||
            docker_mkdir_p(st->workspace_host) != 0)
   {
      free(st);
      return -1;
   }
   snprintf(st->cwd, sizeof(st->cwd), "%s", resolve_docker_workdir());

   /* Try `docker start` first — if a container with this name already
    * exists (operator opted hibernate=1 last release), starting it
    * resumes the same workspace. If start fails, create + start. */
   const char *start_argv[] = {"docker", "start", st->container_name, NULL};
   if (run_docker(start_argv) != 0)
   {
      /* Build the volume mount string: <host>:/workspace */
      const char *workdir = resolve_docker_workdir();
      char mount[MAX_PATH_LEN + 32];
      snprintf(mount, sizeof(mount), "%s:%s%s", st->workspace_host, workdir,
               st->mount_read_only ? ":ro" : "");
      /* Run as the server's uid:gid when the mount is the caller's real tree.
       * Containers run as root by default, so every file the delegate creates in
       * the user's checkout would land root-owned — the user could not then edit
       * or delete their own files, and git would report "dubious ownership" and
       * refuse to operate on the tree at all. Our own scratch dir keeps the
       * historical behaviour: nobody else owns it. */
      char userflag[64] = "";
      if (st->mount_host_tree)
         snprintf(userflag, sizeof(userflag), "%u:%u", (unsigned)getuid(), (unsigned)getgid());

      const char *create_argv[12 + 2];
      int n = 0;
      create_argv[n++] = "docker";
      create_argv[n++] = "create";
      create_argv[n++] = "--name";
      create_argv[n++] = st->container_name;
      if (userflag[0])
      {
         create_argv[n++] = "--user";
         create_argv[n++] = userflag;
      }
      create_argv[n++] = "-v";
      create_argv[n++] = mount;
      create_argv[n++] = "-w";
      create_argv[n++] = workdir;
      create_argv[n++] = st->image;
      create_argv[n++] = "sleep";
      create_argv[n++] = "infinity";
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
   if (snprintf(out, outsz, "%s/%s", resolve_docker_workdir(), rel) >= (int)outsz)
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
