/* dependency_breaker.h: small process-local circuit breaker for optional services.
 *
 * Callers own the state and the clock. The breaker performs no retries itself:
 * after consecutive failures it suppresses calls for a bounded, jittered
 * exponential delay, then grants exactly one half-open recovery probe. This
 * keeps request paths bounded while allowing recovery without a process restart.
 *
 * The implementation is header-only so the same C11 primitive can be used by
 * the server-side KB client and the KB-side embedder without adding a new link
 * dependency to the many focused unit-test binaries that exercise those paths. */
#ifndef DEC_DEPENDENCY_BREAKER_H
#define DEC_DEPENDENCY_BREAKER_H 1

#include <stdatomic.h>
#include <stdint.h>
#include <string.h>

#define DEPENDENCY_BREAKER_DEFAULT_THRESHOLD 3U
#define DEPENDENCY_BREAKER_DEFAULT_BASE_MS   1000LL
#define DEPENDENCY_BREAKER_DEFAULT_MAX_MS    30000LL

typedef struct
{
   atomic_flag lock;
   unsigned failure_streak;
   unsigned open_count;
   int probe_inflight;
   int64_t opened_at_ms;
   int64_t retry_at_ms;
   int64_t last_success_ms;
   int64_t last_failure_ms;
   uint64_t suppressed_calls;
} dependency_breaker_t;

#define DEPENDENCY_BREAKER_INITIALIZER {ATOMIC_FLAG_INIT, 0U, 0U, 0, 0, 0, 0, 0, 0U}

typedef struct
{
   unsigned failure_streak;
   unsigned open_count;
   int open;
   int probe_inflight;
   int64_t retry_after_ms;
   int64_t last_success_ms;
   int64_t last_failure_ms;
   uint64_t suppressed_calls;
} dependency_breaker_snapshot_t;

static inline void dependency_breaker_lock(dependency_breaker_t *breaker)
{
   while (atomic_flag_test_and_set_explicit(&breaker->lock, memory_order_acquire))
      ;
}

static inline void dependency_breaker_unlock(dependency_breaker_t *breaker)
{
   atomic_flag_clear_explicit(&breaker->lock, memory_order_release);
}

/* Return true for ordinary closed-state calls and for the single caller that
 * claims an eligible half-open probe. retry_after_ms is populated on denial. */
static inline int dependency_breaker_allow(dependency_breaker_t *breaker, int64_t now_ms,
                                           int64_t *retry_after_ms)
{
   if (retry_after_ms)
      *retry_after_ms = 0;
   if (!breaker)
      return 1;

   dependency_breaker_lock(breaker);
   int allow = 1;
   if (breaker->retry_at_ms > 0)
   {
      /* A backward wall-clock jump must not extend a bounded outage forever. */
      int eligible = now_ms >= breaker->retry_at_ms || now_ms < breaker->opened_at_ms;
      if (eligible && !breaker->probe_inflight)
         breaker->probe_inflight = 1;
      else
      {
         allow = 0;
         breaker->suppressed_calls++;
         if (retry_after_ms && !eligible)
            *retry_after_ms = breaker->retry_at_ms - now_ms;
      }
   }
   dependency_breaker_unlock(breaker);
   return allow;
}

static inline int64_t dependency_breaker_delay_ms(unsigned open_count, int64_t now_ms,
                                                  int64_t base_ms, int64_t max_ms)
{
   if (base_ms < 1)
      base_ms = 1;
   if (max_ms < base_ms)
      max_ms = base_ms;

   int64_t delay = base_ms;
   for (unsigned i = 0; i < open_count && delay < max_ms; i++)
      delay = delay > max_ms / 2 ? max_ms : delay * 2;

   int64_t jitter_cap = delay / 4;
   if (jitter_cap > max_ms - delay)
      jitter_cap = max_ms - delay;
   if (jitter_cap > 0)
   {
      uint64_t mixed = (uint64_t)now_ms ^ ((uint64_t)(open_count + 1U) * 0x9e3779b97f4a7c15ULL);
      delay += (int64_t)(mixed % (uint64_t)(jitter_cap + 1));
   }
   return delay;
}

static inline void dependency_breaker_report_success(dependency_breaker_t *breaker, int64_t now_ms)
{
   if (!breaker)
      return;
   dependency_breaker_lock(breaker);
   breaker->failure_streak = 0;
   breaker->open_count = 0;
   breaker->probe_inflight = 0;
   breaker->opened_at_ms = 0;
   breaker->retry_at_ms = 0;
   breaker->last_success_ms = now_ms;
   dependency_breaker_unlock(breaker);
}

/* Release a claimed half-open probe when a local preflight error prevented any
 * dependency call. Neither success nor failure may be inferred from that. */
static inline void dependency_breaker_cancel_probe(dependency_breaker_t *breaker)
{
   if (!breaker)
      return;
   dependency_breaker_lock(breaker);
   breaker->probe_inflight = 0;
   dependency_breaker_unlock(breaker);
}

static inline void dependency_breaker_report_failure(dependency_breaker_t *breaker, int64_t now_ms,
                                                     unsigned threshold, int64_t base_ms,
                                                     int64_t max_ms)
{
   if (!breaker)
      return;
   if (threshold < 1U)
      threshold = 1U;
   dependency_breaker_lock(breaker);
   if (breaker->failure_streak < UINT32_MAX)
      breaker->failure_streak++;
   breaker->last_failure_ms = now_ms;
   breaker->probe_inflight = 0;
   if (breaker->failure_streak >= threshold)
   {
      int64_t delay = dependency_breaker_delay_ms(breaker->open_count, now_ms, base_ms, max_ms);
      breaker->opened_at_ms = now_ms;
      breaker->retry_at_ms = now_ms + delay;
      if (breaker->open_count < UINT32_MAX)
         breaker->open_count++;
   }
   dependency_breaker_unlock(breaker);
}

static inline void dependency_breaker_snapshot(dependency_breaker_t *breaker, int64_t now_ms,
                                               dependency_breaker_snapshot_t *out)
{
   if (!out)
      return;
   memset(out, 0, sizeof(*out));
   if (!breaker)
      return;
   dependency_breaker_lock(breaker);
   out->failure_streak = breaker->failure_streak;
   out->open_count = breaker->open_count;
   out->open = breaker->retry_at_ms > 0;
   out->probe_inflight = breaker->probe_inflight;
   if (breaker->retry_at_ms > now_ms && now_ms >= breaker->opened_at_ms)
      out->retry_after_ms = breaker->retry_at_ms - now_ms;
   out->last_success_ms = breaker->last_success_ms;
   out->last_failure_ms = breaker->last_failure_ms;
   out->suppressed_calls = breaker->suppressed_calls;
   dependency_breaker_unlock(breaker);
}

static inline void dependency_breaker_reset(dependency_breaker_t *breaker)
{
   if (!breaker)
      return;
   dependency_breaker_lock(breaker);
   breaker->failure_streak = 0;
   breaker->open_count = 0;
   breaker->probe_inflight = 0;
   breaker->opened_at_ms = 0;
   breaker->retry_at_ms = 0;
   breaker->last_success_ms = 0;
   breaker->last_failure_ms = 0;
   breaker->suppressed_calls = 0;
   dependency_breaker_unlock(breaker);
}

#endif /* DEC_DEPENDENCY_BREAKER_H */
