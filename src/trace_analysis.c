/* trace_analysis.c: mine execution traces for recurring patterns */
#include "aimee.h"
#include "db1.h"
#include "modules/db2/c/anti_patterns.h"
#include "modules/db2/c/memory_payload.h"
#include "modules/db2/c/trace_mining.h"
#include "memory.h"
#include "trace_analysis.h"
#include "log.h"

#define RETRY_THRESHOLD    3   /* min repeated calls to flag as retry loop */
#define SEQUENCE_THRESHOLD 0.6 /* fraction for common sequence detection */
#define MAX_TRACES         512
#define MAX_SEQUENCES      128

/* A single trace row we care about */
typedef struct
{
   int64_t id;
   int plan_id;
   int turn;
   char direction[16];
   char tool_name[64];
   char tool_args[512];
   int has_error; /* 1 if tool_result contains error indicators */
} trace_row_t;

/* A tool pair for common sequence detection */
typedef struct
{
   char tool_a[64];
   char tool_b[64];
   int count;
   int total_plans;
} tool_pair_t;

/* Check if a tool result looks like an error */
static int result_looks_like_error(const char *result)
{
   if (!result || !*result)
      return 0;
   /* Common error indicators in tool results */
   if (strstr(result, "error") || strstr(result, "Error") || strstr(result, "ERROR"))
      return 1;
   if (strstr(result, "failed") || strstr(result, "Failed") || strstr(result, "FAILED"))
      return 1;
   if (strstr(result, "No such file") || strstr(result, "not found"))
      return 1;
   if (strstr(result, "Permission denied") || strstr(result, "command not found"))
      return 1;
   return 0;
}

/* trace-mining cursor lives in db1/trace_mining.{h,c} */

/* Load trace rows newer than last_mined_id. Reads come from DB1 via
 * db1_execution_trace_list_after_id; callers do not pass a handle. */
static int load_traces(int64_t after_id, trace_row_t *out, int max)
{
   db1_execution_trace_mining_row_t rows[MAX_TRACES];
   int loaded = db1_execution_trace_list_after_id(after_id, rows, max);
   if (loaded <= 0)
      return 0;

   int count = 0;
   while (count < loaded && count < max)
   {
      trace_row_t *r = &out[count];
      r->id = rows[count].id;
      r->plan_id = rows[count].plan_id;
      r->turn = rows[count].turn;
      snprintf(r->direction, sizeof(r->direction), "%s", rows[count].direction);
      snprintf(r->tool_name, sizeof(r->tool_name), "%s", rows[count].tool_name);
      snprintf(r->tool_args, sizeof(r->tool_args), "%s", rows[count].tool_args);
      r->has_error = result_looks_like_error(rows[count].tool_result);
      count++;
   }
   return count;
}

/* Check if an anti-pattern with this exact pattern text already exists */
static int anti_pattern_exists(const char *pattern)
{
   return db2_anti_pattern_exists_exact(pattern);
}

/* memory_key_exists moved to db2/memory_payload.c (db2_memory_key_exists). */

/* Detect retry loops: same tool with similar args called 3+ times with
 * failures. All anti-pattern reads/writes go through DB1 directly. */
static int detect_retry_loops(trace_row_t *traces, int count)
{
   int patterns = 0;

   for (int i = 0; i < count; i++)
   {
      if (!traces[i].tool_name[0])
         continue;

      /* Count consecutive calls to the same tool on the same plan */
      int run = 1;
      int errors = traces[i].has_error ? 1 : 0;

      for (int j = i + 1; j < count; j++)
      {
         if (traces[j].plan_id != traces[i].plan_id)
            break;
         if (strcmp(traces[j].tool_name, traces[i].tool_name) != 0)
            break;
         run++;
         if (traces[j].has_error)
            errors++;
      }

      if (run >= RETRY_THRESHOLD && errors >= 2)
      {
         char pattern[512];
         snprintf(pattern, sizeof(pattern), "Retry loop: %s called %d times with %d errors",
                  traces[i].tool_name, run, errors);

         if (!anti_pattern_exists(pattern))
         {
            char desc[1024];
            snprintf(desc, sizeof(desc),
                     "Tool '%s' was called %d consecutive times with %d failures."
                     " Consider a different approach after 2 failures.",
                     traces[i].tool_name, run, errors);

            db2_anti_pattern_insert(pattern, desc, "trace_mining", "", 0.7, NULL);
            patterns++;
         }

         /* Skip past this run */
         i += run - 1;
      }
   }

   return patterns;
}

/* Detect recovery sequences: tool A fails, then tool B succeeds on same plan */
static int detect_recovery_sequences(trace_row_t *traces, int count)
{
   int patterns = 0;
   const char *sid = session_id();

   for (int i = 0; i + 1 < count; i++)
   {
      if (!traces[i].has_error || !traces[i].tool_name[0])
         continue;
      if (traces[i + 1].plan_id != traces[i].plan_id)
         continue;
      if (traces[i + 1].has_error || !traces[i + 1].tool_name[0])
         continue;
      if (strcmp(traces[i].tool_name, traces[i + 1].tool_name) == 0)
         continue;

      char key[512];
      snprintf(key, sizeof(key), "recovery:%s->%s", traces[i].tool_name, traces[i + 1].tool_name);

      if (!db2_memory_key_exists(key))
      {
         char content[2048];
         snprintf(content, sizeof(content), "When '%s' fails, try '%s' as a recovery step.",
                  traces[i].tool_name, traces[i + 1].tool_name);

         memory_insert(TIER_L0, KIND_PROCEDURE, key, content, 0.7, sid, NULL);
         patterns++;
      }
   }

   return patterns;
}

/* Detect common sequences: tool A followed by tool B across multiple plans */
static int detect_common_sequences(trace_row_t *traces, int count)
{
   int patterns = 0;
   const char *sid = session_id();

   /* Collect unique plan IDs */
   int plan_ids[MAX_TRACES];
   int num_plans = 0;
   for (int i = 0; i < count; i++)
   {
      if (traces[i].plan_id == 0)
         continue;
      int found = 0;
      for (int p = 0; p < num_plans; p++)
      {
         if (plan_ids[p] == traces[i].plan_id)
         {
            found = 1;
            break;
         }
      }
      if (!found && num_plans < MAX_TRACES)
         plan_ids[num_plans++] = traces[i].plan_id;
   }

   if (num_plans < 3) /* need at least 3 plans for meaningful statistics */
      return 0;

   /* Count tool pair occurrences across plans */
   tool_pair_t pairs[MAX_SEQUENCES];
   memset(pairs, 0, sizeof(pairs));
   int num_pairs = 0;

   for (int i = 0; i + 1 < count; i++)
   {
      if (!traces[i].tool_name[0] || !traces[i + 1].tool_name[0])
         continue;
      if (traces[i].plan_id != traces[i + 1].plan_id)
         continue;
      if (traces[i].plan_id == 0)
         continue;

      /* Find or create this pair */
      int found = -1;
      for (int p = 0; p < num_pairs; p++)
      {
         if (strcmp(pairs[p].tool_a, traces[i].tool_name) == 0 &&
             strcmp(pairs[p].tool_b, traces[i + 1].tool_name) == 0)
         {
            found = p;
            break;
         }
      }

      if (found >= 0)
      {
         /* Check if this plan is already counted for this pair */
         /* Simple approach: just increment count (counts occurrences, not unique plans) */
         pairs[found].count++;
      }
      else if (num_pairs < MAX_SEQUENCES)
      {
         snprintf(pairs[num_pairs].tool_a, sizeof(pairs[num_pairs].tool_a), "%s",
                  traces[i].tool_name);
         snprintf(pairs[num_pairs].tool_b, sizeof(pairs[num_pairs].tool_b), "%s",
                  traces[i + 1].tool_name);
         pairs[num_pairs].count = 1;
         pairs[num_pairs].total_plans = num_plans;
         num_pairs++;
      }
   }

   /* Check which pairs appear in enough plans */
   for (int p = 0; p < num_pairs; p++)
   {
      /* Count unique plans containing this pair */
      int plans_with_pair = 0;
      for (int pi = 0; pi < num_plans; pi++)
      {
         int pid = plan_ids[pi];
         for (int i = 0; i + 1 < count; i++)
         {
            if (traces[i].plan_id != pid)
               continue;
            if (traces[i + 1].plan_id != pid)
               continue;
            if (strcmp(traces[i].tool_name, pairs[p].tool_a) == 0 &&
                strcmp(traces[i + 1].tool_name, pairs[p].tool_b) == 0)
            {
               plans_with_pair++;
               break;
            }
         }
      }

      double ratio = (double)plans_with_pair / num_plans;
      if (ratio < SEQUENCE_THRESHOLD)
         continue;

      char key[512];
      snprintf(key, sizeof(key), "sequence:%s->%s", pairs[p].tool_a, pairs[p].tool_b);

      if (!db2_memory_key_exists(key))
      {
         char content[2048];
         snprintf(content, sizeof(content),
                  "Common pattern: '%s' is typically followed by '%s'"
                  " (observed in %d/%d plans, %.0f%%).",
                  pairs[p].tool_a, pairs[p].tool_b, plans_with_pair, num_plans, ratio * 100);

         memory_insert(TIER_L0, KIND_PROCEDURE, key, content, 0.6 + ratio * 0.2, sid, NULL);
         patterns++;
      }
   }

   return patterns;
}

int trace_mine(void)
{

   int64_t last_id = db2_trace_mining_last_id();

   trace_row_t *traces = calloc(MAX_TRACES, sizeof(trace_row_t));
   if (!traces)
      return -1;

   int count = load_traces(last_id, traces, MAX_TRACES);
   if (count == 0)
   {
      free(traces);
      return 0;
   }

   int64_t max_id = traces[count - 1].id;

   int retry_patterns = detect_retry_loops(traces, count);
   int recovery_patterns = detect_recovery_sequences(traces, count);
   int sequence_patterns = detect_common_sequences(traces, count);

   free(traces);

   int total = retry_patterns + recovery_patterns + sequence_patterns;

   /* Record the mining run */
   db2_trace_mining_record(max_id);

   if (total > 0)
      aimee_log(LOG_INFO, "trace_analysis",
                "trace mining: %d patterns (%d retry, %d recovery, %d sequence)", total,
                retry_patterns, recovery_patterns, sequence_patterns);

   return total;
}
