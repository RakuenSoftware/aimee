/* primary_session_adapter.c: direct primary-agent conversation sessions. */
#include "primary_session_adapter.h"
#include "ingress_preinject.h" /* S2 binding: publish primary session id per turn */

#include "agent_adapter.h"
#include "agent_exec.h"
#include "config.h"
#include "db1.h"
#include "headers/context_engine.h"
#include "log.h"
#include "model_registry.h"
#include "provider_cli_adapter.h"
#include "session_compact.h"
#include "util.h"
#include "cJSON.h"
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* Route compaction through the active context engine.
 * Falls back to session_compact() when the active engine is the compactor. */
static int primary_compact(cJSON *messages, session_compact_result_t *result)
{
   const context_engine_t *eng = context_engine_get_active();
   const context_engine_t *def = context_engine_get_default();
   if (eng && eng != def && eng->compress)
   {
      int rc = eng->compress((context_engine_t *)eng, messages, NULL);
      if (result && rc == 0)
      {
         result->compacted = 1;
         result->messages_before = cJSON_GetArraySize(messages);
         result->messages_after = cJSON_GetArraySize(messages);
         result->messages_removed = 0;
      }
      return rc;
   }
   return session_compact(messages, NULL, result);
}

typedef struct primary_session_state
{
   char id[128];
   char agent_name[MAX_AGENT_NAME];
   char provider[64];
   cJSON *messages;
   int busy;
   time_t last_used;
   struct primary_session_state *next;
} primary_session_state_t;

static pthread_mutex_t g_primary_sessions_lock = PTHREAD_MUTEX_INITIALIZER;
static primary_session_state_t *g_primary_sessions;
static unsigned long g_primary_session_seq;

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

static int primary_session_load_messages_for_id(primary_session_state_t *state, const char *id)
{
   if (!state || !id || !id[0])
      return 0;

   char *json = db1_primary_session_load(id, state->agent_name, state->provider);
   if (!json)
      return 0;

   cJSON *messages = cJSON_Parse(json);
   free(json);
   if (!messages || !cJSON_IsArray(messages))
   {
      cJSON_Delete(messages);
      return 0;
   }

   cJSON_Delete(state->messages);
   state->messages = messages;
   return 1;
}

static void primary_session_load_messages_locked(primary_session_state_t *state,
                                                 const char *alternate_id)
{
   if (!state || state->messages)
      return;
   if (primary_session_load_messages_for_id(state, state->id))
      return;
   if (alternate_id && alternate_id[0] && strcmp(alternate_id, state->id) != 0)
      (void)primary_session_load_messages_for_id(state, alternate_id);
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
   if (!req || !req->agent || !out || !req->user_prompt || !req->user_prompt[0])
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
   char *persist_json = NULL;
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
   primary_session_load_messages_locked(state, req->aimee_session_id);
   if (state->busy)
   {
      pthread_mutex_unlock(&g_primary_sessions_lock);
      snprintf(out->error, sizeof(out->error), "primary session '%s' already has an active turn",
               state->id);
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
            if (primary_compact(initial_messages, &sc_result) == 0 && sc_result.compacted)
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

   cJSON *updated_messages = NULL;
   int max_tokens = req->max_tokens > 0 ? req->max_tokens : AGENT_DEFAULT_MAX_TOKENS;
   double temperature = req->temperature >= 0.0 ? req->temperature : 0.3;
   int rc = agent_execute_session_with_tools(req->agent, req->network, req->system_prompt,
                                             req->user_prompt, max_tokens, temperature,
                                             initial_messages, &updated_messages, out);

   session_id_clear_override();
   ingress_preinject_set_session_id(""); /* don't leak this turn's session id */
   run_cmd_set_cwd(NULL);
   cJSON_Delete(initial_messages);

   if (rc == 0 && updated_messages)
      persist_json = cJSON_PrintUnformatted(updated_messages);

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

   if (rc == 0 && persist_json)
   {
      (void)db1_primary_session_save(session_id, req->agent->name, req->agent->provider,
                                     persist_json);
      if (effective_aimee_session_id[0] && strcmp(effective_aimee_session_id, session_id) != 0)
         (void)db1_primary_session_save(effective_aimee_session_id, req->agent->name,
                                        req->agent->provider, persist_json);
   }
   free(persist_json);

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
   if (!primary_session_adapter_can_handle(req->agent))
   {
      if (error_out && error_len > 0)
         snprintf(error_out, error_len, "agent '%s' is not a direct primary session adapter",
                  req->agent->name);
      return -1;
   }

   const char *requested_id = "";
   if (req->provider_session_id && req->provider_session_id[0])
      requested_id = req->provider_session_id;
   else if (req->aimee_session_id && req->aimee_session_id[0])
      requested_id = req->aimee_session_id;
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
   primary_session_load_messages_locked(state, req->aimee_session_id);
   if (state->busy)
   {
      pthread_mutex_unlock(&g_primary_sessions_lock);
      if (error_out && error_len > 0)
         snprintf(error_out, error_len, "primary session '%s' already has an active turn",
                  state->id);
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
   int rc = primary_compact(messages, &local_result);
   if (result)
      *result = local_result;

   char *persist_json = NULL;
   if (rc == 0)
      persist_json = cJSON_PrintUnformatted(messages);

   snprintf(effective_aimee_session_id, sizeof(effective_aimee_session_id), "%s",
            (req->aimee_session_id && req->aimee_session_id[0]) ? req->aimee_session_id
                                                                : session_id);

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

   if (rc != 0 || !persist_json)
   {
      free(persist_json);
      if (error_out && error_len > 0)
         snprintf(error_out, error_len, "failed to compact primary session '%s'", session_id);
      return -1;
   }

   (void)db1_primary_session_save(session_id, req->agent->name, req->agent->provider, persist_json);
   if (effective_aimee_session_id[0] && strcmp(effective_aimee_session_id, session_id) != 0)
      (void)db1_primary_session_save(effective_aimee_session_id, req->agent->name,
                                     req->agent->provider, persist_json);
   free(persist_json);

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

   char agent_name[MAX_AGENT_NAME] = {0};
   char provider[64] = {0};
   cJSON *messages = NULL;

   pthread_mutex_lock(&g_primary_sessions_lock);
   for (primary_session_state_t *s = g_primary_sessions; s; s = s->next)
   {
      if (strcmp(s->id, session_id) != 0)
         continue;
      if (s->busy)
      {
         pthread_mutex_unlock(&g_primary_sessions_lock);
         if (error_out && error_len > 0)
            snprintf(error_out, error_len, "session '%s' is busy", session_id);
         return -1;
      }
      if (!s->messages || !cJSON_IsArray(s->messages))
         break;
      s->busy = 1;
      snprintf(agent_name, sizeof(agent_name), "%s", s->agent_name);
      snprintf(provider, sizeof(provider), "%s", s->provider);
      messages = cJSON_Duplicate(s->messages, 1);
      break;
   }
   pthread_mutex_unlock(&g_primary_sessions_lock);

   if (!messages)
   {
      if (error_out && error_len > 0)
         snprintf(error_out, error_len, "no active session '%s' to compact", session_id);
      return -1;
   }

   session_compact_result_t local_result;
   int rc = primary_compact(messages, &local_result);
   if (result_out && rc == 0)
      *result_out = local_result;

   char *persist_json =
       (rc == 0 && local_result.compacted) ? cJSON_PrintUnformatted(messages) : NULL;

   pthread_mutex_lock(&g_primary_sessions_lock);
   for (primary_session_state_t *s = g_primary_sessions; s; s = s->next)
   {
      if (strcmp(s->id, session_id) != 0)
         continue;
      if (rc == 0 && local_result.compacted)
      {
         cJSON_Delete(s->messages);
         s->messages = messages;
         messages = NULL;
      }
      s->busy = 0;
      s->last_used = time(NULL);
      break;
   }
   pthread_mutex_unlock(&g_primary_sessions_lock);
   cJSON_Delete(messages);

   if (persist_json)
   {
      (void)db1_primary_session_save(session_id, agent_name, provider, persist_json);
      free(persist_json);
   }

   if (rc != 0)
   {
      if (error_out && error_len > 0)
         snprintf(error_out, error_len, "failed to compact session '%s'", session_id);
      return -1;
   }
   return 0;
}

void primary_session_adapter_reset(void)
{
   pthread_mutex_lock(&g_primary_sessions_lock);
   primary_session_state_t *s = g_primary_sessions;
   g_primary_sessions = NULL;
   while (s)
   {
      primary_session_state_t *next = s->next;
      cJSON_Delete(s->messages);
      free(s);
      s = next;
   }
   pthread_mutex_unlock(&g_primary_sessions_lock);
}
