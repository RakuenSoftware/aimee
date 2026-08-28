#ifndef AIMEE_KB_INDEX_TIMEOUTS_H
#define AIMEE_KB_INDEX_TIMEOUTS_H

#include <stdlib.h>

/* Shared by the thin client's outer async poll and the server's inner request.
 * Keeping the resolver here prevents one layer from timing out while the other
 * is still legitimately processing the same large repository scan. */
static inline int aimee_kb_index_scan_timeout_ms(void)
{
   const char *env = getenv("AIMEE_KB_SCAN_TIMEOUT_MS");
   if (env && env[0])
   {
      long value = strtol(env, NULL, 10);
      if (value > 0 && value <= 24L * 60 * 60 * 1000)
         return (int)value;
   }
   return 15 * 60 * 1000;
}

#endif
