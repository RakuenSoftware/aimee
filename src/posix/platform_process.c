/* platform_process.c: process spawning, exec, and signal handling (POSIX) */
#include "platform_process.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

pid_t platform_spawn_daemon(const char *const argv[])
{
   /* Double-fork: the intermediate child exits immediately so the caller's
    * waitpid() returns without delay.  The actual daemon is reparented under
    * init (PID 1) which reaps it when it exits — no zombie left in the caller
    * regardless of whether it ever calls waitpid(). */
   pid_t pid = fork();
   if (pid < 0)
      return -1;

   if (pid > 0)
   {
      /* Parent: reap the short-lived intermediate child immediately. */
      waitpid(pid, NULL, 0);
      return pid;
   }

   /* Intermediate child: spawn the actual daemon, then exit. */
   pid_t daemon_pid = fork();
   if (daemon_pid < 0)
      _exit(1);
   if (daemon_pid > 0)
      _exit(0); /* intermediate exits; daemon is reparented to init */

   /* Actual daemon */
   setsid();

   int devnull = open("/dev/null", O_RDWR);
   if (devnull >= 0)
   {
      dup2(devnull, STDIN_FILENO);
      dup2(devnull, STDOUT_FILENO);
      dup2(devnull, STDERR_FILENO);
      if (devnull > STDERR_FILENO)
         close(devnull);
   }

   execvp(argv[0], (char *const *)argv);
   _exit(127);
}

int platform_exec_capture_cancellable(const char *cmd, char **out, size_t *out_len, int timeout_ms,
                                      platform_process_cancel_fn_t cancel_fn, void *cancel_ctx)
{
   int pipefd[2];
   if (pipe(pipefd) < 0)
      return -1;

   pid_t pid = fork();
   if (pid < 0)
   {
      close(pipefd[0]);
      close(pipefd[1]);
      return -1;
   }

   if (pid == 0)
   {
      /* Child */
      setpgid(0, 0);
      close(pipefd[0]);
      dup2(pipefd[1], STDOUT_FILENO);
      dup2(pipefd[1], STDERR_FILENO);
      close(pipefd[1]);
      execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
      _exit(127);
   }

   /* Parent: read output */
   setpgid(pid, pid);
   close(pipefd[1]);

   /* Set pipe to non-blocking if we need to poll timeout or cancellation. */
   if (timeout_ms > 0 || cancel_fn)
   {
      int flags = fcntl(pipefd[0], F_GETFL, 0);
      if (flags >= 0)
         fcntl(pipefd[0], F_SETFL, flags | O_NONBLOCK);
   }

   size_t cap = 4096;
   size_t len = 0;
   char *buf = malloc(cap);
   if (!buf)
   {
      close(pipefd[0]);
      kill(pid, SIGKILL);
      waitpid(pid, NULL, 0);
      return -1;
   }

   int timed_out = 0;
   int elapsed_ms = 0;
   const int poll_interval_ms = 10;
   const struct timespec poll_ts = {0, poll_interval_ms * 1000000L};

   for (;;)
   {
      if (len + 1024 > cap)
      {
         cap *= 2;
         char *tmp = realloc(buf, cap);
         if (!tmp)
            break;
         buf = tmp;
      }
      ssize_t n = read(pipefd[0], buf + len, cap - len - 1);
      if (n > 0)
      {
         len += (size_t)n;
         continue;
      }
      if (n == 0)
         break; /* EOF */
      /* n < 0 */
      if (errno == EAGAIN || errno == EWOULDBLOCK)
      {
         /* Check if child has exited */
         int status = 0;
         pid_t w = waitpid(pid, &status, WNOHANG);
         if (w > 0)
         {
            /* Child exited; drain remaining output */
            for (;;)
            {
               if (len + 1024 > cap)
               {
                  cap *= 2;
                  char *tmp = realloc(buf, cap);
                  if (!tmp)
                     break;
                  buf = tmp;
               }
               ssize_t r = read(pipefd[0], buf + len, cap - len - 1);
               if (r <= 0)
                  break;
               len += (size_t)r;
            }
            close(pipefd[0]);
            buf[len] = '\0';
            *out = buf;
            if (out_len)
               *out_len = len;
            return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
         }
         /* Check timeout */
         if (timeout_ms > 0 && elapsed_ms >= timeout_ms)
         {
            timed_out = 1;
            break;
         }
         if (cancel_fn && cancel_fn(cancel_ctx))
            break;
         nanosleep(&poll_ts, NULL);
         elapsed_ms += poll_interval_ms;
         continue;
      }
      /* Real read error */
      break;
   }
   close(pipefd[0]);

   if (timed_out)
   {
      kill(-pid, SIGTERM);
      /* Give child a brief grace period to exit */
      const struct timespec grace = {0, 100 * 1000000L}; /* 100ms */
      nanosleep(&grace, NULL);
      if (waitpid(pid, NULL, WNOHANG) == 0)
      {
         kill(-pid, SIGKILL);
         waitpid(pid, NULL, 0);
      }
      fprintf(stderr, "aimee: child process timed out after %d ms\n", timeout_ms);
      buf[len] = '\0';
      *out = buf;
      if (out_len)
         *out_len = len;
      return -1;
   }

   if (cancel_fn && cancel_fn(cancel_ctx))
   {
      kill(-pid, SIGTERM);
      const struct timespec grace = {0, 100 * 1000000L}; /* 100ms */
      nanosleep(&grace, NULL);
      if (waitpid(pid, NULL, WNOHANG) == 0)
      {
         kill(-pid, SIGKILL);
         waitpid(pid, NULL, 0);
      }
      buf[len] = '\0';
      *out = buf;
      if (out_len)
         *out_len = len;
      return -1;
   }

   /* Reap child (if not already) */
   int status = 0;
   waitpid(pid, &status, 0);
   buf[len] = '\0';
   *out = buf;
   if (out_len)
      *out_len = len;
   return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

int platform_exec_capture(const char *cmd, char **out, size_t *out_len, int timeout_ms)
{
   return platform_exec_capture_cancellable(cmd, out, out_len, timeout_ms, NULL, NULL);
}

/* Bounded subprocess execution. See platform_process.h for the contract.
 *
 * The previous implementation had three independent live defects, all reachable
 * from request-serving paths:
 *
 *  1. DEADLOCK. It wrote ALL input, closed stdin, and only then read stdout. When
 *     both directions exceed pipe capacity (~64 KiB) the child blocks writing to
 *     a full stdout pipe while the parent blocks writing to a full stdin pipe.
 *     Reproduced: 1 MiB in, child emitting 1 MiB first, no return. This is fixed
 *     by servicing both pipes concurrently under poll(), never write-all-then-read.
 *  2. NO BOUND. waitpid(pid, NULL, 0) with no timeout: a child that never exits
 *     hung the caller forever. On the serial kb_http accept loop that stops the
 *     whole listener.
 *  3. UNBOUNDED OUTPUT. The reader doubled its buffer with no ceiling, so a fast
 *     emitting child grew the parent's heap until the OOM killer intervened.
 *
 * One monotonic deadline spans write, read and wait, so the total is bounded
 * rather than each step being bounded separately. The child gets its own process
 * group (setsid) so termination reaches grandchildren that would otherwise keep
 * the pipe open; termination escalates SIGTERM -> grace -> SIGKILL to the group.
 */

static long long mono_ms(void)
{
   struct timespec ts;
   clock_gettime(CLOCK_MONOTONIC, &ts);
   return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void reap_bounded(pid_t pid, int grace_ms, int *status_out)
{
   /* Escalate to the process GROUP: killing only the child leaves grandchildren
    * holding the pipe. -pid addresses the group the child created with setsid. */
   kill(-pid, SIGTERM);
   long long deadline = mono_ms() + grace_ms;
   for (;;)
   {
      int st = 0;
      pid_t r = waitpid(pid, &st, WNOHANG);
      if (r == pid)
      {
         if (status_out)
            *status_out = st;
         return;
      }
      if (r < 0 && errno == EINTR)
         continue;
      if (r < 0)
         return; /* already reaped or gone */
      if (mono_ms() >= deadline)
         break;
      struct timespec nap = {0, 5 * 1000000};
      nanosleep(&nap, NULL);
   }
   kill(-pid, SIGKILL);
   for (;;)
   {
      int st = 0;
      pid_t r = waitpid(pid, &st, 0);
      if (r == pid)
      {
         if (status_out)
            *status_out = st;
         return;
      }
      if (r < 0 && errno == EINTR)
         continue;
      return;
   }
}

int platform_exec_pipe_bounded(const char *cmd, const char *input, size_t input_len, char **out,
                               size_t *out_len, int timeout_ms, size_t max_output)
{
   if (out)
      *out = NULL;
   if (out_len)
      *out_len = 0;
   if (!cmd || timeout_ms <= 0 || max_output == 0)
      return PLATFORM_EXEC_ERR_SPAWN;

   int stdin_pipe[2], stdout_pipe[2];
   if (pipe(stdin_pipe) < 0)
      return PLATFORM_EXEC_ERR_SPAWN;
   if (pipe(stdout_pipe) < 0)
   {
      close(stdin_pipe[0]);
      close(stdin_pipe[1]);
      return PLATFORM_EXEC_ERR_SPAWN;
   }

   pid_t pid = fork();
   if (pid < 0)
   {
      close(stdin_pipe[0]);
      close(stdin_pipe[1]);
      close(stdout_pipe[0]);
      close(stdout_pipe[1]);
      return PLATFORM_EXEC_ERR_SPAWN;
   }

   if (pid == 0)
   {
      /* Own process group so termination reaches descendants, not just us. */
      setsid();
      close(stdin_pipe[1]);
      close(stdout_pipe[0]);
      dup2(stdin_pipe[0], STDIN_FILENO);
      dup2(stdout_pipe[1], STDOUT_FILENO);
      dup2(stdout_pipe[1], STDERR_FILENO);
      close(stdin_pipe[0]);
      close(stdout_pipe[1]);
      execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
      _exit(127);
   }

   close(stdin_pipe[0]);
   close(stdout_pipe[1]);

   /* Non-blocking both ways: a blocking write is exactly how the deadlock arose. */
   fcntl(stdin_pipe[1], F_SETFL, fcntl(stdin_pipe[1], F_GETFL, 0) | O_NONBLOCK);
   fcntl(stdout_pipe[0], F_SETFL, fcntl(stdout_pipe[0], F_GETFL, 0) | O_NONBLOCK);

   /* A child that exits without draining stdin makes our write raise SIGPIPE;
    * with the default disposition that kills THIS process. Ignore across the
    * exchange and surface EPIPE instead. */
   struct sigaction old_pipe, ignore_pipe;
   memset(&ignore_pipe, 0, sizeof ignore_pipe);
   ignore_pipe.sa_handler = SIG_IGN;
   sigemptyset(&ignore_pipe.sa_mask);
   int restore_pipe = sigaction(SIGPIPE, &ignore_pipe, &old_pipe) == 0;

   size_t cap = 4096, len = 0, off = 0;
   char *buf = malloc(cap);
   int rc = PLATFORM_EXEC_ERR_SPAWN;
   int limit_hit = 0, timed_out = 0;
   const long long deadline = mono_ms() + timeout_ms;

   if (!buf)
   {
      close(stdin_pipe[1]);
      close(stdout_pipe[0]);
      if (restore_pipe)
         sigaction(SIGPIPE, &old_pipe, NULL);
      reap_bounded(pid, 200, NULL);
      return PLATFORM_EXEC_ERR_SPAWN;
   }

   int in_open = 1, out_open = 1;
   if (!input || input_len == 0)
   {
      close(stdin_pipe[1]);
      in_open = 0;
   }

   while (out_open)
   {
      long long remaining = deadline - mono_ms();
      if (remaining <= 0)
      {
         timed_out = 1;
         break;
      }

      struct pollfd pfd[2];
      int n = 0;
      int in_idx = -1, out_idx = -1;
      if (in_open)
      {
         pfd[n].fd = stdin_pipe[1];
         pfd[n].events = POLLOUT;
         pfd[n].revents = 0;
         in_idx = n++;
      }
      if (out_open)
      {
         pfd[n].fd = stdout_pipe[0];
         pfd[n].events = POLLIN;
         pfd[n].revents = 0;
         out_idx = n++;
      }

      int pr = poll(pfd, (nfds_t)n, remaining > 1000 ? 1000 : (int)remaining);
      if (pr < 0)
      {
         if (errno == EINTR)
            continue;
         break;
      }

      if (in_idx >= 0 && (pfd[in_idx].revents & (POLLOUT | POLLERR | POLLHUP)))
      {
         if (pfd[in_idx].revents & (POLLERR | POLLHUP))
         {
            close(stdin_pipe[1]);
            in_open = 0;
         }
         else
         {
            ssize_t w = write(stdin_pipe[1], input + off, input_len - off);
            if (w > 0)
            {
               off += (size_t)w;
               if (off >= input_len)
               {
                  close(stdin_pipe[1]);
                  in_open = 0;
               }
            }
            else if (w < 0 && errno != EAGAIN && errno != EINTR)
            {
               close(stdin_pipe[1]);
               in_open = 0;
            }
         }
      }

      if (out_idx >= 0 && (pfd[out_idx].revents & (POLLIN | POLLERR | POLLHUP)))
      {
         for (;;)
         {
            if (len + 1024 > cap)
            {
               size_t ncap = cap * 2;
               if (ncap > max_output + 1)
                  ncap = max_output + 1;
               if (ncap <= cap)
               {
                  limit_hit = 1;
                  break;
               }
               char *tmp = realloc(buf, ncap);
               if (!tmp)
               {
                  limit_hit = 1;
                  break;
               }
               buf = tmp;
               cap = ncap;
            }
            ssize_t r = read(stdout_pipe[0], buf + len, cap - len - 1);
            if (r > 0)
            {
               len += (size_t)r;
               if (len >= max_output)
               {
                  limit_hit = 1;
                  break;
               }
               continue;
            }
            if (r == 0)
            {
               out_open = 0;
               break;
            }
            if (errno == EINTR)
               continue;
            if (errno == EAGAIN)
               break;
            out_open = 0;
            break;
         }
         if (limit_hit)
            break;
      }
   }

   if (in_open)
      close(stdin_pipe[1]);
   close(stdout_pipe[0]);
   if (restore_pipe)
      sigaction(SIGPIPE, &old_pipe, NULL);

   int status = 0;
   if (timed_out || limit_hit)
   {
      reap_bounded(pid, 200, &status);
      rc = timed_out ? PLATFORM_EXEC_ERR_TIMEOUT : PLATFORM_EXEC_ERR_OUTPUT_LIMIT;
   }
   else
   {
      /* Normal exit still gets a bounded wait: the child may have closed stdout
       * and then hung. Race-safe against a natural exit, and EINTR-retried. */
      long long remaining = deadline - mono_ms();
      int reaped = 0;
      while (remaining > 0)
      {
         pid_t r = waitpid(pid, &status, WNOHANG);
         if (r == pid)
         {
            reaped = 1;
            break;
         }
         if (r < 0 && errno == EINTR)
            continue;
         if (r < 0)
            break;
         struct timespec nap = {0, 5 * 1000000};
         nanosleep(&nap, NULL);
         remaining = deadline - mono_ms();
      }
      if (!reaped)
      {
         reap_bounded(pid, 200, &status);
         timed_out = 1;
         rc = PLATFORM_EXEC_ERR_TIMEOUT;
      }
      else
      {
         rc = WIFEXITED(status) ? WEXITSTATUS(status) : PLATFORM_EXEC_ERR_SPAWN;
      }
   }

   buf[len] = '\0';
   if (out && rc >= 0)
   {
      *out = buf;
      if (out_len)
         *out_len = len;
   }
   else
   {
      free(buf);
   }
   return rc;
}

int platform_setenv(const char *name, const char *value)
{
   if (!name)
      return -1;
   return setenv(name, value ? value : "", 1);
}

unsigned int platform_getuid(void)
{
   return (unsigned int)getuid();
}

void platform_signal_term(platform_signal_handler_t handler)
{
   signal(SIGTERM, handler);
}

void platform_signal_int(platform_signal_handler_t handler)
{
   signal(SIGINT, handler);
}

int platform_signal_send_term(int pid)
{
   return kill(pid, SIGTERM);
}

int platform_exec_pipe(const char *cmd, const char *input, size_t input_len, char **out,
                       size_t *out_len)
{
   return platform_exec_pipe_bounded(cmd, input, input_len, out, out_len,
                                     PLATFORM_EXEC_DEFAULT_TIMEOUT_MS,
                                     PLATFORM_EXEC_DEFAULT_MAX_OUTPUT);
}
