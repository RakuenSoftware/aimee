#include <aimee/translation/harness_ir.h>
#include <aimee/translation/aimee_backend.h>
#include "agent_config.h"
#include "agent_exec.h"
#include "cli_codex_profile.h"
/* cli_codex.c: see cli_codex.h. Spawns `codex app-server`, drives the
 * JSON-RPC handshake (initialize → thread/start → turn/start), reads
 * notifications until turn/completed, and assembles the final
 * agent_result_t. */

#include "cli_codex.h"
#include "util.h"

#include "cJSON.h"
#include "log.h"
#include "provider_cli_adapter.h"
#include "server_session_control.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <poll.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* Default idle ceiling. The codex side has its own timeout; this is the
 * safety net for "subprocess wedged with no output." */
#define CLI_CODEX_DEFAULT_IDLE_TIMEOUT_MS -1     /* no idle timeout */
#define CLI_CODEX_QUIET_POLL_MS           300000 /* 5 min */
#define CLI_CODEX_LINE_MAX                (8 * 1024 * 1024)

/* Reasons cli_codex_read_line can return NULL — propagated into the
 * stall message so operators can tell EOF (codex exited) from an idle
 * deadline expiry. */
typedef enum
{
   CLI_CODEX_READ_OK = 0,
   CLI_CODEX_READ_DEADLINE,     /* idle deadline_ms reached */
   CLI_CODEX_READ_LINE_TIMEOUT, /* poll() returned 0 within per-line budget */
   CLI_CODEX_READ_POLL_ERR,
   CLI_CODEX_READ_EOF, /* read() returned 0 → codex exited */
   CLI_CODEX_READ_READ_ERR,
   CLI_CODEX_READ_LINE_TOO_LARGE,
   CLI_CODEX_READ_OOM,
   CLI_CODEX_READ_FRAME_ERR,
} cli_codex_read_status_t;

typedef struct
{
   pid_t pid;
   int in_fd;  /* aimee writes here → codex stdin */
   int out_fd; /* aimee reads here ← codex stdout */
   int err_fd; /* aimee reads here ← codex stderr (diagnostics) */
   /* incremental line buffer for stdout */
   char *buf;
   char *line; /* Borrowed by readers until the next read or close. */
   size_t buf_len;
   size_t buf_cap;
   /* tail-buffer of stderr — last STDERR_TAIL_CAP bytes only, drained
    * non-blockingly so a noisy codex doesn't wedge us. Surfaced in
    * agent_result_t.error when we end on a stall, which is the only
    * way operators see codex's own error messages. */
#define CLI_CODEX_STDERR_TAIL_CAP 4096
   char err_tail[CLI_CODEX_STDERR_TAIL_CAP];
   size_t err_tail_len;
   /* Full forensic log of every byte read from stderr. The tail buffer
    * is for the in-band error message; this file is for post-mortem.
    * Path is /tmp/aimee-codex-<pid>.log; recorded so the stall path can
    * print it and operators know where to look. */
   int err_log_fd;
   char err_log_path[64];
   int child_reaped;
   int child_status;
   cli_codex_read_status_t last_read_status;
   aimee_harness_ir_t ir;
   const cli_codex_host_launch_t *host_launch;
   int (*progress)(void *context);
   void *progress_context;
   const aimee_request_t *tool_catalog;
   int (*tool_call)(const aimee_block_t *, aimee_block_t *, void *);
   void *tool_context;
   cJSON *dynamic_tools;
   cJSON *dynamic_seen;
   char *dynamic_thread;
   char *dynamic_turn;
   int dynamic_failed;
} cli_codex_t;

/* Read whatever stderr has buffered without blocking, append to the
 * tail buffer (overwriting from the start when full so we keep the
 * MOST RECENT bytes), and tee the full byte stream to the per-PID
 * forensic log. Safe to call repeatedly. */
static void cli_codex_drain_stderr(cli_codex_t *c)
{
   if (c->err_fd < 0)
      return;
   char chunk[1024];
   for (;;)
   {
      ssize_t r = read(c->err_fd, chunk, sizeof(chunk));
      if (r == 0)
      {
         close(c->err_fd);
         c->err_fd = -1;
         break;
      }
      if (r < 0)
      {
         if (errno == EINTR)
            continue;
         break;
      }
      if (c->err_log_fd >= 0)
      {
         ssize_t w = write(c->err_log_fd, chunk, (size_t)r);
         (void)w;
      }
      /* Append into the tail buffer; rotate if we'd overflow. The chunk
       * itself is always smaller than the tail cap (sizeof(chunk)=1024 <
       * CLI_CODEX_STDERR_TAIL_CAP=4096), so we only need to drop a
       * prefix of the existing tail to make room. */
      size_t written = (size_t)r;
      if (c->err_tail_len + written > CLI_CODEX_STDERR_TAIL_CAP)
      {
         size_t drop = (c->err_tail_len + written) - CLI_CODEX_STDERR_TAIL_CAP;
         memmove(c->err_tail, c->err_tail + drop, c->err_tail_len - drop);
         c->err_tail_len -= drop;
      }
      memcpy(c->err_tail + c->err_tail_len, chunk, written);
      c->err_tail_len += written;
   }
}

static int cli_codex_utf8_sequence_len(unsigned char c)
{
   if (c < 0x80)
      return 1;
   if (c >= 0xc2 && c <= 0xdf)
      return 2;
   if (c >= 0xe0 && c <= 0xef)
      return 3;
   if (c >= 0xf0 && c <= 0xf4)
      return 4;
   return 0;
}

static int cli_codex_utf8_valid_sequence(const unsigned char *s, size_t remaining, int len)
{
   if (!s || len <= 0 || remaining < (size_t)len)
      return 0;
   for (int i = 1; i < len; i++)
      if ((s[i] & 0xc0) != 0x80)
         return 0;

   if (len == 3)
   {
      if (s[0] == 0xe0 && s[1] < 0xa0)
         return 0;
      if (s[0] == 0xed && s[1] >= 0xa0)
         return 0;
   }
   else if (len == 4)
   {
      if (s[0] == 0xf0 && s[1] < 0x90)
         return 0;
      if (s[0] == 0xf4 && s[1] > 0x8f)
         return 0;
   }
   return 1;
}

static char *cli_codex_sanitize_utf8(const char *input)
{
   static const char repl[] = "\xef\xbf\xbd";
   if (!input)
      input = "";

   size_t len = strlen(input);
   size_t cap = len * 3 + 1;
   char *out = malloc(cap ? cap : 1);
   if (!out)
      return NULL;

   size_t off = 0;
   const unsigned char *s = (const unsigned char *)input;
   for (size_t i = 0; i < len;)
   {
      int seq_len = cli_codex_utf8_sequence_len(s[i]);
      if (seq_len > 0 && cli_codex_utf8_valid_sequence(s + i, len - i, seq_len))
      {
         memcpy(out + off, s + i, (size_t)seq_len);
         off += (size_t)seq_len;
         i += (size_t)seq_len;
      }
      else
      {
         memcpy(out + off, repl, sizeof(repl) - 1);
         off += sizeof(repl) - 1;
         i++;
      }
   }
   out[off] = '\0';
   return out;
}

static void cli_codex_add_utf8_string(cJSON *obj, const char *name, const char *value)
{
   char *clean = cli_codex_sanitize_utf8(value);
   cJSON_AddStringToObject(obj, name, clean ? clean : "");
   free(clean);
}

static int cli_codex_reap_child(cli_codex_t *c, int wait_ms)
{
   if (!c || c->pid <= 0 || c->child_reaped)
      return 1;

   long long deadline = util_now_ms() + wait_ms;
   for (;;)
   {
      int status = 0;
      pid_t r = waitpid(c->pid, &status, WNOHANG);
      if (r == c->pid)
      {
         c->child_status = status;
         c->child_reaped = 1;
         return 1;
      }
      if (r < 0)
      {
         if (errno == EINTR)
            continue;
         if (errno == ECHILD)
         {
            c->child_status = 0;
            c->child_reaped = 1;
            return 1;
         }
         return 0;
      }

      cli_codex_drain_stderr(c);
      if (wait_ms <= 0 || util_now_ms() >= deadline)
         return 0;
      struct timespec ts = {0, 10 * 1000000L};
      nanosleep(&ts, NULL);
   }
}

static void cli_codex_collect_exit_diagnostics(cli_codex_t *c)
{
   if (!c)
      return;
   cli_codex_drain_stderr(c);
   (void)cli_codex_reap_child(c, 500);
   cli_codex_drain_stderr(c);
}

static void cli_codex_exit_status_desc(const cli_codex_t *c, char *out, size_t outsz)
{
   if (!out || outsz == 0)
      return;
   out[0] = '\0';
   if (!c || !c->child_reaped)
      return;
   if (WIFEXITED(c->child_status))
      snprintf(out, outsz, ", status %d", WEXITSTATUS(c->child_status));
   else if (WIFSIGNALED(c->child_status))
      snprintf(out, outsz, ", signal %d", WTERMSIG(c->child_status));
   else
      snprintf(out, outsz, ", unknown exit status");
}

/* Spawn `codex app-server`. Returns 0 on success. */
static int cli_codex_spawn(cli_codex_t *c)
{
   if (c->host_launch && c->host_launch->spawn_guard &&
       (!c->host_launch->spawn_guard->child || !c->host_launch->spawn_guard->parent))
      return -1;
   int in_pipe[2] = {-1, -1};
   int out_pipe[2] = {-1, -1};
   int err_pipe[2] = {-1, -1};
   if (cli_native_pipe(in_pipe) != 0 || cli_native_pipe(out_pipe) != 0 ||
       cli_native_pipe(err_pipe) != 0)
   {
      if (in_pipe[0] >= 0)
         close(in_pipe[0]);
      if (in_pipe[1] >= 0)
         close(in_pipe[1]);
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
      close(in_pipe[0]);
      close(in_pipe[1]);
      close(out_pipe[0]);
      close(out_pipe[1]);
      close(err_pipe[0]);
      close(err_pipe[1]);
      return -1;
   }
   if (pid == 0)
   {
      int executable = cli_native_pin_fd(c->host_launch->executable_fd);
      int cwd = cli_native_pin_fd(c->host_launch->cwd_fd);
      if (executable < 0 || cwd < 0)
         _exit(126);
      /* Child. Become process-group leader so SIGTERM to -pgid kills the
       * codex node wrapper AND the inner codex binary it execs. Without
       * this, signaling the wrapper PID alone leaves the inner binary
       * reparented to PID 1, leaking processes on every stalled abort. */
      if (setsid() < 0 && c->host_launch)
         _exit(126);
      if (dup2(in_pipe[0], STDIN_FILENO) < 0 || dup2(out_pipe[1], STDOUT_FILENO) < 0 ||
          dup2(err_pipe[1], STDERR_FILENO) < 0)
         _exit(126);
      close(in_pipe[0]);
      close(in_pipe[1]);
      close(out_pipe[0]);
      close(out_pipe[1]);
      close(err_pipe[0]);
      close(err_pipe[1]);
      if (c->host_launch)
      {
         const cli_codex_host_launch_t *host = c->host_launch;
         if (host->executable_fd < 0 || host->cwd_fd < 0 || !host->environment || fchdir(cwd))
            _exit(126);
         /* The child inherits only its pipes and explicitly owned launch
          * resources. Never leak server/Vault sockets through SDK descendants. */
         long limit = sysconf(_SC_OPEN_MAX);
         if (limit < 0)
            _exit(126);
         for (int fd = 3; fd < limit; fd++)
            if (fcntl(fd, F_SETFD, FD_CLOEXEC) < 0 && errno != EBADF)
               _exit(126);
         if (host->spawn_guard &&
             host->spawn_guard->child(host->spawn_guard->context, executable, cwd))
            _exit(126);
         char *const arguments[] = {"codex", "app-server", NULL};
#if defined(__linux__)
         fexecve(executable, arguments, host->environment);
#else
         /* This owner needs an executable-descriptor launch implementation on
          * other hosts before claiming custody; no ambient executable fallback. */
         (void)arguments;
#endif
         _exit(127);
      }
   }
   /* Parent: the optional owner attaches this exact child before SDK exec. */
   if (c->host_launch && c->host_launch->spawn_guard &&
       c->host_launch->spawn_guard->parent(c->host_launch->spawn_guard->context, pid))
   {
      (void)kill(pid, SIGKILL);
      while (waitpid(pid, NULL, 0) < 0 && errno == EINTR)
      {
      }
      close(in_pipe[0]);
      close(in_pipe[1]);
      close(out_pipe[0]);
      close(out_pipe[1]);
      close(err_pipe[0]);
      close(err_pipe[1]);
      return -1;
   }
   close(in_pipe[0]);
   close(out_pipe[1]);
   close(err_pipe[1]);
   /* Make stderr non-blocking so drain is safe to call any time. */
   {
      int fl = fcntl(err_pipe[0], F_GETFL, 0);
      if (fl >= 0)
         fcntl(err_pipe[0], F_SETFL, fl | O_NONBLOCK);
   }
   c->pid = pid;
   c->in_fd = in_pipe[1];
   c->out_fd = out_pipe[0];
   c->err_fd = err_pipe[0];
   /* Non-blocking stdin so cli_codex_send can drain codex stdout/stderr
    * concurrently with the write. A single blocking write() on a large
    * (>pipe-buffer) prompt could deadlock if codex produces output while
    * we are stuck in the write — the stalls observed at ~32 KB inputs. */
   {
      int fl = fcntl(c->in_fd, F_GETFL, 0);
      if (fl >= 0)
         fcntl(c->in_fd, F_SETFL, fl | O_NONBLOCK);
   }
   c->buf = NULL;
   c->buf_len = 0;
   c->buf_cap = 0;
   c->err_tail_len = 0;
   c->child_reaped = 0;
   c->child_status = 0;
   c->last_read_status = CLI_CODEX_READ_OK;
   /* Open the per-PID forensic stderr log. Best-effort: a missing /tmp
    * or full disk shouldn't kill the delegate — just disables the
    * forensic path while the in-memory tail still works. */
   snprintf(c->err_log_path, sizeof(c->err_log_path), "/tmp/aimee-codex-%d.log", (int)pid);
   c->err_log_fd = open(c->err_log_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
   return 0;
}

static void cli_codex_close(cli_codex_t *c)
{
   /* Keep stderr/log open until after the child has exited. Some provider
    * failures close stdout first and emit diagnostics as they unwind. */
   cli_codex_drain_stderr(c);
   if (c->in_fd >= 0)
   {
      close(c->in_fd);
      c->in_fd = -1;
   }
   if (c->out_fd >= 0)
   {
      close(c->out_fd);
      c->out_fd = -1;
   }
   if (c->pid > 0)
   {
      /* Give the child a moment to exit cleanly after we closed stdin;
       * SIGTERM the whole process group (set up via setsid in spawn) so
       * we hit the codex node wrapper AND the inner binary it execs.
       * Falls back to SIGKILL after another grace if SIGTERM is ignored. */
      int status = 0;
      for (int i = 0; i < 30; i++)
      {
         if (cli_codex_reap_child(c, 0))
            break;
         struct timespec ts = {0, 50 * 1000000L};
         nanosleep(&ts, NULL);
      }
      if (!cli_codex_reap_child(c, 0))
      {
         kill(-c->pid, SIGTERM);
         for (int i = 0; i < 20; i++)
         {
            if (cli_codex_reap_child(c, 0))
               break;
            struct timespec ts = {0, 50 * 1000000L};
            nanosleep(&ts, NULL);
         }
         if (!cli_codex_reap_child(c, 0))
         {
            kill(-c->pid, SIGKILL);
            while (waitpid(c->pid, &status, 0) < 0 && errno == EINTR)
               ;
            c->child_status = status;
            c->child_reaped = 1;
         }
      }
      cli_codex_drain_stderr(c);
      c->pid = 0;
   }
   if (c->err_fd >= 0)
   {
      close(c->err_fd);
      c->err_fd = -1;
   }
   if (c->err_log_fd >= 0)
   {
      close(c->err_log_fd);
      c->err_log_fd = -1;
   }
   free(c->buf);
   c->buf = NULL;
   free(c->line);
   c->line = NULL;
   aimee_harness_ir_free(&c->ir);
   cJSON_Delete(c->dynamic_tools);
   cJSON_Delete(c->dynamic_seen);
   free(c->dynamic_thread);
   free(c->dynamic_turn);
}

static int cli_codex_reserve_stdout(cli_codex_t *c, size_t need, cli_codex_read_status_t *status)
{
   if (need > CLI_CODEX_LINE_MAX)
   {
      if (status)
         *status = CLI_CODEX_READ_LINE_TOO_LARGE;
      return -1;
   }
   if (c->buf_cap >= need)
      return 0;

   size_t new_cap = c->buf_cap ? c->buf_cap : 8192;
   while (new_cap < need)
   {
      if (new_cap >= CLI_CODEX_LINE_MAX / 2)
      {
         new_cap = CLI_CODEX_LINE_MAX;
         break;
      }
      new_cap *= 2;
   }
   if (new_cap < need)
   {
      if (status)
         *status = CLI_CODEX_READ_LINE_TOO_LARGE;
      return -1;
   }

   char *nb = realloc(c->buf, new_cap);
   if (!nb)
   {
      if (status)
         *status = CLI_CODEX_READ_OOM;
      return -1;
   }
   c->buf = nb;
   c->buf_cap = new_cap;
   return 0;
}

static int cli_codex_owner_live(cli_codex_t *c)
{
   if (c->progress && c->progress(c->progress_context))
      aimee_ir_session_fail_capture();
   return !c->dynamic_failed && !agent_request_cancelled() && !aimee_ir_session_capture_failed();
}

/* Write `total` bytes from `data` to c->in_fd in chunks. The fd is
 * non-blocking; when it would block, poll writability AND
 * concurrent readability of stdout/stderr so codex can't pipe-deadlock
 * us by producing output before draining stdin. */
static int cli_codex_write_all(cli_codex_t *c, const char *data, size_t total)
{
   size_t off = 0;
   long long deadline = util_now_ms() + 30000;
   while (off < total)
   {
      if (!cli_codex_owner_live(c))
         return -1;
      ssize_t w = write(c->in_fd, data + off, total - off);
      if (w > 0)
      {
         off += (size_t)w;
         continue;
      }
      if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
      {
         struct pollfd descriptors[] = {
             {c->in_fd, POLLOUT, 0}, {c->out_fd, POLLIN, 0}, {c->err_fd, POLLIN, 0}};
         long long remaining = deadline - util_now_ms();
         if (remaining <= 0)
            return -1;
         int ready = poll(descriptors, 3, remaining < 100 ? (int)remaining : 100);
         if (ready < 0)
         {
            if (errno == EINTR)
               continue;
            return -1;
         }
         if (!ready)
            continue; /* Recheck the actual owner while stdin is backpressured. */
         for (int i = 0; i < 3; i++)
            if (descriptors[i].revents & POLLNVAL)
               return -1;

         /* Drain whatever codex produced while we were blocked so its
          * stdout pipe doesn't backpressure into stdin. */
         if (c->out_fd >= 0 && (descriptors[1].revents & (POLLIN | POLLHUP | POLLERR)))
         {
            size_t read_room =
                c->buf_len < CLI_CODEX_LINE_MAX ? CLI_CODEX_LINE_MAX - c->buf_len : 0;
            if (read_room > 4096)
               read_room = 4096;
            if (read_room > 0)
               (void)cli_codex_reserve_stdout(c, c->buf_len + read_room, NULL);
            if (c->buf_cap > c->buf_len)
            {
               ssize_t r = read(c->out_fd, c->buf + c->buf_len, c->buf_cap - c->buf_len);
               if (r > 0)
                  c->buf_len += (size_t)r;
            }
         }
         cli_codex_drain_stderr(c);
         continue;
      }
      if (w < 0 && errno == EINTR)
         continue;
      return -1;
   }
   return 0;
}

static int cli_codex_send(cli_codex_t *c, cJSON *msg)
{
   if (aimee_ir_session_get_sink(NULL) && aimee_codex_reply_ir(msg, &c->ir) != 0)
      return -1;
   char *s = cJSON_PrintUnformatted(msg);
   if (!s)
      return -1;
   size_t n = strlen(s);
   int rc = cli_codex_write_all(c, s, n);
   free(s);
   if (rc != 0)
      return -1;
   return cli_codex_write_all(c, "\n", 1);
}

/* Read until a newline, return the line without terminator (caller must NOT
 * free; pointer is into the internal buffer and stable until the next call).
 * On NULL, *status (if non-NULL) is set to the specific failure mode so the
 * caller can distinguish EOF (codex exited) from an idle deadline expiry. */
static char *cli_codex_read_line(cli_codex_t *c, int timeout_ms, long long deadline_ms,
                                 cli_codex_read_status_t *status)
{
   if (status)
      *status = CLI_CODEX_READ_OK;
   if (c->out_fd < 0)
   {
      if (status)
         *status = CLI_CODEX_READ_EOF;
      return NULL;
   }
   long long line_deadline = timeout_ms > 0 ? util_now_ms() + timeout_ms : 0;
   for (;;)
   {
      if (!cli_codex_owner_live(c))
      {
         if (status)
            *status = CLI_CODEX_READ_READ_ERR;
         return NULL;
      }
      /* Look for newline in current buffer. */
      char *nl = c->buf ? memchr(c->buf, '\n', c->buf_len) : NULL;
      if (nl)
      {
         size_t length = (size_t)(nl - c->buf);
         /* Admit the exact byte frame before any C-string operation can hide
          * an embedded NUL, a duplicate field or trailing JSON. */
         cJSON *frame = aimee_ir_json_parse_frame(c->buf, length);
         if (!cJSON_IsObject(frame))
         {
            cJSON_Delete(frame);
            c->dynamic_failed = 1;
            aimee_ir_session_fail_capture();
            if (status)
               *status = CLI_CODEX_READ_FRAME_ERR;
            return NULL;
         }
         char *line_dup = malloc(length + 1);
         if (!line_dup)
         {
            cJSON_Delete(frame);
            if (status)
               *status = CLI_CODEX_READ_OOM;
            return NULL;
         }
         memcpy(line_dup, c->buf, length);
         line_dup[length] = 0;
         free(c->line);
         c->line = line_dup;
         size_t consumed = length + 1;
         memmove(c->buf, c->buf + consumed, c->buf_len - consumed);
         c->buf_len -= consumed;
         int rc = aimee_codex_frame_ir(frame, &c->ir);
         cJSON_Delete(frame);
         if (rc)
         {
            if (status)
               *status = CLI_CODEX_READ_READ_ERR;
            return NULL;
         }
         return line_dup;
      }

      /* Need more bytes. */
      long ms = timeout_ms;
      if (deadline_ms > 0)
      {
         long long remaining = deadline_ms - util_now_ms();
         if (remaining <= 0)
         {
            if (status)
               *status = CLI_CODEX_READ_DEADLINE;
            return NULL;
         }
         ms = remaining < timeout_ms ? (long)remaining : timeout_ms;
      }

      struct pollfd descriptors[] = {{c->out_fd, POLLIN, 0}, {c->err_fd, POLLIN, 0}};
      /* Poll cancellation while preserving the caller's complete line budget. */
      if (ms > 100 && aimee_ir_session_get_sink(NULL))
         ms = 100;
      int sel = ms < 0 ? -1 : poll(descriptors, 2, (int)ms);
      if (ms < 0)
         errno = EINVAL;
      if (sel > 0 && ((descriptors[0].revents | descriptors[1].revents) & POLLNVAL))
      {
         errno = EBADF;
         sel = -1;
      }
      if (sel == 0)
      {
         if (aimee_ir_session_get_sink(NULL) && (!line_deadline || util_now_ms() < line_deadline))
            continue;
         if (status)
            *status = CLI_CODEX_READ_LINE_TIMEOUT;
         return NULL;
      }
      if (sel < 0)
      {
         if (errno == EINTR)
            continue;
         if (status)
            *status = CLI_CODEX_READ_POLL_ERR;
         return NULL;
      }
      if (c->err_fd >= 0 && (descriptors[1].revents & (POLLIN | POLLHUP | POLLERR)))
         cli_codex_drain_stderr(c);
      if (!(descriptors[0].revents & (POLLIN | POLLHUP | POLLERR)))
         continue;

      /* Grow buffer if needed. */
      size_t read_room = c->buf_len < CLI_CODEX_LINE_MAX ? CLI_CODEX_LINE_MAX - c->buf_len : 0;
      if (read_room == 0)
      {
         if (status)
            *status = CLI_CODEX_READ_LINE_TOO_LARGE;
         return NULL;
      }
      if (read_room > 4096)
         read_room = 4096;
      if (cli_codex_reserve_stdout(c, c->buf_len + read_room, status) != 0)
         return NULL;
      ssize_t r = read(c->out_fd, c->buf + c->buf_len, c->buf_cap - c->buf_len);
      if (r == 0)
      {
         if (status)
            *status = CLI_CODEX_READ_EOF;
         return NULL;
      }
      if (r < 0)
      {
         if (errno == EINTR)
            continue;
         if (status)
            *status = CLI_CODEX_READ_READ_ERR;
         return NULL;
      }
      c->buf_len += (size_t)r;
   }
}

/* The per-line timeout is a quiet-period poll, not the turn ceiling. Codex can
 * be healthy while staying silent for several minutes during delegated work, so
 * keep waiting until the idle deadline or a concrete pipe/process failure. */
static char *cli_codex_read_line_until_deadline(cli_codex_t *c, long long deadline_ms,
                                                cli_codex_read_status_t *status)
{
   for (;;)
   {
      cli_codex_read_status_t rstatus = CLI_CODEX_READ_OK;
      char *line = cli_codex_read_line(c, CLI_CODEX_QUIET_POLL_MS, deadline_ms, &rstatus);
      if (line || rstatus != CLI_CODEX_READ_LINE_TIMEOUT)
      {
         if (status)
            *status = rstatus;
         return line;
      }
      cli_codex_drain_stderr(c);
      if (deadline_ms > 0 && util_now_ms() >= deadline_ms)
      {
         if (status)
            *status = CLI_CODEX_READ_DEADLINE;
         return NULL;
      }
   }
}

static const char *cli_codex_read_status_str(cli_codex_read_status_t s)
{
   switch (s)
   {
   case CLI_CODEX_READ_OK:
      return "ok";
   case CLI_CODEX_READ_DEADLINE:
      return "idle-timeout-expired";
   case CLI_CODEX_READ_LINE_TIMEOUT:
      return "line-read-timeout";
   case CLI_CODEX_READ_POLL_ERR:
      return "select-error";
   case CLI_CODEX_READ_EOF:
      return "codex-exited-eof";
   case CLI_CODEX_READ_READ_ERR:
      return "read-error";
   case CLI_CODEX_READ_LINE_TOO_LARGE:
      return "line-too-large";
   case CLI_CODEX_READ_OOM:
      return "out-of-memory";
   case CLI_CODEX_READ_FRAME_ERR:
      return "invalid-json-rpc-frame";
   }
   return "unknown";
}

/* --- Pure parse helpers (declared in cli_codex.h, tested separately) --- */

static cJSON *cli_codex_text_input_array(const char *text)
{
   cJSON *input = cJSON_CreateArray();
   if (!input)
      return NULL;
   cJSON *txt = cJSON_CreateObject();
   if (!txt)
   {
      cJSON_Delete(input);
      return NULL;
   }
   cJSON_AddStringToObject(txt, "type", "text");
   cli_codex_add_utf8_string(txt, "text", text);
   cJSON *text_elements = cJSON_CreateArray();
   if (!text_elements)
   {
      cJSON_Delete(input);
      cJSON_Delete(txt);
      return NULL;
   }
   cJSON_AddItemToObject(txt, "text_elements", text_elements);
   cJSON_AddItemToArray(input, txt);
   return input;
}

static char *cli_codex_copy_error_text(const char *raw)
{
   if (!raw || !raw[0])
      return NULL;

   cJSON *nested = cJSON_Parse(raw);
   if (nested)
   {
      cJSON *err = cJSON_GetObjectItemCaseSensitive(nested, "error");
      cJSON *msg = cJSON_GetObjectItemCaseSensitive(err, "message");
      if (!cJSON_IsString(msg))
         msg = cJSON_GetObjectItemCaseSensitive(nested, "message");
      if (cJSON_IsString(msg) && msg->valuestring && msg->valuestring[0])
      {
         char *out = strdup(msg->valuestring);
         cJSON_Delete(nested);
         return out;
      }
      cJSON_Delete(nested);
   }

   return strdup(raw);
}

int cli_codex_parse_error_message(const char *line, char **out_text)
{
   if (!line || !out_text)
      return 0;
   cJSON *obj = cJSON_Parse(line);
   if (!obj)
      return 0;

   int rc = 0;
   cJSON *msg = NULL;
   cJSON *method = cJSON_GetObjectItemCaseSensitive(obj, "method");
   if (cJSON_IsString(method) && strcmp(method->valuestring, "error") == 0)
   {
      cJSON *params = cJSON_GetObjectItemCaseSensitive(obj, "params");
      cJSON *error = cJSON_GetObjectItemCaseSensitive(params, "error");
      msg = cJSON_GetObjectItemCaseSensitive(error, "message");
   }
   else if (cJSON_IsString(method) && strcmp(method->valuestring, "turn/completed") == 0)
   {
      cJSON *params = cJSON_GetObjectItemCaseSensitive(obj, "params");
      cJSON *turn = cJSON_GetObjectItemCaseSensitive(params, "turn");
      cJSON *error = cJSON_GetObjectItemCaseSensitive(turn, "error");
      msg = cJSON_GetObjectItemCaseSensitive(error, "message");
   }
   else
   {
      cJSON *error = cJSON_GetObjectItemCaseSensitive(obj, "error");
      msg = cJSON_GetObjectItemCaseSensitive(error, "message");
   }

   if (cJSON_IsString(msg) && msg->valuestring && msg->valuestring[0])
   {
      *out_text = cli_codex_copy_error_text(msg->valuestring);
      rc = (*out_text != NULL);
   }

   cJSON_Delete(obj);
   return rc;
}

int cli_codex_parse_method(const char *line, char *out, size_t outsz)
{
   if (!line || !out || outsz == 0)
      return 0;
   cJSON *obj = cJSON_Parse(line);
   if (!obj)
      return 0;
   int rc = 0;
   cJSON *method = cJSON_GetObjectItemCaseSensitive(obj, "method");
   if (cJSON_IsString(method) && method->valuestring)
   {
      snprintf(out, outsz, "%s", method->valuestring);
      rc = 1;
   }
   cJSON_Delete(obj);
   return rc;
}

static int cli_codex_send_jsonrpc_result(cli_codex_t *c, cJSON *id, cJSON *result)
{
   cJSON *resp = cJSON_CreateObject();
   cJSON *id_copy = cJSON_Duplicate(id, 1);
   cJSON *body = result ? result : cJSON_CreateObject();
   if (!resp || !id_copy || !body)
   {
      cJSON_Delete(resp);
      cJSON_Delete(id_copy);
      cJSON_Delete(body);
      return -1;
   }
   cJSON_AddStringToObject(resp, "jsonrpc", "2.0");
   cJSON_AddItemToObject(resp, "id", id_copy);
   cJSON_AddItemToObject(resp, "result", body);
   int rc = cli_codex_send(c, resp);
   cJSON_Delete(resp);
   return rc;
}

static int cli_codex_send_jsonrpc_error(cli_codex_t *c, cJSON *id, int code, const char *message)
{
   cJSON *resp = cJSON_CreateObject();
   cJSON *id_copy = cJSON_Duplicate(id, 1);
   cJSON *err = cJSON_CreateObject();
   if (!resp || !id_copy || !err)
   {
      cJSON_Delete(resp);
      cJSON_Delete(id_copy);
      cJSON_Delete(err);
      return -1;
   }
   cJSON_AddStringToObject(resp, "jsonrpc", "2.0");
   cJSON_AddItemToObject(resp, "id", id_copy);
   cJSON_AddNumberToObject(err, "code", code);
   cJSON_AddStringToObject(err, "message", message ? message : "request unavailable");
   cJSON_AddItemToObject(resp, "error", err);
   int rc = cli_codex_send(c, resp);
   cJSON_Delete(resp);
   return rc;
}

static const char *codex_string(const cJSON *object, const char *key)
{
   return cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(object, key));
}
static int codex_bind_dynamic_tools(cli_codex_t *c, const aimee_request_t *catalog,
                                    int (*call)(const aimee_block_t *, aimee_block_t *, void *),
                                    void *context)
{
   if (!catalog && !call)
      return 0;
   if (!catalog || !call)
      return -1;
   c->dynamic_tools = cli_codex_dynamic_tools(catalog);
   c->dynamic_seen = cJSON_CreateObject();
   if (!c->dynamic_tools || !c->dynamic_seen)
      return -1;
   c->tool_catalog = catalog;
   c->tool_call = call;
   c->tool_context = context;
   return 0;
}
static int codex_handle_dynamic_call(cli_codex_t *c, const cJSON *frame, cJSON *rpcid)
{
   const cJSON *params = cJSON_GetObjectItemCaseSensitive(frame, "params");
   const char *thread = codex_string(params, "threadId"), *turn = codex_string(params, "turnId");
   aimee_block_t call = {0}, result = {0};
   cJSON *reply = NULL;
   int valid = c->tool_call && c->dynamic_thread && c->dynamic_turn && thread && turn &&
               !strcmp(thread, c->dynamic_thread) && !strcmp(turn, c->dynamic_turn) &&
               aimee_ir_json_unique(frame) && (cJSON_IsString(rpcid) || cJSON_IsNumber(rpcid)) &&
               !cli_codex_dynamic_call(params, c->tool_catalog, &call);
   /* Provider call IDs can repeat in later model responses. Only a repeated
    * JSON-RPC request is a transport replay; original-source claims belong to
    * the host callback. Keep string and numeric RPC identities distinct. */
   cJSON *identity = valid ? aimee_ir_json_canonical_copy(rpcid) : NULL;
   char *key = identity ? cJSON_PrintUnformatted(identity) : NULL;
   cJSON_Delete(identity);
   if (valid)
      valid = key && !cJSON_GetObjectItemCaseSensitive(c->dynamic_seen, key) &&
              cJSON_AddTrueToObject(c->dynamic_seen, key);
   free(key);
   if (valid)
      valid = !c->tool_call(&call, &result, c->tool_context) && result.tool_id &&
              !strcmp(result.tool_id, call.tool_id) && (reply = cli_codex_dynamic_result(&result));
   int rc =
       valid ? cli_codex_send_jsonrpc_result(c, rpcid, reply)
             : cli_codex_send_jsonrpc_error(c, rpcid, -32602, "Host tool source or result refused");
   aimee_block_free_contents(&call);
   aimee_block_free_contents(&result);
   if (!valid || rc)
   {
      c->dynamic_failed = 1;
      aimee_ir_session_fail_capture();
   }
   return 1;
}

/* Native control replies come only from the current canonical operator scope. */
static int cli_codex_handle_server_request(cli_codex_t *c, const char *line)
{
   if (!line)
      return 0;
   cJSON *obj = cJSON_Parse(line);
   if (!obj)
      return 0;
   cJSON *id = cJSON_GetObjectItemCaseSensitive(obj, "id");
   cJSON *method = cJSON_GetObjectItemCaseSensitive(obj, "method");
   if (!id || cJSON_IsNull(id) || !cJSON_IsString(method) || !method->valuestring)
   {
      cJSON_Delete(obj);
      return 0;
   }

   const char *m = method->valuestring;
   if (!strcmp(m, "item/tool/call"))
   {
      int handled = codex_handle_dynamic_call(c, obj, id);
      cJSON_Delete(obj);
      return handled;
   }
   server_control_scope_t *scope = server_control_scope_current();
   int supported = strstr(m, "requestApproval") || !strcmp(m, "item/tool/requestUserInput") ||
                   !strcmp(m, "mcpServer/elicitation/request") ||
                   !strcmp(m, "execCommandApproval") || !strcmp(m, "applyPatchApproval");
   if (scope && supported)
   {
      char *key = cJSON_PrintUnformatted(id), *native_id = NULL, *kind = NULL;
      cJSON *request = NULL, *reply = NULL;
      int taken = 0;
      while (key && !agent_request_cancelled() && !aimee_ir_session_capture_failed())
      {
         taken = server_control_take(scope, key, &native_id, &kind, &request, &reply);
         if (taken != 0)
            break;
         struct timespec pause = {0, 100000000L};
         nanosleep(&pause, NULL);
      }
      if (taken == 1)
      {
         cJSON *result =
             aimee_codex_control_reply(m, cJSON_GetObjectItemCaseSensitive(obj, "params"), reply);
         if (result && aimee_harness_control_decision_ir(&c->ir, native_id, reply))
         {
            cJSON_Delete(result);
            result = NULL;
            aimee_ir_session_fail_capture();
         }
         int rc = aimee_ir_session_capture_failed() ? -1
                  : result
                      ? cli_codex_send_jsonrpc_result(c, id, result)
                      : cli_codex_send_jsonrpc_error(
                            c, id, -32602, "Operator reply could not satisfy this control request");
         if (rc != 0)
            aimee_ir_session_fail_capture();
      }
      else if (!agent_request_cancelled())
         aimee_ir_session_fail_capture();
      free(key);
      free(native_id);
      free(kind);
      cJSON_Delete(request);
      cJSON_Delete(reply);
      cJSON_Delete(obj);
      return 1;
   }

   /* No operator scope means no authority to answer controls. A managed
    * SDK may not fall back to autonomous or ambient permission decisions. */
   if (supported)
   {
      (void)cli_codex_send_jsonrpc_error(c, id, -32000, "Host control scope unavailable");
      aimee_ir_session_fail_capture();
   }
   else
      (void)cli_codex_send_jsonrpc_error(c, id, -32601, "Unsupported Codex request");

   cJSON_Delete(obj);
   return 1;
}

/* Send a request, drain notifications until we see the response with the
 * matching id. Returns the response object (caller cJSON_Delete) or NULL. */
static long long cli_codex_idle_deadline_ms(int idle_timeout_ms)
{
   if (idle_timeout_ms < 0)
      return 0;
   return util_now_ms() + idle_timeout_ms;
}

static int cli_codex_agent_idle_timeout_ms(const agent_t *agent)
{
   if (agent->cli_idle_timeout_ms > 0)
      return agent->cli_idle_timeout_ms;
   if (agent->timeout_ms > 0 && agent->timeout_ms != AGENT_DEFAULT_TIMEOUT_MS)
      return agent->timeout_ms;
   return CLI_CODEX_DEFAULT_IDLE_TIMEOUT_MS;
}

static cJSON *cli_codex_request(cli_codex_t *c, const char *method, cJSON *params, int id,
                                long long *deadline_ms, int idle_timeout_ms)
{
   if (c->host_launch && (!strcmp(method, "thread/start") || !strcmp(method, "thread/resume")) &&
       !cJSON_AddStringToObject(params, "cwd", c->host_launch->cwd))
   {
      cJSON_Delete(params);
      return NULL;
   }
   if (c->dynamic_tools)
   {
      if (!strcmp(method, "initialize"))
      {
         cJSON *capabilities = cJSON_AddObjectToObject(params, "capabilities");
         if (!capabilities || !cJSON_AddTrueToObject(capabilities, "experimentalApi"))
         {
            cJSON_Delete(params);
            return NULL;
         }
      }
      if (!strcmp(method, "thread/start"))
      {
         cJSON *tools = cJSON_Duplicate(c->dynamic_tools, 1);
         if (!tools || !cJSON_AddItemToObject(params, "dynamicTools", tools))
         {
            cJSON_Delete(tools);
            cJSON_Delete(params);
            return NULL;
         }
      }
   }
   cJSON *req = cJSON_CreateObject();
   cJSON_AddStringToObject(req, "jsonrpc", "2.0");
   cJSON_AddNumberToObject(req, "id", id);
   cJSON_AddStringToObject(req, "method", method);
   cJSON_AddItemToObject(req, "params", params ? params : cJSON_CreateObject());
   int rc = cli_codex_send(c, req);
   cJSON_Delete(req);
   c->last_read_status = CLI_CODEX_READ_OK;
   if (rc != 0)
   {
      c->last_read_status = CLI_CODEX_READ_READ_ERR;
      return NULL;
   }

   /* Per-line quiet polls keep stderr drained while waiting for acks. Any
    * app-server frame is progress, so refresh the idle deadline after each
    * line instead of enforcing a fixed wall-clock ceiling. */
   for (;;)
   {
      cli_codex_read_status_t rstatus = CLI_CODEX_READ_OK;
      char *line = cli_codex_read_line_until_deadline(c, *deadline_ms, &rstatus);
      if (!line)
      {
         c->last_read_status = rstatus;
         return NULL;
      }
      c->last_read_status = CLI_CODEX_READ_OK;
      *deadline_ms = cli_codex_idle_deadline_ms(idle_timeout_ms);
      if (cli_codex_handle_server_request(c, line))
         continue;
      cJSON *obj = cJSON_Parse(line);
      if (!obj)
         continue;
      cJSON *jid = cJSON_GetObjectItemCaseSensitive(obj, "id");
      if (cJSON_IsNumber(jid) && jid->valueint == id)
      {
         if (c->dynamic_tools && !cJSON_GetObjectItemCaseSensitive(obj, "error"))
         {
            const cJSON *result = cJSON_GetObjectItemCaseSensitive(obj, "result");
            const char *identity = NULL;
            char **destination = NULL;
            if (!strcmp(method, "thread/start") || !strcmp(method, "thread/resume"))
            {
               identity = codex_string(cJSON_GetObjectItemCaseSensitive(result, "thread"), "id");
               destination = &c->dynamic_thread;
            }
            else if (!strcmp(method, "turn/start"))
            {
               identity = codex_string(cJSON_GetObjectItemCaseSensitive(result, "turn"), "id");
               destination = &c->dynamic_turn;
            }
            if (destination)
            {
               char *owned = identity && identity[0] ? strdup(identity) : NULL;
               if (!owned)
               {
                  cJSON_Delete(obj);
                  c->dynamic_failed = 1;
                  return NULL;
               }
               free(*destination);
               *destination = owned;
            }
         }
         /* Either a "result" or "error" envelope; return for caller to inspect. */
         return obj;
      }
      /* Notification or response for a different id; discard. */
      cJSON_Delete(obj);
   }
}

static int cli_codex_response_thread_id(cJSON *resp, char *out, size_t outsz)
{
   if (!resp || !out || outsz == 0)
      return 0;
   cJSON *result = cJSON_GetObjectItemCaseSensitive(resp, "result");
   cJSON *thread = cJSON_GetObjectItemCaseSensitive(result, "thread");
   cJSON *tid = cJSON_GetObjectItemCaseSensitive(thread, "id");
   if (!cJSON_IsString(tid) || !tid->valuestring[0])
      return 0;
   snprintf(out, outsz, "%s", tid->valuestring);
   return 1;
}

static void cli_codex_set_stall_error(cli_codex_t *c, cli_codex_read_status_t rstatus, char *out,
                                      size_t outsz)
{
   if (!out || outsz == 0)
      return;

   cli_codex_collect_exit_diagnostics(c);
   char err_snippet[400] = {0};
   if (c->err_tail_len > 0)
   {
      size_t snip_len =
          c->err_tail_len < sizeof(err_snippet) - 1 ? c->err_tail_len : sizeof(err_snippet) - 1;
      memcpy(err_snippet, c->err_tail + (c->err_tail_len - snip_len), snip_len);
      err_snippet[snip_len] = '\0';
   }

   const char *reason = cli_codex_read_status_str(rstatus);
   const char *kind = rstatus == CLI_CODEX_READ_EOF ? "codex exited" : "codex stalled";
   char exit_status[64] = {0};
   cli_codex_exit_status_desc(c, exit_status, sizeof(exit_status));
   if (err_snippet[0])
      snprintf(out, outsz, "%s (%s%s); full stderr at %s; tail: %s", kind, reason, exit_status,
               c->err_log_path[0] ? c->err_log_path : "(no log)", err_snippet);
   else
      snprintf(out, outsz, "%s (%s%s); full stderr at %s; (no stderr captured)", kind, reason,
               exit_status, c->err_log_path[0] ? c->err_log_path : "(no log)");
}

static void cli_codex_set_context_error(cli_codex_t *c, const char *context,
                                        cli_codex_read_status_t rstatus, char *out, size_t outsz)
{
   if (!out || outsz == 0)
      return;
   if (rstatus == CLI_CODEX_READ_OK)
   {
      snprintf(out, outsz, "%s", context ? context : "codex request failed");
      return;
   }

   char detail[512] = {0};
   cli_codex_set_stall_error(c, rstatus, detail, sizeof(detail));
   if (context && context[0])
      snprintf(out, outsz, "%s: %s", context, detail);
   else
      snprintf(out, outsz, "%s", detail);
}

/* Stable SDK boundary: failed adoption refuses before any model turn starts. */
static int cli_codex_adopt_history(cli_codex_t *c, const char *thread_id,
                                   const aimee_request_t *history, int *next_id,
                                   long long *deadline_ms, int idle_timeout_ms)
{
   if (!history)
      return 0;
   cJSON *items = aimee_codex_history_items(history);
   if (!items)
      return -1;
   if (!cJSON_GetArraySize(items))
   {
      cJSON_Delete(items);
      return 0;
   }
   cJSON *params = cJSON_CreateObject();
   if (!params || !cJSON_AddStringToObject(params, "threadId", thread_id) ||
       !cJSON_AddItemToObject(params, "items", items))
   {
      cJSON_Delete(items);
      cJSON_Delete(params);
      return -1;
   }
   cJSON *reply = cli_codex_request(c, "thread/inject_items", params, (*next_id)++, deadline_ms,
                                    idle_timeout_ms);
   int accepted = reply && cJSON_IsObject(cJSON_GetObjectItemCaseSensitive(reply, "result")) &&
                  !cJSON_GetObjectItemCaseSensitive(reply, "error");
   cJSON_Delete(reply);
   return accepted ? 0 : -1;
}

static int cli_codex_host_ready(cli_codex_t *c, cli_codex_host_replay_t *replay, int *next_id,
                                long long *deadline_ms, int idle_timeout_ms)
{
   const cli_codex_host_launch_t *host = c->host_launch;
   if (!host->home || !host->cwd || !host->build_digest || strlen(host->build_digest) != 64 ||
       !host->gateway_url || !host->gateway_token || !host->expected_config || !host->ready)
      return -1;
   cJSON *params = cJSON_CreateObject();
   if (!params || !cJSON_AddStringToObject(params, "cwd", host->cwd) ||
       !cJSON_AddTrueToObject(params, "includeLayers"))
   {
      cJSON_Delete(params);
      return -1;
   }
   cJSON *reply =
       cli_codex_request(c, "config/read", params, (*next_id)++, deadline_ms, idle_timeout_ms);
   char digest[65];
   int rc = cli_codex_profile_verify(host, reply, digest);
   cJSON_Delete(reply);
   return rc ? -1 : host->ready(digest, replay, host->context);
}

static int codex_execute_cfg(const provider_cli_cfg_t *cfg, agent_result_t *out)
{
   if (!out)
      return -1;
   memset(out, 0, sizeof(*out));
   if (!cfg)
   {
      snprintf(out->error, sizeof(out->error), "missing Codex agent configuration");
      return -1;
   }
   const agent_t *agent = cfg->agent;
   const char *user_prompt = cfg->user_prompt;
   if (!agent)
   {
      snprintf(out->error, sizeof(out->error), "missing agent");
      return -1;
   }
   const cli_codex_host_launch_t *host = cfg->codex_host;
   if (!host || host->executable_fd < 0 || host->cwd_fd < 0 ||
       fcntl(host->executable_fd, F_GETFD) < 0 || fcntl(host->cwd_fd, F_GETFD) < 0 ||
       !host->environment || !host->home || !host->cwd || !host->build_digest ||
       strlen(host->build_digest) != 64 || !host->gateway_url || !host->gateway_token ||
       !host->expected_config || !host->ready)
   {
      snprintf(out->error, sizeof out->error, "Codex requires an owned native launch");
      return AGENT_RC_CONTEXT_REFUSED;
   }
   if (!server_control_scope_current())
   {
      snprintf(out->error, sizeof out->error, "Codex requires an owned control scope");
      return AGENT_RC_CONTEXT_REFUSED;
   }
   snprintf(out->agent_name, MAX_AGENT_NAME, "%s", agent->name);

   if (!user_prompt || !user_prompt[0])
   {
      snprintf(out->error, sizeof(out->error), "empty prompt");
      return -1;
   }

   long long start_ms = util_now_ms();
   int idle_timeout_ms = cli_codex_agent_idle_timeout_ms(agent);
   long long deadline_ms = cli_codex_idle_deadline_ms(idle_timeout_ms);

   cli_codex_t c = {0};
   c.in_fd = -1;
   c.out_fd = -1;
   c.err_fd = -1;
   c.err_log_fd = -1;
   c.host_launch = cfg->codex_host;
   c.progress = cfg->native_progress;
   c.progress_context = cfg->native_progress_context;
   if (codex_bind_dynamic_tools(&c, cfg->tool_catalog, cfg->tool_call, cfg->tool_context))
   {
      if (out)
         snprintf(out->error, sizeof(out->error), "invalid typed host tool bridge");
      cli_codex_close(&c);
      return -1;
   }
   if (cli_codex_spawn(&c) != 0)
   {
      snprintf(out->error, sizeof out->error, "failed to spawn owned Codex app-server");
      return -1;
   }

   int next_id = 1;
   /* 1. initialize */
   {
      cJSON *params = cJSON_CreateObject();
      cJSON *ci = cJSON_CreateObject();
      cJSON_AddStringToObject(ci, "name", "aimee");
      cJSON_AddStringToObject(ci, "version", AIMEE_VERSION);
      cJSON_AddItemToObject(params, "clientInfo", ci);
      cJSON *resp =
          cli_codex_request(&c, "initialize", params, next_id++, &deadline_ms, idle_timeout_ms);
      if (!resp || !cJSON_GetObjectItemCaseSensitive(resp, "result"))
      {
         cJSON_Delete(resp);
         cli_codex_set_context_error(&c, "codex initialize failed", c.last_read_status, out->error,
                                     sizeof(out->error));
         cli_codex_close(&c);
         return -1;
      }
      cJSON_Delete(resp);
   }

   cli_codex_host_replay_t replay = {0};
   if (cli_codex_host_ready(&c, &replay, &next_id, &deadline_ms, idle_timeout_ms))
   {
      snprintf(out->error, sizeof out->error, "native SDK configuration could not be verified");
      aimee_ir_session_fail_capture();
      cli_codex_close(&c);
      return AGENT_RC_CAPTURE_FAILED;
   }
   /* Resolve the native handle within this canonical execution scope. */
   char thread_id[128] = {0};
   {
      int resume = replay.resume_id && replay.resume_id[0];
      cJSON *params = cJSON_CreateObject();
      if (resume)
         cJSON_AddStringToObject(params, "threadId", replay.resume_id);
      cJSON *resp = cli_codex_request(&c, resume ? "thread/resume" : "thread/start", params,
                                      next_id++, &deadline_ms, idle_timeout_ms);
      int got_id = resp && cli_codex_response_thread_id(resp, thread_id, sizeof thread_id);
      if (!got_id && resume && resp && cJSON_GetObjectItemCaseSensitive(resp, "result"))
      {
         snprintf(thread_id, sizeof thread_id, "%s", replay.resume_id);
         got_id = 1;
      }
      cJSON_Delete(resp);
      if (!got_id || (cfg->bind_session && cfg->bind_session(thread_id, cfg->bind_context) != 0))
      {
         cli_codex_close(&c);
         snprintf(out->error, sizeof(out->error), "codex native session could not be bound");
         aimee_ir_session_fail_capture();
         return AGENT_RC_CAPTURE_FAILED;
      }
   }

   if (cli_codex_adopt_history(&c, thread_id, replay.history, &next_id, &deadline_ms,
                               idle_timeout_ms) != 0)
   {
      snprintf(out->error, sizeof(out->error), "codex canonical history adoption failed");
      cli_codex_close(&c);
      return -1;
   }

   /* 3. turn/start */
   {
      cJSON *params = cJSON_CreateObject();
      cJSON_AddStringToObject(params, "threadId", thread_id);
      if (agent->model[0])
         cJSON_AddStringToObject(params, "model", agent->model);
      cJSON *input = cli_codex_text_input_array(user_prompt);
      cJSON_AddItemToObject(params, "input", input);
      cJSON *resp =
          cli_codex_request(&c, "turn/start", params, next_id++, &deadline_ms, idle_timeout_ms);
      if (!resp || !cJSON_GetObjectItemCaseSensitive(resp, "result"))
      {
         cJSON_Delete(resp);
         cli_codex_set_context_error(&c, "codex turn/start failed", c.last_read_status, out->error,
                                     sizeof(out->error));
         cli_codex_close(&c);
         return -1;
      }
      cJSON_Delete(resp);
   }

   /* 4. Drain transport notifications until turn/completed. The host owns
    * canonical model usage and committed tool outcomes. */
   for (;;)
   {
      /* The quiet poll keeps stderr drained while Codex is silent. Any
       * app-server frame below refreshes deadline_ms, so this is an idle
       * ceiling rather than a fixed wall-clock turn limit. */
      cli_codex_read_status_t rstatus = CLI_CODEX_READ_OK;
      char *line = cli_codex_read_line_until_deadline(&c, deadline_ms, &rstatus);
      if (!line)
      {
         cli_codex_set_stall_error(&c, rstatus, out->error, sizeof(out->error));
         cli_codex_close(&c);
         return -1;
      }
      deadline_ms = cli_codex_idle_deadline_ms(idle_timeout_ms);
      /* Periodic stderr drain so the tail buffer stays current
       * regardless of whether we end up bailing on a stall. */
      cli_codex_drain_stderr(&c);
      if (cli_codex_handle_server_request(&c, line))
         continue;
      char *err_text = NULL;
      if (cli_codex_parse_error_message(line, &err_text))
      {
         if (!out->error[0])
            snprintf(out->error, sizeof(out->error), "%s", err_text);
         free(err_text);
      }

      char method_buf[64] = {0};
      if (cli_codex_parse_method(line, method_buf, sizeof(method_buf)) &&
          strcmp(method_buf, "turn/completed") == 0)
         break;
   }

   cli_codex_close(&c);
   out->latency_ms = (int)(util_now_ms() - start_ms);
   out->turns = 1;
   out->success = 1;
   return 0;
}

static int codex_adapter_execute(const provider_cli_cfg_t *cfg, agent_result_t *out)
{
   return cfg && cfg->agent ? codex_execute_cfg(cfg, out) : -1;
}

const provider_cli_adapter_t codex_provider_cli_adapter = {
    .cli_kind = "codex",
    .display_name = "Codex CLI",
    /* Codex CLI runs the gpt-5-codex family (272k context, tool use). Declaring
     * the real window lets capability routing keep codex as a delegate for roles
     * with a min-context floor; 0 here previously dropped it from every such
     * fleet ("no configured model supports required capabilities"). */
    .caps = {.max_context_tokens = 272000,
             .supports_tool_use = 1,
             .proto_stability = PROVIDER_CLI_PROTO_STABLE,
             .write_confidence = 0.95f},
    .execute = codex_adapter_execute,
};
