/* Queue retry/backoff UTC helpers. Model execution is owned by canonical IR. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "kb_curator_sidecar.h"
#include <stdlib.h>
#include <time.h>

/* --- retry backoff (see the header for why this exists) --- */

int kb_curator_retry_delay_seconds(int attempts)
{
   if (attempts < 1)
      attempts = 1;
   long d = KB_CURATOR_RETRY_BASE_S;
   /* base * 2^(attempts-1), bailing before the shift can overflow. */
   for (int i = 1; i < attempts; i++)
   {
      d *= 2;
      if (d >= KB_CURATOR_RETRY_MAX_S)
         return KB_CURATOR_RETRY_MAX_S;
   }
   return (int)d;
}

static void kb_curator_utc_text(time_t t, char *out, size_t outlen)
{
   if (!out || outlen == 0)
      return;
   struct tm tmv;
   gmtime_r(&t, &tmv);
   strftime(out, outlen, "%Y-%m-%d %H:%M:%S", &tmv);
}

void kb_curator_now_text(char *out, size_t outlen)
{
   kb_curator_utc_text(time(NULL), out, outlen);
}

void kb_curator_next_attempt_at(int attempts, char *out, size_t outlen)
{
   int delay = kb_curator_retry_delay_seconds(attempts);
   /* +/-10% jitter so a batch that failed on one shared cause does not retry in
    * lockstep. rand() is fine here: this spreads load, it is not security. */
   int span = delay / 10;
   int jitter = span > 0 ? (rand() % (2 * span + 1)) - span : 0;
   kb_curator_utc_text(time(NULL) + delay + jitter, out, outlen);
}
