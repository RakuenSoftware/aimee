/* workspace_provider.c: the `shared` (co-located) resource provider — direct
 * filesystem access on the server host. See workspace_provider.h for the
 * provider contract and the workspace-resource-plane proposal for the plan to
 * add a `detached` (marshalled-over-/v1) provider behind the same interface.
 *
 * Every primitive here is a thin wrapper over a single libc call, so a
 * co-located deployment pays nothing for going through the interface. */
#include "workspace_provider.h"

#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static int shared_read_all(const workspace_provider_t *p, const char *path, char **out, size_t *len)
{
   (void)p;
   if (out)
      *out = NULL;
   if (len)
      *len = 0;
   if (!path || !out)
      return -1;

   FILE *f = fopen(path, "rb");
   if (!f)
      return -1;
   if (fseek(f, 0, SEEK_END) != 0)
   {
      fclose(f);
      return -1;
   }
   long sz = ftell(f);
   if (sz < 0 || fseek(f, 0, SEEK_SET) != 0)
   {
      fclose(f);
      return -1;
   }
   char *buf = malloc((size_t)sz + 1);
   if (!buf)
   {
      fclose(f);
      return -1;
   }
   size_t rd = fread(buf, 1, (size_t)sz, f);
   if (ferror(f))
   {
      free(buf);
      fclose(f);
      return -1;
   }
   fclose(f);
   buf[rd] = '\0';
   *out = buf;
   if (len)
      *len = rd;
   return 0;
}

static int shared_write_all(const workspace_provider_t *p, const char *path, const char *data,
                            size_t len)
{
   (void)p;
   if (!path)
      return -1;

   FILE *f = fopen(path, "wb");
   if (!f)
      return -1;
   if (len > 0 && data && fwrite(data, 1, len, f) != len)
   {
      fclose(f);
      return -1;
   }
   if (fclose(f) != 0)
      return -1;
   return 0;
}

static int shared_stat(const workspace_provider_t *p, const char *path, ws_stat_t *st)
{
   (void)p;
   if (!st)
      return -1;
   st->exists = 0;
   st->is_dir = 0;
   st->size = 0;

   struct stat sb;
   if (path && stat(path, &sb) == 0)
   {
      st->exists = 1;
      st->is_dir = S_ISDIR(sb.st_mode) ? 1 : 0;
      st->size = S_ISDIR(sb.st_mode) ? 0 : (long)sb.st_size;
   }
   return 0;
}

static int shared_list(const workspace_provider_t *p, const char *dir, const char *pattern,
                       char ***out, int *count)
{
   (void)p;
   if (out)
      *out = NULL;
   if (count)
      *count = 0;
   if (!dir || !out || !count)
      return -1;

   char glob_pat[4096];
   if (pattern && pattern[0])
      snprintf(glob_pat, sizeof(glob_pat), "%s/%s", dir, pattern);
   else
      snprintf(glob_pat, sizeof(glob_pat), "%s/*", dir);

   glob_t g;
   memset(&g, 0, sizeof(g));
   int rc = glob(glob_pat, GLOB_NOSORT, NULL, &g);
   if (rc != 0 && rc != GLOB_NOMATCH)
   {
      globfree(&g);
      return -1;
   }

   int n = (rc == GLOB_NOMATCH) ? 0 : (int)g.gl_pathc;
   char **arr = NULL;
   if (n > 0)
   {
      arr = calloc((size_t)n, sizeof(char *));
      if (!arr)
      {
         globfree(&g);
         return -1;
      }
      for (int i = 0; i < n; i++)
      {
         arr[i] = strdup(g.gl_pathv[i]);
         if (!arr[i])
         {
            ws_provider_free_list(arr, i);
            globfree(&g);
            return -1;
         }
      }
   }
   globfree(&g);
   *out = arr;
   *count = n;
   return 0;
}

/* safe_exec_capture (posix/util.c): fork/exec argv, capture combined output.
 * Forward-declared to keep this TU free of the heavyweight aimee.h include. */
extern int safe_exec_capture(const char *const argv[], char **out_buf, size_t max_out);

/* run_cmd (posix/util.c): run a shell command string, capturing output.
 * Forward-declared so exec_shell can delegate to it. */
extern char *run_cmd(const char *cmd, int *exit_code);

static int shared_exec(const workspace_provider_t *p, const char *const argv[], char **out,
                       size_t max_out)
{
   (void)p;
   return safe_exec_capture(argv, out, max_out);
}

static char *shared_exec_shell(const workspace_provider_t *p, const char *cmd, int *exit_code)
{
   (void)p;
   return run_cmd(cmd, exit_code);
}

static int set_nonblock(int fd)
{
   int fl = fcntl(fd, F_GETFL, 0);
   return (fl < 0) ? -1 : fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

/* Fork/exec argv with `stdin_data` piped to the child and its stdout streamed
 * out through `on_chunk` as it is produced. A poll loop drives the stdin write
 * and stdout read together so neither side deadlocks on a full pipe. This is
 * what the client-side runner uses to execute a local-CLI agent (claude -p). */
static int shared_exec_stream(const workspace_provider_t *p, const char *const argv[],
                              const char *stdin_data, size_t stdin_len, const char *cwd,
                              ws_exec_chunk_fn on_chunk, void *cb_ctx)
{
   (void)p;
   if (!argv || !argv[0])
      return -1;

   int in_pipe[2] = {-1, -1};
   int out_pipe[2] = {-1, -1};
   if (pipe(in_pipe) != 0)
      return -1;
   if (pipe(out_pipe) != 0)
   {
      close(in_pipe[0]);
      close(in_pipe[1]);
      return -1;
   }

   pid_t pid = fork();
   if (pid < 0)
   {
      close(in_pipe[0]);
      close(in_pipe[1]);
      close(out_pipe[0]);
      close(out_pipe[1]);
      return -1;
   }
   if (pid == 0)
   {
      /* child */
      dup2(in_pipe[0], STDIN_FILENO);
      dup2(out_pipe[1], STDOUT_FILENO);
      close(in_pipe[0]);
      close(in_pipe[1]);
      close(out_pipe[0]);
      close(out_pipe[1]);
      if (cwd && cwd[0] && chdir(cwd) != 0)
         _exit(127);
      execvp(argv[0], (char *const *)argv);
      _exit(127);
   }

   /* parent */
   close(in_pipe[0]);
   close(out_pipe[1]);
   int in_fd = in_pipe[1];
   int out_fd = out_pipe[0];
   set_nonblock(in_fd);
   set_nonblock(out_fd);

   struct sigaction old_pipe, ignore_pipe;
   memset(&ignore_pipe, 0, sizeof(ignore_pipe));
   ignore_pipe.sa_handler = SIG_IGN;
   sigemptyset(&ignore_pipe.sa_mask);
   int restore_pipe = sigaction(SIGPIPE, &ignore_pipe, &old_pipe) == 0;

   size_t in_off = 0;
   if (!stdin_data || stdin_len == 0)
   {
      close(in_fd);
      in_fd = -1;
   }
   int aborted = 0;

   for (;;)
   {
      /* drain stdout */
      for (;;)
      {
         char buf[8192];
         ssize_t r = read(out_fd, buf, sizeof(buf));
         if (r > 0)
         {
            if (on_chunk && on_chunk(cb_ctx, buf, (size_t)r) != 0)
            {
               aborted = 1;
               break;
            }
            continue;
         }
         if (r == 0)
         {
            close(out_fd);
            out_fd = -1;
            break;
         }
         if (r < 0 && errno == EINTR)
            continue;
         break; /* EAGAIN — poll below */
      }
      if (aborted || out_fd < 0)
         break;

      /* push stdin */
      while (in_fd >= 0 && in_off < stdin_len)
      {
         ssize_t n = write(in_fd, stdin_data + in_off, stdin_len - in_off);
         if (n > 0)
         {
            in_off += (size_t)n;
            continue;
         }
         if (n < 0 && errno == EINTR)
            continue;
         break; /* EAGAIN, or write error → close below */
      }
      if (in_fd >= 0 && in_off >= stdin_len)
      {
         close(in_fd);
         in_fd = -1;
      }

      struct pollfd pfds[2];
      nfds_t nfds = 0;
      if (out_fd >= 0)
      {
         pfds[nfds].fd = out_fd;
         pfds[nfds].events = POLLIN;
         nfds++;
      }
      if (in_fd >= 0)
      {
         pfds[nfds].fd = in_fd;
         pfds[nfds].events = POLLOUT;
         nfds++;
      }
      if (nfds == 0)
         break;
      poll(pfds, nfds, 1000);
   }

   if (in_fd >= 0)
      close(in_fd);
   if (out_fd >= 0)
      close(out_fd);
   if (aborted)
      kill(pid, SIGKILL);
   if (restore_pipe)
      sigaction(SIGPIPE, &old_pipe, NULL);

   int status = 0;
   while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
      ;
   if (aborted)
      return -1;
   return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

static const workspace_provider_t g_shared_provider = {
    .kind = WS_PROVIDER_SHARED,
    .read_all = shared_read_all,
    .write_all = shared_write_all,
    .stat = shared_stat,
    .list = shared_list,
    .exec = shared_exec,
    .exec_shell = shared_exec_shell,
    .exec_stream = shared_exec_stream,
};

const workspace_provider_t *workspace_provider_shared(void)
{
   return &g_shared_provider;
}

const workspace_provider_t *workspace_provider_for_kind(ws_provider_kind_t kind)
{
   /* Only `shared` is implemented in Phase 1; `detached` falls back to it so
    * callers can already pass a kind through without special-casing. */
   (void)kind;
   return &g_shared_provider;
}

/* Per-thread active provider — bound for the duration of a turn that acts on a
 * non-shared workspace, alongside the thread-local cwd the tools already use. */
static __thread const workspace_provider_t *t_active_provider = NULL;

const workspace_provider_t *workspace_provider_active(void)
{
   return t_active_provider ? t_active_provider : &g_shared_provider;
}

void workspace_provider_set_active(const workspace_provider_t *p)
{
   t_active_provider = p;
}

void workspace_provider_clear_active(void)
{
   t_active_provider = NULL;
}

void ws_provider_free_list(char **entries, int count)
{
   if (!entries)
      return;
   for (int i = 0; i < count; i++)
      free(entries[i]);
   free(entries);
}

/* "container" is deliberately absent: a container provider needs a live container
 * handle, so it is bound per delegate, never named in config. Mapping the name here
 * would let a workspace ask for `container` and silently get `shared` — the delegate
 * would run on the host believing it was sandboxed. See WS_PROVIDER_CONTAINER. */
ws_provider_kind_t ws_provider_kind_from_string(const char *name)
{
   if (name && strcmp(name, "detached") == 0)
      return WS_PROVIDER_DETACHED;
   if (name && strcmp(name, "mirror") == 0)
      return WS_PROVIDER_MIRROR;
   return WS_PROVIDER_SHARED;
}

const char *ws_provider_kind_to_string(ws_provider_kind_t kind)
{
   switch (kind)
   {
   case WS_PROVIDER_DETACHED:
      return "detached";
   case WS_PROVIDER_MIRROR:
      return "mirror";
   case WS_PROVIDER_CONTAINER:
      /* Must be named, even though config can never select it: this string is what
       * logs and /v1 report, and a container provider reported as "shared" would
       * describe a sandboxed delegate as running on the host. */
      return "container";
   default:
      return "shared";
   }
}
