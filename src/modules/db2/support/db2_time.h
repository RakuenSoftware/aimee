#ifndef AIMEE_DB2_SUPPORT_TIME_H
#define AIMEE_DB2_SUPPORT_TIME_H

#include <stddef.h>
#include <time.h>

void now_utc(char *buf, size_t len);
time_t parse_utc_ts(const char *s);

#endif
