/* server_compute_mailbox.c: split from server_compute.c into a real translation unit
 * (was server_compute_mailbox.inc, textually included only to stay under the
 * line-check ceiling). Cross-TU declarations live in the module header. */
#ifndef _GNU_SOURCE /* strcasestr/memmem are GNU extensions (container gcc) */
#define _GNU_SOURCE
#endif
#include "server_compute_internal.h"
#include "aimee.h"
#include "json_fluent.h" /* jo_ok */
#include "db1.h"
#include "server_delegate_monitor.h" /* delegate heartbeat begin/end (keep slow delegates alive) */
#include "server_compute_impl.h"
#include "agent_config.h"
#include "gateway_policy.h"
#include "presence.h"
#include "compute_pool.h"
#include "agent.h"
#include "agent_coord.h"
#include "cmd_agent_delegate_impl.h"
#include "config.h"
#include "token_tracker.h"
#include "delegate_credential_retry.h"
#include "delegate_launch.h"
#include "delegate_source_authority.h"
#include "agent_source_authority.h" /* TLS source-authority context (race-free in-process) */
#include "server_coord_dispatcher.h"
#include "delegate_credentials.h"
#include "vault_service.h" /* WP-C.1 vault-first credential resolution */
#include <openssl/crypto.h>
#include "delegate_economics.h"
#include "delegate_run_phases.h"
#include "db1/delegate_learning.h"
#include "kb_client.h"
#include "kb_bandit.h"
#include "db1/interaction_events.h"
#include "delegate_role.h"
#include "delegate_ensemble.h"
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
#include "workspace.h"
#include "workspace_provider.h"
#include "workspace_turn.h"
#include "cJSON.h"
#include <ctype.h>
#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

delegation_mailbox_t *mailbox_acquire(const char *delegation_id)
{
   pthread_mutex_lock(&g_mailbox_lock);
   for (int i = 0; i < MAX_ACTIVE_DELEGATIONS; i++)
   {
      if (!g_mailboxes[i].active)
      {
         delegation_mailbox_t *mb = &g_mailboxes[i];
         snprintf(mb->delegation_id, sizeof(mb->delegation_id), "%s", delegation_id);
         pthread_mutex_init(&mb->lock, NULL);
         pthread_cond_init(&mb->reply_ready, NULL);
         mb->reply[0] = '\0';
         mb->has_reply = 0;
         mb->active = 1;
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

void mailbox_release(delegation_mailbox_t *mb)
{
   if (!mb)
      return;
   pthread_mutex_lock(&g_mailbox_lock);
   mb->active = 0;
   mb->delegation_id[0] = '\0';
   pthread_mutex_destroy(&mb->lock);
   pthread_cond_destroy(&mb->reply_ready);
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
         pthread_mutex_unlock(&g_mailbox_lock);
         return mb;
      }
   }
   pthread_mutex_unlock(&g_mailbox_lock);
   return NULL;
}

/* Wait for a parent reply with timeout. Returns 0 on reply, -1 on timeout. */
int mailbox_wait(delegation_mailbox_t *mb, char *out, size_t out_len, int timeout_secs)
{
   struct timespec ts;
   clock_gettime(CLOCK_REALTIME, &ts);
   ts.tv_sec += timeout_secs;

   pthread_mutex_lock(&mb->lock);
   while (!mb->has_reply)
   {
      int rc = pthread_cond_timedwait(&mb->reply_ready, &mb->lock, &ts);
      if (rc != 0) /* ETIMEDOUT or error */
      {
         pthread_mutex_unlock(&mb->lock);
         return -1;
      }
   }
   snprintf(out, out_len, "%s", mb->reply);
   mb->has_reply = 0;
   mb->reply[0] = '\0';
   pthread_mutex_unlock(&mb->lock);
   return 0;
}

/* Send a reply to a waiting delegate. */
void mailbox_reply(delegation_mailbox_t *mb, const char *content)
{
   pthread_mutex_lock(&mb->lock);
   snprintf(mb->reply, sizeof(mb->reply), "%s", content);
   mb->has_reply = 1;
   pthread_cond_signal(&mb->reply_ready);
   pthread_mutex_unlock(&mb->lock);
}

/* Global: current delegation mailbox for the calling thread (used by tool_request_input) */
__thread delegation_mailbox_t *tl_mailbox = NULL;

/* Delegation depth tracking: parent delegation ID and current depth for this thread */
__thread char tl_parent_delegation_id[64] = {0};
__thread int tl_delegation_depth = 0;

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
   if (!tl_mailbox || !tl_mailbox->active)
      return NULL;

   /* Record the question */
   (void)db1_delegation_message_record(tl_mailbox->delegation_id, "delegate_to_parent", question);

   /* Wait for parent reply */
   char reply[4096];
   if (mailbox_wait(tl_mailbox, reply, sizeof(reply), DELEGATION_INPUT_TIMEOUT) != 0)
      return NULL; /* timeout */

   /* Record the reply */
   (void)db1_delegation_message_record(tl_mailbox->delegation_id, "parent_to_delegate", reply);

   size_t len = strlen(reply);
   char *out = malloc(len + 1);
   if (out)
      memcpy(out, reply, len + 1);
   return out;
}
