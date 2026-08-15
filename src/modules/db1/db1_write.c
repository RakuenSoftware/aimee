/* db1_write.c: DB1 write-path helpers — periodic passive WAL checkpoint.
 *
 * The jittered busy handler lives in db1_init.c (static to that file) so
 * that every compilation unit linking db1_init.o gets it automatically
 * without a separate link dependency on this file.
 *
 * This file provides db1_maybe_checkpoint, which callers invoke after
 * successful commits to keep the WAL file from growing unbounded. */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "db1_write.h"

#include <sqlite3.h>
#include <stddef.h>

#define CHECKPOINT_STRIDE 50

static int g_write_count = 0;

void db1_maybe_checkpoint(struct sqlite3 *db)
{
   if (!db)
      return;
   if (++g_write_count % CHECKPOINT_STRIDE == 0)
      sqlite3_wal_checkpoint_v2(db, NULL, SQLITE_CHECKPOINT_PASSIVE, NULL, NULL);
}
