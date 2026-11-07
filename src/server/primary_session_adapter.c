/* primary_session_adapter.c: direct primary-agent conversation sessions. */
#include "primary_session_adapter.h"
#include "headers/server_session_capture.h"
#include "ingress_preinject.h" /* S2 binding: publish primary session id per turn */

#include "agent_adapter.h"
#include "agent_exec.h"
#include "agent_config.h"
#include "db1_client/server_sessions.h"
#include <openssl/rand.h>
#include "config.h"
#include "db1_client/db1.h"
#include "log.h"
#include "model_registry.h"
#include "provider_cli_adapter.h"
#include <aimee/translation/aimee_frontend.h>
#include <aimee/ir/session_event.h>
#include "db1_client/primary_sessions.h"
#include "session_compact.h"
#include "util.h"
#include "cJSON.h"
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

extern int server_clean_retry_begin(const primary_session_request_t *, cJSON **, char **, char[65],
                                    char *, size_t) __attribute__((weak));
extern int server_clean_retry_finish(int, const agent_result_t *) __attribute__((weak));

typedef struct primary_session_state
{
   char id[128];
   char scope_id[128];
   int history_failed;
   char agent_name[MAX_AGENT_NAME];
   char provider[sizeof(((agent_t *)0)->provider)];
   cJSON *messages;
   int busy;
   time_t last_used;
   struct primary_session_state *next;
} primary_session_state_t;

static pthread_mutex_t g_primary_sessions_lock = PTHREAD_MUTEX_INITIALIZER;
static primary_session_state_t *g_primary_sessions;
static unsigned long g_primary_session_seq;
static primary_session_native_reconcile_fn g_primary_native_reconcile;
static primary_session_native_browse_reconcile_fn g_primary_native_browse_reconcile;
static primary_session_native_history_load_fn g_primary_native_history_load;
void primary_session_adapter_set_native_history_load(primary_session_native_history_load_fn fn)
{
   pthread_mutex_lock(&g_primary_sessions_lock);
   g_primary_native_history_load = fn;
   pthread_mutex_unlock(&g_primary_sessions_lock);
}
void primary_session_adapter_set_native_reconcile(primary_session_native_reconcile_fn fn)
{
   pthread_mutex_lock(&g_primary_sessions_lock);
   g_primary_native_reconcile = fn;
   pthread_mutex_unlock(&g_primary_sessions_lock);
}
void primary_session_adapter_set_native_browse_reconcile(
    primary_session_native_browse_reconcile_fn fn)
{
   pthread_mutex_lock(&g_primary_sessions_lock);
   g_primary_native_browse_reconcile = fn;
   pthread_mutex_unlock(&g_primary_sessions_lock);
}
static int primary_native_owners_empty(const char *session, const char *actor)
{
   char *wire = NULL;
   int rc = db1_session_native_active_owned(actor, session, &wire);
   cJSON *owners = !rc && wire && strlen(wire) <= 512 * 1024
                       ? aimee_ir_json_parse_frame(wire, strlen(wire))
                       : NULL;
   int empty = cJSON_IsArray(owners) && cJSON_GetArraySize(owners) == 0;
   free(wire);
   cJSON_Delete(owners);
   return empty ? 0 : -1;
}

static void primary_session_generate_id(char *out, size_t out_len)
{
   if (!out || out_len == 0)
      return;
   struct timespec ts;
   if (clock_gettime(CLOCK_REALTIME, &ts) != 0)
   {
      ts.tv_sec = time(NULL);
      ts.tv_nsec = 0;
   }
   unsigned long seq = ++g_primary_session_seq;
   snprintf(out, out_len, "ps-%d-%lld%09ld-%lu", (int)getpid(), (long long)ts.tv_sec, ts.tv_nsec,
            seq);
}

static int primary_session_matches(const primary_session_state_t *s, const agent_t *agent)
{
   return s && agent && strcmp(s->agent_name, agent->name) == 0 &&
          strcmp(s->provider, agent->provider) == 0;
}

static primary_session_state_t *primary_session_find_locked(const char *id, const agent_t *agent)
{
   if (!id || !id[0] || !agent)
      return NULL;
   for (primary_session_state_t *s = g_primary_sessions; s; s = s->next)
      if (strcmp(s->id, id) == 0 && primary_session_matches(s, agent))
         return s;
   return NULL;
}

static primary_session_state_t *primary_session_create_with_id_locked(const agent_t *agent,
                                                                      const char *id)
{
   primary_session_state_t *s = calloc(1, sizeof(*s));
   if (!s)
      return NULL;
   if (id && id[0])
      snprintf(s->id, sizeof(s->id), "%s", id);
   else
      primary_session_generate_id(s->id, sizeof(s->id));
   snprintf(s->scope_id, sizeof(s->scope_id), "%s", s->id);
   snprintf(s->agent_name, sizeof(s->agent_name), "%s", agent->name);
   snprintf(s->provider, sizeof(s->provider), "%s", agent->provider);
   s->last_used = time(NULL);
   s->next = g_primary_sessions;
   g_primary_sessions = s;
   return s;
}

static primary_session_state_t *primary_session_create_locked(const agent_t *agent)
{
   return primary_session_create_with_id_locked(agent, NULL);
}

/* A one-time import is acknowledged only after its exact legacy source and
 * fidelity report/checkpoint have committed atomically under the durable owner. */
static int primary_history_import_source(const char *target, const char *id, const char *actor,
                                         cJSON **imported, int browse)
{
   *imported = NULL;
   db1_primary_session_row_t row;
   int status = db1_primary_session_get_latest(id, &row);
   if (status != 0)
      return status == 1 ? 1 : -1;
   cJSON *report = NULL;
   cJSON *messages = aimee_frontend_history_import_report(row.messages_json, row.provider, id,
                                                          row.agent_name, &report);
   char *metadata = report ? cJSON_PrintUnformatted(report) : NULL;
   char *history = messages ? cJSON_PrintUnformatted(messages) : strdup("[]");
   const char *digest =
       report ? cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(report, "source_sha256"))
              : NULL;
   int acknowledged =
       metadata && history && digest && actor && actor[0] &&
       (browse ? db1_session_ir_history_import_browse(target, actor, row.agent_name, digest,
                                                      history, metadata) >= 0
               : !db1_session_ir_history_import(target, "primary", actor, id, row.agent_name,
                                                digest, history, metadata));
   free(metadata);
   free(history);
   cJSON_Delete(report);
   db1_primary_session_row_clear(&row);
   if (!acknowledged)
   {
      cJSON_Delete(messages);
      LOG_WARN("primary.session", "legacy source has no acknowledged resumable canonical import");
      return -1;
   }
   *imported = messages;
   return 0;
}

int primary_session_history_open(const char *session, const char *actor)
{
   const char *attested = agent_get_request_vault_principal();
   db1_server_session_owner_t owner;
   if (!session || !session[0] || !actor || !actor[0] || !attested || strcmp(actor, attested) ||
       db1_server_session_owner_get(session, &owner) || strcmp(owner.principal, actor))
      return -1;
   pthread_mutex_lock(&g_primary_sessions_lock);
   primary_session_native_browse_reconcile_fn reconcile = g_primary_native_browse_reconcile;
   pthread_mutex_unlock(&g_primary_sessions_lock);
   char error[512] = {0};
   if (reconcile ? reconcile(session, actor, error) : primary_native_owners_empty(session, actor))
      return -1;
   cJSON *messages = NULL;
   int status = primary_history_import_source(session, session, actor, &messages, 1);
   cJSON_Delete(messages);
   /* Archived/refused sources are browseable metadata, never model admission.
    * Host50 rechecks exact source/hash/owner under its atomic append locks. */
   return status < 0 ? -1 : 0;
}

static int primary_session_adopt_latest_for_id(primary_session_state_t *state, const char *id)
{
   if (!state || !id || !id[0])
      return 0;
   cJSON *messages = NULL;
   int status = primary_history_import_source(state->scope_id, id,
                                              agent_get_request_vault_principal(), &messages, 0);
   if (status == 1)
      return 0;
   if (status < 0 || !messages)
   {
      cJSON_Delete(messages);
      return -1;
   }
   cJSON_Delete(state->messages);
   state->messages = messages;
   return 1;
}

/* Match only documentary annotations on unchanged, ordered User TEXT. The
 * canonical owner is the authority; a compatibility/model view cannot supply a
 * new attachment or select a different occurrence of an identical prompt. */
typedef struct
{
   cJSON *block;
   size_t user_message, block_index;
} primary_user_text_t;

static int primary_user_texts(cJSON *messages, primary_user_text_t *texts, size_t *count)
{
   *count = 0;
   size_t user_message = 0;
   cJSON *message;
   cJSON_ArrayForEach(message, messages)
   {
      const char *role = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(message, "role"));
      if (!role || strcmp(role, "user"))
         continue;
      cJSON *blocks = cJSON_GetObjectItemCaseSensitive(message, "blocks"), *block;
      size_t block_index = 0;
      cJSON_ArrayForEach(block, blocks)
      {
         const char *type = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(block, "type"));
         if (type && !strcmp(type, "text"))
         {
            if (*count == AIMEE_IR_MAX_BLOCKS)
               return -1;
            texts[(*count)++] = (primary_user_text_t){block, user_message, block_index};
         }
         ++block_index;
      }
      ++user_message;
   }
   return 0;
}

static void primary_scan_metadata(cJSON *messages, int *inputs, int *tools)
{
   *inputs = *tools = 0;
   cJSON *message, *block;
   cJSON_ArrayForEach(message, messages)
       cJSON_ArrayForEach(block, cJSON_GetObjectItemCaseSensitive(message, "blocks"))
   {
      *inputs |= cJSON_GetObjectItemCaseSensitive(block, "input_attachment") != NULL;
      *tools |= cJSON_GetObjectItemCaseSensitive(block, "tool_source") != NULL;
   }
}

static int primary_history_metadata(cJSON *messages, int *inputs, int *tools)
{
   if (aimee_ir_history_admit_host(messages) != 0)
      return -1;
   primary_scan_metadata(messages, inputs, tools);
   return 0;
}

static int primary_restore_inputs(cJSON *canonical, cJSON *messages)
{
   primary_user_text_t *before = calloc(AIMEE_IR_MAX_BLOCKS, sizeof(*before));
   primary_user_text_t *after = calloc(AIMEE_IR_MAX_BLOCKS, sizeof(*after));
   size_t n = 0, m = 0;
   int result = -1;
   if (!before || !after || primary_user_texts(canonical, before, &n) ||
       primary_user_texts(messages, after, &m) || n != m)
      goto done;
   for (size_t i = 0; i < n; ++i)
   {
      if (before[i].user_message != after[i].user_message ||
          before[i].block_index != after[i].block_index)
         goto done;
      const char *left =
          cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(before[i].block, "text"));
      const char *right =
          cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(after[i].block, "text"));
      if (!left || !right || strcmp(left, right))
         goto done;
      cJSON *original = cJSON_GetObjectItemCaseSensitive(before[i].block, "input_attachment");
      cJSON *supplied = cJSON_GetObjectItemCaseSensitive(after[i].block, "input_attachment");
      if (supplied && (!original || !cJSON_Compare(original, supplied, 1)))
         goto done;
      if (!original || supplied)
         continue;
      /* No text-only inference when more than one User occurrence could match. */
      for (size_t j = 0; j < n; ++j)
         if (i != j && !strcmp(left, cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(
                                         before[j].block, "text"))))
            goto done;
      cJSON *copy = cJSON_Duplicate(original, 1);
      if (!copy || !cJSON_AddItemToObject(after[i].block, "input_attachment", copy))
      {
         cJSON_Delete(copy);
         goto done;
      }
   }
   result = 0;
done:
   free(before);
   free(after);
   return result;
}

/* Called with cancellation disabled until the primary busy lease is released.
 * The capture history RPC owns its own mutex; no primary global lock is held. */
static int primary_session_store_history(primary_session_state_t *state, cJSON *messages,
                                         const char *active_turn)
{
   unsigned char random[16];
   char checkpoint[33];
   const char *principal = agent_get_request_vault_principal();
   server_session_capture_t *capture = NULL;
   cJSON *canonical = NULL;
   char *json = NULL;
   int inputs = 0, tools = 0, canonical_inputs = 0, canonical_tools = 0;
   int prior_inputs = 0, prior_tools = 0;
   primary_scan_metadata(state->messages, &prior_inputs, &prior_tools);
   int result = -1;
   if (!principal || !principal[0] || primary_history_metadata(messages, &inputs, &tools))
      goto done;
   /* A compatibility compactor cannot erase the evidence that it needs a
    * preserving checkpoint. Source-bearing compaction requires a live owner. */
   if ((prior_inputs || prior_tools) && (!active_turn || !active_turn[0]))
      goto done;
   if (active_turn && active_turn[0])
   {
      if (server_session_capture_retain_current(state->scope_id, principal, &capture) || !capture ||
          strcmp(server_session_capture_session(capture), state->scope_id) ||
          strcmp(server_session_capture_turn(capture), active_turn) ||
          server_session_capture_history(capture, &canonical) ||
          primary_history_metadata(canonical, &canonical_inputs, &canonical_tools))
         goto done;
      if (prior_inputs && !canonical_inputs)
         goto done;
      if ((canonical_inputs || inputs) && primary_restore_inputs(canonical, messages))
         goto done;
      if (primary_history_metadata(messages, &inputs, &tools))
         goto done;
   }
   if ((inputs || tools) && !capture)
      goto done;
   json = cJSON_PrintUnformatted(messages);
   if (!json || RAND_bytes(random, sizeof random) != 1)
      goto done;
   for (int i = 0; i < 16; i++)
      snprintf(checkpoint + 2 * i, 3, "%02x", random[i]);
   if (inputs)
      result = db1_session_input_attachment_history_save(state->scope_id, checkpoint, "primary",
                                                         principal, active_turn, json);
   else if (tools || prior_tools || canonical_tools)
      result = db1_session_tool_source_history_save(state->scope_id, checkpoint, "primary",
                                                    principal, active_turn, json);
   else
      result = db1_session_ir_history_save(state->scope_id, checkpoint, "primary", principal, json);
done:
   free(json);
   cJSON_Delete(canonical);
   server_session_capture_release(capture);
   return result;
}

static int primary_session_load_messages_locked(primary_session_state_t *state,
                                                const char *alternate_id, const char *active_turn)
{
   const char *principal = agent_get_request_vault_principal();
   if (state->history_failed || !principal || !principal[0])
   {
      LOG_WARN("primary.session", "canonical history refused: failed=%d attested=%d",
               state->history_failed, principal && principal[0] ? 1 : 0);
      return -1;
   }
   snprintf(state->scope_id, sizeof state->scope_id, "%s",
            alternate_id && alternate_id[0] ? alternate_id : state->id);
   db1_server_session_owner_t owner;
   if (db1_server_session_owner_get(state->scope_id, &owner) != 0)
   {
      if (db1_server_session_create(state->scope_id, "primary", principal) != 0 ||
          db1_server_session_owner_get(state->scope_id, &owner) != 0)
         return -1;
   }
   if (strcmp(owner.principal, principal))
   {
      LOG_WARN("primary.session", "canonical history owner mismatch");
      return -1;
   }
   /* Keep this state alive/busy, but do not hold every primary session behind
    * an old native process boundary's bounded quiescence wait. */
   char recovery_error[512];
   state->busy = 1;
   primary_session_native_reconcile_fn reconcile = g_primary_native_reconcile;
   pthread_mutex_unlock(&g_primary_sessions_lock);
   int recovered = reconcile ? reconcile(state->scope_id, principal, active_turn, recovery_error)
                             : primary_native_owners_empty(state->scope_id, principal);
   pthread_mutex_lock(&g_primary_sessions_lock);
   state->busy = 0;
   if (recovered)
   {
      LOG_WARN("primary.session", "canonical history ownership recovery refused");
      return -1;
   }
   char *json = NULL;
   int rc = db1_session_ir_history_load(state->scope_id, "primary", principal, active_turn, &json);
   if (rc < 0)
   {
      LOG_WARN("primary.session", "canonical history read unavailable");
      return -1;
   }
   if (rc == 0)
   {
      cJSON *history = NULL, *receipts = NULL;
      int admitted;
      if (g_primary_native_history_load)
      {
         /*28 preserves the definitive Missing/import distinction. Protected
          * diagnostics come exclusively from the authenticated45 projection. */
         admitted = !g_primary_native_history_load(state->scope_id, principal, active_turn, 0,
                                                   &history, &receipts) &&
                    cJSON_IsArray(history) && cJSON_IsArray(receipts);
      }
      else
      {
         history = json ? aimee_ir_json_parse_frame(json, strlen(json)) : NULL;
         admitted = history && !aimee_ir_history_admit(history);
      }
      cJSON_Delete(receipts);
      free(json);
      if (!admitted)
      {
         cJSON_Delete(history);
         return -1;
      }
      cJSON_Delete(state->messages);
      state->messages = history;
      return 0;
   }
   /* Only a log with no completed history may import the old provider rows.
    * Every later turn reads the log, including A -> B -> A and process reload. */
   cJSON_Delete(state->messages);
   state->messages = NULL;
   rc = primary_session_adopt_latest_for_id(state, state->scope_id);
   if (rc != 0)
      return rc < 0 ? -1 : 0;
   if (strcmp(state->scope_id, state->id))
   {
      rc = primary_session_adopt_latest_for_id(state, state->id);
      if (rc != 0)
         return rc < 0 ? -1 : 0;
   }
   return 0;
}

static int primary_session_busy_locked(const primary_session_state_t *state, const char *alternate)
{
   const char *scope = alternate && alternate[0] ? alternate : state->id;
   for (const primary_session_state_t *s = g_primary_sessions; s; s = s->next)
      if (s->busy && (!strcmp(s->id, state->id) || !strcmp(s->scope_id, scope)))
         return 1;
   return 0;
}

int primary_session_adapter_can_handle(const agent_t *agent)
{
   const agent_adapter_t *adapter = NULL;
   if (!agent)
      return 0;

   if (agent_adapter_agent_is_direct(agent))
   {
      adapter = agent_adapter_for_agent(agent);
      return agent_adapter_supports(adapter, AGENT_ADAPTER_CAP_PRIMARY_SESSION);
   }

   if (strcmp(agent->backend, AGENT_BACKEND_PROVIDER_CLI) == 0 ||
       strcmp(agent->backend, AGENT_BACKEND_CLI_STDIO) == 0)
   {
      const provider_cli_adapter_t *provider_cli = provider_cli_adapter_get(agent->cli_kind);
      if (!provider_cli || !provider_cli->native_provider || !provider_cli->native_provider[0])
         return 0;
      adapter = agent_adapter_for_provider(provider_cli->native_provider);
      return agent_adapter_supports(adapter, AGENT_ADAPTER_CAP_PRIMARY_SESSION);
   }

   return 0;
}

int primary_session_adapter_turn(const primary_session_request_t *req, agent_result_t *out,
                                 char *session_id_out, size_t session_id_len)
{
   if (out)
      memset(out, 0, sizeof(*out));
   if (session_id_out && session_id_len > 0)
      session_id_out[0] = '\0';
   if (!req || !req->agent || !out || !req->request ||
       !agent_request_with_tools_supported(req->request) || req->request->n_messages != 1 ||
       strcmp(req->request->messages[0].role, "user"))
   {
      if (out)
         snprintf(out->error, sizeof(out->error), "invalid primary session request");
      return -1;
   }
   if (!primary_session_adapter_can_handle(req->agent))
   {
      snprintf(out->error, sizeof(out->error), "agent '%s' is not a primary session adapter",
               req->agent->name);
      return -1;
   }

   cJSON *initial_messages = NULL;
   char session_id[128] = {0};
   char effective_aimee_session_id[128] = {0};
   const char *requested_id = "";
   if (req->provider_session_id && req->provider_session_id[0])
      requested_id = req->provider_session_id;
   else if (req->aimee_session_id && req->aimee_session_id[0])
      requested_id = req->aimee_session_id;

   pthread_mutex_lock(&g_primary_sessions_lock);
   primary_session_state_t *state = primary_session_find_locked(requested_id, req->agent);
   if (!state && requested_id[0])
      state = primary_session_create_with_id_locked(req->agent, requested_id);
   if (!state)
      state = primary_session_create_locked(req->agent);
   if (!state)
   {
      pthread_mutex_unlock(&g_primary_sessions_lock);
      snprintf(out->error, sizeof(out->error), "failed to create primary session");
      return -1;
   }
   if (primary_session_busy_locked(state, req->aimee_session_id))
   {
      pthread_mutex_unlock(&g_primary_sessions_lock);
      snprintf(out->error, sizeof(out->error), "primary session '%s' already has an active turn",
               state->id);
      return -1;
   }
   if (primary_session_load_messages_locked(state, req->aimee_session_id, req->active_turn_id) != 0)
   {
      pthread_mutex_unlock(&g_primary_sessions_lock);
      snprintf(out->error, sizeof(out->error), "failed to import primary session history");
      return -1;
   }
   state->busy = 1;
   state->last_used = time(NULL);
   snprintf(session_id, sizeof(session_id), "%s", state->id);
   if (state->messages)
      initial_messages = cJSON_Duplicate(state->messages, 1);
   pthread_mutex_unlock(&g_primary_sessions_lock);

   if (session_id_out && session_id_len > 0)
      snprintf(session_id_out, session_id_len, "%s", session_id);
   snprintf(effective_aimee_session_id, sizeof(effective_aimee_session_id), "%s",
            (req->aimee_session_id && req->aimee_session_id[0]) ? req->aimee_session_id
                                                                : session_id);

   /* Compact accumulated history if it is approaching the model's context limit.
    * Only fires when session_compact_pressure says we're at the compact threshold —
    * not on every turn. This prevents a new turn from starting with an already-full
    * context, which would cause the in-loop compaction to fire immediately. */
   if (initial_messages)
   {
      int ctx_window = (req->agent->middleware.context_window > 0)
                           ? req->agent->middleware.context_window
                           : model_context_window(req->agent->model);
      if (ctx_window > 0)
      {
         int estimated = session_compact_estimate_tokens(initial_messages);
         if (session_compact_pressure(estimated, 0, ctx_window, NULL) == SESSION_PRESSURE_COMPACT)
         {
            session_compact_result_t sc_result;
            if (session_compact(initial_messages, NULL, &sc_result) == 0 && sc_result.compacted)
               aimee_log(LOG_INFO, "primary_session",
                         "pre-turn compact: %d→%d messages (%d removed)", sc_result.messages_before,
                         sc_result.messages_after, sc_result.messages_removed);
         }
      }
   }

   if (aimee_path_is_absolute(req->cwd) && !strstr(req->cwd, "/../") && !strstr(req->cwd, "/.."))
      run_cmd_set_cwd(req->cwd);
   if (effective_aimee_session_id[0])
      session_id_set_override(effective_aimee_session_id);
   /* S2 binding seam: publish the primary session id so the gateway router can bind
    * an enforced work-item for this turn (agent_execute below runs the provider on
    * THIS thread). Cleared alongside the override after the turn. */
   ingress_preinject_set_session_id(effective_aimee_session_id);
   ingress_preinject_set_task_requirements(req->task_request);

   cJSON *updated_messages = NULL;
   char retry_attempt[65] = {0};
   char *retry_summary = NULL;
   aimee_request_t retry_request = {0};
   const aimee_request_t *request = req->request;
   int rc = 0;
   if (cJSON_GetObjectItemCaseSensitive(req->task_request, "clean_retry"))
   {
      if (!server_clean_retry_begin)
      {
         snprintf(out->error, sizeof(out->error), "clean retry runtime unavailable");
         rc = -1;
      }
      else
         rc = server_clean_retry_begin(req, &initial_messages, &retry_summary, retry_attempt,
                                       out->error, sizeof(out->error));
   }
   if (rc == 0 && retry_summary && retry_summary[0])
   {
      if (aimee_ir_request_clone(request, &retry_request))
         rc = AGENT_RC_CONTEXT_REFUSED;
      else
      {
         aimee_message_t *message = &retry_request.messages[0];
         aimee_block_t *blocks =
             message->n_blocks < AIMEE_IR_MAX_BLOCKS
                 ? realloc(message->blocks, (size_t)(message->n_blocks + 1) * sizeof *blocks)
                 : NULL;
         if (!blocks)
            rc = AGENT_RC_CONTEXT_REFUSED;
         else
         {
            message->blocks = blocks;
            message->content_scalar = 0;
            aimee_block_t *block = &blocks[message->n_blocks++];
            *block = (aimee_block_t){.type = AIMEE_BLK_TEXT, .text = strdup(retry_summary)};
            aimee_ir_block_set_context(block, AIMEE_CTX_ORIGIN_MEMORY, AIMEE_CTX_AUTH_EVIDENCE,
                                       AIMEE_CTX_TRUST_UNVERIFIED, AIMEE_CTX_SENS_INTERNAL, 1, NULL,
                                       NULL, 0);
            if (!block->text || aimee_ir_request_admit(&retry_request))
               rc = AGENT_RC_CONTEXT_REFUSED;
            request = &retry_request;
         }
      }
   }
   cJSON *retry_task = NULL;
   if (rc == 0 && retry_attempt[0])
   {
      retry_task = req->task_request ? cJSON_Duplicate(req->task_request, 1) : cJSON_CreateObject();
      cJSON *requirements = cJSON_GetObjectItemCaseSensitive(retry_task, "evidence_requirements");
      if (cJSON_IsObject(requirements))
      {
         cJSON_DeleteItemFromObjectCaseSensitive(requirements, "task_revision");
         cJSON_AddStringToObject(requirements, "task_revision", retry_attempt);
      }
      ingress_preinject_set_task_requirements(retry_task);
   }
   if (rc == 0)
      rc = agent_execute_session_with_tools(req->agent, req->network, request, initial_messages,
                                            &updated_messages, out);
   if (retry_attempt[0])
   {
      snprintf(out->clean_retry_attempt, sizeof(out->clean_retry_attempt), "%s", retry_attempt);
      if (!server_clean_retry_finish || server_clean_retry_finish(rc, out) != 0)
      {
         snprintf(out->error, sizeof(out->error),
                  "retry outcome persistence unavailable; reconciliation required");
         rc = -1;
      }
   }
   ingress_preinject_set_task_requirements(NULL);
   cJSON_Delete(retry_task);
   free(retry_summary);
   aimee_request_free(&retry_request);

   session_id_clear_override();
   ingress_preinject_set_session_id(""); /* don't leak this turn's session id */
   run_cmd_set_cwd(NULL);
   cJSON_Delete(initial_messages);

   int checkpoint_cancel_state;
   pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &checkpoint_cancel_state);
   if (rc == 0 && updated_messages &&
       primary_session_store_history(state, updated_messages, req->active_turn_id) != 0)
   {
      state->history_failed = 1;
      rc = AGENT_RC_CAPTURE_FAILED;
      out->success = 0;
      snprintf(out->error, sizeof out->error,
               "canonical history checkpoint failed; reconciliation required");
      snprintf(out->stop_reason, sizeof out->stop_reason, "capture_failed");
   }
   pthread_mutex_lock(&g_primary_sessions_lock);
   state = primary_session_find_locked(session_id, req->agent);
   if (state)
   {
      if (rc == 0 && updated_messages)
      {
         cJSON_Delete(state->messages);
         state->messages = updated_messages;
         updated_messages = NULL;
      }
      state->busy = 0;
      state->last_used = time(NULL);
   }
   pthread_mutex_unlock(&g_primary_sessions_lock);
   cJSON_Delete(updated_messages);

   pthread_setcancelstate(checkpoint_cancel_state, NULL);

   return rc;
}

int primary_session_adapter_compact(const primary_session_request_t *req,
                                    session_compact_result_t *result, char *session_id_out,
                                    size_t session_id_len, char *error_out, size_t error_len)
{
   if (result)
      memset(result, 0, sizeof(*result));
   if (session_id_out && session_id_len > 0)
      session_id_out[0] = '\0';
   if (error_out && error_len > 0)
      error_out[0] = '\0';

   if (!req || !req->agent)
   {
      if (error_out && error_len > 0)
         snprintf(error_out, error_len, "invalid primary session compact request");
      return -1;
   }
   /* Compaction belongs to canonical history; it does not launch the selected
    * model or require a provider-specific primary execution adapter. */
   const char *requested_id = "";
   if (req->aimee_session_id && req->aimee_session_id[0])
      requested_id = req->aimee_session_id;
   else if (req->provider_session_id && req->provider_session_id[0])
      requested_id = req->provider_session_id;
   if (!requested_id[0])
   {
      if (error_out && error_len > 0)
         snprintf(error_out, error_len, "no active primary session to compact");
      return -1;
   }

   char session_id[128] = {0};
   char effective_aimee_session_id[128] = {0};
   cJSON *messages = NULL;

   pthread_mutex_lock(&g_primary_sessions_lock);
   primary_session_state_t *state = primary_session_find_locked(requested_id, req->agent);
   if (!state)
      state = primary_session_create_with_id_locked(req->agent, requested_id);
   if (!state)
   {
      pthread_mutex_unlock(&g_primary_sessions_lock);
      if (error_out && error_len > 0)
         snprintf(error_out, error_len, "failed to create primary session");
      return -1;
   }
   if (primary_session_busy_locked(state, req->aimee_session_id))
   {
      pthread_mutex_unlock(&g_primary_sessions_lock);
      if (error_out && error_len > 0)
         snprintf(error_out, error_len, "primary session '%s' already has an active turn",
                  state->id);
      return -1;
   }
   if (primary_session_load_messages_locked(state, req->aimee_session_id, req->active_turn_id) != 0)
   {
      pthread_mutex_unlock(&g_primary_sessions_lock);
      if (error_out && error_len > 0)
         snprintf(error_out, error_len, "failed to import primary session history");
      return -1;
   }
   if (!state->messages || !cJSON_IsArray(state->messages))
   {
      pthread_mutex_unlock(&g_primary_sessions_lock);
      if (error_out && error_len > 0)
         snprintf(error_out, error_len, "primary session '%s' has no conversation history",
                  state->id);
      return -1;
   }
   state->busy = 1;
   state->last_used = time(NULL);
   snprintf(session_id, sizeof(session_id), "%s", state->id);
   messages = cJSON_Duplicate(state->messages, 1);
   pthread_mutex_unlock(&g_primary_sessions_lock);

   if (!messages)
   {
      pthread_mutex_lock(&g_primary_sessions_lock);
      state = primary_session_find_locked(session_id, req->agent);
      if (state)
         state->busy = 0;
      pthread_mutex_unlock(&g_primary_sessions_lock);
      if (error_out && error_len > 0)
         snprintf(error_out, error_len, "failed to copy primary session history");
      return -1;
   }

   session_compact_result_t local_result;
   int rc = session_compact(messages, NULL, &local_result);
   if (result)
      *result = local_result;

   int checkpoint_cancel_state;
   pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &checkpoint_cancel_state);

   snprintf(effective_aimee_session_id, sizeof(effective_aimee_session_id), "%s",
            (req->aimee_session_id && req->aimee_session_id[0]) ? req->aimee_session_id
                                                                : session_id);

   if (rc == 0 && primary_session_store_history(state, messages, req->active_turn_id) != 0)
      rc = -1;

   pthread_mutex_lock(&g_primary_sessions_lock);
   state = primary_session_find_locked(session_id, req->agent);
   if (state)
   {
      if (rc == 0)
      {
         cJSON_Delete(state->messages);
         state->messages = messages;
         messages = NULL;
      }
      state->busy = 0;
      state->last_used = time(NULL);
   }
   pthread_mutex_unlock(&g_primary_sessions_lock);
   cJSON_Delete(messages);

   pthread_setcancelstate(checkpoint_cancel_state, NULL);
   if (rc != 0)
   {
      if (error_out && error_len > 0)
         snprintf(error_out, error_len, "failed to compact primary session '%s'", session_id);
      return -1;
   }

   if (session_id_out && session_id_len > 0)
      snprintf(session_id_out, session_id_len, "%s", session_id);
   return 0;
}

int primary_session_adapter_compact_by_session_id(const char *session_id,
                                                  session_compact_result_t *result_out,
                                                  char *error_out, size_t error_len)
{
   if (result_out)
      memset(result_out, 0, sizeof(*result_out));
   if (!session_id || !session_id[0])
   {
      if (error_out && error_len > 0)
         snprintf(error_out, error_len, "session_id required");
      return -1;
   }

   agent_t agent = {0};
   char id[128] = "", scope[128] = "";
   pthread_mutex_lock(&g_primary_sessions_lock);
   for (primary_session_state_t *s = g_primary_sessions; s; s = s->next)
   {
      if (strcmp(s->id, session_id) && strcmp(s->scope_id, session_id))
         continue;
      snprintf(agent.name, sizeof agent.name, "%s", s->agent_name);
      snprintf(agent.provider, sizeof agent.provider, "%s", s->provider);
      snprintf(id, sizeof id, "%s", s->id);
      snprintf(scope, sizeof scope, "%s", s->scope_id);
      break;
   }
   pthread_mutex_unlock(&g_primary_sessions_lock);
   if (!id[0])
   {
      if (error_out && error_len)
         snprintf(error_out, error_len, "no active session '%s' to compact", session_id);
      return -1;
   }
   primary_session_request_t req = {
       .agent = &agent, .provider_session_id = id, .aimee_session_id = scope};
   return primary_session_adapter_compact(&req, result_out, NULL, 0, error_out, error_len);
}

void primary_session_adapter_reset(void)
{
   pthread_mutex_lock(&g_primary_sessions_lock);
   primary_session_state_t **slot = &g_primary_sessions;
   while (*slot)
   {
      primary_session_state_t *s = *slot;
      if (s->busy)
      {
         slot = &s->next;
         continue;
      }
      *slot = s->next;
      cJSON_Delete(s->messages);
      free(s);
   }
   pthread_mutex_unlock(&g_primary_sessions_lock);
}
