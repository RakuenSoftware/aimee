/* server_compute_mailbox.c: split from server_compute.c into a real translation unit
 * (was server_compute_mailbox.inc, textually included only to stay under the
 * line-check ceiling). Cross-TU declarations live in the module header. */
#ifndef _GNU_SOURCE /* strcasestr/memmem are GNU extensions (container gcc) */
#define _GNU_SOURCE
#endif
#include "server_compute_internal.h"
#include "server_delegate_scope.h"
#include "server_session_question.h"
#include "server_session_control.h"
#include "aimee.h"
#include "json_fluent.h" /* jo_ok */
#include "db1_client/db1.h"
#include "server_delegate_monitor.h" /* delegate heartbeat begin/end (keep slow delegates alive) */
#include "server_compute_impl.h"
#include "agent_config.h"
#include <aimee/gateway/gateway_policy.h>
#include "presence.h"
#include "compute_pool.h"
#include "agent.h"
#include "agent_coord.h"
#include "cmd_agent_delegate_impl.h"
#include "config.h"
#include "token_tracker.h"
#include <aimee/delegates/delegate_credential_retry.h>
#include <aimee/delegates/delegate_launch.h>
#include <aimee/delegates/delegate_source_authority.h>
#include "agent_source_authority.h" /* TLS source-authority context (race-free in-process) */
#include "server_coord_dispatcher.h"
#include <aimee/delegates/delegate_credentials.h>
#include "vault_service.h" /* WP-C.1 vault-first credential resolution */
#include <openssl/crypto.h>
#include <aimee/delegates/delegate_economics.h>
#include <aimee/delegates/delegate_run_phases.h>
#include "db1_client/delegate_learning.h"
#include "kb_client.h"
#include "kb_bandit.h"
#include "db1_client/interaction_events.h"
#include <aimee/delegates/delegate_role.h>
#include "evidence_replay.h"
#include "guardrails.h"
#include "liveness.h"
#include "log.h"
#include "model_registry.h"
#include "openai_runs_store.h"
#include "platform_process.h"
#include "prompts.h"
#include "persona.h"
#include "server_http.h"
#include "provider_catalog.h"
#include "role_templates.h"
#include <aimee/workspace/workspace.h>
#include "modules/workspace/workspace_provider.h"
#include "modules/workspace/workspace_turn.h"
#include "cJSON.h"
#include <ctype.h>
#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static int mailbox_condition_init(pthread_cond_t *condition)
{
#ifdef __APPLE__
   /* Darwin supplies a relative wait instead of condition clock selection. */
   return pthread_cond_init(condition, NULL);
#else
   pthread_condattr_t attributes;
   int result = pthread_condattr_init(&attributes);
   if (result)
      return result;
   result = pthread_condattr_setclock(&attributes, CLOCK_MONOTONIC);
   if (!result)
      result = pthread_cond_init(condition, &attributes);
   pthread_condattr_destroy(&attributes);
   return result;
#endif
}

delegation_mailbox_t *mailbox_acquire(const char *delegation_id)
{
   pthread_mutex_lock(&g_mailbox_lock);
   for (int i = 0; i < MAX_ACTIVE_DELEGATIONS; i++)
   {
      if (!g_mailboxes[i].references)
      {
         delegation_mailbox_t *mb = &g_mailboxes[i];
         snprintf(mb->delegation_id, sizeof(mb->delegation_id), "%s", delegation_id);
         int result = pthread_mutex_init(&mb->lock, NULL);
         if (result)
         {
            pthread_mutex_unlock(&g_mailbox_lock);
            return NULL;
         }
         result = mailbox_condition_init(&mb->reply_ready);
         if (result)
         {
            pthread_mutex_destroy(&mb->lock);
            pthread_mutex_unlock(&g_mailbox_lock);
            return NULL;
         }
         mb->reply[0] = '\0';
         mb->has_reply = 0;
         mb->active = 1;
         mb->references = 1;
         pthread_mutex_unlock(&g_mailbox_lock);
         return mb;
      }
   }
   pthread_mutex_unlock(&g_mailbox_lock);
   return NULL;
}

int delegate_agent_uses_mistral_path(const agent_t *agent)
{
   if (!agent)
      return 0;
   if (strcmp(agent->provider, "mistral") == 0)
      return 1;
   return strcmp(agent->backend, AGENT_BACKEND_PROVIDER_CLI) == 0 &&
          (strcmp(agent->cli_kind, "mistral") == 0 ||
           strcmp(agent->cli_kind, "mistral-plan") == 0 || strcmp(agent->cli_kind, "vibe") == 0 ||
           strcmp(agent->cli_kind, "vibe-plan") == 0);
}

static int mailbox_retain(delegation_mailbox_t *mb)
{
   if (!mb)
      return 0;
   pthread_mutex_lock(&g_mailbox_lock);
   int retained = mb->references && mb->active;
   if (retained)
      mb->references++;
   pthread_mutex_unlock(&g_mailbox_lock);
   return retained;
}
static void mailbox_put_locked(delegation_mailbox_t *mb)
{
   if (--mb->references == 0)
   {
      pthread_cond_destroy(&mb->reply_ready);
      pthread_mutex_destroy(&mb->lock);
      mb->delegation_id[0] = '\0';
      mb->reply[0] = '\0';
      mb->has_reply = 0;
   }
}
void mailbox_put(delegation_mailbox_t *mb)
{
   if (!mb)
      return;
   pthread_mutex_lock(&g_mailbox_lock);
   mailbox_put_locked(mb);
   pthread_mutex_unlock(&g_mailbox_lock);
}
void mailbox_release(delegation_mailbox_t *mb)
{
   if (!mb)
      return;
   pthread_mutex_lock(&g_mailbox_lock);
   pthread_mutex_lock(&mb->lock);
   mb->active = 0;
   pthread_cond_broadcast(&mb->reply_ready);
   pthread_mutex_unlock(&mb->lock);
   mailbox_put_locked(mb);
   pthread_mutex_unlock(&g_mailbox_lock);
}

delegation_mailbox_t *mailbox_find(const char *delegation_id)
{
   pthread_mutex_lock(&g_mailbox_lock);
   for (int i = 0; i < MAX_ACTIVE_DELEGATIONS; i++)
   {
      if (g_mailboxes[i].active && strcmp(g_mailboxes[i].delegation_id, delegation_id) == 0)
      {
         delegation_mailbox_t *mb = &g_mailboxes[i];
         mb->references++;
         pthread_mutex_unlock(&g_mailbox_lock);
         return mb;
      }
   }
   pthread_mutex_unlock(&g_mailbox_lock);
   return NULL;
}

/* Keep the original reply/consume contract, while a stopped host may join the
 * delegate without waiting for the entire input timeout. Deadlines are monotonic. */
int mailbox_wait(delegation_mailbox_t *mb, char *out, size_t out_len, int timeout_secs)
{
   if (!mailbox_retain(mb))
      return -1;
   struct timespec deadline;
   if (clock_gettime(CLOCK_MONOTONIC, &deadline))
   {
      mailbox_put(mb);
      return -1;
   }
   deadline.tv_sec += timeout_secs;
   pthread_mutex_lock(&mb->lock);
   while (!mb->has_reply)
   {
      struct timespec now;
      if (!mb->active || agent_request_cancelled() || clock_gettime(CLOCK_MONOTONIC, &now) ||
          now.tv_sec > deadline.tv_sec ||
          (now.tv_sec == deadline.tv_sec && now.tv_nsec >= deadline.tv_nsec))
      {
         pthread_mutex_unlock(&mb->lock);
         mailbox_put(mb);
         return -1;
      }
      struct timespec wake = now;
      wake.tv_nsec += 20000000;
      if (wake.tv_nsec >= 1000000000)
      {
         wake.tv_sec++;
         wake.tv_nsec -= 1000000000;
      }
      if (wake.tv_sec > deadline.tv_sec ||
          (wake.tv_sec == deadline.tv_sec && wake.tv_nsec > deadline.tv_nsec))
         wake = deadline;
#ifdef __APPLE__
      struct timespec relative = {.tv_sec = wake.tv_sec - now.tv_sec,
                                  .tv_nsec = wake.tv_nsec - now.tv_nsec};
      if (relative.tv_nsec < 0)
      {
         relative.tv_sec--;
         relative.tv_nsec += 1000000000;
      }
      int rc = pthread_cond_timedwait_relative_np(&mb->reply_ready, &mb->lock, &relative);
#else
      int rc = pthread_cond_timedwait(&mb->reply_ready, &mb->lock, &wake);
#endif
      if (rc && rc != ETIMEDOUT)
      {
         pthread_mutex_unlock(&mb->lock);
         mailbox_put(mb);
         return -1;
      }
   }
   if (!mb->active || agent_request_cancelled())
   {
      pthread_mutex_unlock(&mb->lock);
      mailbox_put(mb);
      return -1;
   }
   snprintf(out, out_len, "%s", mb->reply);
   mb->has_reply = 0;
   mb->reply[0] = '\0';
   pthread_mutex_unlock(&mb->lock);
   mailbox_put(mb);
   return 0;
}

/* Send a reply to a waiting delegate. */
int mailbox_reply(delegation_mailbox_t *mb, const char *content)
{
   pthread_mutex_lock(&mb->lock);
   if (!mb->active)
   {
      pthread_mutex_unlock(&mb->lock);
      return 1;
   }
   snprintf(mb->reply, sizeof(mb->reply), "%s", content);
   mb->has_reply = 1;
   pthread_cond_signal(&mb->reply_ready);
   pthread_mutex_unlock(&mb->lock);
   return 0;
}

/* Global: current delegation mailbox for the calling thread (used by tool_request_input) */
__thread delegation_mailbox_t *tl_mailbox = NULL;

/* Delegation depth tracking: parent delegation ID and current depth for this thread */
__thread char tl_parent_delegation_id[64] = {0};
__thread int tl_delegation_depth = 0;

void server_delegate_scope_capture(server_delegate_scope_t *out)
{
   memcpy(out->parent, tl_parent_delegation_id, sizeof out->parent);
   out->depth = tl_delegation_depth;
   out->mailbox = tl_mailbox;
}
void server_delegate_scope_restore(const server_delegate_scope_t *scope)
{
   memcpy(tl_parent_delegation_id, scope->parent, sizeof tl_parent_delegation_id);
   tl_delegation_depth = scope->depth;
   tl_mailbox = scope->mailbox;
}

/* Returns the delegation ID of the currently-running delegate on this
 * thread, or NULL when not inside a delegate. Provides agent_tools_dispatch
 * with a write-tracking key that's distinct from the caller's session_id —
 * lets db1_session_stale_reads tell "the child wrote what the parent read"
 * apart from "this session read and wrote the same file." */
const char *delegation_active_id(void)
{
   return tl_parent_delegation_id[0] ? tl_parent_delegation_id : NULL;
}

/* Called by tool_request_input from within the delegate agent */
char *delegation_request_input(const char *question)
{
   if (server_control_scope_current())
      return server_session_question(question, DELEGATION_INPUT_TIMEOUT);
   delegation_mailbox_t *mailbox = tl_mailbox;
   if (!mailbox_retain(mailbox))
      return NULL;

   /* Record the question */
   (void)db1_delegation_message_record(mailbox->delegation_id, "delegate_to_parent", question);

   /* Wait for parent reply */
   char reply[4096];
   if (mailbox_wait(mailbox, reply, sizeof(reply), DELEGATION_INPUT_TIMEOUT) != 0)
   {
      mailbox_put(mailbox);
      return NULL;
   }

   /* Record the reply */
   (void)db1_delegation_message_record(mailbox->delegation_id, "parent_to_delegate", reply);

   mailbox_put(mailbox);
   size_t len = strlen(reply);
   char *out = malloc(len + 1);
   if (out)
      memcpy(out, reply, len + 1);
   return out;
}
