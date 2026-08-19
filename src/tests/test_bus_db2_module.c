#define _GNU_SOURCE

#include <aimee/core/event_bus/bus_client.h>
#include <aimee/core/event_bus/bus_endpoint.h>
#include <aimee/core/event_bus/bus_host.h>
#include <aimee/core/event_bus/bus_runtime.h>
#include <aimee/core/event_bus/module_client.h>
#include <aimee/core/event_bus/module_runtime.h>
#include <aimee/db2/client.h>
#include <aimee/db2/module_api.h>

#include "platform_test_util.h"

#include <assert.h>
#include <limits.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define MODULE_REF 29u
#define CALLER_REF 90u

typedef struct
{
   int (*is_initialized)(void);
   int (*health_probe)(int *schema_ok, int *have_pg_trgm);
   int (*kb_health_probe)(int *kb_tables_ok);
   int (*embedding_dimension)(void);
   int (*level3_count)(void);
   int (*level2_count)(void);
   int (*orphaned_l0_count)(void);
   int64_t (*total_count)(void);
   int (*session_l2_count)(const char *source_session);
   int (*key_exists)(const char *key);
   int64_t (*find_id_by_key_kind)(const char *key, const char *kind);
   int (*key_exists_in_tier_pair)(const char *key, const char *tier_a, const char *tier_b);
   int (*clear_effectiveness)(int64_t memory_id);
   int (*set_effectiveness)(int64_t memory_id, double value);
   int (*retention_delete)(const char *sensitivity, int days);
   int (*demote_effectiveness)(double threshold);
   int (*effectiveness_stats)(double low_threshold, double *avg_effectiveness,
                              int *low_effectiveness, int *high_impact);
   int (*list_l2_memory_ids)(int64_t *out, int max);
   int (*count_memories)(void);
   int (*count_recent_conflicts)(int days);
   void (*health_record)(int total_memories, int contradictions_detected, int promotions,
                         int demotions, int expirations);
   int (*prune_health)(int days);
   int (*prune_contradictions)(int days);
   int (*health_counters)(int promote_use_count, double promote_confidence,
                          aimee_db2_health_counters_t *counters);
   int (*stats_counts)(aimee_db2_memory_stats_t *stats);
   int (*delete_l0_provenance)(void);
   int (*delete_l0)(void);
   int (*list_kinds_in_tier)(const char *tier, char (*kinds)[16], int max);
   int (*kind_expire_days)(const char *kind);
   int (*delete_stale_l1_provenance)(const char *kind, const char *days_neg);
   int (*delete_stale_l1)(const char *kind, const char *days_neg);
   void (*now_utc)(char *buf, size_t len);
   int (*kind_demote_policy)(const char *kind, double *confidence, int *days);
   int (*demote_kind)(const char *ts, const char *kind, double confidence, const char *days_neg);
   int (*demote_cascade)(const char *ts);
   int (*promote_stable)(const char *ts);
   int (*reclassify_directives)(int require_approval);
   int (*record_l4_approval)(int64_t memory_id, const char *approver, const char *note);
   int (*prune_orphaned_l0)(void);
   int (*lifecycle_sweep_expired)(void);
   int (*demote_id)(int64_t memory_id);
   int (*has_workspace_tag)(int64_t memory_id);
   int (*delete_row)(int64_t memory_id);
   int (*touch)(int64_t memory_id);
   int (*link_delete)(int64_t link_id);
   int (*valid_at)(int64_t memory_id, const char *as_of);
   int (*has_scope_type)(int64_t memory_id, const char *scope_type);
   int (*reject)(int64_t memory_id);
   int (*update_content)(int64_t memory_id, const char *content);
   void (*decay_confidence)(int64_t memory_id);
   void (*workspace_tag_insert)(int64_t memory_id, const char *workspace);
   void (*set_cognified_kind)(int64_t memory_id, const char *kind);
   void (*set_source_session)(int64_t memory_id, const char *session_id);
   void (*negation_tokens_update)(int64_t memory_id, const char *tokens);
   int (*pool_status)(aimee_db2_pool_status_t *status);
   int (*embedding_refusals)(aimee_db2_embedding_refusals_t *status);
   int (*postgres_status)(aimee_db2_postgres_status_t *status);
   int (*reembed_status)(aimee_db2_reembed_status_t *status);
   int (*reembed_clear)(void);
   int (*reembed_clear_maintenance)(int force, int *was_in_progress, int *recorded, int *running);
   const char *(*embedder_serving_id)(void);
   int (*dimension_reset)(uint32_t target_dimension, uint32_t force, uint32_t dry_run,
                          aimee_db2_dimension_reset_t *status);
} aimee_db2_module_backend_t;

extern aimee_module_status_t aimee_module_handler(const aimee_module_invocation_t *invocation,
                                                  const uint8_t *request_body, uint32_t request_len,
                                                  uint8_t *response_body,
                                                  uint32_t response_capacity,
                                                  uint32_t *response_len, void *user_data);

typedef struct
{
   aimee_module_process_config_t config;
   int result;
} process_thread_t;

typedef struct
{
   bus_host_t *host;
   pthread_mutex_t *lock;
   atomic_int stop;
} pump_thread_t;

static int health_calls;
static int kb_health_calls;
static int initialized_calls;
static int embedding_dimension_calls;
static int level3_count_calls;
static int level2_count_calls;
static int orphaned_l0_count_calls;
static int prune_orphaned_l0_calls;
static int lifecycle_sweep_calls;
static int demote_id_calls;
static int64_t demote_id_last;
static int workspace_tag_calls;
static int delete_row_calls;
static int touch_calls;
static int64_t touch_last;
static int link_delete_calls;
static int64_t link_delete_last;
static int valid_at_calls;
static char valid_at_last[64];
static int scope_type_calls;
static char scope_type_last[64];
static int reject_calls;
static int64_t reject_last;
static int update_content_calls;
static char update_content_last[2048];
static int decay_confidence_calls;
static int64_t decay_confidence_last;
static int workspace_tag_insert_calls;
static char workspace_tag_insert_last[512];
static int cognified_kind_calls;
static char cognified_kind_last[32];
static int source_session_calls;
static char source_session_last[160];
static int negation_tokens_calls;
static char negation_tokens_last[2048];
static int total_count_calls;
static int session_l2_count_calls;
static int key_exists_calls;
static int find_id_by_key_kind_calls;
static int key_exists_in_tier_pair_calls;
static int clear_effectiveness_calls;
static int set_effectiveness_calls;
static int retention_delete_calls;
static int demote_effectiveness_calls;
static int effectiveness_stats_calls;
static int list_l2_memory_ids_calls;
static int health_record_calls;
static int health_record_total;
static int health_record_contradictions;
static int health_record_promotions;
static int prune_health_calls;
static int prune_contradictions_calls;
static int health_counters_calls;
static int stats_counts_calls;
static int expire_l0_provenance_calls;
static int expire_stale_provenance_calls;
static int demote_cascade_calls;
static char demote_kind_stamp[32];
static int promote_stable_calls;
static int reclassify_last_gate;
static int approval_calls;
static char approval_last_approver[64];
static atomic_int block_health;
static atomic_int health_entered;
static atomic_int health_release;

static int health_probe(int *schema_ok, int *have_pg_trgm)
{
   health_calls++;
   if (atomic_load_explicit(&block_health, memory_order_acquire))
   {
      const struct timespec pause = {.tv_sec = 0, .tv_nsec = 1000000};
      atomic_store_explicit(&health_entered, 1, memory_order_release);
      while (!atomic_load_explicit(&health_release, memory_order_acquire))
         nanosleep(&pause, NULL);
   }
   *schema_ok = 1;
   *have_pg_trgm = 0;
   return 0;
}

static int kb_health_probe(int *kb_tables_ok)
{
   kb_health_calls++;
   *kb_tables_ok = 1;
   return 0;
}

static int is_initialized(void)
{
   initialized_calls++;
   return 1;
}

int db2_is_initialized(void)
{
   return is_initialized();
}

int db2_health_probe(int *schema_ok, int *have_pg_trgm)
{
   return health_probe(schema_ok, have_pg_trgm);
}

int db2_kb_health_probe(int *kb_tables_ok)
{
   return kb_health_probe(kb_tables_ok);
}

int db2_embedding_dim(void)
{
   embedding_dimension_calls++;
   return 384;
}

static int embedding_dimension(void)
{
   embedding_dimension_calls++;
   return 384;
}

int db2_memory_count_l3(void)
{
   level3_count_calls++;
   return 42;
}

static int level3_count(void)
{
   level3_count_calls++;
   return 42;
}

int db2_memory_count_l2(void)
{
   level2_count_calls++;
   return 17;
}

static int level2_count(void)
{
   level2_count_calls++;
   return 17;
}

int db2_memory_count_orphaned_l0(void)
{
   orphaned_l0_count_calls++;
   return 5;
}

static int orphaned_l0_count(void)
{
   orphaned_l0_count_calls++;
   return 5;
}

int64_t db2_memory_count(void)
{
   total_count_calls++;
   return 1234567890123LL;
}

static int64_t total_count(void)
{
   total_count_calls++;
   return 1234567890123LL;
}

int db2_memory_count_l2_for_session(const char *source_session)
{
   session_l2_count_calls++;
   return strcmp(source_session, "session-123") == 0 ? 3 : 0;
}

static int session_l2_count(const char *source_session)
{
   session_l2_count_calls++;
   return strcmp(source_session, "session-123") == 0 ? 3 : 0;
}

int db2_memory_key_exists(const char *key)
{
   key_exists_calls++;
   return strcmp(key, "recovery:tool-a->tool-b") == 0 ? 1 : 0;
}

static int key_exists(const char *key)
{
   key_exists_calls++;
   return strcmp(key, "recovery:tool-a->tool-b") == 0 ? 1 : 0;
}

int64_t db2_memory_find_id_by_key_kind(const char *key, const char *kind)
{
   find_id_by_key_kind_calls++;
   return strcmp(key, "task:deploy-fix") == 0 && strcmp(kind, "task") == 0 ? 42 : 0;
}

static int64_t find_id_by_key_kind(const char *key, const char *kind)
{
   find_id_by_key_kind_calls++;
   return strcmp(key, "task:deploy-fix") == 0 && strcmp(kind, "task") == 0 ? 42 : 0;
}

int db2_memory_key_exists_in_tier_pair(const char *key, const char *tier_a, const char *tier_b)
{
   key_exists_in_tier_pair_calls++;
   return strcmp(key, "recovery:tool-a->tool-b") == 0 && strcmp(tier_a, "L3") == 0 &&
          strcmp(tier_b, "L4") == 0;
}

static int key_exists_in_tier_pair(const char *key, const char *tier_a, const char *tier_b)
{
   key_exists_in_tier_pair_calls++;
   return strcmp(key, "recovery:tool-a->tool-b") == 0 && strcmp(tier_a, "L3") == 0 &&
          strcmp(tier_b, "L4") == 0;
}

int db2_memory_health_clear_effectiveness(int64_t memory_id)
{
   clear_effectiveness_calls++;
   return memory_id == 42 ? 0 : -1;
}

int db2_memory_health_set_effectiveness(int64_t memory_id, double value)
{
   set_effectiveness_calls++;
   return memory_id == 42 && value == 0.75 ? 0 : -1;
}

static int clear_effectiveness(int64_t memory_id)
{
   clear_effectiveness_calls++;
   return memory_id == 42 ? 0 : -1;
}

static int set_effectiveness(int64_t memory_id, double value)
{
   set_effectiveness_calls++;
   return memory_id == 42 && value == 0.75 ? 0 : -1;
}

static int retention_delete_impl(const char *sensitivity, int days)
{
   retention_delete_calls++;
   if (strcmp(sensitivity, AIMEE_DB2_RETENTION_RESTRICTED) == 0)
      return days == (int)AIMEE_DB2_RETENTION_RESTRICTED_DAYS ? 2 : -1;
   if (strcmp(sensitivity, AIMEE_DB2_RETENTION_SENSITIVE) == 0)
      return days == (int)AIMEE_DB2_RETENTION_SENSITIVE_DAYS ? 3 : -1;
   return -1;
}

int db2_memory_health_delete_by_sensitivity(const char *sensitivity, int days)
{
   return retention_delete_impl(sensitivity, days);
}

static int retention_delete(const char *sensitivity, int days)
{
   return retention_delete_impl(sensitivity, days);
}

static int demote_effectiveness_impl(double threshold)
{
   demote_effectiveness_calls++;
   return threshold == AIMEE_DB2_EFFECTIVENESS_DEMOTE_THRESHOLD ? 2 : -1;
}

int db2_memory_health_demote_low_effectiveness(double threshold)
{
   return demote_effectiveness_impl(threshold);
}

static int demote_effectiveness(double threshold)
{
   return demote_effectiveness_impl(threshold);
}

static int effectiveness_stats_impl(double low_threshold, double *avg_effectiveness,
                                    int *low_effectiveness, int *high_impact)
{
   effectiveness_stats_calls++;
   if (low_threshold != AIMEE_DB2_EFFECTIVENESS_STATS_LOW_THRESHOLD)
      return -1;
   if (avg_effectiveness)
      *avg_effectiveness = 0.5;
   if (low_effectiveness)
      *low_effectiveness = 3;
   if (high_impact)
      *high_impact = 1;
   return 0;
}

int db2_memory_health_effectiveness_stats(double low_threshold, double *avg_effectiveness,
                                          int *low_effectiveness, int *high_impact)
{
   return effectiveness_stats_impl(low_threshold, avg_effectiveness, low_effectiveness,
                                   high_impact);
}

static int effectiveness_stats(double low_threshold, double *avg_effectiveness,
                               int *low_effectiveness, int *high_impact)
{
   return effectiveness_stats_impl(low_threshold, avg_effectiveness, low_effectiveness,
                                   high_impact);
}

static int list_l2_memory_ids_impl(int64_t *out, int max)
{
   list_l2_memory_ids_calls++;
   static const int64_t rows[] = {7, 19, 4242};
   int listed = 0;
   for (; listed < (int)(sizeof(rows) / sizeof(rows[0])) && listed < max; listed++)
      out[listed] = rows[listed];
   return listed;
}

int db2_memory_health_list_l2_memory_ids(int64_t *out, int max)
{
   return list_l2_memory_ids_impl(out, max);
}

static int list_l2_memory_ids(int64_t *out, int max)
{
   return list_l2_memory_ids_impl(out, max);
}

int db2_memory_health_count_memories(void)
{
   return 512;
}

static int count_memories(void)
{
   return 512;
}

int db2_memory_health_count_recent_conflicts(int days)
{
   return days == AIMEE_DB2_HEALTH_RECORD_CONFLICT_WINDOW_DAYS ? 6 : -1;
}

static int count_recent_conflicts(int days)
{
   return days == AIMEE_DB2_HEALTH_RECORD_CONFLICT_WINDOW_DAYS ? 6 : -1;
}

static void health_record_impl(int total_memories, int contradictions_detected, int promotions,
                               int demotions, int expirations)
{
   health_record_calls++;
   health_record_total = total_memories;
   health_record_contradictions = contradictions_detected;
   health_record_promotions = promotions;
   (void)demotions;
   (void)expirations;
}

void db2_memory_health_record(int total_memories, int contradictions_detected, int promotions,
                              int demotions, int expirations)
{
   health_record_impl(total_memories, contradictions_detected, promotions, demotions, expirations);
}

static void health_record(int total_memories, int contradictions_detected, int promotions,
                          int demotions, int expirations)
{
   health_record_impl(total_memories, contradictions_detected, promotions, demotions, expirations);
}

static int prune_health_impl(int days)
{
   prune_health_calls++;
   return days == AIMEE_DB2_HEALTH_RETENTION_SNAPSHOT_DAYS ? 11 : -1;
}

int db2_memory_health_prune_old(int days)
{
   return prune_health_impl(days);
}

static int prune_health(int days)
{
   return prune_health_impl(days);
}

static int prune_contradictions_impl(int days)
{
   prune_contradictions_calls++;
   return days == AIMEE_DB2_HEALTH_RETENTION_CONTRADICTION_DAYS ? 3 : -1;
}

int db2_memory_health_prune_old_contradictions(int days)
{
   return prune_contradictions_impl(days);
}

static int prune_contradictions(int days)
{
   return prune_contradictions_impl(days);
}

/* The counter struct is DB2-private, so the production symbol takes void * here
 * the way db2_dim_change_reset does; these tests drive their own backend. */
int db2_memory_health_query_counters(int promote_use_count, double promote_confidence, void *out)
{
   (void)promote_use_count;
   (void)promote_confidence;
   (void)out;
   return -1;
}

/* memory_stats_t is a host type, so the production symbol takes void * here the
 * way db2_dim_change_reset does; these tests drive their own backend. */
int db2_memory_stats_counts(void *out)
{
   (void)out;
   return -1;
}

int db2_memory_promotion_delete_l0_provenance(void)
{
   return 0;
}

int db2_memory_promotion_delete_l0(void)
{
   return 0;
}

int db2_memory_promotion_list_kinds_in_tier(const char *tier, void *out, int max)
{
   (void)tier;
   (void)out;
   (void)max;
   return 0;
}

int db2_memory_promotion_delete_stale_l1_provenance(const char *kind, const char *days_neg)
{
   (void)kind;
   (void)days_neg;
   return 0;
}

int db2_memory_promotion_delete_stale_l1(const char *kind, const char *days_neg)
{
   (void)kind;
   (void)days_neg;
   return 0;
}

int db2_kind_lifecycle_load(const char *kind, void *out)
{
   (void)kind;
   (void)out;
   return -1;
}

static int delete_l0_provenance(void)
{
   expire_l0_provenance_calls++;
   return 0;
}

static int delete_l0(void)
{
   return 9;
}

/* Both tier-cycle operations enumerate kinds through this one entry: expire
 * walks L1, demote walks L2. */
static int list_kinds_in_tier(const char *tier, char (*kinds)[16], int max)
{
   if (max < 2)
      return -1;
   if (strcmp(tier, AIMEE_DB2_EXPIRE_STALE_TIER) != 0 && strcmp(tier, AIMEE_DB2_DEMOTE_TIER) != 0)
      return -1;
   snprintf(kinds[0], sizeof(kinds[0]), "%s", "scratch");
   snprintf(kinds[1], sizeof(kinds[1]), "%s", "fact");
   return 2;
}

static int kind_expire_days(const char *kind)
{
   return strcmp(kind, "scratch") == 0 ? 7 : 30;
}

static int delete_stale_l1_provenance(const char *kind, const char *days_neg)
{
   (void)kind;
   (void)days_neg;
   expire_stale_provenance_calls++;
   return 0;
}

static int delete_stale_l1(const char *kind, const char *days_neg)
{
   if (strcmp(kind, "scratch") == 0)
      return strcmp(days_neg, "-7") == 0 ? 5 : -1;
   return strcmp(days_neg, "-30") == 0 ? 12 : -1;
}

int db2_memory_promotion_demote_kind(const char *ts, const char *kind, double confidence,
                                     const char *days_neg)
{
   (void)ts;
   (void)kind;
   (void)confidence;
   (void)days_neg;
   return 0;
}

int db2_memory_promotion_demote_cascade(const char *ts)
{
   (void)ts;
   return 0;
}

static void bus_now_utc(char *buf, size_t len)
{
   snprintf(buf, len, "%s", "2026-08-18T09:00:00Z");
}

static int kind_demote_policy(const char *kind, double *confidence, int *days)
{
   *confidence = strcmp(kind, "scratch") == 0 ? 0.4 : 0.6;
   *days = strcmp(kind, "scratch") == 0 ? 3 : 14;
   return 0;
}

static int demote_kind(const char *ts, const char *kind, double confidence, const char *days_neg)
{
   snprintf(demote_kind_stamp, sizeof(demote_kind_stamp), "%s", ts);
   if (strcmp(kind, "scratch") == 0)
      return (confidence == 0.4 && strcmp(days_neg, "-3") == 0) ? 4 : -1;
   return (confidence == 0.6 && strcmp(days_neg, "-14") == 0) ? 2 : -1;
}

static int demote_cascade(const char *ts)
{
   demote_cascade_calls++;
   /* The cascade must see exactly the stamp the demotions carried. */
   return strcmp(ts, demote_kind_stamp) == 0 ? 3 : -1;
}

int db2_memory_promotion_promote_stable_l2_to_l3(const char *ts)
{
   (void)ts;
   return 0;
}

static int promote_stable(const char *ts)
{
   promote_stable_calls++;
   return ts && ts[0] ? 4 : -1;
}

int db2_memory_promotion_reclassify_directives(int require_approval)
{
   (void)require_approval;
   return 0;
}

static int reclassify_directives(int require_approval)
{
   reclassify_last_gate = require_approval;
   /* The gated path promotes fewer rows than the open one. */
   return require_approval ? 3 : 7;
}

int db2_memory_promotion_record_l4_approval(int64_t memory_id, const char *approver,
                                            const char *note)
{
   (void)memory_id;
   (void)approver;
   (void)note;
   return -1;
}

static int record_l4_approval(int64_t memory_id, const char *approver, const char *note)
{
   approval_calls++;
   snprintf(approval_last_approver, sizeof(approval_last_approver), "%s", approver);
   return (memory_id == 42 && note && note[0]) ? 0 : -1;
}

int db2_memory_prune_orphaned_l0(void)
{
   prune_orphaned_l0_calls++;
   return 3;
}

static int prune_orphaned_l0(void)
{
   prune_orphaned_l0_calls++;
   return 3;
}

int db2_memory_lifecycle_sweep_expired(void)
{
   lifecycle_sweep_calls++;
   return 4;
}

static int lifecycle_sweep_expired(void)
{
   lifecycle_sweep_calls++;
   return 4;
}

int db2_memory_promotion_demote_id(int64_t memory_id)
{
   (void)memory_id;
   return 0;
}

static int demote_id(int64_t memory_id)
{
   demote_id_calls++;
   demote_id_last = memory_id;
   return 1;
}

int db2_memory_has_any_workspace_tag(int64_t memory_id)
{
   (void)memory_id;
   return 0;
}

static int has_workspace_tag(int64_t memory_id)
{
   workspace_tag_calls++;
   return memory_id == 42 ? 1 : 0;
}

int db2_memory_delete_row(int64_t memory_id)
{
   (void)memory_id;
   return 0;
}

static int delete_row(int64_t memory_id)
{
   delete_row_calls++;
   return memory_id == 42 ? 1 : 0;
}

int db2_memory_touch(int64_t memory_id)
{
   (void)memory_id;
   return -1;
}

static int touch(int64_t memory_id)
{
   touch_calls++;
   touch_last = memory_id;
   return memory_id == 42 ? 0 : -1;
}

int db2_memory_link_delete(int64_t link_id)
{
   (void)link_id;
   return -1;
}

static int link_delete(int64_t link_id)
{
   link_delete_calls++;
   link_delete_last = link_id;
   return link_id == 7 ? 0 : -1;
}

int db2_memory_valid_at(int64_t memory_id, const char *as_of)
{
   (void)memory_id;
   (void)as_of;
   return -1;
}

static int valid_at(int64_t memory_id, const char *as_of)
{
   valid_at_calls++;
   snprintf(valid_at_last, sizeof(valid_at_last), "%s", as_of);
   if (memory_id == 42)
      return 1;
   if (memory_id == 43)
      return 0;
   return -1;
}

int db2_memory_has_scope_type(int64_t memory_id, const char *scope_type)
{
   (void)memory_id;
   (void)scope_type;
   return 0;
}

static int has_scope_type(int64_t memory_id, const char *scope_type)
{
   (void)memory_id;
   scope_type_calls++;
   snprintf(scope_type_last, sizeof(scope_type_last), "%s", scope_type);
   return strcmp(scope_type, "workspace") == 0 ? 1 : 0;
}

int db2_memory_reject(int64_t memory_id, const char *reason)
{
   (void)memory_id;
   (void)reason;
   return -1;
}

static int reject(int64_t memory_id)
{
   reject_calls++;
   reject_last = memory_id;
   return memory_id == 42 ? 0 : -1;
}

int db2_memory_update_content(int64_t memory_id, const char *content)
{
   (void)memory_id;
   (void)content;
   return 0;
}

static int update_content(int64_t memory_id, const char *content)
{
   update_content_calls++;
   snprintf(update_content_last, sizeof(update_content_last), "%s", content);
   return memory_id == 42 ? 1 : 0;
}

void db2_memory_decay_confidence(int64_t memory_id)
{
   (void)memory_id;
}

static void decay_confidence(int64_t memory_id)
{
   decay_confidence_calls++;
   decay_confidence_last = memory_id;
}

void db2_memory_workspace_tag_insert(int64_t memory_id, const char *workspace)
{
   (void)memory_id;
   (void)workspace;
}

static void workspace_tag_insert(int64_t memory_id, const char *workspace)
{
   (void)memory_id;
   workspace_tag_insert_calls++;
   snprintf(workspace_tag_insert_last, sizeof(workspace_tag_insert_last), "%s", workspace);
}

void db2_memory_set_cognified_kind(int64_t memory_id, const char *kind)
{
   (void)memory_id;
   (void)kind;
}

static void set_cognified_kind(int64_t memory_id, const char *kind)
{
   (void)memory_id;
   cognified_kind_calls++;
   snprintf(cognified_kind_last, sizeof(cognified_kind_last), "%s", kind);
}

void db2_memory_set_source_session(int64_t memory_id, const char *session_id)
{
   (void)memory_id;
   (void)session_id;
}

static void set_source_session(int64_t memory_id, const char *session_id)
{
   (void)memory_id;
   source_session_calls++;
   snprintf(source_session_last, sizeof(source_session_last), "%s", session_id);
}

void db2_memory_negation_tokens_update(int64_t memory_id, const char *new_tokens)
{
   (void)memory_id;
   (void)new_tokens;
}

static void negation_tokens_update(int64_t memory_id, const char *tokens)
{
   (void)memory_id;
   negation_tokens_calls++;
   snprintf(negation_tokens_last, sizeof(negation_tokens_last), "%s", tokens);
}

static int stats_counts(aimee_db2_memory_stats_t *stats)
{
   stats_counts_calls++;
   *stats = (aimee_db2_memory_stats_t){
       .tier_counts = {3, 12, 30, 8, 2, 1},
       .kind_counts = {14, 5, 6, 9, 4, 3, 2, 1, 7, 5},
       .total = 56,
       .conflicts = 4,
   };
   return 0;
}

static int health_counters(int promote_use_count, double promote_confidence,
                           aimee_db2_health_counters_t *counters)
{
   health_counters_calls++;
   if (promote_use_count != (int)AIMEE_DB2_HEALTH_COUNTERS_PROMOTE_USE_COUNT ||
       promote_confidence != AIMEE_DB2_HEALTH_COUNTERS_PROMOTE_CONFIDENCE)
      return -1;
   *counters = (aimee_db2_health_counters_t){
       .cycles = 7,
       .total_contradictions = 13,
       .total_promotions = 5,
       .total_demotions = 2,
       .total_expirations = 4,
       .new_memories = 21,
       .l1_eligible = 9,
       .l2_total = 30,
       .l2_stale_30_days = 6,
   };
   return 0;
}

void db2_pool_stats(int *size, int *in_use, int *waiters, long *lease_grants, long *lease_timeouts,
                    long *stuck, long *poisoned)
{
   (void)size;
   (void)in_use;
   (void)waiters;
   (void)lease_grants;
   (void)lease_timeouts;
   (void)stuck;
   (void)poisoned;
}

static int pool_status(aimee_db2_pool_status_t *status)
{
   *status = (aimee_db2_pool_status_t){16, 2, 1, 10, 3, 4, 5};
   return 0;
}

long long db2_embedding_dim_refused_count(void)
{
   return 7;
}

int db2_embedding_dim_last_offered(void)
{
   return 768;
}

static int embedding_refusals(aimee_db2_embedding_refusals_t *status)
{
   *status = (aimee_db2_embedding_refusals_t){7, 768};
   return 0;
}

int db2_pg_stat_summary(int *active, int *maximum, int *replica, int64_t *lag)
{
   if (active)
      *active = 12;
   if (maximum)
      *maximum = 100;
   if (replica)
      *replica = 1;
   if (lag)
      *lag = 1048576;
   return 0;
}

static int postgres_status(aimee_db2_postgres_status_t *status)
{
   *status = (aimee_db2_postgres_status_t){15, 12, 100, 1, 1048576};
   return 0;
}

int db2_reembed_in_progress_get(int *target, long *started)
{
   if (target)
      *target = 384;
   if (started)
      *started = 1700000000;
   return 1;
}

static int reembed_status(aimee_db2_reembed_status_t *status)
{
   *status = (aimee_db2_reembed_status_t){384, 1700000000};
   return 1;
}

static int dimension_reset(uint32_t target_dimension, uint32_t force, uint32_t dry_run,
                           aimee_db2_dimension_reset_t *status)
{
   assert(target_dimension == 384 && force == 0 && dry_run == 1);
   *status = (aimee_db2_dimension_reset_t){768, 384, 6, 0, 1234, -1, 7};
   return 0;
}

int db2_reembed_in_progress_clear(void)
{
   return 0;
}

int db2_reembed_clear_maintenance(int force, int *was_in_progress, int *recorded, int *running)
{
   (void)force;
   if (was_in_progress)
      *was_in_progress = 1;
   if (recorded)
      *recorded = 384;
   if (running)
      *running = 384;
   return 0;
}

const char *db2_embedder_serving_id(void)
{
   static char serving_id[AIMEE_DB2_EMBEDDER_SERVING_ID_MAX + 1];
   if (serving_id[0] == '\0')
   {
      memset(serving_id, 'x', sizeof(serving_id) - 1);
      serving_id[sizeof(serving_id) - 1] = '\0';
   }
   return serving_id;
}

int db2_probe_embedder_dim(int budget_ms, int *out)
{
   (void)budget_ms;
   if (out)
      *out = 384;
   return 0;
}

int db2_dim_change_reset(int target_dim, int force, int dry_run, void *out)
{
   (void)target_dim;
   (void)force;
   (void)dry_run;
   (void)out;
   return -1;
}

static void *run_process(void *argument)
{
   process_thread_t *thread = argument;
   thread->result = aimee_module_process_run(&thread->config);
   return NULL;
}

static void pump(bus_host_t *host, pthread_mutex_t *lock)
{
   pthread_mutex_lock(lock);
   (void)bus_host_pump(host);
   pthread_mutex_unlock(lock);
}

static void *run_pump(void *argument)
{
   pump_thread_t *state = argument;
   const struct timespec pause = {.tv_sec = 0, .tv_nsec = 1000000};
   while (!atomic_load_explicit(&state->stop, memory_order_acquire))
   {
      pump(state->host, state->lock);
      nanosleep(&pause, NULL);
   }
   return NULL;
}

typedef struct
{
   atomic_int *cancel;
   int entered;
} cancel_inflight_t;

static int cancellation_flag(void *context)
{
   return atomic_load_explicit((atomic_int *)context, memory_order_acquire);
}

static void *cancel_inflight(void *argument)
{
   cancel_inflight_t *state = argument;
   const struct timespec pause = {.tv_sec = 0, .tv_nsec = 1000000};
   for (int attempt = 0; attempt < 5000; ++attempt)
   {
      if (atomic_load_explicit(&health_entered, memory_order_acquire))
      {
         state->entered = 1;
         break;
      }
      nanosleep(&pause, NULL);
   }
   atomic_store_explicit(state->cancel, 1, memory_order_release);
   for (int attempt = 0; attempt < 10; ++attempt)
      nanosleep(&pause, NULL);
   atomic_store_explicit(&health_release, 1, memory_order_release);
   return NULL;
}

static void wait_for_clients(bus_host_t *host, pthread_mutex_t *lock)
{
   const struct timespec pause = {.tv_sec = 0, .tv_nsec = 1000000};
   for (int attempt = 0; attempt < 2000; ++attempt)
   {
      pthread_mutex_lock(lock);
      uint32_t admitted = host->admitted;
      pthread_mutex_unlock(lock);
      if (admitted >= 2)
         return;
      nanosleep(&pause, NULL);
   }
   assert(!"timed out waiting for DB2 module clients");
}

static aimee_module_call_result_t
call_client(void *context, uint32_t event_kind, uint32_t stage_id, uint64_t trace_id,
            uint64_t deadline_ns, const void *request_body, uint32_t request_len,
            void *response_body, uint32_t response_capacity, uint32_t *response_len,
            aimee_module_cancelled_fn cancelled, void *cancel_context)
{
   return aimee_module_client_call(context, event_kind, stage_id, trace_id, deadline_ns,
                                   request_body, request_len, response_body, response_capacity,
                                   response_len, cancelled, cancel_context);
}

int main(void)
{
   char directory[256];
   snprintf(directory, sizeof(directory), "%s/aimee-db2-module-bus-XXXXXX", platform_tmpdir());
   assert(mkdtemp(directory) != NULL);
   char socket_path[512], executable[PATH_MAX];
   assert(snprintf(socket_path, sizeof(socket_path), "%s/module.sock", directory) > 0);
   assert(realpath("/proc/self/exe", executable) != NULL);

   const uint32_t served[] = {AIMEE_DB2_EVENT_HEALTH, AIMEE_DB2_EVENT_LEVEL3_COUNT};
   bus_runtime_grant_t grants[] = {
       {.principal_class = 1,
        .principal_ref = MODULE_REF,
        .uid = BUS_RUNTIME_SELF_UID,
        .executable = executable,
        .serve = served,
        .serve_count = 2},
       {.principal_class = 1,
        .principal_ref = CALLER_REF,
        .uid = BUS_RUNTIME_SELF_UID,
        .executable = executable,
        .request = served,
        .request_count = 2},
   };
   bus_host_config_t host_config = {.max_slots = 4,
                                    .slot_size = 256,
                                    .inline_budget = 128,
                                    .queue_capacity = 8,
                                    .arena_size = 4096};
   bus_host_t host;
   assert(bus_host_create(&host, &host_config, NULL, NULL) == BUS_HOST_OK);
   pthread_mutex_t host_lock = PTHREAD_MUTEX_INITIALIZER;
   bus_runtime_config_t runtime_config = {.socket_path = socket_path,
                                          .socket_mode = 0600,
                                          .backlog = 4,
                                          .stale_after_ns = 5000000000ULL,
                                          .grants = grants,
                                          .grant_count = 2};
   bus_runtime_t *runtime = bus_runtime_start(&host, &host_lock, &runtime_config);
   assert(runtime != NULL);

   static const aimee_module_stage_t stages[] = {
       {AIMEE_DB2_EVENT_HEALTH, AIMEE_DB2_STAGE_HEALTH},
       {AIMEE_DB2_EVENT_LEVEL3_COUNT, AIMEE_DB2_STAGE_LEVEL3_COUNT},
   };
   static const aimee_db2_module_backend_t backend = {
       .is_initialized = is_initialized,
       .health_probe = health_probe,
       .kb_health_probe = kb_health_probe,
       .embedding_dimension = embedding_dimension,
       .level3_count = level3_count,
       .level2_count = level2_count,
       .orphaned_l0_count = orphaned_l0_count,
       .total_count = total_count,
       .session_l2_count = session_l2_count,
       .key_exists = key_exists,
       .find_id_by_key_kind = find_id_by_key_kind,
       .key_exists_in_tier_pair = key_exists_in_tier_pair,
       .clear_effectiveness = clear_effectiveness,
       .set_effectiveness = set_effectiveness,
       .retention_delete = retention_delete,
       .demote_effectiveness = demote_effectiveness,
       .effectiveness_stats = effectiveness_stats,
       .list_l2_memory_ids = list_l2_memory_ids,
       .count_memories = count_memories,
       .count_recent_conflicts = count_recent_conflicts,
       .health_record = health_record,
       .prune_health = prune_health,
       .prune_contradictions = prune_contradictions,
       .health_counters = health_counters,
       .stats_counts = stats_counts,
       .delete_l0_provenance = delete_l0_provenance,
       .delete_l0 = delete_l0,
       .list_kinds_in_tier = list_kinds_in_tier,
       .kind_expire_days = kind_expire_days,
       .delete_stale_l1_provenance = delete_stale_l1_provenance,
       .delete_stale_l1 = delete_stale_l1,
       .now_utc = bus_now_utc,
       .kind_demote_policy = kind_demote_policy,
       .demote_kind = demote_kind,
       .demote_cascade = demote_cascade,
       .promote_stable = promote_stable,
       .reclassify_directives = reclassify_directives,
       .record_l4_approval = record_l4_approval,
       .prune_orphaned_l0 = prune_orphaned_l0,
       .lifecycle_sweep_expired = lifecycle_sweep_expired,
       .demote_id = demote_id,
       .has_workspace_tag = has_workspace_tag,
       .delete_row = delete_row,
       .touch = touch,
       .link_delete = link_delete,
       .valid_at = valid_at,
       .has_scope_type = has_scope_type,
       .reject = reject,
       .update_content = update_content,
       .decay_confidence = decay_confidence,
       .workspace_tag_insert = workspace_tag_insert,
       .set_cognified_kind = set_cognified_kind,
       .set_source_session = set_source_session,
       .negation_tokens_update = negation_tokens_update,
       .pool_status = pool_status,
       .embedding_refusals = embedding_refusals,
       .postgres_status = postgres_status,
       .reembed_status = reembed_status,
       .reembed_clear = db2_reembed_in_progress_clear,
       .reembed_clear_maintenance = db2_reembed_clear_maintenance,
       .embedder_serving_id = db2_embedder_serving_id,
       .dimension_reset = dimension_reset,
   };
   process_thread_t process = {
       .config = {.socket_path = socket_path,
                  .module_name = "db2",
                  .principal_class = 1,
                  .principal_ref = MODULE_REF,
                  .stages = stages,
                  .stage_count = 2,
                  .handler = aimee_module_handler,
                  .user_data = (void *)&backend},
   };
   pthread_t module_thread;
   assert(pthread_create(&module_thread, NULL, run_process, &process) == 0);

   int caller_fd = -1;
   bus_client_t caller;
   assert(bus_endpoint_connect(socket_path, &caller_fd) == 0);
   assert(bus_client_attach_as(caller_fd, &caller, 1, CALLER_REF) == BUS_CLIENT_OK);
   assert(bus_endpoint_close(&caller_fd) == 0);
   wait_for_clients(&host, &host_lock);

   pump_thread_t pump_state = {.host = &host, .lock = &host_lock};
   atomic_init(&pump_state.stop, 0);
   pthread_t pump_thread;
   assert(pthread_create(&pump_thread, NULL, run_pump, &pump_state) == 0);

   aimee_module_client_t client;
   assert(aimee_module_client_init(&client, &caller) == 0);
   int schema_ok = 0, have_pg_trgm = 1, kb_tables_ok = 0;
   assert(aimee_db2_health_call(call_client, &client, 7001, 0, &schema_ok, &have_pg_trgm,
                                &kb_tables_ok, NULL, NULL) == AIMEE_MODULE_CALL_OK);
   assert(schema_ok == 1 && have_pg_trgm == 0 && kb_tables_ok == 1);
   assert(initialized_calls == 1 && health_calls == 1 && kb_health_calls == 1);

   uint32_t domain_result = 9, dimension = 9;
   assert(aimee_db2_embedding_dimension_call(call_client, &client, 7010, 0, &domain_result,
                                             &dimension, NULL, NULL) == AIMEE_MODULE_CALL_OK);
   assert(domain_result == AIMEE_DB2_RESULT_OK && dimension == 384);
   assert(embedding_dimension_calls == 1);

   uint32_t level3_total = 99;
   assert(aimee_db2_level3_count_call(call_client, &client, 7020, 0, &level3_total, NULL, NULL) ==
          AIMEE_MODULE_CALL_OK);
   assert(level3_total == 42 && level3_count_calls == 1);

   uint32_t level2_total = 99;
   assert(aimee_db2_level2_count_call(call_client, &client, 7021, 0, &level2_total, NULL, NULL) ==
          AIMEE_MODULE_CALL_OK);
   assert(level2_total == 17 && level2_count_calls == 1);

   uint32_t orphaned_l0_total = 99;
   assert(aimee_db2_orphaned_l0_count_call(call_client, &client, 7022, 0, &orphaned_l0_total, NULL,
                                           NULL) == AIMEE_MODULE_CALL_OK);
   assert(orphaned_l0_total == 5 && orphaned_l0_count_calls == 1);

   uint64_t memory_total = 99;
   assert(aimee_db2_total_count_call(call_client, &client, 7023, 0, &memory_total, NULL, NULL) ==
          AIMEE_MODULE_CALL_OK);
   assert(memory_total == 1234567890123ULL && total_count_calls == 1);

   uint32_t session_l2_total = 99;
   assert(aimee_db2_session_l2_count_call(call_client, &client, 7024, 0, "session-123",
                                          &session_l2_total, NULL, NULL) == AIMEE_MODULE_CALL_OK);
   assert(session_l2_total == 3 && session_l2_count_calls == 1);

   uint32_t exists = 99;
   assert(aimee_db2_key_exists_call(call_client, &client, 7025, 0, "recovery:tool-a->tool-b",
                                    &exists, NULL, NULL) == AIMEE_MODULE_CALL_OK);
   assert(exists == 1 && key_exists_calls == 1);

   uint32_t found = 99;
   uint64_t memory_id = 99;
   assert(aimee_db2_find_id_by_key_kind_call(call_client, &client, 7026, 0, "task:deploy-fix",
                                             "task", &found, &memory_id, NULL,
                                             NULL) == AIMEE_MODULE_CALL_OK);
   assert(found == 1 && memory_id == 42 && find_id_by_key_kind_calls == 1);

   exists = 99;
   assert(aimee_db2_key_exists_in_tier_pair_call(call_client, &client, 7027, 0,
                                                 "recovery:tool-a->tool-b", "L3", "L4", &exists,
                                                 NULL, NULL) == AIMEE_MODULE_CALL_OK);
   assert(exists == 1 && key_exists_in_tier_pair_calls == 1);

   domain_result = 99;
   assert(aimee_db2_effectiveness_update_call(call_client, &client, 7028, 0, 42, 1, 0.75,
                                              &domain_result, NULL, NULL) == AIMEE_MODULE_CALL_OK);
   assert(domain_result == AIMEE_DB2_RESULT_OK && set_effectiveness_calls == 1);

   uint32_t deleted_count = 99;
   assert(aimee_db2_retention_enforce_call(call_client, &client, 7029, 0, &deleted_count, NULL,
                                           NULL) == AIMEE_MODULE_CALL_OK);
   assert(deleted_count == 5 && retention_delete_calls == 2);

   uint32_t demoted_count = 99;
   assert(aimee_db2_effectiveness_demote_call(call_client, &client, 7030, 0, &demoted_count, NULL,
                                              NULL) == AIMEE_MODULE_CALL_OK);
   assert(demoted_count == 2 && demote_effectiveness_calls == 1);

   aimee_db2_effectiveness_stats_t stats = {0};
   assert(aimee_db2_effectiveness_stats_call(call_client, &client, 7031, 0, &stats, NULL, NULL) ==
          AIMEE_MODULE_CALL_OK);
   assert(stats.avg_effectiveness == 0.5 && stats.low_effectiveness_count == 3 &&
          stats.high_impact_count == 1 && effectiveness_stats_calls == 1);

   uint64_t l2_ids[AIMEE_DB2_L2_MEMORY_IDS_MAX];
   uint32_t l2_count = 99;
   assert(aimee_db2_l2_memory_ids_call(call_client, &client, 7032, 0, l2_ids,
                                       AIMEE_DB2_L2_MEMORY_IDS_MAX, &l2_count, NULL,
                                       NULL) == AIMEE_MODULE_CALL_OK);
   assert(l2_count == 3 && l2_ids[0] == 7 && l2_ids[1] == 19 && l2_ids[2] == 4242 &&
          list_l2_memory_ids_calls == 1);

   assert(aimee_db2_health_record_call(call_client, &client, 7033, 0, 4u, 2u, 9u, NULL, NULL) ==
          AIMEE_MODULE_CALL_OK);
   assert(health_record_calls == 1 && health_record_total == 512 &&
          health_record_contradictions == 6 && health_record_promotions == 4);

   uint32_t snapshots_deleted = 99, contradictions_deleted = 99;
   assert(aimee_db2_health_retention_call(call_client, &client, 7034, 0, &snapshots_deleted,
                                          &contradictions_deleted, NULL,
                                          NULL) == AIMEE_MODULE_CALL_OK);
   assert(snapshots_deleted == 11 && contradictions_deleted == 3 && prune_health_calls == 1 &&
          prune_contradictions_calls == 1);

   aimee_db2_health_counters_t counters = {0};
   assert(aimee_db2_health_counters_call(call_client, &client, 7035, 0, &counters, NULL, NULL) ==
          AIMEE_MODULE_CALL_OK);
   assert(counters.cycles == 7 && counters.total_contradictions == 13 &&
          counters.l2_stale_30_days == 6 && health_counters_calls == 1);

   aimee_db2_memory_stats_t corpus = {0};
   assert(aimee_db2_stats_counts_call(call_client, &client, 7036, 0, &corpus, NULL, NULL) ==
          AIMEE_MODULE_CALL_OK);
   assert(corpus.tier_counts[0] == 3 && corpus.tier_counts[5] == 1 && corpus.kind_counts[0] == 14 &&
          corpus.kind_counts[9] == 5 && corpus.total == 56 && corpus.conflicts == 4 &&
          stats_counts_calls == 1);

   uint32_t level0_deleted = 99, stale_deleted = 99;
   assert(aimee_db2_expire_call(call_client, &client, 7037, 0, &level0_deleted, &stale_deleted,
                                NULL, NULL) == AIMEE_MODULE_CALL_OK);
   /* Each kind expires on its own window: scratch at -7, fact at -30. */
   assert(level0_deleted == 9 && stale_deleted == 17 && expire_l0_provenance_calls == 1 &&
          expire_stale_provenance_calls == 2);

   uint32_t tier_demoted = 99, tier_cascaded = 99;
   assert(aimee_db2_demote_call(call_client, &client, 7038, 0, &tier_demoted, &tier_cascaded, NULL,
                                NULL) == AIMEE_MODULE_CALL_OK);
   /* Both kinds demote on their own threshold and window, then one cascade
    * runs against the stamp they shared. */
   assert(tier_demoted == 6 && tier_cascaded == 3 && demote_cascade_calls == 1);

   uint32_t tier_promoted = 99;
   assert(aimee_db2_promote_stable_call(call_client, &client, 7039, 0, &tier_promoted, NULL,
                                        NULL) == AIMEE_MODULE_CALL_OK);
   assert(tier_promoted == 4 && promote_stable_calls == 1);

   uint32_t reclassified = 99;
   assert(aimee_db2_reclassify_directives_call(call_client, &client, 7040, 0, 1u, &reclassified,
                                               NULL, NULL) == AIMEE_MODULE_CALL_OK);
   assert(reclassified == 3 && reclassify_last_gate == 1);
   assert(aimee_db2_reclassify_directives_call(call_client, &client, 7041, 0, 0u, &reclassified,
                                               NULL, NULL) == AIMEE_MODULE_CALL_OK);
   assert(reclassified == 7 && reclassify_last_gate == 0);

   assert(aimee_db2_record_l4_approval_call(call_client, &client, 7042, 0, 42u, "operator",
                                            "reviewed", NULL, NULL) == AIMEE_MODULE_CALL_OK);
   assert(approval_calls == 1 && strcmp(approval_last_approver, "operator") == 0);

   uint32_t pruned = 99u;
   assert(aimee_db2_prune_orphaned_l0_call(call_client, &client, 7043, 0, &pruned, NULL, NULL) ==
          AIMEE_MODULE_CALL_OK);
   assert(pruned == 3 && prune_orphaned_l0_calls == 1);

   uint32_t archived = 99u;
   assert(aimee_db2_lifecycle_sweep_expired_call(call_client, &client, 7044, 0, &archived, NULL,
                                                 NULL) == AIMEE_MODULE_CALL_OK);
   assert(archived == 4 && lifecycle_sweep_calls == 1);

   uint32_t decayed = 99u;
   assert(aimee_db2_demote_id_call(call_client, &client, 7045, 0, 42u, &decayed, NULL, NULL) ==
          AIMEE_MODULE_CALL_OK);
   assert(decayed == 1 && demote_id_calls == 1 && demote_id_last == 42);

   uint32_t tagged = 99u;
   assert(aimee_db2_has_workspace_tag_call(call_client, &client, 7046, 0, 42u, &tagged, NULL,
                                           NULL) == AIMEE_MODULE_CALL_OK);
   assert(tagged == 1 && workspace_tag_calls == 1);
   assert(aimee_db2_has_workspace_tag_call(call_client, &client, 7047, 0, 43u, &tagged, NULL,
                                           NULL) == AIMEE_MODULE_CALL_OK);
   assert(tagged == 0 && workspace_tag_calls == 2);

   uint32_t removed = 99u;
   assert(aimee_db2_delete_row_call(call_client, &client, 7048, 0, 42u, &removed, NULL, NULL) ==
          AIMEE_MODULE_CALL_OK);
   assert(removed == 1 && delete_row_calls == 1);
   assert(aimee_db2_delete_row_call(call_client, &client, 7049, 0, 43u, &removed, NULL, NULL) ==
          AIMEE_MODULE_CALL_OK);
   assert(removed == 0 && delete_row_calls == 2);

   assert(aimee_db2_touch_call(call_client, &client, 7050, 0, 42u, NULL, NULL) ==
          AIMEE_MODULE_CALL_OK);
   assert(touch_calls == 1 && touch_last == 42);
   /* A memory the backend refuses surfaces as INTERNAL, not as a quiet ok. */
   assert(aimee_db2_touch_call(call_client, &client, 7051, 0, 43u, NULL, NULL) ==
          AIMEE_MODULE_CALL_INTERNAL);

   assert(aimee_db2_link_delete_call(call_client, &client, 7052, 0, 7u, NULL, NULL) ==
          AIMEE_MODULE_CALL_OK);
   assert(link_delete_calls == 1 && link_delete_last == 7);
   assert(aimee_db2_link_delete_call(call_client, &client, 7053, 0, 8u, NULL, NULL) ==
          AIMEE_MODULE_CALL_INTERNAL);

   uint32_t valid_result = 99u, in_force = 99u;
   assert(aimee_db2_valid_at_call(call_client, &client, 7054, 0, 42u, "2026-08-18 12:00:00",
                                  &valid_result, &in_force, NULL, NULL) == AIMEE_MODULE_CALL_OK);
   assert(valid_result == AIMEE_DB2_RESULT_OK && in_force == 1 && valid_at_calls == 1);
   assert(strcmp(valid_at_last, "2026-08-18 12:00:00") == 0);
   assert(aimee_db2_valid_at_call(call_client, &client, 7055, 0, 43u, "2026-08-18 12:00:00",
                                  &valid_result, &in_force, NULL, NULL) == AIMEE_MODULE_CALL_OK);
   assert(valid_result == AIMEE_DB2_RESULT_OK && in_force == 0);
   /* Could not evaluate is its own answer, not a negative verdict. */
   assert(aimee_db2_valid_at_call(call_client, &client, 7056, 0, 44u, "2026-08-18 12:00:00",
                                  &valid_result, &in_force, NULL, NULL) == AIMEE_MODULE_CALL_OK);
   assert(valid_result == AIMEE_DB2_RESULT_INVALID_STATE && in_force == 0);

   uint32_t scoped = 99u;
   assert(aimee_db2_has_scope_type_call(call_client, &client, 7057, 0, 42u, "workspace", &scoped,
                                        NULL, NULL) == AIMEE_MODULE_CALL_OK);
   assert(scoped == 1 && scope_type_calls == 1 && strcmp(scope_type_last, "workspace") == 0);
   assert(aimee_db2_has_scope_type_call(call_client, &client, 7058, 0, 42u, "project", &scoped,
                                        NULL, NULL) == AIMEE_MODULE_CALL_OK);
   assert(scoped == 0 && scope_type_calls == 2);

   assert(aimee_db2_reject_call(call_client, &client, 7059, 0, 42u, NULL, NULL) ==
          AIMEE_MODULE_CALL_OK);
   assert(reject_calls == 1 && reject_last == 42);
   assert(aimee_db2_reject_call(call_client, &client, 7060, 0, 43u, NULL, NULL) ==
          AIMEE_MODULE_CALL_INTERNAL);

   uint32_t rewritten = 99u;
   assert(aimee_db2_update_content_call(call_client, &client, 7061, 0, 42u, "revised text",
                                        &rewritten, NULL, NULL) == AIMEE_MODULE_CALL_OK);
   assert(rewritten == 1 && update_content_calls == 1 &&
          strcmp(update_content_last, "revised text") == 0);
   assert(aimee_db2_update_content_call(call_client, &client, 7062, 0, 43u, "revised text",
                                        &rewritten, NULL, NULL) == AIMEE_MODULE_CALL_OK);
   assert(rewritten == 0);

   assert(aimee_db2_decay_confidence_call(call_client, &client, 7063, 0, 42u, NULL, NULL) ==
          AIMEE_MODULE_CALL_OK);
   assert(decay_confidence_calls == 1 && decay_confidence_last == 42);

   assert(aimee_db2_workspace_tag_insert_call(call_client, &client, 7064, 0, 42u, "aimee", NULL,
                                              NULL) == AIMEE_MODULE_CALL_OK);
   assert(workspace_tag_insert_calls == 1 && strcmp(workspace_tag_insert_last, "aimee") == 0);

   assert(aimee_db2_set_cognified_kind_call(call_client, &client, 7065, 0, 42u, "preference", NULL,
                                            NULL) == AIMEE_MODULE_CALL_OK);
   assert(cognified_kind_calls == 1 && strcmp(cognified_kind_last, "preference") == 0);

   assert(aimee_db2_set_source_session_call(call_client, &client, 7066, 0, 42u, "sess-1", NULL,
                                            NULL) == AIMEE_MODULE_CALL_OK);
   assert(source_session_calls == 1 && strcmp(source_session_last, "sess-1") == 0);
   /* Clearing is a real call, and the empty value must reach the backend. */
   assert(aimee_db2_set_source_session_call(call_client, &client, 7067, 0, 42u, "", NULL, NULL) ==
          AIMEE_MODULE_CALL_OK);
   assert(source_session_calls == 2 && source_session_last[0] == '\0');

   assert(aimee_db2_negation_tokens_update_call(call_client, &client, 7068, 0, 42u, "not never",
                                                NULL, NULL) == AIMEE_MODULE_CALL_OK);
   assert(negation_tokens_calls == 1 && strcmp(negation_tokens_last, "not never") == 0);
   /* A memory with no negations extracts to nothing, and that must store. */
   assert(aimee_db2_negation_tokens_update_call(call_client, &client, 7069, 0, 42u, "", NULL,
                                                NULL) == AIMEE_MODULE_CALL_OK);
   assert(negation_tokens_calls == 2 && negation_tokens_last[0] == '\0');

   aimee_db2_pool_status_t pool = {0};
   domain_result = 9;
   assert(aimee_db2_pool_status_call(call_client, &client, 7011, 0, &domain_result, &pool, NULL,
                                     NULL) == AIMEE_MODULE_CALL_OK);
   assert(domain_result == AIMEE_DB2_RESULT_OK && pool.size == 16 && pool.in_use == 2 &&
          pool.waiters == 1 && pool.lease_grants == 10 && pool.lease_timeouts == 3 &&
          pool.stuck == 4 && pool.poisoned == 5);

   aimee_db2_embedding_refusals_t refusals = {0};
   domain_result = 9;
   assert(aimee_db2_embedding_refusals_call(call_client, &client, 7012, 0, &domain_result,
                                            &refusals, NULL, NULL) == AIMEE_MODULE_CALL_OK);
   assert(domain_result == AIMEE_DB2_RESULT_OK && refusals.refused_count == 7 &&
          refusals.last_offered == 768);

   aimee_db2_postgres_status_t postgres = {0};
   domain_result = 9;
   assert(aimee_db2_postgres_status_call(call_client, &client, 7013, 0, &domain_result, &postgres,
                                         NULL, NULL) == AIMEE_MODULE_CALL_OK);
   assert(domain_result == AIMEE_DB2_RESULT_OK && postgres.available == 15 &&
          postgres.active_connections == 12 && postgres.max_connections == 100 &&
          postgres.is_replica == 1 && postgres.replica_lag_bytes == 1048576);

   aimee_db2_reembed_status_t reembed = {0};
   domain_result = 9;
   assert(aimee_db2_reembed_status_call(call_client, &client, 7014, 0, &domain_result, &reembed,
                                        NULL, NULL) == AIMEE_MODULE_CALL_OK);
   assert(domain_result == AIMEE_DB2_RESULT_OK && reembed.target_dimension == 384 &&
          reembed.started_epoch == 1700000000);

   domain_result = 9;
   assert(aimee_db2_reembed_clear_call(call_client, &client, 7015, 0, &domain_result, NULL, NULL) ==
          AIMEE_MODULE_CALL_OK);
   assert(domain_result == AIMEE_DB2_RESULT_OK);

   aimee_db2_reembed_clear_maintenance_t maintenance = {0};
   domain_result = 9;
   assert(aimee_db2_reembed_clear_maintenance_call(call_client, &client, 7016, 0, 1, &domain_result,
                                                   &maintenance, NULL,
                                                   NULL) == AIMEE_MODULE_CALL_OK);
   assert(domain_result == AIMEE_DB2_RESULT_OK && maintenance.was_in_progress == 1 &&
          maintenance.recorded_dimension == 384 && maintenance.running_dimension == 384);

   char serving_id[AIMEE_DB2_EMBEDDER_SERVING_ID_MAX + 1] = {0};
   domain_result = 9;
   assert(aimee_db2_embedder_serving_id_call(call_client, &client, 7017, 0, &domain_result,
                                             serving_id, sizeof(serving_id), NULL,
                                             NULL) == AIMEE_MODULE_CALL_OK);
   assert(domain_result == AIMEE_DB2_RESULT_OK &&
          strlen(serving_id) == AIMEE_DB2_EMBEDDER_SERVING_ID_MAX);
   for (size_t index = 0; index < strlen(serving_id); ++index)
      assert(serving_id[index] == 'x');

   aimee_db2_dimension_reset_t reset = {0};
   domain_result = 9;
   assert(aimee_db2_dimension_reset_call(call_client, &client, 7018, 0, 384, 0, 1, &domain_result,
                                         &reset, NULL, NULL) == AIMEE_MODULE_CALL_OK);
   assert(domain_result == AIMEE_DB2_RESULT_OK && reset.recorded_dimension == 768 &&
          reset.target_dimension == 384 && reset.tables_discovered == 6 &&
          reset.tables_dropped == 0 && reset.rows_cleared == 1234 && reset.curator_requeued == -1 &&
          reset.evidence_requeued == 7);

   schema_ok = have_pg_trgm = kb_tables_ok = 9;
   assert(aimee_db2_health_call(call_client, &client, 7002, 1, &schema_ok, &have_pg_trgm,
                                &kb_tables_ok, NULL, NULL) == AIMEE_MODULE_CALL_DEADLINE_EXCEEDED);
   assert(schema_ok == 0 && have_pg_trgm == 0 && kb_tables_ok == 0);

   atomic_store_explicit(&block_health, 1, memory_order_release);
   atomic_store_explicit(&health_entered, 0, memory_order_release);
   atomic_store_explicit(&health_release, 0, memory_order_release);
   atomic_int cancel;
   atomic_init(&cancel, 0);
   cancel_inflight_t cancel_state = {.cancel = &cancel};
   pthread_t cancel_thread;
   assert(pthread_create(&cancel_thread, NULL, cancel_inflight, &cancel_state) == 0);
   schema_ok = have_pg_trgm = kb_tables_ok = 9;
   assert(aimee_db2_health_call(call_client, &client, 7003, 0, &schema_ok, &have_pg_trgm,
                                &kb_tables_ok, cancellation_flag,
                                &cancel) == AIMEE_MODULE_CALL_CANCELLED);
   assert(schema_ok == 0 && have_pg_trgm == 0 && kb_tables_ok == 0);
   assert(pthread_join(cancel_thread, NULL) == 0 && cancel_state.entered == 1);
   atomic_store_explicit(&block_health, 0, memory_order_release);

   /* The cancelled handler finishes after its caller. The typed client must
    * drain that stale terminal reply and keep the next correlation healthy. */
   schema_ok = have_pg_trgm = kb_tables_ok = 0;
   assert(aimee_db2_health_call(call_client, &client, 7004, 0, &schema_ok, &have_pg_trgm,
                                &kb_tables_ok, NULL, NULL) == AIMEE_MODULE_CALL_OK);
   assert(schema_ok == 1 && have_pg_trgm == 0 && kb_tables_ok == 1);
   assert(initialized_calls == 3 && health_calls == 3 && kb_health_calls == 3);

   aimee_module_client_destroy(&client);
   aimee_module_process_stop();
   assert(pthread_join(module_thread, NULL) == 0 && process.result == 0);
   atomic_store_explicit(&pump_state.stop, 1, memory_order_release);
   assert(pthread_join(pump_thread, NULL) == 0);
   bus_client_detach(&caller);
   bus_runtime_stop(&runtime);
   bus_host_destroy(&host);
   pthread_mutex_destroy(&host_lock);
   assert(rmdir(directory) == 0);
   puts("test_bus_db2_module: typed client, deadline, and cancellation crossed the real event bus");
   return 0;
}
