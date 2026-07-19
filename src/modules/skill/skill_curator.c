/* skill_curator.c: weekly skill cluster analysis and consolidation planner.
 *
 * Phase 4 of the agent-self-improvement-skill-lifecycle proposal.
 *
 * Entry point: skill_curator_maybe() — idempotent, thread-safe, uses the
 * maintenance_state table as an idle guard so callers can invoke it on
 * every hook without burning cycles.
 *
 * Curator never deletes skills; archive is the maximum action it may propose. */
#include "aimee.h"
#include "util.h"
#include "skill.h"
#include "skill_curator.h"
#include "config.h"
#include "cJSON.h"
#include "db1_optional.h"
#include "log.h"
#include <pthread.h>
#include <string.h>
#include <time.h>

#define SC_CURATOR_KEY        "skill_curator"
#define SC_MIN_INTERVAL_HOURS 1

/* Thread-safe metrics. */
static pthread_mutex_t s_sc_mu = PTHREAD_MUTEX_INITIALIZER;
static int64_t s_sc_metric_runs = 0;
static int64_t s_sc_metric_skips = 0;
static int64_t s_sc_metric_clusters = 0;

void skill_curator_metrics(int64_t *runs, int64_t *skips, int64_t *clusters)
{
   pthread_mutex_lock(&s_sc_mu);
   if (runs)
      *runs = s_sc_metric_runs;
   if (skips)
      *skips = s_sc_metric_skips;
   if (clusters)
      *clusters = s_sc_metric_clusters;
   pthread_mutex_unlock(&s_sc_mu);
}

static void sc_metrics_record(int skipped, int clusters_found)
{
   pthread_mutex_lock(&s_sc_mu);
   s_sc_metric_runs++;
   if (skipped)
      s_sc_metric_skips++;
   else
      s_sc_metric_clusters += clusters_found;
   pthread_mutex_unlock(&s_sc_mu);
}

/* ------------------------------------------------------------------ */
/* Timestamp utilities                                                 */
/* ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ */
/* Skill name prefix extraction                                        */
/* ------------------------------------------------------------------ */

/* Extract the first hyphen-or-underscore delimited segment as the cluster
 * prefix.  "mcp-tools" -> "mcp", "git_verify" -> "git", "clip" -> "clip". */
static void sc_extract_prefix(const char *name, char *buf, size_t bufsz)
{
   if (!name || !buf || bufsz == 0)
      return;
   buf[0] = '\0';
   size_t i = 0;
   while (i < bufsz - 1 && name[i] && name[i] != '-' && name[i] != '_')
   {
      buf[i] = name[i];
      i++;
   }
   buf[i] = '\0';
}

static int sc_prefix_match(const char *a, const char *b)
{
   if (!a || !b)
      return 0;
   return strcmp(a, b) == 0;
}

/* ------------------------------------------------------------------ */
/* Description word overlap                                           */
/* ------------------------------------------------------------------ */

/* Split `desc` on whitespace; push unique tokens into `tokens` (up to max).
 * Returns count written. */
static int sc_tokenize(const char *desc, char tokens[][64], int max)
{
   if (!desc || !tokens || max <= 0)
      return 0;
   int n = 0;
   char work[1024];
   snprintf(work, sizeof(work), "%s", desc);
   char *save = NULL;
   char *tok = strtok_r(work, " \t\r\n", &save);
   while (tok && n < max)
   {
      int dup = 0;
      for (int j = 0; j < n; j++)
         if (strcasecmp(tokens[j], tok) == 0)
         {
            dup = 1;
            break;
         }
      if (!dup)
         snprintf(tokens[n++], 64, "%s", tok);
      tok = strtok_r(NULL, " \t\r\n", &save);
   }
   return n;
}

static double sc_word_overlap(const char *a, const char *b)
{
   if (!a || !b)
      return 0.0;
   char ta[128][64];
   char tb[128][64];
   int na = sc_tokenize(a, ta, 128);
   int nb = sc_tokenize(b, tb, 128);
   if (na == 0 || nb == 0)
      return 0.0;
   int shared = 0;
   for (int i = 0; i < na; i++)
      for (int j = 0; j < nb; j++)
         if (strcasecmp(ta[i], tb[j]) == 0)
         {
            shared++;
            break;
         }
   int smaller = na < nb ? na : nb;
   return (double)shared / (double)smaller;
}

/* ------------------------------------------------------------------ */
/* Cluster grouping                                                    */
/* ------------------------------------------------------------------ */

typedef struct
{
   char name[SKILL_NAME_MAX];
   char prefix[32];
   int use_count;
   int pinned;
   char created_by[16];
} sc_skill_info_t;

typedef struct
{
   char prefix[32];
   int indices[SKILL_MAX_SKILLS];
   int count;
   int all_low_usage;
   double overlap;
} sc_cluster_t;

static int sc_build_clusters(sc_skill_info_t *skills, int n, sc_cluster_t *clusters_out,
                             int max_clusters)
{
   if (!skills || n <= 0 || !clusters_out || max_clusters <= 0)
      return 0;

   char prefixes[SKILL_MAX_SKILLS][32];
   for (int i = 0; i < n; i++)
      sc_extract_prefix(skills[i].name, prefixes[i], 32);

   /* Group by prefix. */
   int added[SKILL_MAX_SKILLS] = {0};
   int nclusters = 0;

   for (int i = 0; i < n && nclusters < max_clusters; i++)
   {
      if (added[i])
         continue;

      /* Collect all skills sharing this prefix. */
      int idx[SKILL_MAX_SKILLS];
      int nc = 0;
      for (int j = i; j < n; j++)
      {
         if (added[j])
            continue;
         if (sc_prefix_match(prefixes[i], prefixes[j]))
            idx[nc++] = j;
      }

      if (nc < 3)
      {
         /* Mark all isolated entries so they're skipped. */
         for (int j = 0; j < nc; j++)
            added[idx[j]] = 1;
         continue;
      }

      /* Check all_low_usage (every one has use_count < 5). */
      int all_low = 1;
      double ov = 0.0;
      int ov_n = 0;

      for (int a = 0; a < nc; a++)
      {
         if (skills[idx[a]].use_count >= 5)
            all_low = 0;
         for (int b = a + 1; b < nc; b++)
         {
            char desc_a[512];
            char desc_b[512];
            desc_a[0] = '\0';
            desc_b[0] = '\0';
            (void)skill_description(NULL, skills[idx[a]].name, desc_a, sizeof(desc_a));
            (void)skill_description(NULL, skills[idx[b]].name, desc_b, sizeof(desc_b));
            ov += sc_word_overlap(desc_a, desc_b);
            ov_n++;
         }
      }

      double avg_ov = ov_n > 0 ? ov / (double)ov_n : 0.0;

      /* Actionable if: all low usage OR avg overlap >= 0.60 */
      if (!all_low && avg_ov < 0.60)
      {
         for (int j = 0; j < nc; j++)
            added[idx[j]] = 1;
         continue;
      }

      /* Clear only non-pinned entries. */
      int non_pinned = 0;
      for (int j = 0; j < nc; j++)
         if (!skills[idx[j]].pinned)
            non_pinned++;

      if (non_pinned == 0)
      {
         for (int j = 0; j < nc; j++)
            added[idx[j]] = 1;
         continue;
      }

      sc_cluster_t *c = &clusters_out[nclusters++];
      memset(c, 0, sizeof(*c));
      snprintf(c->prefix, sizeof(c->prefix), "%s", prefixes[i]);
      c->count = nc;
      c->all_low_usage = all_low;
      c->overlap = avg_ov;
      for (int j = 0; j < nc; j++)
         c->indices[j] = idx[j];

      for (int j = 0; j < nc; j++)
         added[idx[j]] = 1;
   }

   return nclusters;
}

/* ------------------------------------------------------------------ */
/* Persistence — same DB1 pattern as skill_capability_autostub         */
/* ------------------------------------------------------------------ */

/* Write consolidation plan to maintenance_state.last_summary_json as a
 * JSON artifact.  This matches how memory_maintenance.c stores its summary. */
static int sc_save_consolidation(const cJSON *plan, const char *snapshot_path, int elapsed_ms)
{
   if (!db1_maintenance_state_save)
      return 0;

   db1_maintenance_state_t st = {0};
   st.present = 1;
   now_utc(st.last_run_at, sizeof(st.last_run_at));
   st.last_elapsed_ms = (double)elapsed_ms;

   char plan_json[16384];
   if (plan)
   {
      char *raw = cJSON_PrintUnformatted(plan);
      if (raw)
      {
         snprintf(plan_json, sizeof(plan_json), "%s", raw);
         free(raw);
      }
      else
      {
         plan_json[0] = '\0';
      }
   }
   else
   {
      plan_json[0] = '\0';
   }
   snprintf(st.last_summary_json, sizeof(st.last_summary_json), "%s", plan_json);

   return db1_maintenance_state_save(SC_CURATOR_KEY, &st);
}

/* ------------------------------------------------------------------ */
/* Main entry point                                                    */
/* ------------------------------------------------------------------ */

int skill_curator_maybe(void)
{
   config_t cfg;
   config_load(&cfg);

   /* Disabled by default. */
   if (cfg.skills_curator_interval_hours <= 0)
      return 0;

   int interval_hours = cfg.skills_curator_interval_hours;
   if (interval_hours < SC_MIN_INTERVAL_HOURS)
      interval_hours = SC_MIN_INTERVAL_HOURS;

   /* Load idle guard timestamp. */
   db1_maintenance_state_t state = {0};
   if (db1_maintenance_state_load)
      (void)db1_maintenance_state_load(SC_CURATOR_KEY, &state);

   if (state.present)
   {
      time_t last = str_parse_iso8601(state.last_run_at);
      if (last != (time_t)0)
      {
         int elapsed_h = (int)difftime(time(NULL), last) / 3600;
         if (elapsed_h < interval_hours)
         {
            sc_metrics_record(1, 0);
            return 0;
         }
      }
   }

   /* TODO: call skill_snapshot_create(NULL) here if it exists in skill.c.
    * For now, log that a snapshot would be taken. */
   LOG_DEBUG("skill_curator", "skill_curator: would take pre-run snapshot");

   /* List all skills. */
   char names[SKILL_MAX_SKILLS][SKILL_NAME_MAX];
   int n = skill_list(NULL, names, SKILL_MAX_SKILLS);
   if (n <= 0)
   {
      if (n < 0)
         LOG_WARN("skill_curator", "skill_curator: skill_list failed");
      sc_save_consolidation(NULL, "", 0);
      sc_metrics_record(0, 0);

      return 0;
   }

   /* Build info array, excluding pinned and non-agent skills from low-usage
    * suggestions, but still including them for description-overlap detection. */
   sc_skill_info_t skills[SKILL_MAX_SKILLS];
   int ninfo = 0;
   for (int i = 0; i < n && ninfo < SKILL_MAX_SKILLS; i++)
   {
      memset(&skills[ninfo], 0, sizeof(skills[ninfo]));
      snprintf(skills[ninfo].name, sizeof(skills[ninfo].name), "%s", names[i]);
      skill_usage_t usage;
      if (skill_usage_get(NULL, names[i], &usage) == 0)
      {
         skills[ninfo].use_count = usage.use_count;
         skills[ninfo].pinned = usage.pinned;
         snprintf(skills[ninfo].created_by, sizeof(skills[ninfo].created_by), "%s",
                  usage.created_by);
      }
      ninfo++;
   }

   /* Cluster analysis. */
   sc_cluster_t clusters[64];
   int nclusters = sc_build_clusters(skills, ninfo, clusters, 64);

   /* Build JSON plan. */
   cJSON *root = cJSON_CreateObject();
   if (!root)
   {
      sc_metrics_record(0, 0);

      return 0;
   }
   cJSON_AddStringToObject(root, "kind", "skill_consolidation");
   cJSON_AddStringToObject(root, "action", "pending_review");

   char ts[64];
   now_utc(ts, sizeof(ts));
   cJSON_AddStringToObject(root, "generated_at", ts);

   char snapshot_note[256];
   snprintf(snapshot_note, sizeof(snapshot_note),
            "pre-run snapshot should be created by skill_snapshot_create");
   cJSON_AddStringToObject(root, "snapshot_note", snapshot_note);

   cJSON *arr = cJSON_AddArrayToObject(root, "clusters");
   for (int c = 0; c < nclusters; c++)
   {
      sc_cluster_t *cl = &clusters[c];

      /* Filter non-agent and pinned skills out of the suggestion. */
      int agent_count = 0;
      for (int j = 0; j < cl->count; j++)
         if (strcmp(skills[cl->indices[j]].created_by, "agent") == 0 &&
             !skills[cl->indices[j]].pinned)
            agent_count++;

      if (agent_count == 0)
         continue;

      cJSON *obj = cJSON_CreateObject();
      cJSON_AddStringToObject(obj, "prefix", cl->prefix);

      cJSON *skArr = cJSON_AddArrayToObject(obj, "skills");
      for (int j = 0; j < cl->count; j++)
         cJSON_AddItemToArray(skArr, cJSON_CreateString(skills[cl->indices[j]].name));

      if (cl->all_low_usage)
         cJSON_AddStringToObject(obj, "mode", "merge_into_umbrella");
      else
         cJSON_AddStringToObject(obj, "mode", "suggest_consolidation");

      cJSON_AddStringToObject(obj, "suggested_umbrella", cl->prefix);

      char reason[256];
      if (cl->all_low_usage)
         snprintf(reason, sizeof(reason), "prefix '%s' has %d skills with use_count < 5",
                  cl->prefix, cl->count);
      else
         snprintf(reason, sizeof(reason),
                  "prefix '%s' has %d skills with %.0f%% description word overlap", cl->prefix,
                  cl->count, cl->overlap * 100.0);
      cJSON_AddStringToObject(obj, "reason", reason);
      cJSON_AddItemToArray(arr, obj);
   }

   /* Save artifact. */
   sc_save_consolidation(root, "", 0);
   cJSON_Delete(root);

   LOG_INFO("skill_curator", "skill_curator: found %d consolidation clusters", nclusters);
   sc_metrics_record(0, nclusters);

   return 0;
}
