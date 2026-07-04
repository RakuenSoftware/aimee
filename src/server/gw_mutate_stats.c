/* gw_mutate_stats.c: see gw_mutate_stats.h. Flat counters are stdatomic; the
 * reason registry is a small fixed table under one mutex (exceptional-event rate). */
#include "gw_mutate_stats.h"

#include <pthread.h>
#include <stdatomic.h>
#include <string.h>

static _Atomic uint64_t g_flat[GW_STAT__COUNT];

#define GW_STAT_REASON_MAX 64
#define GW_STAT_GROUP_LEN  24
#define GW_STAT_REASON_LEN 40

typedef struct
{
   char group[GW_STAT_GROUP_LEN];
   char reason[GW_STAT_REASON_LEN];
   uint64_t count;
} reason_entry_t;

static reason_entry_t g_reasons[GW_STAT_REASON_MAX];
static int g_reason_n;
static pthread_mutex_t g_reason_lock = PTHREAD_MUTEX_INITIALIZER;

/* Sampled token-delta accumulators (1-in-N deterministic sample). */
#define GW_STAT_TOKEN_SAMPLE_N 100
static _Atomic uint64_t g_token_seen;         /* applied mutations seen (for the sample gate) */
static _Atomic uint64_t g_token_sample_count; /* samples actually recorded */
static _Atomic uint64_t g_token_baseline_sum;
static _Atomic uint64_t g_token_reduced_sum;

void gw_stat_inc(gw_stat_t which)
{
   if (which < 0 || which >= GW_STAT__COUNT)
      return;
   atomic_fetch_add_explicit(&g_flat[which], 1, memory_order_relaxed);
}

uint64_t gw_stat_get(gw_stat_t which)
{
   if (which < 0 || which >= GW_STAT__COUNT)
      return 0;
   return atomic_load_explicit(&g_flat[which], memory_order_relaxed);
}

/* Find an existing (group,reason) row; caller holds g_reason_lock. */
static reason_entry_t *reason_find_locked(const char *group, const char *reason)
{
   for (int i = 0; i < g_reason_n; i++)
      if (strncmp(g_reasons[i].group, group, GW_STAT_GROUP_LEN) == 0 &&
          strncmp(g_reasons[i].reason, reason, GW_STAT_REASON_LEN) == 0)
         return &g_reasons[i];
   return NULL;
}

/* Register a new (group,reason) row, or return the overflow row when the table is
 * full so counts are never silently dropped. Caller holds g_reason_lock. */
static reason_entry_t *reason_intern_locked(const char *group, const char *reason)
{
   reason_entry_t *e = reason_find_locked(group, reason);
   if (e)
      return e;
   if (g_reason_n < GW_STAT_REASON_MAX)
   {
      e = &g_reasons[g_reason_n++];
      snprintf(e->group, sizeof(e->group), "%s", group ? group : "");
      snprintf(e->reason, sizeof(e->reason), "%s", reason ? reason : "");
      e->count = 0;
      return e;
   }
   /* Full: fold into a per-group overflow bucket (register it if room, else the
    * very last row). */
   e = reason_find_locked(group, "_overflow");
   if (e)
      return e;
   e = &g_reasons[GW_STAT_REASON_MAX - 1];
   snprintf(e->group, sizeof(e->group), "%s", group ? group : "");
   snprintf(e->reason, sizeof(e->reason), "_overflow");
   return e;
}

void gw_stat_inc_reason(const char *group, const char *reason)
{
   if (!group)
      group = "";
   if (!reason)
      reason = "";
   pthread_mutex_lock(&g_reason_lock);
   reason_entry_t *e = reason_intern_locked(group, reason);
   if (e)
      e->count++;
   pthread_mutex_unlock(&g_reason_lock);
}

uint64_t gw_stat_get_reason(const char *group, const char *reason)
{
   uint64_t v = 0;
   pthread_mutex_lock(&g_reason_lock);
   reason_entry_t *e = reason_find_locked(group ? group : "", reason ? reason : "");
   if (e)
      v = e->count;
   pthread_mutex_unlock(&g_reason_lock);
   return v;
}

void gw_stat_record_token_delta(int baseline_tokens, int reduced_tokens)
{
   if (baseline_tokens < 0 || reduced_tokens < 0)
      return;
   /* Deterministic 1-in-N sample: only every Nth applied mutation is accumulated. */
   uint64_t n = atomic_fetch_add_explicit(&g_token_seen, 1, memory_order_relaxed);
   if (n % GW_STAT_TOKEN_SAMPLE_N != 0)
      return;
   /* Publish the sums BEFORE the count so a concurrent gw_stat_dump that reads count
    * then sums never sees a count ahead of its sums (at worst the sums are momentarily
    * ahead — a conservative mean, never a torn over-count). */
   atomic_fetch_add_explicit(&g_token_baseline_sum, (uint64_t)baseline_tokens,
                             memory_order_relaxed);
   atomic_fetch_add_explicit(&g_token_reduced_sum, (uint64_t)reduced_tokens, memory_order_relaxed);
   atomic_fetch_add_explicit(&g_token_sample_count, 1, memory_order_relaxed);
}

uint64_t gw_stat_token_sample_count(void)
{
   return atomic_load_explicit(&g_token_sample_count, memory_order_relaxed);
}
uint64_t gw_stat_token_baseline_sum(void)
{
   return atomic_load_explicit(&g_token_baseline_sum, memory_order_relaxed);
}
uint64_t gw_stat_token_reduced_sum(void)
{
   return atomic_load_explicit(&g_token_reduced_sum, memory_order_relaxed);
}

static const char *g_flat_names[GW_STAT__COUNT] = {
    "gateway_mutate_attempted", "gateway_mutate_applied",       "gateway_4xx_restore_resend",
    "gateway_5xx_disable",      "gateway_stream_error_disable", "gateway_session_disabled_blocks",
};

void gw_stat_dump(FILE *out)
{
   if (!out)
      return;
   for (int i = 0; i < GW_STAT__COUNT; i++)
   {
      uint64_t v = gw_stat_get((gw_stat_t)i);
      if (v)
         fprintf(out, "%s %llu\n", g_flat_names[i], (unsigned long long)v);
   }
   uint64_t sc = gw_stat_token_sample_count();
   if (sc)
   {
      fprintf(out, "gateway_token_delta_sample_count %llu\n", (unsigned long long)sc);
      fprintf(out, "gateway_token_delta_baseline_sum %llu\n",
              (unsigned long long)gw_stat_token_baseline_sum());
      fprintf(out, "gateway_token_delta_reduced_sum %llu\n",
              (unsigned long long)gw_stat_token_reduced_sum());
   }
   pthread_mutex_lock(&g_reason_lock);
   for (int i = 0; i < g_reason_n; i++)
   {
      if (!g_reasons[i].count)
         continue;
      /* All reasons are static literals today, but escape the label value so a
       * future dynamic caller cannot break the Prometheus line format (quotes,
       * backslashes, control chars). */
      char esc[GW_STAT_REASON_LEN * 2 + 1];
      size_t o = 0;
      for (const char *p = g_reasons[i].reason; *p && o + 2 < sizeof(esc); p++)
      {
         unsigned char c = (unsigned char)*p;
         if (c == '"' || c == '\\')
            esc[o++] = '\\';
         esc[o++] = (c < 0x20) ? '_' : (char)c;
      }
      esc[o] = '\0';
      fprintf(out, "gateway_%s{reason=\"%s\"} %llu\n", g_reasons[i].group, esc,
              (unsigned long long)g_reasons[i].count);
   }
   pthread_mutex_unlock(&g_reason_lock);
}

void gw_stat_reset(void)
{
   for (int i = 0; i < GW_STAT__COUNT; i++)
      atomic_store_explicit(&g_flat[i], 0, memory_order_relaxed);
   atomic_store_explicit(&g_token_seen, 0, memory_order_relaxed);
   atomic_store_explicit(&g_token_sample_count, 0, memory_order_relaxed);
   atomic_store_explicit(&g_token_baseline_sum, 0, memory_order_relaxed);
   atomic_store_explicit(&g_token_reduced_sum, 0, memory_order_relaxed);
   pthread_mutex_lock(&g_reason_lock);
   memset(g_reasons, 0, sizeof(g_reasons));
   g_reason_n = 0;
   pthread_mutex_unlock(&g_reason_lock);
}
