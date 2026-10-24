#ifndef AIMEE_CLI_NATIVE_SPAWN_GUARD_H
#define AIMEE_CLI_NATIVE_SPAWN_GUARD_H
#include <sys/types.h>
/* Optional host-only two-sided pre-exec custody. Parent must attach the actual
 * forked child; child refuses EOF and cannot exec before parent admission. */
typedef struct cli_native_spawn_guard
{
   int (*child)(void *context, int executable_fd, int cwd_fd);
   int (*parent)(void *context, pid_t child);
   void *context;
} cli_native_spawn_guard_t;
#if !defined(_WIN32)
#include <fcntl.h>
#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/syscall.h>
#endif
/* Keep launch resources and custody channels outside standard descriptor slots.
 * The borrowed original remains owned by its caller. Identity is unchanged. */
static inline int cli_native_pin_fd(int descriptor)
{
   return fcntl(descriptor, F_DUPFD_CLOEXEC, 10);
}
static inline int cli_native_pipe(int descriptors[2])
{
   int raw[2];
#ifdef __linux__
   if (syscall(SYS_pipe2, raw, O_CLOEXEC))
#else
   if (pipe(raw))
#endif
      return -1;
   int input = cli_native_pin_fd(raw[0]);
   int output = cli_native_pin_fd(raw[1]);
   close(raw[0]);
   close(raw[1]);
   if (input < 0 || output < 0)
   {
      if (input >= 0)
         close(input);
      if (output >= 0)
         close(output);
      return -1;
   }
   descriptors[0] = input;
   descriptors[1] = output;
   return 0;
}

/* Private supervised stdio shared by admitted ACP agents and their trusted
 * container resource transport. Callers supply already-owned executable/cwd
 * descriptors; this function never selects a command or creates authority. */
typedef struct
{
   int in_fd, out_fd, pidfd;
   pid_t pid;
} cli_native_stdio_t;
static inline void cli_native_stdio_close(cli_native_stdio_t *process)
{
   if (process->in_fd >= 0)
      close(process->in_fd);
   if (process->out_fd >= 0)
      close(process->out_fd);
   if (process->pid > 0)
   {
      /* The unreaped child owns this process-group identity. */
      kill(-process->pid, SIGTERM);
      usleep(50000);
      kill(-process->pid, SIGKILL);
      while (waitpid(process->pid, NULL, 0) < 0 && errno == EINTR)
         ;
   }
   if (process->pidfd >= 0)
      close(process->pidfd);
   *process = (cli_native_stdio_t){.in_fd = -1, .out_fd = -1, .pidfd = -1};
}
static inline int cli_native_stdio_spawn(int executable_fd, int cwd_fd, char *const argv[],
                                         char *const environment[], int (*validate)(void *),
                                         void *context, cli_native_stdio_t *process)
{
   if (!process || !validate || validate(context) || executable_fd < 0 || cwd_fd < 0 || !argv ||
       !argv[0] || !environment)
      return -1;
   *process = (cli_native_stdio_t){.in_fd = -1, .out_fd = -1, .pidfd = -1};
   long limit = sysconf(_SC_OPEN_MAX);
   if (limit < 4 || limit > INT_MAX)
      return -1;
   int input[2], output[2], gate[2];
   if (cli_native_pipe(input))
      return -1;
   if (cli_native_pipe(output))
   {
      close(input[0]);
      close(input[1]);
      return -1;
   }
   if (cli_native_pipe(gate))
   {
      close(input[0]);
      close(input[1]);
      close(output[0]);
      close(output[1]);
      return -1;
   }
   pid_t pid = fork();
   if (!pid)
   {
      close(gate[1]);
      char admitted = 0;
      if (setsid() < 0 || read(gate[0], &admitted, 1) != 1 || admitted != 1)
         _exit(126);
      int executable = cli_native_pin_fd(executable_fd), directory = cli_native_pin_fd(cwd_fd);
      int diagnostic = open("/dev/null", O_WRONLY | O_CLOEXEC);
      int error = diagnostic >= 0 ? cli_native_pin_fd(diagnostic) : -1;
      if (executable < 0 || directory < 0 || error < 0 || fchdir(directory) ||
          dup2(input[0], 0) < 0 || dup2(output[1], 1) < 0 || dup2(error, 2) < 0 ||
          dup2(executable, 3) < 0)
         _exit(126);
#ifdef SYS_close_range
      if (syscall(SYS_close_range, 4u, ~0u, 0))
#endif
         for (long fd = 4; fd < limit; fd++)
            close((int)fd);
      fexecve(3, argv, environment);
      _exit(127);
   }
   close(input[0]);
   close(output[1]);
   close(gate[0]);
#ifdef SYS_pidfd_open
   int pidfd = pid > 0 ? (int)syscall(SYS_pidfd_open, pid, 0) : -1;
#else
   int pidfd = -1;
#endif
   if (pid < 0 || validate(context)
#ifdef SYS_pidfd_open
       || pidfd < 0
#endif
   )
   {
      close(gate[1]);
      close(input[1]);
      close(output[0]);
      if (pid > 0)
         while (waitpid(pid, NULL, 0) < 0 && errno == EINTR)
            ;
      if (pidfd >= 0)
         close(pidfd);
      return -1;
   }
   char admitted = 1;
   ssize_t released = write(gate[1], &admitted, 1);
   close(gate[1]);
   *process =
       (cli_native_stdio_t){.pid = pid, .pidfd = pidfd, .in_fd = input[1], .out_fd = output[0]};
   int flags = fcntl(process->in_fd, F_GETFL, 0);
   if (released != 1 || flags < 0 || fcntl(process->in_fd, F_SETFL, flags | O_NONBLOCK))
   {
      cli_native_stdio_close(process);
      return -1;
   }
   return 0;
}
#endif
#endif
