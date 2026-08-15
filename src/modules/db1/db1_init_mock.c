/* db1/db1_init_mock.c: no-op lifecycle for mock builds.
 *
 * Link this INSTEAD OF db1_init.c when the test links wm_mock.c (and
 * any other *_mock.c). The mock subsystems hold their own state; there's
 * no sqlite connection to open. The init/shutdown functions exist so
 * that callers can still invoke db1_init()/db1_shutdown() uniformly. */

#include "db1.h"

int db1_init(const char *path)
{
   (void)path;
   return 0;
}

void db1_shutdown(void)
{
}
