/* memory_directives.c: epistemic-directive orchestration. The SQL surface
 * lives in db2/epistemic_directives.c — this file owns validation, dedup
 * recovery, metrics, dogfood logging, and the staged matcher.
 *
 * One row in `epistemic_directives` per durable "we know this gap exists,
 * ask when relevant" record; auto-created from contradictions and repeated
 * retrieval failures, resolved when the answer lands. See
 * docs/proposals/done/epistemic-directives-and-active-clarification.md;
 * the public contract lives in src/headers/memory.h. */
#include "aimee.h"
#include "util.h"
#include "cJSON.h"
#if !defined(AIMEE_DB2_DISABLED)
#include "db2/epistemic_directives.h"
#include "db2/failed_queries.h"
#include "log.h"
#endif
#include "memory.h"
#include <ctype.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static pthread_mutex_t s_ed_metrics_mu = PTHREAD_MUTEX_INITIALIZER;
static int64_t s_ed_metric_created_total = 0;
static int64_t s_ed_metric_resolved_total = 0;
static int64_t s_ed_metric_expired_total = 0;
static int64_t s_ed_metric_surfaced_total = 0;
static int64_t s_ed_metric_match_calls = 0;
static double s_ed_metric_match_ms_sum = 0.0;
static double s_ed_metric_match_ms_max = 0.0;

void memory_directive_metrics(int64_t *created_total, int64_t *resolved_total,
                              int64_t *expired_total, int64_t *surfaced_total, int64_t *match_calls,
                              double *match_ms_avg, double *match_ms_max)
{
   pthread_mutex_lock(&s_ed_metrics_mu);
   if (created_total)
      *created_total = s_ed_metric_created_total;
   if (resolved_total)
      *resolved_total = s_ed_metric_resolved_total;
   if (expired_total)
      *expired_total = s_ed_metric_expired_total;
   if (surfaced_total)
      *surfaced_total = s_ed_metric_surfaced_total;
   if (match_calls)
      *match_calls = s_ed_metric_match_calls;
   if (match_ms_avg)
      *match_ms_avg = s_ed_metric_match_calls > 0
                          ? s_ed_metric_match_ms_sum / (double)s_ed_metric_match_calls
                          : 0.0;
   if (match_ms_max)
      *match_ms_max = s_ed_metric_match_ms_max;
   pthread_mutex_unlock(&s_ed_metrics_mu);
}

#if defined(AIMEE_DB2_DISABLED)
/* Directive storage is DB2-owned. Server code should access it through
 * aimee-kb RPCs, not by linking DB2 directive helpers. */
int memory_directive_get(int64_t id, memory_directive_t *out)
{
   (void)id;
   (void)out;
   return -1;
}

int memory_directive_create(const char *question, const char *topic, const char *anchor_entity,
                            const char *anchor_file, const char *cause, int priority,
                            int64_t memory_a_id, int64_t memory_b_id, const char *evidence,
                            const char *source_session, const char *valid_until,
                            memory_directive_t *out)
{
   (void)question;
   (void)topic;
   (void)anchor_entity;
   (void)anchor_file;
   (void)cause;
   (void)priority;
   (void)memory_a_id;
   (void)memory_b_id;
   (void)evidence;
   (void)source_session;
   (void)valid_until;
   (void)out;
   return -1;
}

int memory_directive_list(const char *state, const char *cause, memory_directive_t *out, int max)
{
   (void)state;
   (void)cause;
   (void)out;
   (void)max;
   return 0;
}

int memory_directive_counts(memory_directive_counts_t *out)
{
   if (out)
      memset(out, 0, sizeof(*out));
   return -1;
}

int memory_directive_resolve(int64_t id, int64_t resolution_memory_id, const char *note)
{
   (void)id;
   (void)resolution_memory_id;
   (void)note;
   return -1;
}

int memory_directive_suppress(int64_t id)
{
   (void)id;
   return -1;
}

int memory_directive_sweep_expired(void)
{
   return 0;
}

int memory_directive_match(const char *turn_text, const char *active_entity,
                           const char *active_file, memory_directive_t *out, int max)
{
   (void)turn_text;
   (void)active_entity;
   (void)active_file;
   (void)out;
   (void)max;
   return 0;
}

int memory_directive_mark_surfaced(int64_t id)
{
   (void)id;
   return -1;
}

int64_t memory_directive_record_retrieval_failure(const char *query_norm, int threshold,
                                                  const char *source_session)
{
   (void)query_norm;
   (void)threshold;
   (void)source_session;
   return 0;
}

int64_t memory_directive_record_contradiction(int64_t memory_a_id, int64_t memory_b_id,
                                              const char *topic, const char *anchor_entity,
                                              const char *content_a, const char *content_b,
                                              const char *source_session)
{
   (void)memory_a_id;
   (void)memory_b_id;
   (void)topic;
   (void)anchor_entity;
   (void)content_a;
   (void)content_b;
   (void)source_session;
   return 0;
}

int memory_directive_resolve_contradiction(int64_t memory_a_id, int64_t memory_b_id,
                                           int64_t resolution_memory_id, const char *note)
{
   (void)memory_a_id;
   (void)memory_b_id;
   (void)resolution_memory_id;
   (void)note;
   return 0;
}
#else

static void ed_metrics_inc(int64_t *field)
{
   pthread_mutex_lock(&s_ed_metrics_mu);
   (*field)++;
   pthread_mutex_unlock(&s_ed_metrics_mu);
}

static void ed_metrics_add(int64_t *field, int n)
{
   if (n <= 0)
      return;
   pthread_mutex_lock(&s_ed_metrics_mu);
   *field += n;
   pthread_mutex_unlock(&s_ed_metrics_mu);
}

static void ed_metrics_record_match(double ms)
{
   pthread_mutex_lock(&s_ed_metrics_mu);
   s_ed_metric_match_calls++;
   s_ed_metric_match_ms_sum += ms;
   if (ms > s_ed_metric_match_ms_max)
      s_ed_metric_match_ms_max = ms;
   pthread_mutex_unlock(&s_ed_metrics_mu);
}

static int ed_cause_valid(const char *cause)
{
   if (!cause)
      return 0;
   return strcmp(cause, MEMORY_DIRECTIVE_CAUSE_CONTRADICTION) == 0 ||
          strcmp(cause, MEMORY_DIRECTIVE_CAUSE_RETRIEVAL_FAILURE) == 0 ||
          strcmp(cause, MEMORY_DIRECTIVE_CAUSE_MISSING_CONFIG) == 0 ||
          strcmp(cause, MEMORY_DIRECTIVE_CAUSE_USER_FOLLOW_UP) == 0;
}

int memory_directive_get(int64_t id, memory_directive_t *out)
{
   if (!out || id <= 0)
      return -1;
   return db2_directive_get(id, out);
}

int memory_directive_create(const char *question, const char *topic, const char *anchor_entity,
                            const char *anchor_file, const char *cause, int priority,
                            int64_t memory_a_id, int64_t memory_b_id, const char *evidence,
                            const char *source_session, const char *valid_until,
                            memory_directive_t *out)
{
   if (!question || !question[0] || !ed_cause_valid(cause))
      return -1;
   if (priority < 0)
      priority = 0;
   if (priority > 100)
      priority = 100;

   int64_t new_id = 0;
   int existed = 0;
   int rc = db2_directive_insert_ignore(question, topic, anchor_entity, anchor_file, cause,
                                        priority, memory_a_id, memory_b_id, evidence,
                                        source_session, valid_until, &new_id, &existed);
   if (rc != 0)
      return -1;
   if (existed)
      return 1;

   if (out)
      db2_directive_get(new_id, out);
   ed_metrics_inc(&s_ed_metric_created_total);
   aimee_log(LOG_INFO, "memory.directives", "created id=%lld cause=%s priority=%d topic=%s",
             (long long)new_id, cause, priority, topic ? topic : "");
   return 0;
}

int memory_directive_list(const char *state, const char *cause, memory_directive_t *out, int max)
{
   return db2_directive_list(state, cause, out, max);
}

int memory_directive_counts(memory_directive_counts_t *out)
{
   if (!out)
      return -1;
   memset(out, 0, sizeof(*out));
   return db2_directive_counts_by_state(&out->open, &out->suppressed, &out->resolved,
                                        &out->expired);
}

int memory_directive_resolve(int64_t id, int64_t resolution_memory_id, const char *note)
{
   if (id <= 0)
      return -1;
   if (db2_directive_resolve(id, resolution_memory_id) != 0)
      return -1;

   ed_metrics_inc(&s_ed_metric_resolved_total);
   aimee_log(LOG_INFO, "memory.directives", "resolved id=%lld resolution_memory_id=%lld note=%s",
             (long long)id, (long long)resolution_memory_id, note ? note : "");
   return 0;
}

int memory_directive_suppress(int64_t id)
{
   if (id <= 0)
      return -1;
   if (db2_directive_suppress(id) != 0)
      return -1;
   aimee_log(LOG_INFO, "memory.directives", "suppressed id=%lld", (long long)id);
   return 0;
}

int memory_directive_sweep_expired(void)
{
   int n = db2_directive_sweep_expired();
   ed_metrics_add(&s_ed_metric_expired_total, n);
   if (n > 0)
      aimee_log(LOG_INFO, "memory.directives", "sweep expired=%d", n);
   return n;
}

static int ed_build_lexical_query(const char *turn_text, char *out, size_t out_len)
{
   if (!out || out_len == 0)
      return 0;
   out[0] = '\0';
   if (!turn_text)
      return 0;

   size_t o = 0;
   int wrote = 0;
   size_t tlen = strlen(turn_text);
   for (size_t i = 0; i < tlen && out_len - o > 24;)
   {
      while (i < tlen && !isalnum((unsigned char)turn_text[i]))
         i++;
      size_t start = i;
      while (i < tlen && (isalnum((unsigned char)turn_text[i]) || turn_text[i] == '_'))
         i++;
      size_t wlen = i - start;
      if (wlen < 3)
         continue;
      if (wrote)
      {
         if (out_len - o <= 4)
            break;
         out[o++] = ' ';
         out[o++] = 'O';
         out[o++] = 'R';
         out[o++] = ' ';
      }
      if (wlen + 2 >= out_len - o)
         break;
      out[o++] = '"';
      for (size_t k = 0; k < wlen; k++)
         out[o++] = (char)tolower((unsigned char)turn_text[start + k]);
      out[o++] = '"';
      wrote++;
   }
   out[o] = '\0';
   return wrote > 0;
}

static void ed_merge_unique(memory_directive_t *out, int *out_count, int max, int64_t *seen,
                            int *seen_count, int seen_cap, const memory_directive_t *src,
                            int src_count)
{
   for (int i = 0; i < src_count && *out_count < max; i++)
   {
      int dup = 0;
      for (int s = 0; s < *seen_count; s++)
      {
         if (seen[s] == src[i].id)
         {
            dup = 1;
            break;
         }
      }
      if (dup)
         continue;
      out[(*out_count)++] = src[i];
      if (*seen_count < seen_cap)
         seen[(*seen_count)++] = src[i].id;
   }
}

int memory_directive_match(const char *turn_text, const char *active_entity,
                           const char *active_file, memory_directive_t *out, int max)
{
   if (!out || max <= 0)
      return 0;
   struct timespec t0, t1;
   clock_gettime(CLOCK_MONOTONIC, &t0);

   int out_count = 0;
   int64_t seen[MEMORY_DIRECTIVE_MAX_MATCHES * 4];
   int seen_count = 0;
   const int cap_seen = (int)(sizeof(seen) / sizeof(seen[0]));

   /* Stage 1: anchor_entity case-insensitive. */
   if (active_entity && active_entity[0])
   {
      char ent_lc[MEMORY_DIRECTIVE_ANCHOR_LEN];
      util_lower_copy(active_entity, ent_lc, sizeof(ent_lc));
      memory_directive_t buf[MEMORY_DIRECTIVE_MAX_MATCHES];
      int n = db2_directive_match_by_entity(ent_lc, buf, max);
      ed_merge_unique(out, &out_count, max, seen, &seen_count, cap_seen, buf, n);
   }

   /* Stage 2: anchor_file exact. */
   if (active_file && active_file[0] && out_count < max)
   {
      memory_directive_t buf[MEMORY_DIRECTIVE_MAX_MATCHES];
      int n = db2_directive_match_by_file(active_file, buf, max);
      ed_merge_unique(out, &out_count, max, seen, &seen_count, cap_seen, buf, n);
   }

   /* Stage 3: lexical match over question+topic. */
   if (turn_text && turn_text[0] && out_count < max)
   {
      char match_clause[2048];
      if (ed_build_lexical_query(turn_text, match_clause, sizeof(match_clause)))
      {
         memory_directive_t buf[MEMORY_DIRECTIVE_MAX_MATCHES];
         int n = db2_directive_match_by_lexical(match_clause, buf, max);
         ed_merge_unique(out, &out_count, max, seen, &seen_count, cap_seen, buf, n);
      }
   }

   clock_gettime(CLOCK_MONOTONIC, &t1);
   double ms = (t1.tv_sec - t0.tv_sec) * 1000.0 + (t1.tv_nsec - t0.tv_nsec) / 1.0e6;
   ed_metrics_record_match(ms);
   return out_count;
}

int memory_directive_mark_surfaced(int64_t id)
{
   if (id <= 0)
      return -1;
   if (db2_directive_record_surface(id) != 0)
      return -1;
   ed_metrics_inc(&s_ed_metric_surfaced_total);
   return 0;
}

int64_t memory_directive_record_retrieval_failure(const char *query_norm, int threshold,
                                                  const char *source_session)
{
   if (!query_norm || !query_norm[0])
      return 0;
   if (threshold <= 0)
      threshold = MEMORY_DIRECTIVE_RETRIEVAL_FAILURE_THRESHOLD_DEFAULT;

   int count = db2_failed_query_bump(query_norm);
   if (count < threshold)
      return 0;

   /* Crossed threshold: create directive. The (cause, topic) unique
    * index ensures at most one retrieval_failure directive per
    * normalized query — subsequent failures bump the counter without
    * creating duplicates. */
   char question[MEMORY_DIRECTIVE_QUESTION_LEN];
   snprintf(question, sizeof(question),
            "I keep failing to find a confident answer about '%s'. Can you tell me the answer "
            "so I can record it?",
            query_norm);
   char evidence[MEMORY_DIRECTIVE_EVIDENCE_LEN];
   snprintf(evidence, sizeof(evidence), "{\"query\":\"%s\",\"failures\":%d}", query_norm, count);

   memory_directive_t out;
   int rcd = memory_directive_create(question, query_norm, "", "",
                                     MEMORY_DIRECTIVE_CAUSE_RETRIEVAL_FAILURE, 60, 0, 0, evidence,
                                     source_session, "", &out);
   if (rcd == 0)
      return out.id;
   if (rcd == 1)
   {
      /* Pre-existing row — fetch its id so callers can link to it. */
      memory_directive_t found;
      if (db2_directive_find_by_cause_topic(MEMORY_DIRECTIVE_CAUSE_RETRIEVAL_FAILURE, query_norm,
                                            &found) == 0)
         return found.id;
   }
   return 0;
}

int64_t memory_directive_record_contradiction(int64_t memory_a_id, int64_t memory_b_id,
                                              const char *topic, const char *anchor_entity,
                                              const char *content_a, const char *content_b,
                                              const char *source_session)
{
   if (memory_a_id <= 0 || memory_b_id <= 0)
      return 0;
   /* Normalise pair order so the dedup index is stable regardless of
    * caller ordering. */
   if (memory_a_id > memory_b_id)
   {
      int64_t tmp = memory_a_id;
      memory_a_id = memory_b_id;
      memory_b_id = tmp;
      const char *tc = content_a;
      content_a = content_b;
      content_b = tc;
   }

   char question[MEMORY_DIRECTIVE_QUESTION_LEN];
   snprintf(question, sizeof(question), "Which is correct about '%s': '%s' or '%s'?",
            topic ? topic : "this", content_a ? content_a : "(a)", content_b ? content_b : "(b)");
   char evidence[MEMORY_DIRECTIVE_EVIDENCE_LEN];
   snprintf(evidence, sizeof(evidence), "{\"memory_a\":%lld,\"memory_b\":%lld}",
            (long long)memory_a_id, (long long)memory_b_id);

   memory_directive_t out;
   int rcd =
       memory_directive_create(question, topic ? topic : "", anchor_entity ? anchor_entity : "", "",
                               MEMORY_DIRECTIVE_CAUSE_CONTRADICTION, 80, memory_a_id, memory_b_id,
                               evidence, source_session, "", &out);
   if (rcd == 0)
      return out.id;
   if (rcd == 1)
   {
      /* Same dedup recovery shape as retrieval failure, but the unique
       * index is on (cause='contradiction', memory_a_id, memory_b_id).
       * Resolve via the contradiction-specific helper, which targets
       * the symmetric pair. */
      memory_directive_t found;
      memset(&found, 0, sizeof(found));
      /* db2_directive_find_by_cause_topic only matches by topic; for a
       * contradiction with the same topic a duplicate match is the
       * existing row. */
      if (db2_directive_find_by_cause_topic(MEMORY_DIRECTIVE_CAUSE_CONTRADICTION,
                                            topic ? topic : "", &found) == 0 &&
          ((found.memory_a_id == memory_a_id && found.memory_b_id == memory_b_id) ||
           (found.memory_a_id == memory_b_id && found.memory_b_id == memory_a_id)))
         return found.id;
   }
   return 0;
}

int memory_directive_resolve_contradiction(int64_t memory_a_id, int64_t memory_b_id,
                                           int64_t resolution_memory_id, const char *note)
{
   (void)note;
   if (memory_a_id <= 0 || memory_b_id <= 0)
      return 0;
   int n = db2_directive_resolve_contradiction(memory_a_id, memory_b_id, resolution_memory_id);
   if (n > 0)
      ed_metrics_add(&s_ed_metric_resolved_total, n);
   return n > 0 ? n : 0;
}

#endif

struct cJSON *memory_directive_to_json(const memory_directive_t *d)
{
   if (!d)
      return NULL;
   cJSON *j = cJSON_CreateObject();
   if (!j)
      return NULL;
   cJSON_AddNumberToObject(j, "id", (double)d->id);
   cJSON_AddStringToObject(j, "question", d->question);
   cJSON_AddStringToObject(j, "topic", d->topic);
   cJSON_AddStringToObject(j, "anchor_entity", d->anchor_entity);
   cJSON_AddStringToObject(j, "anchor_file", d->anchor_file);
   cJSON_AddStringToObject(j, "cause", d->cause);
   cJSON_AddNumberToObject(j, "priority", d->priority);
   cJSON_AddStringToObject(j, "state", d->state);
   cJSON_AddNumberToObject(j, "memory_a_id", (double)d->memory_a_id);
   cJSON_AddNumberToObject(j, "memory_b_id", (double)d->memory_b_id);
   cJSON_AddNumberToObject(j, "resolution_memory_id", (double)d->resolution_memory_id);
   cJSON_AddStringToObject(j, "evidence", d->evidence);
   cJSON_AddNumberToObject(j, "surfaced_count", d->surfaced_count);
   cJSON_AddStringToObject(j, "last_surfaced_at", d->last_surfaced_at);
   cJSON_AddStringToObject(j, "resolved_at", d->resolved_at);
   cJSON_AddStringToObject(j, "valid_until", d->valid_until);
   cJSON_AddStringToObject(j, "created_at", d->created_at);
   return j;
}

static void djson_copy_str(cJSON *obj, const char *key, char *dst, size_t cap)
{
   cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
   if (cJSON_IsString(item) && item->valuestring)
      snprintf(dst, cap, "%s", item->valuestring);
   else
      dst[0] = '\0';
}

int memory_directive_from_json(const struct cJSON *obj, memory_directive_t *out)
{
   if (!obj || !out)
      return -1;
   memset(out, 0, sizeof(*out));
   cJSON *src = (cJSON *)obj;
   cJSON *id_j = cJSON_GetObjectItemCaseSensitive(src, "id");
   if (!cJSON_IsNumber(id_j))
      return -1;
   out->id = (int64_t)id_j->valuedouble;
   cJSON *pri_j = cJSON_GetObjectItemCaseSensitive(src, "priority");
   out->priority = cJSON_IsNumber(pri_j) ? (int)pri_j->valuedouble : 0;
   cJSON *a_j = cJSON_GetObjectItemCaseSensitive(src, "memory_a_id");
   out->memory_a_id = cJSON_IsNumber(a_j) ? (int64_t)a_j->valuedouble : 0;
   cJSON *b_j = cJSON_GetObjectItemCaseSensitive(src, "memory_b_id");
   out->memory_b_id = cJSON_IsNumber(b_j) ? (int64_t)b_j->valuedouble : 0;
   cJSON *r_j = cJSON_GetObjectItemCaseSensitive(src, "resolution_memory_id");
   out->resolution_memory_id = cJSON_IsNumber(r_j) ? (int64_t)r_j->valuedouble : 0;
   cJSON *sc_j = cJSON_GetObjectItemCaseSensitive(src, "surfaced_count");
   out->surfaced_count = cJSON_IsNumber(sc_j) ? (int)sc_j->valuedouble : 0;
   djson_copy_str(src, "question", out->question, sizeof(out->question));
   djson_copy_str(src, "topic", out->topic, sizeof(out->topic));
   djson_copy_str(src, "anchor_entity", out->anchor_entity, sizeof(out->anchor_entity));
   djson_copy_str(src, "anchor_file", out->anchor_file, sizeof(out->anchor_file));
   djson_copy_str(src, "cause", out->cause, sizeof(out->cause));
   djson_copy_str(src, "state", out->state, sizeof(out->state));
   djson_copy_str(src, "evidence", out->evidence, sizeof(out->evidence));
   djson_copy_str(src, "last_surfaced_at", out->last_surfaced_at, sizeof(out->last_surfaced_at));
   djson_copy_str(src, "resolved_at", out->resolved_at, sizeof(out->resolved_at));
   djson_copy_str(src, "valid_until", out->valid_until, sizeof(out->valid_until));
   djson_copy_str(src, "created_at", out->created_at, sizeof(out->created_at));
   return 0;
}
