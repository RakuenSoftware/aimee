/* server_main.c: POSIX-specific server lifecycle helpers */
#include "platform_process.h"
#include "log.h"
#include <signal.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>

void platform_server_redirect_stderr(FILE *log_fp)
{
   dup2(fileno(log_fp), STDERR_FILENO);
}

int platform_server_service_dispatch(const char *socket_path, int log_level,
                                     int (*run_server_fn)(const char *, int))
{
   (void)socket_path;
   (void)log_level;
   (void)run_server_fn;
   fprintf(stderr, "aimee-server: --service is only for Windows\n");
   return 1;
}
