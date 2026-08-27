/* memory_conflict.c: session folding (L0 → L1 compression), conflict
 * detection, and retroactive conflict-detection. Extracted from
 * memory_logic.c. */
#include "aimee.h"
#include "cJSON.h"
#include "modules/db2/c/memory_conflicts.h"
#include "modules/db2/c/memory_payload.h"
#include "modules/db2/c/memory_query.h"
#include "kb.h"
#include "log.h"
#include "memory.h"
#include "memory_context_internal.h"
#include "memory_ontology.h"
#include "platform_process.h"
#include <ctype.h>
#include <limits.h>
#include <math.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

/* Timestamp parsing is shared (parse_utc_ts in util.c). The copy that lived here
 * matched only the ISO spelling, and these columns also hold the canonical text
 * form that pg_now_text() writes -- the exact mirror of the space-only copy that
 * used to live in db2/demotion.c. Both returned 0 for the spelling they did not
 * know, and 0 is a real timestamp, so neither failed loudly. */

/* --- Session Folding --- */

#define FOLD_SOURCE_CAP 64

int memory_fold_session(const char *session_id, char *summary_out, size_t summary_out_len)
{
   if (summary_out && summary_out_len > 0)
      summary_out[0] = '\0';
   if (!session_id)
      return -1;

   /* Read one sentinel past the bounded pass. Seeing it means the pass is
    * incomplete, so defer the whole fold: a prefix must never be presented as
    * a complete session summary or used as permission to purge its suffix. */
   db2_memory_session_l0_row_t rows[FOLD_SOURCE_CAP + 1];
   int n = db2_memory_session_l0_list(session_id, rows, FOLD_SOURCE_CAP + 1);
   if (n > FOLD_SOURCE_CAP)
   {
      LOG_WARN("memory", "session fold refused for %.96s: source cap %d reached", session_id,
               FOLD_SOURCE_CAP);
      return -1;
   }
   if (n <= 0)
      return 0;

   int64_t source_ids[FOLD_SOURCE_CAP];
   for (int i = 0; i < n; i++)
      source_ids[i] = rows[i].id;
   if (!memory_derived_sources_allowed(source_ids, n))
   {
      LOG_WARN("memory", "session fold refused for %.96s by recursive lineage gate", session_id);
      return -1;
   }

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

   int episode_existed = db2_memory_key_exists(ep_key) == 1;
   memory_t episode = {0};
   if (memory_insert(TIER_L1, KIND_EPISODE, ep_key, checkpoint, 0.8, session_id, &episode) != 0 ||
       episode.id <= 0)
      return -1;

   for (int i = 0; i < n; i++)
   {
      char ref[48];
      snprintf(ref, sizeof(ref), "memory:%lld", (long long)source_ids[i]);
      if (memory_lineage_insert("memory", episode.id, "memory", ref, 0.8) < 0)
      {
         if (!episode_existed)
            (void)memory_delete(episode.id);
         LOG_WARN("memory", "session fold lineage write failed for derived memory %lld",
                  (long long)episode.id);
         return -1;
      }
   }

   /* Purge only the rows proven to be in this fold. New L0 rows arriving after
    * the bounded read remain for the next pass. */
   if (db2_memory_session_l0_purge(session_id, source_ids, n) != n)
   {
      LOG_WARN("memory", "session fold source purge failed for %.96s", session_id);
      return -1;
   }

   /* Only publish a digest after lineage and source cleanup both succeed; this
    * prevents a failed/partial fold from becoming downstream evidence. */
   if (summary_out && summary_out_len > 0)
      snprintf(summary_out, summary_out_len, "%s", checkpoint);
   return n;
}

/* --- Conflict Detection --- */

int64_t memory_detect_conflict(const char *key, const char *content)
{
   if (!key || !content)
      return 0;
   /* Drained in pages rather than read into one fixed buffer. The old form
    * stopped at the buffer size, so a heavily-revised key silently stopped
    * being checked past that point and the contradiction it held was never
    * found -- a missed row here is a missed contradiction, so the scan has to
    * be complete. */
   db2_memory_id_content_row_t rows[64];
   const int page = (int)(sizeof(rows) / sizeof(rows[0]));
   int64_t after_id = 0;
   for (;;)
   {
      int n = db2_memory_list_by_key_after(key, after_id, rows, page);
      for (int i = 0; i < n; i++)
      {
         if (is_contradiction(rows[i].content, content))
            return rows[i].id;
      }
      if (n < page)
         break;
      after_id = rows[n - 1].id;
   }
   return 0;
}

int memory_record_conflict(int64_t mem_a, int64_t mem_b)
{
   /* A pair someone already adjudicated is not raised again. Re-extraction
    * re-derives the same contradiction on every pass, and without this the
    * resolution is invisible to the write path: the queue refills with settled
    * questions and the detector's precision stops being measurable. Fails open
    * -- an unreadable store re-raises rather than swallowing a real one. */
   if (db2_memory_conflict_pair_resolved(mem_a, mem_b))
      return 0;

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
}

void memory_log_contradiction(int64_t mem_a, int64_t mem_b, const char *resolution,
                              const char *details)
{
   db2_memory_contradiction_log(mem_a, mem_b, resolution, details);
   aimee_log(LOG_WARN, "memory_promote", "contradiction: memory %lld vs %lld (%s)",
             (long long)mem_a, (long long)mem_b, resolution ? resolution : "pending");
}

/* --- Retroactive Conflict Detection --- */

/* Check if enough time has passed since the last retroactive scan */
static int retro_scan_due(void)
{
   char last[64];
   if (!db2_memory_last_retro_scan(last, sizeof(last)))
      return 1;
   time_t last_scan = parse_utc_ts(last);
   time_t now = time(NULL);
   if (last_scan > (time_t)0 && now > last_scan && (now - last_scan) < RETRO_CONFLICT_INTERVAL)
      return 0;
   return 1;
}

int memory_scan_retroactive_conflicts(void)
{
   /* Rate limit: at most once per day */
   if (!retro_scan_due())
      return 0;

   /* Skip if fewer than RETRO_CONFLICT_MIN_L2 L2 memories */
   if (db2_memory_count_l2() < RETRO_CONFLICT_MIN_L2)
      return 0;

   int conflicts_found = 0;

   db2_memory_pair_row_t pairs[RETRO_CONFLICT_MAX_PAIRS + 1];
   int pair_cap = RETRO_CONFLICT_MAX_PAIRS;

   /* 1. Cross-key scan: L2 memories with overlapping terms but different keys */
   {
      int n = db2_memory_l2_cross_key_pairs(pair_cap + 1, pairs, pair_cap + 1);
      if (n > pair_cap)
      {
         LOG_WARN("memory", "retroactive cross-key conflict batch truncated at %d pairs", pair_cap);
         n = pair_cap;
      }
      for (int i = 0; i < n; i++)
      {
         if (is_contradiction(pairs[i].content_a, pairs[i].content_b))
         {
            memory_record_conflict(pairs[i].id_a, pairs[i].id_b);
            conflicts_found++;
         }
      }
   }

   /* 2. Cross-kind scan: facts vs decisions */
   {
      int n = db2_memory_l2_fact_vs_decision_pairs(pair_cap + 1, pairs, pair_cap + 1);
      if (n > pair_cap)
      {
         LOG_WARN("memory", "retroactive cross-kind conflict batch truncated at %d pairs",
                  pair_cap);
         n = pair_cap;
      }
      for (int i = 0; i < n; i++)
      {
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
}

int memory_list_conflicts(conflict_t *out, int max)
{
   return db2_memory_conflict_list(out, max);
}

int memory_resolve_conflict(int64_t conflict_id, const char *resolution)
{
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
}
