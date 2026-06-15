/* util.c: POSIX-specific utilities (process exec, regex, pclose status) */
#include "aimee.h"
#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <regex.h>
#include <signal.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static long now_ms(void)
{
   struct timespec ts;
   clock_gettime(CLOCK_MONOTONIC, &ts);
   return (long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* Reap `pid` without blocking unbounded: poll waitpid(WNOHANG) for up to
 * budget_ms. Used after a SIGKILL — a killed process normally dies at once, but
 * a child wedged in uninterruptible (D) sleep can ignore even SIGKILL; rather
 * than park the caller forever we give up after the budget (the OS/init reaps
 * the eventual zombie). Returns 1 if reaped, 0 if it timed out. */
static int reap_bounded(pid_t pid, int budget_ms)
{
   long deadline = now_ms() + budget_ms;
   for (;;)
   {
      pid_t w = waitpid(pid, NULL, WNOHANG);
      if (w == pid || w < 0)
         return 1;
      if (now_ms() >= deadline)
         return 0;
      struct timespec ts = {.tv_sec = 0, .tv_nsec = 10 * 1000000}; /* 10ms */
      nanosleep(&ts, NULL);
   }
}

int safe_exec_capture_cwd_env_timeout(const char *const argv[], const char *cwd, char *const envp[],
                                      char **out_buf, size_t max_out, int timeout_ms)
{
   *out_buf = NULL;
   if (!argv || !argv[0])
      return -1;

   int pipefd[2];
   if (pipe(pipefd) != 0)
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
      /* Child: restore a clean signal disposition/mask so the exec'd command
       * doesn't inherit the engine's blocked signals (e.g. SIGCHLD/SIGPIPE). */
      sigset_t empty;
      sigemptyset(&empty);
      sigprocmask(SIG_SETMASK, &empty, NULL);
      /* redirect stdout/stderr to pipe, close stdin */
      close(pipefd[0]);
      dup2(pipefd[1], STDOUT_FILENO);
      dup2(pipefd[1], STDERR_FILENO);
      close(pipefd[1]);
      /* Pin the working directory before exec so the command runs in the caller's
       * chosen dir (e.g. the work-item repo), not the engine's inherited cwd. A
       * failed chdir must NOT silently fall back to the parent cwd. */
      if (cwd && cwd[0] && chdir(cwd) != 0)
         _exit(127);
      /* REPLACE (not augment) the child's environment when envp is given: assigning
       * environ makes execvp pass exactly envp, so ONLY envp's entries survive and
       * every inherited variable — including engine secrets (AIMEE_APPROVAL_KEY,
       * vault tokens) — is dropped. (envp == NULL keeps the inherited environment;
       * that path is used by callers that build a full env themselves.) Portable —
       * no execvpe needed. */
      if (envp)
      {
         extern char **environ;
         environ = (char **)envp;
      }
      execvp(argv[0], (char *const *)argv);
      _exit(127);
   }

   /* Parent */
   close(pipefd[1]);

   char *buf = malloc(max_out + 1);
   if (!buf)
   {
      close(pipefd[0]);
      kill(pid, SIGKILL);
      waitpid(pid, NULL, 0);
      return -1;
   }

   long deadline = (timeout_ms > 0) ? now_ms() + timeout_ms : 0;
   int timed_out = 0;
   size_t total = 0;
   while (total < max_out)
   {
      int wait_ms = -1; /* block indefinitely when no timeout is set */
      if (deadline)
      {
         long remaining = deadline - now_ms();
         if (remaining <= 0)
         {
            timed_out = 1;
            break;
         }
         wait_ms = (remaining > INT_MAX) ? INT_MAX : (int)remaining;
      }
      struct pollfd pfd = {.fd = pipefd[0], .events = POLLIN};
      int pr = poll(&pfd, 1, wait_ms);
      if (pr == 0)
      {
         timed_out = 1; /* deadline elapsed with no data/EOF */
         break;
      }
      if (pr < 0)
      {
         if (errno == EINTR)
            continue;
         break;
      }
      /* Drain any readable data first; only stop on a hangup/error once the pipe
       * has no more bytes (POLLHUP can arrive alongside a final POLLIN). */
      if (!(pfd.revents & (POLLIN | POLLHUP | POLLERR)))
         continue;
      ssize_t n = read(pipefd[0], buf + total, max_out - total);
      if (n < 0)
      {
         if (errno == EINTR)
            continue; /* interrupted before any byte — retry, don't drop output */
         break;       /* real read error */
      }
      if (n == 0)
         break; /* EOF: child closed stdout / exited */
      total += (size_t)n;
   }
   close(pipefd[0]);
   buf[total] = '\0';
   *out_buf = buf;

   if (timed_out)
   {
      /* The command exceeded its wall-clock budget: kill it, reap (bounded — a
       * D-state child can ignore SIGKILL; don't park on it), and report a distinct
       * timeout failure so callers don't park the engine on a hang. */
      kill(pid, SIGKILL);
      reap_bounded(pid, 2000);
      return SAFE_EXEC_TIMEOUT;
   }

   int status = 0;
   if (deadline)
   {
      /* Keep the deadline live across the reap. The read loop can exit with the
       * child still alive — it filled the buffer (total == max_out) or closed
       * stdout early (read <= 0) — so an unbounded waitpid here would re-introduce
       * the very hang the timeout exists to prevent. Poll-reap until the deadline,
       * then kill. */
      for (;;)
      {
         pid_t w = waitpid(pid, &status, WNOHANG);
         if (w == pid)
            break; /* reaped normally */
         if (w < 0)
            /* Can't determine the child's status (e.g. ECHILD — externally
             * reaped): report failure rather than a false success. */
            return -1;
         if (now_ms() >= deadline)
         {
            kill(pid, SIGKILL);
            reap_bounded(pid, 2000);
            return SAFE_EXEC_TIMEOUT;
         }
         struct timespec ts = {.tv_sec = 0, .tv_nsec = 10 * 1000000}; /* 10ms */
         nanosleep(&ts, NULL);
      }
   }
   else
   {
      waitpid(pid, &status, 0);
   }
   return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

int safe_exec_capture_env(const char *const argv[], char *const envp[], char **out_buf,
                          size_t max_out)
{
   return safe_exec_capture_cwd_env_timeout(argv, NULL, envp, out_buf, max_out, 0);
}

int safe_exec_capture(const char *const argv[], char **out_buf, size_t max_out)
{
   return safe_exec_capture_cwd_env_timeout(argv, NULL, NULL, out_buf, max_out, 0);
}

int regex_match(const char *pattern, const char *text, int flags)
{
   if (!pattern || !text)
      return 0;
   regex_t re;
   if (regcomp(&re, pattern, flags | REG_NOSUB) != 0)
      return 0;
   int matched = (regexec(&re, text, 0, NULL, 0) == 0);
   regfree(&re);
   return matched;
}
