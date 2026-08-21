/* db1_module_init.c: open the store before the process serves a single stage.
 *
 * The DB1 domain keeps one process-wide SQLite connection, set by db1_init and
 * read by every domain function through db1_conn(). The module process is a
 * separate process from the daemon, so it has its own copy of that global, and
 * nothing was setting it: the generated main went straight to
 * aimee_module_process_run.
 *
 * The result was worse than a crash. The process attached, registered its
 * stages and answered every request -- with db1_conn() returning NULL, so each
 * domain function took its "no database" branch and returned -1. The stage
 * mapped that to FAILED, or for a read to MISSING, and MISSING is the same
 * shape as "there is no such row". A caller asking who owns a branch was told
 * nobody did. The module looked healthy throughout.
 *
 * The path comes from the environment and is NOT derived here. The daemon's
 * path is a configured value an operator may override, and this module cannot
 * read that configuration: it is deliberately self-contained, linking neither
 * the config module nor aimee_home(). Guessing "the default location" would
 * work until someone moved the database, and then this process would serve a
 * DIFFERENT, empty store -- which is the failure above wearing a disguise.
 * Being told or refusing to start are the only two safe answers.
 */
#include "db1.h"

#include <stdio.h>
#include <stdlib.h>

int aimee_db1_module_init(void)
{
   const char *path = getenv("AIMEE_DB1_PATH");
   if (!path || !path[0])
   {
      fprintf(stderr,
              "db1: AIMEE_DB1_PATH is unset; refusing to serve. This process cannot read the "
              "daemon's configuration, so it must be told which database to open.\n");
      return -1;
   }
   if (db1_init(path) != 0)
   {
      fprintf(stderr, "db1: cannot open %s; refusing to serve\n", path);
      return -1;
   }
   /* The cache, mmap and checkpoint tuning belongs to whoever runs the
      queries, and since the families moved that is this process. The daemon
      used to apply it to a connection it no longer reads or writes through,
      so the settings were going to the one place they could not matter. */
   db1_apply_server_pragmas();
   return 0;
}
