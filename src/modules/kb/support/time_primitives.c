#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "db2_time.h"

#include <stdio.h>
#include <string.h>

void now_utc(char *buf, size_t len)
{
   time_t t = time(NULL);
   struct tm tm;
   gmtime_r(&t, &tm);
   strftime(buf, len, "%Y-%m-%dT%H:%M:%SZ", &tm);
}

time_t parse_utc_ts(const char *s)
{
   if (!s || !s[0])
      return 0;

   int year = 0, mon = 0, day = 0, hour = 0, min = 0, sec = 0;
   char sep = 0;
   int n = sscanf(s, "%d-%d-%d%c%d:%d:%d", &year, &mon, &day, &sep, &hour, &min, &sec);
   if (n == 3)
      hour = min = sec = 0;
   else if (n != 7 || (sep != 'T' && sep != ' '))
      return 0;
   if (year < 1970 || mon < 1 || mon > 12 || day < 1 || day > 31)
      return 0;

   struct tm tmv;
   memset(&tmv, 0, sizeof(tmv));
   tmv.tm_year = year - 1900;
   tmv.tm_mon = mon - 1;
   tmv.tm_mday = day;
   tmv.tm_hour = hour;
   tmv.tm_min = min;
   tmv.tm_sec = sec;
   tmv.tm_isdst = 0;
#if defined(_WIN32) || defined(_WIN64)
   return _mkgmtime(&tmv);
#else
   return timegm(&tmv);
#endif
}
