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


#endif
#endif
