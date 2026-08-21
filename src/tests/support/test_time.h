/* test_time.h: back-dated / post-dated timestamps for DB2 test fixtures.
 *
 * DB2's timestamp columns are TEXT in the canonical 'YYYY-MM-DD HH:MM:SS' UTC
 * spelling (schema.sql defaults them to to_char(CURRENT_TIMESTAMP, ...), and the
 * sqlite shim's pg_now_text() emits the same shape). A fixture that needs "nine
 * days ago" therefore used to reach for datetime('now', '-9 days') -- a sqlite
 * function with no Postgres equivalent, and one the shim's translator cannot
 * reproduce either, because its to_char(CURRENT_TIMESTAMP ...) rule collapses any
 * interval arithmetic inside back to plain now().
 *
 * Computing the instant in C sidesteps the dialect entirely: what lands in the
 * column is a literal, and both engines simply store it. Postgres's few timestamptz
 * columns parse it as UTC as long as the server runs in UTC, which the test
 * template does.
 */
#ifndef AIMEE_TEST_TIME_H
#define AIMEE_TEST_TIME_H

#include <stddef.h>
#include <time.h>

/* Write now + `offset_days` (negative for the past) into `buf` as
 * 'YYYY-MM-DD HH:MM:SS' UTC. Returns `buf`. */
static inline const char *test_ts_days(char *buf, size_t len, int offset_days)
{
   time_t t = time(NULL) + (time_t)offset_days * 86400;
   struct tm tm;
   gmtime_r(&t, &tm);
   strftime(buf, len, "%Y-%m-%d %H:%M:%S", &tm);
   return buf;
}

#define TEST_TS_MAX 32

#endif /* AIMEE_TEST_TIME_H */
