#include "memory_scope_query.h"
#include "module_commands.h"
#include "json_fluent.h"
#include "aimee.h"
#include "module_commands.h"
#include "kb_reqctx.h"
#include "kb/kb_login_throttle.h"
#include "config.h" /* legacy_config_read — reembed default embedder */
#include "kb_background.h"
#include "kb_service.h"
#include "kb_service_code_embed.h"
#include "kb_service_kb.h"
#include "kb.h"
#include "cJSON.h"
#include "json_fluent.h" /* jo_ok */
#include "modules/db2/c/kb_service_backend.h"
#include "modules/db2/c/db2_internal.h"
#include "modules/db2/c/pgvec_kb_service.h"
#include <aimee/learning/learning.h>
#include "curiosity_resolve.h"
#include "kb_bandit.h"
#include <aimee/learning/policy_arms.h>
#include "log.h"
#include "memory.h"
#include "lifecycle.h"
#include "kb_vectors.h"
#include "kb_curator_drain.h" /* kb_curator_stages_json — Option B registry endpoint */
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifndef AIMEE_WINDOWS
#include <unistd.h>
#endif

extern kb_service_ctx_t *g_kb_ctx;

/* ------------------------------------------------------------------ */
/* Connection-worker slot tracking                                     */
/* ------------------------------------------------------------------ */

typedef struct
{
   int active;
   char method[64];
   time_t started_at;
} kb_conn_slot_t;

static kb_conn_slot_t g_kb_conn_slots[KB_WORKER_MAX];
static pthread_mutex_t g_kb_conn_slots_mu = PTHREAD_MUTEX_INITIALIZER;

char *kb_service_conn_slots_json(int configured)
{
   cJSON *arr = cJSON_CreateArray();
   pthread_mutex_lock(&g_kb_conn_slots_mu);
   time_t now = time(NULL);
   for (int i = 0; i < configured && i < KB_WORKER_MAX; i++)
   {
      kb_conn_slot_t *s = &g_kb_conn_slots[i];
      cJSON *w = cJSON_CreateObject();
      cJSON_AddNumberToObject(w, "index", i);
      cJSON_AddBoolToObject(w, "active", s->active);
      if (s->active)
      {
         cJSON_AddStringToObject(w, "method", s->method);
         cJSON_AddNumberToObject(w, "elapsed_secs", (double)(now - s->started_at));
      }
      cJSON_AddItemToArray(arr, w);
   }
   pthread_mutex_unlock(&g_kb_conn_slots_mu);
   char *out = cJSON_PrintUnformatted(arr);
   cJSON_Delete(arr);
   return out;
}

/* ------------------------------------------------------------------ */

static long kb_ipc_write(int fd, const char *buf, size_t len)
{
#ifdef AIMEE_WINDOWS
   HANDLE h = (HANDLE)(intptr_t)fd;
   DWORD written = 0;
   DWORD chunk = len > (size_t)((DWORD)~0u) ? (DWORD)~0u : (DWORD)len;
   if (!WriteFile(h, buf, chunk, &written, NULL))
   {
      errno = GetLastError() == ERROR_BROKEN_PIPE ? EPIPE : EIO;
      return -1;
   }
   return (long)written;
#else
   return (long)write(fd, buf, len);
#endif
}

/* Shared with kb_service_memory.c — keep external linkage. */
int kb_send_response(int fd, cJSON *resp)
{
   char *json = cJSON_PrintUnformatted(resp);
   if (!json)
      return -1;
   size_t len = strlen(json);
   size_t off = 0;
   while (off < len)
   {
      long n = kb_ipc_write(fd, json + off, len - off);
      if (n < 0)
      {
         if (errno == EINTR)
            continue;
         free(json);
         return -1;
      }
      off += (size_t)n;
   }
   free(json);
   return kb_ipc_write(fd, "\n", 1) == 1 ? 0 : -1;
}

/* Shared with kb_service_memory.c — keep external linkage. */
int kb_send_error(int fd, const char *message)
{
   cJSON *resp = cJSON_CreateObject();
   cJSON_AddStringToObject(resp, "status", "error");
   cJSON_AddStringToObject(resp, "message", message);
   int rc = kb_send_response(fd, resp);
   cJSON_Delete(resp);
   return rc;
}

/* Response epilogue shared by every kb_handle_* wrapper: when `resp` is NULL
 * (the backend failed) send an error envelope with `err_msg`; otherwise send
 * `resp`, free it, and return the send rc. */
int kb_reply_or_error(int fd, cJSON *resp, const char *err_msg)
{
   if (!resp)
      return kb_send_error(fd, err_msg);
   int rc = kb_send_response(fd, resp);
   cJSON_Delete(resp);
   return rc;
}

/* memory.embed: single (memory_id>0) or batch (--all) re-embed at the
 * active embedder version.  For batch mode the caller passes the active
 * version so server-side config state is out of the loop. */
/* KB-side handler for the `code_embeddings_refresh` action: embeds the
 * (changed or all) code definitions of a project into code_embeddings and
 * records code_index_ops, via the shared kb_code_embed_refresh. Previously the
 * client wrapper (kb_client_code_embeddings_refresh) had no server-side
 * dispatch, so the action was a no-op — this wires it. */
static int kb_handle_code_embeddings_refresh(int fd, cJSON *req)
{
   cJSON *proj_j = cJSON_GetObjectItemCaseSensitive(req, "project");
   const char *project =
       (cJSON_IsString(proj_j) && proj_j->valuestring[0]) ? proj_j->valuestring : "";
   if (!project[0])
      return kb_send_error(fd, "code_embeddings_refresh requires project");

   cJSON *scope_j = cJSON_GetObjectItemCaseSensitive(req, "scope");
   const char *scope = (cJSON_IsString(scope_j) && scope_j->valuestring[0]) ? scope_j->valuestring
                                                                            : "changed_files";
   cJSON *bs_j = cJSON_GetObjectItemCaseSensitive(req, "batch_size");
   cJSON *mp_j = cJSON_GetObjectItemCaseSensitive(req, "max_points");
   int batch_size = cJSON_IsNumber(bs_j) ? (int)bs_j->valuedouble : 0;
   int max_points = cJSON_IsNumber(mp_j) ? (int)mp_j->valuedouble : 0;
   int dry_run = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(req, "dry_run")) ? 1 : 0;

   const char **paths = NULL;
   int path_count = 0;
   cJSON *paths_j = cJSON_GetObjectItemCaseSensitive(req, "paths");
   if (cJSON_IsArray(paths_j))
   {
      int n = cJSON_GetArraySize(paths_j);
      if (n > 0 && (paths = calloc((size_t)n, sizeof(*paths))) != NULL)
      {
         for (int i = 0; i < n; i++)
         {
            cJSON *p = cJSON_GetArrayItem(paths_j, i);
            if (cJSON_IsString(p) && p->valuestring[0])
               paths[path_count++] = p->valuestring;
         }
      }
   }

   kb_code_embed_result_t out;
   memset(&out, 0, sizeof(out));
   int rc = kb_code_embed_refresh(project, scope, paths, path_count, batch_size, max_points,
                                  dry_run, &out);
   free(paths);
   if (rc != 0)
      return kb_send_error(fd, "code embeddings refresh failed");

   cJSON *resp = jo_ok();
   cJSON_AddStringToObject(resp, "project", out.project);
   cJSON_AddStringToObject(resp, "scope", out.scope);
   cJSON_AddNumberToObject(resp, "embedded", (double)out.embedded);
   cJSON_AddNumberToObject(resp, "skipped_unchanged", (double)out.skipped_unchanged);
   cJSON_AddNumberToObject(resp, "estimated_points", (double)out.estimated_points);
   cJSON_AddBoolToObject(resp, "dry_run", out.dry_run ? 1 : 0);
   cJSON_AddStringToObject(resp, "writer", out.writer);
   int srv_rc = kb_send_response(fd, resp);
   cJSON_Delete(resp);
   return srv_rc;
}

static int kb_handle_curiosity_list(int fd, cJSON *req)
{
   cJSON *state_j = cJSON_GetObjectItemCaseSensitive(req, "state");
   cJSON *limit_j = cJSON_GetObjectItemCaseSensitive(req, "limit");
   const char *state =
       (cJSON_IsString(state_j) && state_j->valuestring[0]) ? state_j->valuestring : NULL;
   int limit = cJSON_IsNumber(limit_j) ? (int)limit_j->valuedouble : 20;
   if (limit < 1)
      limit = 1;
   if (limit > 128)
      limit = 128;

   cJSON *resp = db2_kb_service_curiosity_list_json(state, limit);
   return kb_reply_or_error(fd, resp, "failed to list curiosity items");
}

static int kb_handle_curiosity_create(int fd, cJSON *req)
{
   cJSON *gap_j = cJSON_GetObjectItemCaseSensitive(req, "gap_type");
   cJSON *entity_j = cJSON_GetObjectItemCaseSensitive(req, "target_entity");
   cJSON *topic_j = cJSON_GetObjectItemCaseSensitive(req, "target_topic");
   cJSON *evidence_j = cJSON_GetObjectItemCaseSensitive(req, "evidence");
   cJSON *imp_j = cJSON_GetObjectItemCaseSensitive(req, "importance");
   cJSON *nov_j = cJSON_GetObjectItemCaseSensitive(req, "novelty");
   cJSON *sess_j = cJSON_GetObjectItemCaseSensitive(req, "source_session");
   if (!cJSON_IsString(gap_j))
      return kb_send_error(fd, "curiosity.create requires gap_type");

   const char *entity = cJSON_IsString(entity_j) ? entity_j->valuestring : NULL;
   const char *topic = cJSON_IsString(topic_j) ? topic_j->valuestring : NULL;
   const char *evidence = cJSON_IsString(evidence_j) ? evidence_j->valuestring : NULL;
   const char *sess = cJSON_IsString(sess_j) ? sess_j->valuestring : NULL;
   double importance = cJSON_IsNumber(imp_j) ? imp_j->valuedouble : 0.0;
   double novelty = cJSON_IsNumber(nov_j) ? nov_j->valuedouble : 0.0;

   cJSON *resp = db2_kb_service_curiosity_create_json(gap_j->valuestring, entity, topic, evidence,
                                                      importance, novelty, sess);
   return kb_reply_or_error(fd, resp, "failed to create curiosity item");
}

static int kb_handle_curiosity_sweep(int fd, cJSON *req)
{
   (void)req;
   cJSON *resp = db2_kb_service_curiosity_sweep_json();
   return kb_reply_or_error(fd, resp, "failed to sweep curiosity");
}

static int kb_handle_curiosity_rescore(int fd, cJSON *req)
{
   (void)req;
   cJSON *resp = db2_kb_service_curiosity_rescore_json();
   return kb_reply_or_error(fd, resp, "failed to rescore curiosity");
}

static int kb_handle_curiosity_get(int fd, cJSON *req)
{
   cJSON *id_j = cJSON_GetObjectItemCaseSensitive(req, "id");
   if (!cJSON_IsNumber(id_j))
      return kb_send_error(fd, "curiosity.get requires id");
   cJSON *resp = db2_kb_service_curiosity_get_json((int64_t)id_j->valuedouble);
   return kb_reply_or_error(fd, resp, "failed to get curiosity item");
}

static int kb_handle_curiosity_update_state(int fd, cJSON *req)
{
   cJSON *id_j = cJSON_GetObjectItemCaseSensitive(req, "id");
   cJSON *state_j = cJSON_GetObjectItemCaseSensitive(req, "state");
   if (!cJSON_IsNumber(id_j) || !cJSON_IsString(state_j))
      return kb_send_error(fd, "curiosity.update_state requires id and state");
   cJSON *resp =
       db2_kb_service_curiosity_update_state_json((int64_t)id_j->valuedouble, state_j->valuestring);
   return kb_reply_or_error(fd, resp, "failed to update curiosity state");
}

static int kb_handle_curiosity_route_top(int fd, cJSON *req)
{
   cJSON *limit_j = cJSON_GetObjectItemCaseSensitive(req, "limit");
   cJSON *sess_j = cJSON_GetObjectItemCaseSensitive(req, "source_session");
   int limit = cJSON_IsNumber(limit_j) ? (int)limit_j->valuedouble : 5;
   const char *sess = cJSON_IsString(sess_j) ? sess_j->valuestring : NULL;
   cJSON *resp = db2_kb_service_curiosity_route_top_json(limit, sess);
   return kb_reply_or_error(fd, resp, "failed to route curiosity items");
}

static int kb_handle_note_create(int fd, cJSON *req)
{
   cJSON *title_j = cJSON_GetObjectItemCaseSensitive(req, "title");
   cJSON *content_j = cJSON_GetObjectItemCaseSensitive(req, "content");
   cJSON *tags_j = cJSON_GetObjectItemCaseSensitive(req, "tags");
   cJSON *author_j = cJSON_GetObjectItemCaseSensitive(req, "author");
   if (!cJSON_IsString(title_j) || !cJSON_IsString(content_j))
      return kb_send_error(fd, "notes.create requires title and content");
   const char *tags = cJSON_IsString(tags_j) ? tags_j->valuestring : NULL;
   const char *author = cJSON_IsString(author_j) ? author_j->valuestring : NULL;

   cJSON *resp =
       db2_kb_service_note_create_json(title_j->valuestring, content_j->valuestring, tags, author);
   return kb_reply_or_error(fd, resp, "failed to create note");
}

static int kb_handle_note_list(int fd, cJSON *req)
{
   cJSON *tag_j = cJSON_GetObjectItemCaseSensitive(req, "tag");
   cJSON *limit_j = cJSON_GetObjectItemCaseSensitive(req, "limit");
   const char *tag = (cJSON_IsString(tag_j) && tag_j->valuestring[0]) ? tag_j->valuestring : NULL;
   int limit = cJSON_IsNumber(limit_j) ? (int)limit_j->valuedouble : 20;
   if (limit < 1)
      limit = 1;
   if (limit > 100)
      limit = 100;

   cJSON *resp = db2_kb_service_note_list_json(tag, limit);
   return kb_reply_or_error(fd, resp, "failed to list notes");
}

static int kb_handle_note_search(int fd, cJSON *req)
{
   cJSON *query_j = cJSON_GetObjectItemCaseSensitive(req, "query");
   cJSON *limit_j = cJSON_GetObjectItemCaseSensitive(req, "limit");
   if (!cJSON_IsString(query_j))
      return kb_send_error(fd, "notes.search requires query");
   int limit = cJSON_IsNumber(limit_j) ? (int)limit_j->valuedouble : 10;
   if (limit < 1)
      limit = 1;
   if (limit > 100)
      limit = 100;

   cJSON *resp = db2_kb_service_note_search_json(query_j->valuestring, limit);
   return kb_reply_or_error(fd, resp, "failed to search notes");
}

/* Rules + collab_rules + agent + maintenance + decision_log + anti_pattern
 * dispatch handlers live in kb_service_agent.c */
#include "kb_service_agent.h"
#include "kb_service_graph.h"

/* Memory.* dispatch handlers live in kb_service_memory.c */
#include "kb_service_memory.h"

/* artifacts.* dispatch handlers live in kb_service_artifacts.c */
#include "kb_service_artifacts.h"

/* roadmap.* dispatch handlers live in kb_service_roadmap.c */
#include "kb_service_roadmap.h"

static int kb_handle_learning_list(int fd, cJSON *req)
{
   cJSON *state_j = cJSON_GetObjectItemCaseSensitive(req, "state");
   cJSON *sink_j = cJSON_GetObjectItemCaseSensitive(req, "sink");
   cJSON *limit_j = cJSON_GetObjectItemCaseSensitive(req, "limit");
   const char *state =
       (cJSON_IsString(state_j) && state_j->valuestring[0]) ? state_j->valuestring : NULL;
   const char *sink =
       (cJSON_IsString(sink_j) && sink_j->valuestring[0]) ? sink_j->valuestring : NULL;
   int limit = cJSON_IsNumber(limit_j) ? (int)limit_j->valuedouble : 50;
   if (limit < 1)
      limit = 1;
   if (limit > 64)
      limit = 64;

   cJSON *resp = db2_kb_service_learning_list_json(state, sink, limit);
   return kb_reply_or_error(fd, resp, "failed to list learning proposals");
}

/* The endogeneity gate (recursive-self-improvement S0).
 *
 * It has to be answered HERE. The gate reads the learning ledger, which is
 * DB2, and the daemon builds with DB2 compiled out — so a daemon computing it
 * locally always answered "nothing observed" no matter how self-referential
 * the ledger had become. A live run with both services up is what showed that:
 * four committed proposals in Postgres, and the daemon reporting none. */
static int kb_handle_learning_endogeneity(int fd, cJSON *req)
{
   cJSON *window_j = cJSON_GetObjectItemCaseSensitive(req, "window_days");
   int window = cJSON_IsNumber(window_j) ? (int)window_j->valuedouble : 0;

   learning_endogeneity_t endo;
   learning_gate_state_t gate = learning_gate_check_with(
       window > 0 ? window : LEARNING_METRICS_DEFAULT_WINDOW_DAYS,
       LEARNING_ENDOGENEITY_MIN_EXOGENOUS_RATIO, LEARNING_ENDOGENEITY_MIN_SAMPLE, &endo);

   cJSON *resp = cJSON_CreateObject();
   if (!resp)
      return kb_send_error(fd, "out of memory");
   cJSON_AddStringToObject(resp, "gate",
                           gate == LEARNING_GATE_OPEN                ? "open"
                           : gate == LEARNING_GATE_CLOSED_ENDOGENOUS ? "closed"
                                                                     : "unavailable");
   cJSON_AddNumberToObject(resp, "exogenous_ratio", endo.exogenous_ratio);
   cJSON_AddNumberToObject(resp, "committed_total", (double)endo.committed_total);
   cJSON_AddNumberToObject(resp, "committed_exogenous", (double)endo.committed_exogenous);
   cJSON_AddNumberToObject(resp, "committed_endogenous", (double)endo.committed_endogenous);
   cJSON_AddNumberToObject(resp, "window_days", endo.window_days);
   return kb_reply_or_error(fd, resp, "failed to compute endogeneity");
}

/* S5: enter a verdict the router cannot infer.
 *
 * Supersession and post-commit rejection are observable in the router itself.
 * CONTRADICTED is not — deciding that a later fact contradicts an earlier one
 * is a judgement, and guessing it would put a detector's bar in the hands of a
 * heuristic nobody reviewed. So it is entered here, deliberately, by whoever
 * made that judgement. */
/* S4: the evidence probe, installed where the corpus is.
 *
 * The pass deliberately takes an installed probe rather than assuming one, and
 * until now NOTHING installed it — so `learning resolve` could only ever report
 * "no probe" and close nothing. This is that probe: it asks the knowledge base
 * whether the subject is covered now.
 *
 * Conservative by construction. A search that errors returns UNKNOWN, not
 * NONE, so a broken query never reads as "the gap still stands"; and only a
 * non-empty result set counts as coverage. Both directions of doubt leave the
 * item open. */
static curiosity_evidence_t kb_curiosity_probe(const char *gap_type, const char *subject,
                                               const char *evidence)
{
   (void)gap_type;
   (void)evidence;
   if (!subject || !subject[0])
      return CURIOSITY_EVIDENCE_UNKNOWN;

   cJSON *args = cJSON_CreateObject();
   cJSON_AddStringToObject(args, "query", subject);
   cJSON_AddNumberToObject(args, "limit", 3);
   cJSON *found = NULL;
   (void)aimee_module_commands_dispatch("memory.search_graph", args, &found);
   cJSON_Delete(args);
   if (!found)
      return CURIOSITY_EVIDENCE_UNKNOWN;

   /* The payload shape varies by surface; what matters is whether it carries
    * any row at all. An object with no array, or an empty one, is no coverage. */
   curiosity_evidence_t verdict = CURIOSITY_EVIDENCE_NONE;
   const cJSON *child = NULL;
   cJSON_ArrayForEach(child, found)
   {
      if (cJSON_IsArray(child) && cJSON_GetArraySize(child) > 0)
      {
         verdict = CURIOSITY_EVIDENCE_FOUND;
         break;
      }
   }
   if (cJSON_IsArray(found) && cJSON_GetArraySize(found) > 0)
      verdict = CURIOSITY_EVIDENCE_FOUND;
   cJSON_Delete(found);
   return verdict;
}

static int kb_handle_learning_resolve(int fd, cJSON *req)
{
   cJSON *budget_j = cJSON_GetObjectItemCaseSensitive(req, "budget");
   curiosity_resolve_register_probe(kb_curiosity_probe);

   curiosity_resolve_stats_t stats;
   int resolved =
       curiosity_resolve_pass(cJSON_IsNumber(budget_j) ? (int)budget_j->valuedouble : 0, &stats);
   if (resolved < 0)
      return kb_send_error(fd, "failed to read the curiosity backlog");

   cJSON *resp = cJSON_CreateObject();
   if (!resp)
      return kb_send_error(fd, "out of memory");
   cJSON_AddNumberToObject(resp, "resolved", stats.resolved);
   cJSON_AddNumberToObject(resp, "considered", stats.considered);
   cJSON_AddNumberToObject(resp, "still_open", stats.still_open);
   cJSON_AddNumberToObject(resp, "unknown", stats.unknown);
   cJSON_AddNumberToObject(resp, "skipped", stats.skipped);
   cJSON_AddNumberToObject(resp, "budget", stats.budget);
   cJSON_AddBoolToObject(resp, "no_probe", stats.no_probe);
   return kb_reply_or_error(fd, resp, "failed to resolve the backlog");
}

/* S6: the sampler, installed where the bandit is.
 *
 * The policy registry deliberately falls back to the shipped default when no
 * sampler is installed, and until now NOTHING installed one — so no arm was
 * ever sampled and the registry only ever described a decision nobody made.
 * kb_bandit lives in this binary, so this is where it gets wired.
 *
 * A sampler that cannot answer returns a negative index, which the registry
 * reads as declining. That is the honest response when the bandit sidecar is
 * absent: use the default rather than pretend a choice was measured. */
static int kb_policy_sampler(const char *decision_point,
                             const char (*arms)[LEARNING_POLICY_ARM_LEN], int n)
{
   if (!decision_point || n <= 0)
      return -1;

   char ids[KB_BANDIT_MAX_ARMS][KB_BANDIT_MAX_ARM_ID];
   int count = n < KB_BANDIT_MAX_ARMS ? n : KB_BANDIT_MAX_ARMS;
   for (int i = 0; i < count; i++)
      snprintf(ids[i], sizeof(ids[i]), "%s", arms[i]);

   char decision_id[KB_BANDIT_MAX_DECISION] = "";
   int idx = kb_bandit_sample(decision_point, NULL, ids, count, decision_id);
   if (idx < 0 || idx >= count)
      return -1; /* no sidecar, or it declined: the default stands */
   return idx;
}

static void kb_policy_reward_sink(const char *decision_point, const char *arm, double reward)
{
   /* The decision id is minted per sample; without a carried id the posterior
    * cannot be attributed, so this records against the arm directly and lets
    * kb_bandit reject what it cannot key. Losing a sample is better than
    * crediting the wrong arm. */
   (void)kb_bandit_reward(decision_point, "", arm, reward);
}

/* Register the arms this build declares, so the bandit knows the surface
 * before anything samples it. Called once at service start. */
void kb_policy_arms_init(void)
{
   char arms[LEARNING_POLICY_MAX_ARMS][LEARNING_POLICY_ARM_LEN];
   int n = learning_policy_arms(LEARNING_POLICY_PLAN_ADVISORY, arms, LEARNING_POLICY_MAX_ARMS);
   for (int i = 0; i < n; i++)
      (void)kb_bandit_arm_register(LEARNING_POLICY_PLAN_ADVISORY, arms[i], NULL, NULL);
   learning_policy_register_sampler(kb_policy_sampler);
   learning_policy_register_reward_sink(kb_policy_reward_sink);
}

/* Selection, asked of the service that has the bandit. The daemon renders the
 * fragment but cannot sample it, so it asks here and applies the answer. */
static int kb_handle_learning_policy_select(int fd, cJSON *req)
{
   cJSON *point_j = cJSON_GetObjectItemCaseSensitive(req, "decision_point");
   const char *point = (cJSON_IsString(point_j) && point_j->valuestring[0])
                           ? point_j->valuestring
                           : LEARNING_POLICY_PLAN_ADVISORY;

   char arm[LEARNING_POLICY_ARM_LEN] = "";
   if (learning_policy_select(point, arm, sizeof(arm)) != 0)
      return kb_send_error(fd, "unknown decision point");

   cJSON *resp = cJSON_CreateObject();
   if (!resp)
      return kb_send_error(fd, "out of memory");
   cJSON_AddStringToObject(resp, "decision_point", point);
   cJSON_AddStringToObject(resp, "arm", arm);
   const char *dflt = learning_policy_default_arm(point);
   cJSON_AddStringToObject(resp, "default_arm", dflt ? dflt : "");
   return kb_reply_or_error(fd, resp, "failed to select an arm");
}

static int kb_handle_learning_fate(int fd, cJSON *req)
{
   cJSON *id_j = cJSON_GetObjectItemCaseSensitive(req, "id");
   cJSON *fate_j = cJSON_GetObjectItemCaseSensitive(req, "fate");
   if (!cJSON_IsNumber(id_j))
      return kb_send_error(fd, "missing id");
   if (!cJSON_IsString(fate_j) || !learning_fate_is_valid(fate_j->valuestring))
      return kb_send_error(fd, "fate must be standing, superseded, contradicted, or reverted");

   cJSON *reason_j = cJSON_GetObjectItemCaseSensitive(req, "reason");
   if (learning_fate_record((int)id_j->valuedouble, fate_j->valuestring,
                            cJSON_IsString(reason_j) ? reason_j->valuestring : "operator") != 0)
      return kb_send_error(fd, "failed to record the fate");

   cJSON *resp = cJSON_CreateObject();
   if (!resp)
      return kb_send_error(fd, "out of memory");
   cJSON_AddNumberToObject(resp, "id", id_j->valuedouble);
   cJSON_AddStringToObject(resp, "fate", fate_j->valuestring);
   cJSON_AddBoolToObject(resp, "counts_as_regret", learning_fate_is_regret(fate_j->valuestring));
   return kb_reply_or_error(fd, resp, "failed to record the fate");
}

static int kb_handle_learning_mutate(int fd, cJSON *req, const char *verb)
{
   cJSON *id_j = cJSON_GetObjectItemCaseSensitive(req, "id");
   if (!cJSON_IsNumber(id_j))
      return kb_send_error(fd, "missing id");
   int id = (int)id_j->valuedouble;

   cJSON *proposal = NULL;
   int rc = 0;
   if (strcmp(verb, "get") == 0)
      proposal = db2_kb_service_learning_get_json(id);
   else if (strcmp(verb, "accept") == 0)
   {
      if (!db2_is_initialized())
         return kb_send_error(fd, "failed to open knowledge service store");
      learning_proposal_t row;
      rc = learning_accept_proposal(id, &row);
      if (rc == 0)
         proposal = learning_proposal_to_json(&row);
   }
   else if (strcmp(verb, "reject") == 0)
      proposal = db2_kb_service_learning_reject_json(id);
   else
      rc = -1;

   if (rc != 0 || !proposal)
   {
      char msg[128];
      snprintf(msg, sizeof(msg), "learning proposal %s failed", verb);
      return kb_send_error(fd, msg);
   }

   cJSON *resp = jo_ok();
   cJSON_AddItemToObject(resp, "proposal", proposal);
   int srv_rc = kb_send_response(fd, resp);
   cJSON_Delete(resp);
   return srv_rc;
}

/* curator.stages: the curator stage registry as data for the Pipeline GUI to
 * render dynamically (Option B — single source of truth, no hand-kept mirror). */
static int kb_handle_curator_stages(int fd, cJSON *req)
{
   (void)req;
   cJSON *resp = jo_ok();
   cJSON_AddItemToObject(resp, "stages", kb_curator_stages_json());
   cJSON_AddItemToObject(resp, "presets", kb_curator_presets_json());
   int rc = kb_send_response(fd, resp);
   cJSON_Delete(resp);
   return rc;
}

/* Method -> handler dispatch for the uniform `int (int fd, cJSON *req)` kb RPCs.
 * The handful that do not fit that shape stay inline in kb_handle_request
 * (server.info / server.health, which read ctx; the learning.* mutate verbs,
 * which take an extra arg).
 *
 * Deliberately NOT served by aimee-kb (they need DB1 context the kb process
 * lacks) and are ported through aimee-server with DB2 writes routed back via
 * typed kb RPCs:
 *   - maintenance.eval_feedback_loop  (DB1 eval tasks)
 *   - maintenance.run, maintenance.trace_mine  (DB1 context)
 *   - memory.cognify_drain  (spans DB1 cognify_jobs + DB2 memories) */
typedef int (*kb_rpc_fn)(int fd, cJSON *req);
static const struct
{
   const char *method;
   kb_rpc_fn fn;
} kb_rpc_table[] = {
    {"kb.file.get", kb_handle_file_get},
    {"curator.stages", kb_handle_curator_stages},
    {"kb.maintenance.run", kb_handle_maintenance_run},
    {"kb.export", kb_handle_kb_export},
    {"kb.import", kb_handle_kb_import},
    {"code_embeddings_refresh", kb_handle_code_embeddings_refresh},
    {"curiosity.list", kb_handle_curiosity_list},
    {"curiosity.create", kb_handle_curiosity_create},
    {"curiosity.sweep", kb_handle_curiosity_sweep},
    {"curiosity.rescore", kb_handle_curiosity_rescore},
    {"curiosity.get", kb_handle_curiosity_get},
    {"curiosity.update_state", kb_handle_curiosity_update_state},
    {"curiosity.route_top", kb_handle_curiosity_route_top},
    {"notes.create", kb_handle_note_create},
    {"notes.list", kb_handle_note_list},
    {"notes.search", kb_handle_note_search},
    {"rules.list", kb_handle_rules_list},
    {"rules.generate", kb_handle_rules_generate},
    {"collab_rules.propose", kb_handle_collab_rules_propose},
    {"collab_rules.list", kb_handle_collab_rules_list},
    {"collab_rules.list_active", kb_handle_collab_rules_list_active},
    {"collab_rules.approve", kb_handle_collab_rules_approve},
    {"collab_rules.reject", kb_handle_collab_rules_reject},
    {"collab_rules.retire", kb_handle_collab_rules_retire},
    {"collab_rules.inject", kb_handle_collab_rules_inject},
    {"learning.propose_signal", kb_handle_learning_propose_signal},
    {"learning.record_application", kb_handle_learning_record_application},
    {"agent.outcome_record", kb_handle_agent_outcome_record},
    {"agent.hint_consume", kb_handle_agent_hint_consume},

    {"maintenance.rules_decay", kb_handle_rules_decay},
    {"maintenance.calibrate_promotions", kb_handle_maintenance_calibrate_promotions},
    {"maintenance.compute_demotions", kb_handle_maintenance_compute_demotions},
    {"memory.record_retrieval_outcome", kb_handle_memory_record_retrieval_outcome},
    {"ranker.emit_event", kb_handle_ranker_emit_event},
    {"ranker.record_outcome", kb_handle_ranker_record_outcome},

    {"decision_log.insert", kb_handle_decision_log_insert},
    {"decision_log.list", kb_handle_decision_log_list},
    {"anti_pattern.list", kb_handle_anti_pattern_list},
    {"anti_pattern.insert", kb_handle_anti_pattern_insert},
    {"anti_pattern.delete", kb_handle_anti_pattern_delete},
    {"anti_pattern.check", kb_handle_anti_pattern_check},
    {"anti_pattern.bump", kb_handle_anti_pattern_bump},
    {"rules.delete", kb_handle_rules_delete},
    {"rules.update_directive_type", kb_handle_rules_update_directive_type},
    {"feedback.record", kb_handle_feedback_record},
    {"maintenance.expire_session_directives", kb_handle_directive_expire_session},

    {"dashboard.memory_stats", kb_handle_dashboard_memory_stats},
    {"dashboard.logs", kb_handle_dashboard_logs},
    {"dashboard.reminders", kb_handle_dashboard_reminders},
    {"dashboard.recall", kb_handle_dashboard_recall},
    {"dashboard.directives", kb_handle_dashboard_directives},
    {"session_briefing.commitments", kb_handle_session_briefing_commitments},
    {"session_briefing.directives", kb_handle_session_briefing_directives},
    {"memory.assemble_typed_context", kb_handle_memory_assemble_typed_context},
    {"rules.export_jsonl", kb_handle_rules_export_jsonl},
    {"rules.insert", kb_handle_rules_insert},
    {"tool_registry.snapshot", kb_handle_tool_registry_snapshot},
    {"tool_registry.lookup", kb_handle_tool_registry_lookup},
    {"mcp.call", kb_handle_mcp_call},
    {"graph.sync_code", kb_handle_graph_sync_code},
    {"graph.explain", kb_handle_graph_explain},
    {"code.audit", kb_handle_code_audit},
    {"entities.merge", kb_handle_entities_merge},
    {"entities.unmerge", kb_handle_entities_unmerge},
    {"task.list", kb_handle_task_list},
    {"task.create", kb_handle_task_create},
    {"task.update_state", kb_handle_task_update_state},
    {"task.delete", kb_handle_task_delete},
    {"task.add_edge", kb_handle_task_add_edge},
    {"task.get_edges", kb_handle_task_get_edges},
    {"evidence.emit_retrieval_event", kb_handle_evidence_emit_retrieval_event},
    {"evidence.merge_retrieval_event", kb_handle_evidence_merge_retrieval_event},
    {"evidence.trace_retrieval_event", kb_handle_evidence_trace_retrieval_event},
    {"evidence.provenance_retrieval_event", kb_handle_evidence_provenance},
    {"evidence.fidelity_retrieval_event", kb_handle_evidence_fidelity},
    {"css.signals", kb_handle_css_signals},
    {"memory.search_assertions", kb_handle_memory_search_assertions},
    {"artifacts.list_proposed", kb_handle_artifacts_list_proposed},
    {"artifacts.set_state", kb_handle_artifacts_set_state},
    {"roadmap.create_from_decomposition", kb_handle_roadmap_create_from_decomposition},
    {"roadmap.validate", kb_handle_roadmap_validate},
    {"roadmap.show", kb_handle_roadmap_show},
    {"roadmap.list", kb_handle_roadmap_list},
    {"roadmap.rebuild", kb_handle_roadmap_rebuild},
    {"roadmap.report", kb_handle_roadmap_report},
    {"learning.list_proposals", kb_handle_learning_list},
    {"learning.endogeneity", kb_handle_learning_endogeneity},
    {"learning.fate", kb_handle_learning_fate},
    {"learning.resolve", kb_handle_learning_resolve},
    {"learning.policy_select", kb_handle_learning_policy_select},
};

/* Only verifier-owned request state becomes command context. User arguments
 * remain a separate field on the wire and cannot replace this identity. */
static cJSON *kb_command_context(void)
{
   cJSON *context = cJSON_CreateObject();
   if (!context)
      return NULL;
   const kb_principal_t *actor = kb_reqctx_actor();
   char principal[577] = "", transport[577] = "";
   int authenticated =
       actor && actor->authenticated && kb_identity_key(actor, principal, sizeof(principal)) == 0;
   int user_authority =
       authenticated && (actor->kind != KB_PRIN_OWNER || kb_login_throttle_peer_is_loopback());
   const kb_request_context_t *resolved = kb_reqctx_resolved();
   if (authenticated && resolved && resolved->has_transport)
      (void)kb_identity_key(&resolved->transport, transport, sizeof(transport));
   if (!transport[0])
      snprintf(transport, sizeof(transport), "%s", principal);
   cJSON_AddBoolToObject(context, "authenticated", authenticated);
   cJSON_AddBoolToObject(context, "user_authority", user_authority);
   cJSON_AddStringToObject(context, "principal", authenticated ? principal : "");
   cJSON_AddStringToObject(context, "transport_identity", authenticated ? transport : "");
   const char *scope_kind = NULL, *scope_id = NULL;
   if (authenticated && kb_reqctx_verified_scope(&scope_kind, &scope_id))
   {
      cJSON_AddStringToObject(context, "scope_kind", scope_kind ? scope_kind : "");
      cJSON_AddStringToObject(context, "scope_id", scope_id ? scope_id : "");
   }
   return context;
}

static int kb_handle_request(kb_service_ctx_t *ctx, int fd, cJSON *req)
{
   ctx->last_session_rpc_ts = (long)time(NULL);

   cJSON *method = cJSON_GetObjectItemCaseSensitive(req, "method");
   if (!cJSON_IsString(method))
      return kb_send_error(fd, "missing method");

   if (strcmp(method->valuestring, "server.info") == 0)
   {
      cJSON *resp = jo_ok();
      cJSON_AddNumberToObject(resp, "protocol_version", 1);
      cJSON_AddStringToObject(resp, "server_version", AIMEE_VERSION);
      cJSON_AddStringToObject(resp, "service", "knowledge-service");
      int rc = kb_send_response(fd, resp);
      cJSON_Delete(resp);
      return rc;
   }

   if (strcmp(method->valuestring, "server.health") == 0)
   {
      cJSON *resp = jo_ok();
      cJSON_AddStringToObject(resp, "service", "knowledge-service");
      cJSON_AddNumberToObject(resp, "uptime", (double)(time(NULL) - (time_t)ctx->start_time));
      int rc = kb_send_response(fd, resp);
      cJSON_Delete(resp);
      return rc;
   }

   /* Method names are unique, so table order is irrelevant: first exact match
    * wins, exactly as the prior strcmp ladder did. */
   for (size_t i = 0; i < sizeof(kb_rpc_table) / sizeof(kb_rpc_table[0]); i++)
      if (strcmp(method->valuestring, kb_rpc_table[i].method) == 0)
         return kb_rpc_table[i].fn(fd, req);

   cJSON *module_response = NULL;
   cJSON *command_context = kb_command_context();
   if (!command_context)
      return kb_send_error(fd, "command context unavailable");
   int dispatched = aimee_module_commands_dispatch_context(method->valuestring, req,
                                                           command_context, &module_response);
   cJSON_Delete(command_context);
   if (dispatched)
      return kb_reply_or_error(fd, module_response, "command module unavailable");

   if (strcmp(method->valuestring, "learning.get_proposal") == 0)
      return kb_handle_learning_mutate(fd, req, "get");
   if (strcmp(method->valuestring, "learning.accept_proposal") == 0)
      return kb_handle_learning_mutate(fd, req, "accept");
   if (strcmp(method->valuestring, "learning.reject_proposal") == 0)
      return kb_handle_learning_mutate(fd, req, "reject");

   return kb_send_error(fd, "unknown method");
}

int kb_dispatch_action_json(const char *action, const char *body, int body_len, char *out_buf,
                            int out_cap)
{
   if (!out_buf || out_cap <= 0)
      return 500;
   out_buf[0] = '\0';
   if (!action || !action[0])
   {
      snprintf(out_buf, (size_t)out_cap, "{\"status\":\"error\",\"message\":\"missing action\"}");
      return 400;
   }
   if (strcmp(action, "v1.http") == 0 || strncmp(action, "server.", 7) == 0)
   {
      snprintf(out_buf, (size_t)out_cap, "{\"status\":\"error\",\"message\":\"invalid action\"}");
      return 404;
   }
   if (!g_kb_ctx)
   {
      snprintf(out_buf, (size_t)out_cap,
               "{\"status\":\"error\",\"message\":\"knowledge service unavailable\"}");
      return 503;
   }

   cJSON *req = NULL;
   if (body && body_len > 0)
      req = cJSON_ParseWithLength(body, (size_t)body_len);
   if (!req)
      req = cJSON_CreateObject();
   if (!cJSON_IsObject(req))
   {
      cJSON_Delete(req);
      snprintf(out_buf, (size_t)out_cap,
               "{\"status\":\"error\",\"message\":\"action body must be an object\"}");
      return 400;
   }
   cJSON_DeleteItemFromObjectCaseSensitive(req, "method");
   cJSON_AddStringToObject(req, "method", action);

   FILE *tmp = tmpfile();
   if (!tmp)
   {
      cJSON_Delete(req);
      snprintf(out_buf, (size_t)out_cap,
               "{\"status\":\"error\",\"message\":\"failed to buffer action response\"}");
      return 500;
   }

   int fd = fileno(tmp);
   int rc = kb_handle_request(g_kb_ctx, fd, req);
   cJSON_Delete(req);
   fflush(tmp);
   if (fseek(tmp, 0, SEEK_SET) != 0)
   {
      fclose(tmp);
      snprintf(out_buf, (size_t)out_cap,
               "{\"status\":\"error\",\"message\":\"failed to read action response\"}");
      return 500;
   }

   size_t n = fread(out_buf, 1, (size_t)out_cap - 1, tmp);
   out_buf[n] = '\0';
   fclose(tmp);
   while (n > 0 && (out_buf[n - 1] == '\n' || out_buf[n - 1] == '\r'))
      out_buf[--n] = '\0';
   if (n == 0)
   {
      snprintf(out_buf, (size_t)out_cap,
               "{\"status\":\"error\",\"message\":\"empty action response\"}");
      return 500;
   }
   int status = rc == 0 ? 200 : 500;
   cJSON *parsed = cJSON_Parse(out_buf);
   cJSON *status_j = parsed ? cJSON_GetObjectItemCaseSensitive(parsed, "status") : NULL;
   cJSON *message_j = parsed ? cJSON_GetObjectItemCaseSensitive(parsed, "message") : NULL;
   if (cJSON_IsString(status_j) && strcmp(status_j->valuestring, "error") == 0 &&
       cJSON_IsString(message_j) && strcmp(message_j->valuestring, "unknown method") == 0)
      status = 404;
   cJSON_Delete(parsed);
   return status;
}
