/* memory_conflict.c: session folding (L0 → L1 compression), conflict
 * detection, and retroactive conflict-detection. Extracted from
 * memory_logic.c. */
#include "aimee.h"
#include "cJSON.h"
#if !defined(AIMEE_DB2_DISABLED)
#include "db2/memory_conflicts.h"
#include "db2/memory_query.h"
#include "db2/memory_vectors.h"
#endif
#include "kb.h"
#include "log.h"
#include "memory.h"
#include "memory_context_internal.h"
#include "memory_ontology.h"
#include "platform_process.h"
#include "workspace.h"
#include <ctype.h>
#include <limits.h>
#include <math.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

#if !defined(AIMEE_DB2_DISABLED)
static time_t memory_conflict_parse_utc_timestamp(const char *s)
{
   if (!s || !s[0])
      return (time_t)0;

   int year = 0, month = 0, day = 0, hour = 0, min = 0, sec = 0;
   if (sscanf(s, "%d-%d-%dT%d:%d:%dZ", &year, &month, &day, &hour, &min, &sec) != 6)
      return (time_t)0;

   struct tm tm_buf;
   memset(&tm_buf, 0, sizeof(tm_buf));
   tm_buf.tm_year = year - 1900;
   tm_buf.tm_mon = month - 1;
   tm_buf.tm_mday = day;
   tm_buf.tm_hour = hour;
   tm_buf.tm_min = min;
   tm_buf.tm_sec = sec;
   tm_buf.tm_isdst = 0;
   return timegm(&tm_buf);
}
#endif

/* --- Session Folding --- */

int memory_fold_session(const char *session_id)
{
   if (!session_id)
      return -1;

#if defined(AIMEE_DB2_DISABLED)
   return -1;
#else
   db2_memory_session_l0_row_t rows[64];
   int n = db2_memory_session_l0_list(session_id, rows, (int)(sizeof(rows) / sizeof(rows[0])));

   char checkpoint[200];
   int pos = 0;
   int count = 0;
   for (int i = 0; i < n; i++)
   {
      if (!rows[i].key[0])
         continue;
      const char *text = rows[i].content[0] ? rows[i].content : rows[i].key;
      int remaining = (int)sizeof(checkpoint) - pos - 3;
      if (remaining <= 0)
         break;
      if (pos > 0)
      {
         checkpoint[pos++] = ';';
         checkpoint[pos++] = ' ';
         remaining -= 2;
      }
      int len = (int)strlen(text);
      if (len > remaining)
         len = remaining;
      memcpy(checkpoint + pos, text, len);
      pos += len;
      count++;
   }

   if (count == 0)
      return 0;

   checkpoint[pos] = '\0';

   /* INSERT as L1 episode */
   char ep_key[256];
   snprintf(ep_key, sizeof(ep_key), "session:%s", session_id);

   memory_insert(TIER_L1, KIND_EPISODE, ep_key, checkpoint, 0.8, session_id, NULL);

   db2_memory_session_l0_purge(session_id);
   return 0;
#endif
}

/* --- Conflict Detection --- */

int64_t memory_detect_conflict(const char *key, const char *content)
{
   if (!key || !content)
      return 0;
#if defined(AIMEE_DB2_DISABLED)
   return 0;
#else
   db2_memory_id_content_row_t rows[64];
   int n = db2_memory_list_by_key(key, rows, (int)(sizeof(rows) / sizeof(rows[0])));
   for (int i = 0; i < n; i++)
   {
      if (is_contradiction(rows[i].content, content))
         return rows[i].id;
   }
   return 0;
#endif
}

int memory_record_conflict(int64_t mem_a, int64_t mem_b)
{
#if defined(AIMEE_DB2_DISABLED)
   (void)mem_a;
   (void)mem_b;
   return -1;
#else
   if (db2_memory_conflict_record(mem_a, mem_b) != 0)
      return -1;

   /* Also write to contradiction_log audit trail */
   memory_log_contradiction(mem_a, mem_b, "pending", NULL);

   /* Auto-link: contradicts relationship */
   memory_link_create(mem_a, mem_b, "contradicts");

   /* Auto-create an epistemic directive so future turns can proactively
    * ask about the contradiction instead of silently dropping one side.
    * Dedup index on (cause, memory_a_id, memory_b_id) ensures replays are
    * idempotent. */
   {
      memory_t ra, rb;
      const char *key_a = "this";
      const char *content_a = "";
      const char *content_b = "";
      if (memory_get(mem_a, &ra) == 0)
      {
         key_a = ra.key;
         content_a = ra.content;
      }
      if (memory_get(mem_b, &rb) == 0)
         content_b = rb.content;
      memory_directive_record_contradiction(mem_a, mem_b, key_a, "", content_a, content_b, "");
   }

   return 0;
#endif
}

void memory_log_contradiction(int64_t mem_a, int64_t mem_b, const char *resolution,
                              const char *details)
{
#if !defined(AIMEE_DB2_DISABLED)
   db2_memory_contradiction_log(mem_a, mem_b, resolution, details);
#else
   (void)details;
#endif
   aimee_log(LOG_WARN, "memory_promote", "contradiction: memory %lld vs %lld (%s)",
             (long long)mem_a, (long long)mem_b, resolution ? resolution : "pending");
}

/* --- Retroactive Conflict Detection --- */

/* Check if enough time has passed since the last retroactive scan */
#if !defined(AIMEE_DB2_DISABLED)
static int retro_scan_due(void)
{
   char last[64];
   if (!db2_memory_last_retro_scan(last, sizeof(last)))
      return 1;
   time_t last_scan = memory_conflict_parse_utc_timestamp(last);
   time_t now = time(NULL);
   if (last_scan > (time_t)0 && now > last_scan && (now - last_scan) < RETRO_CONFLICT_INTERVAL)
      return 0;
   return 1;
}
#endif

/* Deep-tier precision guard. When the 4B deep tier is on and BOTH memories have a
 * deep embedding, a clearly-low deep cosine means the lexical overlap that made
 * them a candidate pair was coincidental — they aren't about the same thing, so
 * skip the costly is_contradiction check. Conservative threshold so real
 * contradictions (which are deep-similar) are never skipped; a no-op when deep is
 * off or either row lacks a deep embedding (partial coverage degrades gracefully). */
#define DEEP_CONFLICT_MIN_SIM 0.35

#if !defined(AIMEE_DB2_DISABLED)
static int deep_pair_clearly_unrelated(int deep_on, int64_t id_a, int64_t id_b)
{
   if (!deep_on)
      return 0;
   double sim = 0.0;
   if (pgvec_memory_vector_deep_similarity(id_a, id_b, &sim) == 1 && sim < DEEP_CONFLICT_MIN_SIM)
      return 1;
   return 0;
}
#endif

int memory_scan_retroactive_conflicts(void)
{
#if defined(AIMEE_DB2_DISABLED)
   return 0;
#else
   /* Rate limit: at most once per day */
   if (!retro_scan_due())
      return 0;

   /* Deep-tier precision guard on by config (stored deep embeddings only — no
    * embedder call). Loaded once; the per-pair check is cheap when off. */
   config_t deep_cfg;
   int deep_on = (config_load(&deep_cfg) == 0 && deep_cfg.memory_deep_embedding_enabled);

   /* Skip if fewer than RETRO_CONFLICT_MIN_L2 L2 memories */
   if (db2_memory_count_l2() < RETRO_CONFLICT_MIN_L2)
      return 0;

   int conflicts_found = 0;

   db2_memory_pair_row_t pairs[RETRO_CONFLICT_MAX_PAIRS];
   int pair_cap = (int)(sizeof(pairs) / sizeof(pairs[0]));

   /* 1. Cross-key scan: L2 memories with overlapping terms but different keys */
   {
      int n = db2_memory_l2_cross_key_pairs(RETRO_CONFLICT_MAX_PAIRS, pairs, pair_cap);
      for (int i = 0; i < n; i++)
      {
         if (deep_pair_clearly_unrelated(deep_on, pairs[i].id_a, pairs[i].id_b))
            continue;
         if (is_contradiction(pairs[i].content_a, pairs[i].content_b))
         {
            memory_record_conflict(pairs[i].id_a, pairs[i].id_b);
            conflicts_found++;
         }
      }
   }

   /* 2. Cross-kind scan: facts vs decisions */
   {
      int n = db2_memory_l2_fact_vs_decision_pairs(RETRO_CONFLICT_MAX_PAIRS, pairs, pair_cap);
      for (int i = 0; i < n; i++)
      {
         if (deep_pair_clearly_unrelated(deep_on, pairs[i].id_a, pairs[i].id_b))
            continue;
         if (is_contradiction(pairs[i].content_a, pairs[i].content_b))
         {
            memory_record_conflict(pairs[i].id_a, pairs[i].id_b);
            conflicts_found++;
         }
      }
   }

   /* Record scan marker in contradiction_log for rate limiting */
   {
      char ts[32];
      now_utc(ts, sizeof(ts));
      db2_memory_record_retro_scan_marker(ts);
   }

   if (conflicts_found > 0)
      aimee_log(LOG_INFO, "memory_promote", "retroactive scan: %d conflict(s) detected",
                conflicts_found);

   return conflicts_found;
#endif
}

int memory_list_conflicts(conflict_t *out, int max)
{
#if defined(AIMEE_DB2_DISABLED)
   (void)out;
   (void)max;
   return -1;
#else
   return db2_memory_conflict_list(out, max);
#endif
}

int memory_resolve_conflict(int64_t conflict_id, const char *resolution)
{
#if defined(AIMEE_DB2_DISABLED)
   (void)conflict_id;
   (void)resolution;
   return -1;
#else
   /* Fetch the conflict's memory IDs before resolving */
   int64_t mem_a = 0, mem_b = 0;
   db2_memory_conflict_get_pair(conflict_id, &mem_a, &mem_b);

   int changes = db2_memory_conflict_resolve(conflict_id, resolution);
   if (changes < 0)
      return -1;

   /* Log the resolution to the audit trail */
   if (changes > 0 && mem_a > 0)
   {
      memory_log_contradiction(mem_a, mem_b, resolution ? resolution : "resolved", NULL);
      /* Close out any open contradiction directive for this pair. */
      memory_directive_resolve_contradiction(mem_a, mem_b, 0, resolution ? resolution : "resolved");
   }

   return changes > 0 ? 0 : -1;
#endif
}
