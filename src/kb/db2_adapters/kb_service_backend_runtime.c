/* Caller-side process supervision for legacy DB2-backed KB workers. The DB2
 * process owns queue state, but never discovers or spawns the host executable. */
#include "modules/db2/c/kb_service_backend.h"

#include "aimee.h"
#include "platform_process.h"

int db2_kb_service_async_queue_spawn_worker(void)
{
   char exe[MAX_PATH_LEN];
   if (platform_get_exe_path(exe, sizeof(exe)) != 0)
      return -1;

   const char *argv[] = {exe, "memory", "drain", NULL};
   return platform_spawn_daemon(argv) > 0 ? 0 : -1;
}
