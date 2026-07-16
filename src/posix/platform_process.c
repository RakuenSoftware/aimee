/* platform_process.c: process spawning, exec, and signal handling (POSIX) */
#include "platform_process.h"
#include <errno.h>
#include <fcntl.h>
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

int platform_exec_pipe(const char *cmd, const char *input, size_t input_len, char **out,
                       size_t *out_len)
{
   int stdin_pipe[2];
   int stdout_pipe[2];
   if (pipe(stdin_pipe) < 0 || pipe(stdout_pipe) < 0)
      return -1;

   pid_t pid = fork();
   if (pid < 0)
   {
      close(stdin_pipe[0]);
      close(stdin_pipe[1]);
      close(stdout_pipe[0]);
      close(stdout_pipe[1]);
      return -1;
   }

   if (pid == 0)
   {
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

   /* A child that exits before draining its stdin (e.g. a rerank command that
    * fails fast with a non-zero status) closes the read end, so our write() to it
    * raises SIGPIPE. With the default disposition that terminates THIS process --
    * turning a handled subprocess failure into a hard crash of the caller (seen as
    * a flaky SIGPIPE kill of unit-test-memory under load, and a latent server
    * crash in production). Ignore SIGPIPE across the write so the failed write
    * surfaces as EPIPE and the loop breaks cleanly; restore the prior disposition
    * after. Save/restore matches workspace_provider.c. */
   struct sigaction old_pipe, ignore_pipe;
   memset(&ignore_pipe, 0, sizeof(ignore_pipe));
   ignore_pipe.sa_handler = SIG_IGN;
   sigemptyset(&ignore_pipe.sa_mask);
   int restore_pipe = sigaction(SIGPIPE, &ignore_pipe, &old_pipe) == 0;

   if (input && input_len > 0)
   {
      size_t off = 0;
      while (off < input_len)
      {
         ssize_t n = write(stdin_pipe[1], input + off, input_len - off);
         if (n <= 0)
            break;
         off += (size_t)n;
      }
   }
   close(stdin_pipe[1]);

   if (restore_pipe)
      sigaction(SIGPIPE, &old_pipe, NULL);

   size_t cap = 4096;
   size_t len = 0;
   char *buf = malloc(cap);
   if (!buf)
   {
      close(stdout_pipe[0]);
      kill(pid, SIGKILL);
      waitpid(pid, NULL, 0);
      return -1;
   }

   ssize_t n;
   while ((n = read(stdout_pipe[0], buf + len, cap - len - 1)) > 0)
   {
      len += (size_t)n;
      if (len + 1024 > cap)
      {
         cap *= 2;
         char *tmp = realloc(buf, cap);
         if (!tmp)
            break;
         buf = tmp;
      }
   }
   close(stdout_pipe[0]);
   buf[len] = '\0';

   int status;
   waitpid(pid, &status, 0);

   *out = buf;
   if (out_len)
      *out_len = len;
   return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
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
