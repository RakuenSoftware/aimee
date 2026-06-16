/* delegate_backend_local.c: see delegate_backend_local.h.
 *
 * Workspace lives under $XDG_CACHE_HOME/aimee/delegate/<task_id>/
 * (falls back to $HOME/.cache/aimee/delegate/<task_id>/). exec() forks
 * /bin/bash -c <command> in the workspace cwd with stdout/stderr
 * piped back to caller-provided buffers. */

#include "delegate_backend_local.h"
#include "util.h"

#include "aimee.h" /* MAX_PATH_LEN */

/* Defined in server_compute.c; called post-fork to seed the child's delegate
 * context env from the forking thread's TLS (race-free). */
void delegate_child_export_context_env(void);

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

typedef struct
{
   char workspace[MAX_PATH_LEN];
   char cwd[MAX_PATH_LEN];
} local_state_t;

/* Build the workspace path: prefer $XDG_CACHE_HOME, else $HOME/.cache.
 * Returns 0 on success, -1 if neither is set (which would be a very
 * broken environment). */
static int compute_workspace_root(char *out, size_t outsz)
{
   const char *xdg = getenv("XDG_CACHE_HOME");
   if (xdg && xdg[0])
   {
      snprintf(out, outsz, "%s/aimee/delegate", xdg);
      return 0;
   }
   const char *home = getenv("HOME");
   if (!home || !home[0])
      return -1;
   snprintf(out, outsz, "%s/.cache/aimee/delegate", home);
   return 0;
}

/* mkdir -p equivalent. Returns 0 if the directory exists at the end
 * (created or pre-existing). */
static int mkdir_p(const char *path)
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

/* rm -rf equivalent. Best-effort; failure is logged via errno but the
 * caller cannot do much about it for an ephemeral workspace. */
static int rm_rf(const char *path)
{
   DIR *d = opendir(path);
   if (!d)
   {
      if (errno == ENOENT)
         return 0;
      return -1;
   }
   struct dirent *e;
   while ((e = readdir(d)) != NULL)
   {
      if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
         continue;
      char child[MAX_PATH_LEN];
      snprintf(child, sizeof(child), "%s/%s", path, e->d_name);
      struct stat st;
      if (lstat(child, &st) == 0 && S_ISDIR(st.st_mode))
         rm_rf(child);
      else
         unlink(child);
   }
   closedir(d);
   return rmdir(path);
}

static int local_acquire(delegate_backend_t *self, const char *task_id,
                         const delegate_backend_config_t *cfg, void **state_out)
{
   (void)self;
   (void)cfg;
   if (!task_id || !task_id[0] || !state_out)
      return -1;

   char root[MAX_PATH_LEN];
   if (compute_workspace_root(root, sizeof(root)) != 0)
      return -1;

   local_state_t *st = calloc(1, sizeof(*st));
   if (!st)
      return -1;
   snprintf(st->workspace, sizeof(st->workspace), "%s/%s", root, task_id);
   snprintf(st->cwd, sizeof(st->cwd), "%s", st->workspace);
   if (mkdir_p(st->workspace) != 0)
   {
      free(st);
      return -1;
   }
   *state_out = st;
   return 0;
}

static void local_release(delegate_backend_t *self, void *state, int hibernate)
{
   (void)self;
   if (!state)
      return;
   local_state_t *st = state;
   if (!hibernate && st->workspace[0])
      rm_rf(st->workspace);
   free(st);
}

/* Read up to outsz-1 bytes from `path` into `out`, NUL-terminate, and
 * strip a single trailing newline if present. Returns the number of
 * bytes written (0 if the file does not exist, -1 on read error). */
static int read_small_file(const char *path, char *out, size_t outsz)
{
   if (!path || !out || outsz == 0)
      return -1;
   int fd = open(path, O_RDONLY);
   if (fd < 0)
      return errno == ENOENT ? 0 : -1;
   ssize_t n = read(fd, out, outsz - 1);
   close(fd);
   if (n < 0)
      return -1;
   out[n] = '\0';
   while (n > 0 && (out[n - 1] == '\n' || out[n - 1] == '\r'))
      out[--n] = '\0';
   return (int)n;
}

static int local_exec(delegate_backend_t *self, void *state, const char *command, int timeout_ms,
                      delegate_exec_result_t *result_out)
{
   (void)self;
   if (!state || !command || !result_out)
      return -1;
   local_state_t *st = state;
   long long start = util_now_ms();

   /* Wrap the user command in the shared snapshot wrapper so cwd
    * persists across exec calls — the wrapper writes pwd to CWD_FILE
    * after the user command exits, and we read it back to update
    * state->cwd. The snap file is written too but we don't act on it
    * yet (env/alias persistence is a follow-up iter). */
   char snap_path[MAX_PATH_LEN];
   char cwd_file_path[MAX_PATH_LEN];
   snprintf(snap_path, sizeof(snap_path), "%s/.snap.sh", st->workspace);
   snprintf(cwd_file_path, sizeof(cwd_file_path), "%s/.cwd", st->workspace);
   /* Seed the cwd file with the current state cwd so the wrapper's
    * `cd "$(cat $CWD_FILE)"` step lands us in the right place even on
    * the first exec call. */
   {
      int fd = open(cwd_file_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
      if (fd >= 0)
      {
         dprintf(fd, "%s\n", st->cwd);
         close(fd);
      }
   }
   char *wrapped = NULL;
   if (delegate_backend_wrap_command(snap_path, cwd_file_path, command, &wrapped) != 0 || !wrapped)
      return -1;

   int out_pipe[2] = {-1, -1};
   int err_pipe[2] = {-1, -1};
   if (pipe(out_pipe) != 0 || pipe(err_pipe) != 0)
   {
      free(wrapped);
      if (out_pipe[0] >= 0)
         close(out_pipe[0]);
      if (out_pipe[1] >= 0)
         close(out_pipe[1]);
      if (err_pipe[0] >= 0)
         close(err_pipe[0]);
      if (err_pipe[1] >= 0)
         close(err_pipe[1]);
      return -1;
   }

   pid_t pid = fork();
   if (pid < 0)
   {
      free(wrapped);
      close(out_pipe[0]);
      close(out_pipe[1]);
      close(err_pipe[0]);
      close(err_pipe[1]);
      return -1;
   }
   if (pid == 0)
   {
      /* Child */
      dup2(out_pipe[1], STDOUT_FILENO);
      dup2(err_pipe[1], STDERR_FILENO);
      close(out_pipe[0]);
      close(out_pipe[1]);
      close(err_pipe[0]);
      close(err_pipe[1]);
      if (st->cwd[0] && chdir(st->cwd) != 0)
         _exit(126);
      /* Seed the child's delegate context env from the forking thread's TLS so a
       * sub-`aimee` invocation inside this command sees the correct depth/parent/
       * source context, not a concurrently-clobbered global. */
      delegate_child_export_context_env();
      execlp("bash", "bash", "-c", wrapped, (char *)NULL);
      _exit(127);
   }
   free(wrapped);

   close(out_pipe[1]);
   close(err_pipe[1]);

   /* Drain stdout/stderr concurrently with select(); enforce timeout
    * by killing the child if the deadline passes. timeout_ms <= 0
    * means "no timeout." */
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
   /* If we broke out of the select loop due to timeout, the child got
    * SIGTERM. Close pipes (drops the writer-side reference) and reap;
    * any bytes still in flight are lost — acceptable for a timed-out
    * run. */
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

   /* Pull pwd back from the wrapper-written cwd file so the next
    * exec resumes in the same directory. Empty / missing file means
    * the wrapper never ran (e.g. exec failed before bash) — leave
    * state->cwd untouched in that case. */
   char new_cwd[MAX_PATH_LEN] = {0};
   if (read_small_file(cwd_file_path, new_cwd, sizeof(new_cwd)) > 0)
      snprintf(st->cwd, sizeof(st->cwd), "%s", new_cwd);

   return 0;
}

static int local_get_cwd(delegate_backend_t *self, void *state, char **out)
{
   (void)self;
   if (!state || !out)
      return -1;
   local_state_t *st = state;
   *out = strdup(st->cwd);
   return *out ? 0 : -1;
}

static int local_set_cwd(delegate_backend_t *self, void *state, const char *path)
{
   (void)self;
   if (!state || !path || !path[0])
      return -1;
   local_state_t *st = state;
   snprintf(st->cwd, sizeof(st->cwd), "%s", path);
   return 0;
}

/* Resolve `rel` against the workspace and reject any path that would
 * escape it. Absolute paths and segments containing ".." are rejected
 * to keep the workspace sandbox honest. Returns 0 on success and
 * writes the joined path into `out`; -1 on rejection. */
static int resolve_in_workspace(const local_state_t *st, const char *rel, char *out, size_t outsz)
{
   if (!st || !rel || !out || outsz == 0)
      return -1;
   if (rel[0] == '/')
      return -1;
   /* Walk the components, rejecting "..". Empty components ("//")
    * are tolerated since `path/to/foo` and `path//to/foo` both
    * resolve identically. */
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
   if (snprintf(out, outsz, "%s/%s", st->workspace, rel) >= (int)outsz)
      return -1;
   return 0;
}

/* mkdir -p the parent directory of `path`. */
static int mkdir_parents(const char *path)
{
   char buf[MAX_PATH_LEN];
   snprintf(buf, sizeof(buf), "%s", path);
   char *slash = strrchr(buf, '/');
   if (!slash)
      return 0;
   *slash = '\0';
   return mkdir_p(buf);
}

static int local_read_file(delegate_backend_t *self, void *state, const char *path, int offset,
                           int limit, char **out)
{
   (void)self;
   if (out)
      *out = NULL;
   if (!state || !path || !out || offset < 0 || limit < 0)
      return -1;
   local_state_t *st = state;
   char abs[MAX_PATH_LEN];
   if (resolve_in_workspace(st, path, abs, sizeof(abs)) != 0)
      return -1;
   int fd = open(abs, O_RDONLY);
   if (fd < 0)
      return -1;
   if (offset > 0 && lseek(fd, offset, SEEK_SET) == (off_t)-1)
   {
      close(fd);
      return -1;
   }
   /* limit == 0 means "read to EOF" — bound at 16 MiB to keep the
    * malloc sane. */
   size_t cap = limit > 0 ? (size_t)limit : 16 * 1024 * 1024;
   char *buf = malloc(cap + 1);
   if (!buf)
   {
      close(fd);
      return -1;
   }
   size_t off = 0;
   while (off < cap)
   {
      ssize_t r = read(fd, buf + off, cap - off);
      if (r < 0)
      {
         free(buf);
         close(fd);
         return -1;
      }
      if (r == 0)
         break;
      off += (size_t)r;
   }
   buf[off] = '\0';
   close(fd);
   *out = buf;
   return 0;
}

static int local_write_file(delegate_backend_t *self, void *state, const char *path,
                            const char *content)
{
   (void)self;
   if (!state || !path || !content)
      return -1;
   local_state_t *st = state;
   char abs[MAX_PATH_LEN];
   if (resolve_in_workspace(st, path, abs, sizeof(abs)) != 0)
      return -1;
   if (mkdir_parents(abs) != 0)
      return -1;
   int fd = open(abs, O_WRONLY | O_CREAT | O_TRUNC, 0600);
   if (fd < 0)
      return -1;
   size_t len = strlen(content);
   size_t off = 0;
   while (off < len)
   {
      ssize_t w = write(fd, content + off, len - off);
      if (w <= 0)
      {
         close(fd);
         return -1;
      }
      off += (size_t)w;
   }
   close(fd);
   return 0;
}

static int local_list_dir(delegate_backend_t *self, void *state, const char *path,
                          char ***entries_out)
{
   (void)self;
   if (entries_out)
      *entries_out = NULL;
   if (!state || !path || !entries_out)
      return -1;
   local_state_t *st = state;
   char abs[MAX_PATH_LEN];
   if (resolve_in_workspace(st, path, abs, sizeof(abs)) != 0)
      return -1;
   DIR *d = opendir(abs);
   if (!d)
      return -1;

   /* Two-pass: count then collect. opendir/readdir are cheap relative
    * to malloc and the entry count is small. */
   int n = 0;
   struct dirent *e;
   while ((e = readdir(d)) != NULL)
   {
      if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
         continue;
      n++;
   }
   rewinddir(d);
   char **entries = calloc((size_t)n + 1, sizeof(*entries));
   if (!entries)
   {
      closedir(d);
      return -1;
   }
   int i = 0;
   while ((e = readdir(d)) != NULL && i < n)
   {
      if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
         continue;
      entries[i] = strdup(e->d_name);
      if (!entries[i])
      {
         for (int j = 0; j < i; j++)
            free(entries[j]);
         free(entries);
         closedir(d);
         return -1;
      }
      i++;
   }
   entries[i] = NULL;
   closedir(d);
   *entries_out = entries;
   return i;
}

static delegate_backend_t g_local = {
    .name = "local",
    .description = "in-process subprocess execution under ~/.cache/aimee/delegate/<task_id>/",
    .acquire = local_acquire,
    .release = local_release,
    .exec = local_exec,
    .read_file = local_read_file,
    .write_file = local_write_file,
    .list_dir = local_list_dir,
    .get_cwd = local_get_cwd,
    .set_cwd = local_set_cwd,
};

int delegate_backend_register_local(void)
{
   return delegate_backend_register(&g_local);
}

delegate_backend_t *delegate_backend_local_get(void)
{
   return &g_local;
}
