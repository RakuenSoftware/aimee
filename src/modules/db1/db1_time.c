/* db1_time.c: the timestamp primitive the DB1 module process needs.
 *
 * now_utc is declared in the shared header and defined once per process rather
 * than in a library: the daemon gets it from src/util.c and the DB2 module from
 * its own support file. DB1 is a process now too, so it supplies its own.
 *
 * This file is in the module descriptor and deliberately NOT in DB1_SRCS. The
 * daemon still links the DB1 domain alongside util.c, so a second definition
 * there would be a duplicate symbol; the module links the domain without
 * util.c, so without this one it is an undefined symbol. Each binary gets
 * exactly one, which is why the file exists rather than the call being routed
 * through something shared.
 *
 * It is bit-for-bit the same format the other two produce, because these
 * timestamps are compared as text and written into the same columns. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "aimee.h"

#include <stddef.h>
#include <time.h>

void now_utc(char *buf, size_t len)
{
   time_t t = time(NULL);
   struct tm tm;
   gmtime_r(&t, &tm);
   strftime(buf, len, "%Y-%m-%dT%H:%M:%SZ", &tm);
}
