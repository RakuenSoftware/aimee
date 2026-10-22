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
#endif
