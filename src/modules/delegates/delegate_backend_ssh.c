/* delegate_backend_ssh.c: see delegate_backend_ssh.h.
 *
 * acquire() spawns `ssh -M -N -f -o ControlPath=<sock> ...` so the
 * multiplex master backgrounds itself after auth, then waits for the
 * socket to publish. exec() rides the multiplex socket via the b64
 * wrapper helper, captures stdout/stderr/exit_code through pipes.
 * release() sends `ssh -O exit -S <sock>` to tear the master down
 * (and rm -rf's the remote workspace when hibernate=0).
 *
 * File ops (read_file/write_file/list_dir) all route through ssh_exec
 * — read does `cat <path>` then C-side slices for offset/limit, write
 * b64-encodes content and decodes remotely, list does `ls -1A` and
 * splits lines. All paths are workspace-relative; absolute paths and
 * '..' segments are rejected.
 *
 * Cwd persistence is partial: set_cwd writes to the local state
 * struct and exec() prefixes `cd <cwd> &&` to subsequent commands.
 * Reading pwd back from the remote so an in-command `cd` persists
 * (matching the local backend's shell-state wrapper) is a follow-up iter. */

#include "delegate_backend_ssh.h"
#include "util.h"

#include "aimee.h" /* MAX_PATH_LEN */

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* RFC 4648 base64. Used to encode the remote bash script so that
 * arbitrary quoting in the wrapped command (single quotes, backticks,
 * dollar signs) survives the transit through `ssh ... bash -c '...'`. */
int delegate_backend_ssh_socket_path(const char *task_id, char *out, size_t outsz)
{
   if (!task_id || !task_id[0] || !out || outsz == 0)
      return -1;
   const char *xdg = getenv("XDG_RUNTIME_DIR");
   int n;
   if (xdg && xdg[0])
      n = snprintf(out, outsz, "%s/aimee-ssh-%s.sock", xdg, task_id);
   else
      n = snprintf(out, outsz, "/tmp/aimee-ssh-%u-%s.sock", (unsigned)getuid(), task_id);
   if (n < 0 || (size_t)n >= outsz)
      return -1;
   return 0;
}

int delegate_backend_ssh_wait_for_socket(const char *path, int timeout_ms)
{
   if (!path || !path[0])
      return -1;
   /* 50ms tick. timeout_ms <= 0 means "one quick check, no wait." */
   struct timespec tick = {0, 50 * 1000000L};
   int waited = 0;
   for (;;)
   {
      struct stat st;
      if (stat(path, &st) == 0)
         return 0;
      if (waited >= timeout_ms)
         return -1;
      nanosleep(&tick, NULL);
      waited += 50;
   }
}

int delegate_backend_ssh_build_exec_command(const char *ctrl_socket, const char *host,
                                            const char *wrapped_script, char **out_cmd)
{
   if (out_cmd)
      *out_cmd = NULL;
   if (!ctrl_socket || !ctrl_socket[0] || !host || !host[0] || !wrapped_script || !out_cmd)
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

   /* The remote command pipes the base64 through `base64 -d` into
    * `bash`. Single quotes around the b64 payload are safe — the b64
    * alphabet has no single-quote character. */
   size_t need = strlen(ctrl_socket) * 2 + strlen(host) + b64_cap + 256;
   char *cmd = malloc(need);
   if (!cmd)
   {
      free(b64);
      return -1;
   }
   int n = snprintf(cmd, need,
                    "ssh -S %s -o ControlPath=%s -o BatchMode=yes "
                    "-o ServerAliveInterval=30 %s "
                    "\"echo '%s' | base64 -d | bash\"",
                    ctrl_socket, ctrl_socket, host, b64);
   free(b64);
   if (n < 0 || (size_t)n >= need)
   {
      free(cmd);
      return -1;
   }
   *out_cmd = cmd;
   return 0;
}

/* --- ControlMaster lifecycle --- */

typedef struct
{
   char socket_path[MAX_PATH_LEN];
   char host[256];
   char task_id[128];
   char workspace_remote[MAX_PATH_LEN];
   /* Tracked locally; not yet synced to the remote shell. set_cwd
    * stores here so future exec calls can prefix `cd <cwd> && ...` —
    * cwd readback (matching the local backend's shell-state wrapper)
    * lands in a follow-up iter. */
   char cwd[MAX_PATH_LEN];
} ssh_state_t;

/* Pick the ssh binary. Tests set AIMEE_SSH_BIN to a fixture script
 * so acquire/release can be exercised without a live SSH daemon. */
static const char *resolve_ssh_bin(void)
{
   const char *override = getenv("AIMEE_SSH_BIN");
   if (override && override[0])
      return override;
   return "ssh";
}

/* Spawn `ssh -M -N -f -o ControlPath=<sock> -o ControlPersist=300s
 * <host>` and wait until the multiplex socket appears. -f backgrounds
 * the master after authentication, so the parent exit code reports
 * setup success/failure but the master keeps running detached.
 * Returns 0 on success, -1 on fork/exec failure or socket timeout. */
static int spawn_control_master(const char *socket_path, const char *host)
{
   pid_t pid = fork();
   if (pid < 0)
      return -1;
   if (pid == 0)
   {
      /* Build ControlPath= flag value. */
      char ctrl[MAX_PATH_LEN + 16];
      snprintf(ctrl, sizeof(ctrl), "ControlPath=%s", socket_path);
      execlp(resolve_ssh_bin(), "ssh", "-M", "-N", "-f", "-o", ctrl, "-o", "ControlPersist=300",
             "-o", "BatchMode=yes", "-o", "ServerAliveInterval=30", host, (char *)NULL);
      _exit(127);
   }
   int status = 0;
   if (waitpid(pid, &status, 0) != pid)
      return -1;
   if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
      return -1;
   /* 5s budget for the master to publish its socket. SSH's auth +
    * background handoff is normally < 1s on a healthy network. */
   return delegate_backend_ssh_wait_for_socket(socket_path, 5000);
}

/* Send `ssh -O exit -S <socket> <host>` to tear down the multiplex
 * master. Best-effort — we ignore the exit code because the socket
 * may already be gone if the remote dropped us. */
static void send_master_exit(const char *socket_path, const char *host)
{
   pid_t pid = fork();
   if (pid < 0)
      return;
   if (pid == 0)
   {
      char ctrl[MAX_PATH_LEN + 16];
      snprintf(ctrl, sizeof(ctrl), "ControlPath=%s", socket_path);
      execlp(resolve_ssh_bin(), "ssh", "-O", "exit", "-S", socket_path, "-o", ctrl, "-o",
             "BatchMode=yes", host, (char *)NULL);
      _exit(127);
   }
   int status = 0;
   waitpid(pid, &status, 0);
}

static int ssh_acquire(delegate_backend_t *self, const char *task_id,
                       const delegate_backend_config_t *cfg, void **state_out)
{
   (void)self;
   if (state_out)
      *state_out = NULL;
   if (!task_id || !task_id[0] || !cfg || !cfg->host || !cfg->host[0] || !state_out)
      return -1;

   ssh_state_t *st = calloc(1, sizeof(*st));
   if (!st)
      return -1;
   if (delegate_backend_ssh_socket_path(task_id, st->socket_path, sizeof(st->socket_path)) != 0)
   {
      free(st);
      return -1;
   }
   snprintf(st->host, sizeof(st->host), "%s", cfg->host);
   snprintf(st->task_id, sizeof(st->task_id), "%s", task_id);
   snprintf(st->workspace_remote, sizeof(st->workspace_remote), ".aimee/delegate/%s", task_id);

   if (spawn_control_master(st->socket_path, st->host) != 0)
   {
      /* Best-effort cleanup of any partial socket and free state. */
      unlink(st->socket_path);
      free(st);
      return -1;
   }

   *state_out = st;
   return 0;
}

static void ssh_release(delegate_backend_t *self, void *state, int hibernate)
{
   (void)self;
   if (!state)
      return;
   ssh_state_t *st = state;

   /* On non-hibernate release, ssh out and rm -rf the workspace
    * before we drop the multiplex master. We don't pipe through the
    * wrapped exec helper because this is a one-shot best-effort
    * cleanup; the multiplex socket is already up. */
   if (!hibernate && st->socket_path[0] && st->workspace_remote[0])
   {
      pid_t pid = fork();
      if (pid == 0)
      {
         char rmcmd[MAX_PATH_LEN + 32];
         snprintf(rmcmd, sizeof(rmcmd), "rm -rf %s", st->workspace_remote);
         execlp(resolve_ssh_bin(), "ssh", "-S", st->socket_path, "-o", "BatchMode=yes", st->host,
                rmcmd, (char *)NULL);
         _exit(127);
      }
      if (pid > 0)
      {
         int status = 0;
         waitpid(pid, &status, 0);
      }
   }

   if (st->socket_path[0])
   {
      send_master_exit(st->socket_path, st->host);
      /* Master may not have removed its socket; do it ourselves. */
      unlink(st->socket_path);
   }
   free(st);
}

static int ssh_exec(delegate_backend_t *self, void *state, const char *command, int timeout_ms,
                    delegate_exec_result_t *result_out)
{
   (void)self;
   if (!state || !command || !result_out)
      return -1;
   ssh_state_t *st = state;
   long long start = util_now_ms();

   /* Prefix `cd <cwd> &&` when set_cwd has been called, so successive
    * execs in the same session land in the chosen dir. The local
    * backend uses the shell-state wrapper for this; here we keep it
    * simple while remote-side persistence isn't wired. */
   char *prefixed = NULL;
   const char *to_run = command;
   if (st->cwd[0])
   {
      size_t need = strlen(st->cwd) + strlen(command) + 16;
      prefixed = malloc(need);
      if (prefixed)
      {
         snprintf(prefixed, need, "cd %s && %s", st->cwd, command);
         to_run = prefixed;
      }
   }

   /* Build the ssh shell command via the iter-30 b64 helper so any
    * quoting in `to_run` survives transit through ssh's shell. */
   char *ssh_cmd = NULL;
   if (delegate_backend_ssh_build_exec_command(st->socket_path, st->host, to_run, &ssh_cmd) != 0 ||
       !ssh_cmd)
   {
      free(prefixed);
      return -1;
   }
   free(prefixed);

   /* If AIMEE_SSH_BIN is set, swap the leading "ssh" in the built
    * command for the override binary. Production runs leave the
    * env unset and the leading "ssh" wins via PATH. Tests use this
    * to substitute a fake-ssh fixture and exercise the round-trip
    * without a live sshd. The helper output is guaranteed to start
    * with the literal "ssh " token. */
   const char *bin_override = getenv("AIMEE_SSH_BIN");
   if (bin_override && bin_override[0] && strncmp(ssh_cmd, "ssh ", 4) == 0)
   {
      size_t bin_len = strlen(bin_override);
      size_t tail_len = strlen(ssh_cmd) - 3; /* "ssh" is 3 chars */
      char *swapped = malloc(bin_len + tail_len + 1);
      if (swapped)
      {
         memcpy(swapped, bin_override, bin_len);
         memcpy(swapped + bin_len, ssh_cmd + 3, tail_len + 1);
         free(ssh_cmd);
         ssh_cmd = swapped;
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
      free(ssh_cmd);
      return -1;
   }

   pid_t pid = fork();
   if (pid < 0)
   {
      close(out_pipe[0]);
      close(out_pipe[1]);
      close(err_pipe[0]);
      close(err_pipe[1]);
      free(ssh_cmd);
      return -1;
   }
   if (pid == 0)
   {
      /* Child runs the ssh-cmd through sh so the embedded quoting +
       * pipe (`echo '...' | base64 -d | bash`) is honored. */
      dup2(out_pipe[1], STDOUT_FILENO);
      dup2(err_pipe[1], STDERR_FILENO);
      close(out_pipe[0]);
      close(out_pipe[1]);
      close(err_pipe[0]);
      close(err_pipe[1]);
      execlp("sh", "sh", "-c", ssh_cmd, (char *)NULL);
      _exit(127);
   }
   free(ssh_cmd);
   close(out_pipe[1]);
   close(err_pipe[1]);

   /* Drain pipes via select, enforce timeout. Same shape as
    * delegate_backend_local's exec. */
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

/* Workspace path validation matches the local backend: reject absolute
 * paths and any path with a ".." segment so the workspace sandbox is
 * honest. Returns 0 + writes "<workspace_remote>/<rel>" into out, or
 * -1 on rejection / undersized buffer. */
static int ssh_resolve_in_workspace(const ssh_state_t *st, const char *rel, char *out, size_t outsz)
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
   if (snprintf(out, outsz, "%s/%s", st->workspace_remote, rel) >= (int)outsz)
      return -1;
   return 0;
}

static int ssh_read_file(delegate_backend_t *self, void *state, const char *p, int offset,
                         int limit, char **out)
{
   if (out)
      *out = NULL;
   if (!state || !p || !out || offset < 0 || limit < 0)
      return -1;
   ssh_state_t *st = state;
   char abs[MAX_PATH_LEN];
   if (ssh_resolve_in_workspace(st, p, abs, sizeof(abs)) != 0)
      return -1;
   /* Build a remote `cat <path>` and capture its stdout via ssh_exec.
    * We grab the whole file, then slice in C for offset/limit — keeps
    * the remote command shape simple. limit==0 means "to EOF" capped
    * at 16 MiB to bound the result buffer. */
   size_t cap = limit > 0 ? (size_t)limit : 16 * 1024 * 1024;
   /* Add headroom for the offset bytes we'll discard. */
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
   if (ssh_exec(self, state, cmd, 30000, &r) != 0 || r.exit_code != 0)
   {
      free(buf);
      free(err);
      return -1;
   }
   free(err);
   /* Slice: skip `offset` bytes from the front, truncate at `cap`. */
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

/* RFC 4648 base64 encoder (re-declared here as a small static — the
 * sibling encoder above is also static; sharing would require a header
 * shuffle for one tiny helper). Encodes raw bytes into out (must be
 * sized for ((in_len+2)/3)*4 + 1). */
static int ssh_write_file(delegate_backend_t *self, void *state, const char *p, const char *content)
{
   if (!state || !p || !content)
      return -1;
   ssh_state_t *st = state;
   char abs[MAX_PATH_LEN];
   if (ssh_resolve_in_workspace(st, p, abs, sizeof(abs)) != 0)
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
   /* Remote command: ensure parent dir exists, then decode and write. */
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
   int rc = ssh_exec(self, state, cmd, 30000, &r);
   free(cmd);
   if (rc != 0 || r.exit_code != 0)
      return -1;
   return 0;
}

static int ssh_list_dir(delegate_backend_t *self, void *state, const char *p, char ***entries_out)
{
   if (entries_out)
      *entries_out = NULL;
   if (!state || !p || !entries_out)
      return -1;
   ssh_state_t *st = state;
   char abs[MAX_PATH_LEN];
   if (ssh_resolve_in_workspace(st, p, abs, sizeof(abs)) != 0)
      return -1;

   /* `ls -1A` lists one entry per line, including dotfiles, excluding
    * "." and "..". */
   char buf[64 * 1024] = {0};
   char err[4096] = {0};
   delegate_exec_result_t r = {0, 0, buf, sizeof(buf), err, sizeof(err)};
   char cmd[MAX_PATH_LEN + 16];
   snprintf(cmd, sizeof(cmd), "ls -1A %s", abs);
   if (ssh_exec(self, state, cmd, 30000, &r) != 0 || r.exit_code != 0)
      return -1;

   /* Count lines, then collect. */
   int n = 0;
   for (const char *q = buf; *q; q++)
      if (*q == '\n')
         n++;
   /* Trailing line without newline still counts. */
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

static int ssh_get_cwd(delegate_backend_t *self, void *state, char **out)
{
   (void)self;
   if (!state || !out)
      return -1;
   ssh_state_t *st = state;
   *out = strdup(st->cwd[0] ? st->cwd : "");
   return *out ? 0 : -1;
}

static int ssh_set_cwd(delegate_backend_t *self, void *state, const char *path)
{
   (void)self;
   if (!state || !path || !path[0])
      return -1;
   ssh_state_t *st = state;
   snprintf(st->cwd, sizeof(st->cwd), "%s", path);
   return 0;
}

static delegate_backend_t g_ssh = {
    .name = "ssh",
    .description = "remote subprocess execution via ssh ControlMaster",
    .acquire = ssh_acquire,
    .release = ssh_release,
    .exec = ssh_exec,
    .read_file = ssh_read_file,
    .write_file = ssh_write_file,
    .list_dir = ssh_list_dir,
    .get_cwd = ssh_get_cwd,
    .set_cwd = ssh_set_cwd,
};

int delegate_backend_register_ssh(void)
{
   return delegate_backend_register(&g_ssh);
}

delegate_backend_t *delegate_backend_ssh_get(void)
{
   return &g_ssh;
}
