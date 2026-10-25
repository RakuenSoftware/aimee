#include "server_native_history.h"
#include "server_session_log.h"
#include "server_session_capture.h"
#include "server_session_control.h"
#include "agent_config.h"
#include "agent_tasks.h"
#include "db1_client/agent_jobs.h"
#include "server_learning_outcome.h"
#include "modules/workspace/workspace_turn.h"
#include "util.h"
#include <unistd.h>

int server_learning_project_from_cwd(const char *cwd, char *out, size_t capacity);
#include "aimee_sha256.h"
#include "db1_client/primary_sessions.h"
#include "db1_client/server_sessions.h"
#include <aimee/ir/session_event.h>
#include <openssl/rand.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct server_session_log
{
   char session[128], turn[33], principal[VAULT_PRINCIPAL_MAX];
   char parent[128], role[128];
   int announced;
   cJSON *learning_binding;
   char learning_sequence[32];
   server_session_capture_t *capture, *previous_capture;
   aimee_ir_session_sink_fn previous_sink;
   void *previous_context;
   server_control_scope_t *controls, *previous_controls;
   struct server_session_log *previous;
};
static _Thread_local server_session_log_t *current;
static int random_id(char out[33])
{
   unsigned char bytes[16];
   if (RAND_bytes(bytes, sizeof bytes) != 1)
      return -1;
   for (int i = 0; i < 16; i++)
      snprintf(out + 2 * i, 3, "%02x", bytes[i]);
   return 0;
}
static int log_event(const aimee_session_event_t *event, void *context)
{
   server_session_log_t *scope = context;
   return server_session_capture_sink(event, scope->capture);
}
/* Worker attestation identifies the parent even when no HTTP sink crosses the
 * thread boundary. Only a reference is appended there; all content stays private. */
static int publish_child(server_session_log_t *scope, const char *status)
{
   if (!scope->parent[0])
      return 0;
   aimee_session_event_t event = {.kind = AIMEE_SESSION_CHILD_EXECUTION,
                                  .child_session = scope->session,
                                  .child_turn = scope->turn,
                                  .child_role = scope->role,
                                  .child_learning_binding = scope->learning_binding,
                                  .execution_status = status};
   cJSON *payload = aimee_ir_session_event_json(&event);
   char *wire = payload ? cJSON_PrintUnformatted(payload) : NULL;
   cJSON_Delete(payload);
   char id[96], sequence[32];
   snprintf(id, sizeof id, "%s:child:%s", scope->turn, status);
   int rc = !wire ? -1
            : scope->learning_binding
                ? db1_session_ir_append_verified(scope->parent, id, scope->turn, scope->principal,
                                                 wire, sequence)
                : db1_session_ir_append(scope->parent, id, scope->turn, scope->principal, wire,
                                        sequence);
   if (!rc && scope->learning_binding)
   {
      if (!strcmp(status, "running"))
         snprintf(scope->learning_sequence, sizeof scope->learning_sequence, "%s", sequence);
      else if (scope->learning_sequence[0])
      {
         /* The running binding predates execution and survives a crash between
          * child terminal and parent terminal publication. It is the sole key. */
         event.execution_status = "running";
         cJSON *source_event = aimee_ir_session_event_json(&event);
         char source_id[96];
         snprintf(source_id, sizeof source_id, "%s:child:running", scope->turn);
         server_learning_observe_child(scope->parent, scope->turn, scope->learning_sequence,
                                       source_id, scope->principal, source_event);
         cJSON_Delete(source_event);
      }
   }
   free(wire);
   if (rc)
   {
      server_session_capture_fail(scope->capture);
      aimee_ir_session_fail_capture();
   }
   return rc;
}

static int ensure_owner(const char *session, const char *type, const char *principal)
{
   db1_server_session_owner_t row;
   if (db1_server_session_owner_get(session, &row) != 0)
   {
      // Another admitted worker may create the same parent concurrently.
      (void)db1_server_session_create(session, type, principal);
      if (db1_server_session_owner_get(session, &row) != 0)
         return -1;
   }
   return strcmp(row.principal, principal) ? -1 : 0;
}
static cJSON *delegate_binding(const char *role, const char *agent, const char *provider,
                               const char *model)
{
   /* A nested synchronous model call has no independent durable-job admission,
    * even when it inherits exactly the same role and selected agent. */
   if (current && current->role[0])
      return NULL;
   int job_id = agent_get_durable_job_id();
   if (!role || !agent || !agent[0] || !provider || !provider[0] || !model || !model[0] ||
       job_id <= 0)
      return NULL;
   db1_agent_job_t job = {0};
   if (db1_agent_job_get(job_id, &job))
      return NULL;
   int matches = !strcmp(job.role, role) && !strcmp(job.agent_name, agent);
   db1_agent_job_free(&job);
   if (!matches)
      return NULL; /* Nested model roles cannot attest their inherited outer job. */
   const char *cwd = run_cmd_get_cwd();
   char identity[65], project[257];
   int fd = -1;
   if (!cwd || workspace_turn_source_identity(cwd, &fd, identity))
      return NULL; /* No process-CWD or unapproved workspace fallback. */
   int resolved = server_learning_project_from_cwd(cwd, project, sizeof project);
   if (fd >= 0)
      close(fd);
   if (resolved || !project[0])
      return NULL;
   cJSON *binding = cJSON_CreateObject();
   char job_text[32];
   snprintf(job_text, sizeof job_text, "%d", job_id);
   if (!binding || !cJSON_AddNumberToObject(binding, "version", 1) ||
       !cJSON_AddStringToObject(binding, "job_id", job_text) ||
       !cJSON_AddStringToObject(binding, "scope_kind", "project") ||
       !cJSON_AddStringToObject(binding, "scope_id", project) ||
       !cJSON_AddStringToObject(binding, "agent", agent) ||
       !cJSON_AddStringToObject(binding, "provider", provider) ||
       !cJSON_AddStringToObject(binding, "model", model))
   {
      cJSON_Delete(binding);
      return NULL;
   }
   return binding;
}
int server_session_log_begin(const char *role, const char *prompt, server_session_log_t **out)
{
   return server_session_log_begin_selected(role, prompt, NULL, NULL, NULL, 1, out);
}
int server_session_log_begin_selected(const char *role, const char *prompt, const char *agent,
                                      const char *provider, const char *model, int control_replies,
                                      server_session_log_t **out)
{
   if (!out)
      return -1;
   *out = NULL;
   if (control_replies != 0 && control_replies != 1)
      return -1;
   agent_request_creds_t attestation;
   agent_request_creds_snapshot(&attestation);
   if (!attestation.session_id[0])
      return 1; /* A non-session internal completion has no user history. */
   if (!attestation.vault_principal[0] || aimee_ir_session_capture_failed())
      return -1;
   void *previous_context;
   aimee_ir_session_sink_fn previous_sink = aimee_ir_session_get_sink(&previous_context);
   if (!role && previous_sink)
      return 1;
   server_session_log_t *scope = calloc(1, sizeof *scope);
   if (!scope)
      return -1;
   snprintf(scope->principal, sizeof scope->principal, "%s", attestation.vault_principal);
   if (random_id(scope->turn))
      goto fail;
   if (role)
   {
      if (!role[0] || strlen(role) >= sizeof scope->role)
         goto fail;
      snprintf(scope->role, sizeof scope->role, "%s", role);
      scope->learning_binding = delegate_binding(role, agent, provider, model);
      if (current && strcmp(current->principal, scope->principal))
         goto fail;
      snprintf(scope->parent, sizeof scope->parent, "%s",
               current ? current->session : attestation.session_id);
      int job = agent_get_durable_job_id();
      char key[192], digest[65];
      snprintf(key, sizeof key, "%s:%d:%s", scope->parent, job, job > 0 ? "durable" : scope->turn);
      if (aimee_sha256_hex(key, strlen(key), digest))
         goto fail;
      snprintf(scope->session, sizeof scope->session, "delegate-%s", digest);
   }
   else
      snprintf(scope->session, sizeof scope->session, "%s", attestation.session_id);
   if (ensure_owner(attestation.session_id, "native", scope->principal) ||
       (role && (ensure_owner(scope->parent, "native", scope->principal) ||
                 ensure_owner(scope->session, "delegate", scope->principal))))
      goto fail;
   scope->controls = server_control_scope_open_execution(scope->session, scope->turn,
                                                         scope->principal, attestation.session_id);
   if (!scope->controls)
      goto fail;
   server_control_scope_t *parent_controls = server_control_scope_current();
   if (!control_replies || (parent_controls && !server_control_scope_can_reply(parent_controls)))
      server_control_scope_disable_replies(scope->controls);
   scope->previous_controls = server_control_scope_bind(scope->controls);
   scope->capture = server_session_capture_open(scope->session, scope->turn, scope->principal,
                                                attestation.session_id);
   if (!scope->capture)
   {
      server_control_scope_bind(scope->previous_controls);
      server_control_scope_close(scope->controls);
      goto fail;
   }
   scope->previous_capture = server_session_capture_bind(scope->capture);
   scope->previous_sink = previous_sink;
   scope->previous_context = previous_context;
   scope->previous = current;
   current = scope;
   /* Child conversations own private controls and never borrow the parent's. */
   aimee_ir_session_set_sink(log_event, scope);
   *out = scope;
   aimee_session_event_t started = {.kind = AIMEE_SESSION_EXECUTION, .execution_status = "running"};
   aimee_block_t block = {.type = AIMEE_BLK_TEXT, .text = (char *)(prompt ? prompt : "")};
   aimee_message_t message = {.role = "user", .blocks = &block, .n_blocks = 1};
   aimee_session_event_t user = {
       .kind = AIMEE_SESSION_MESSAGE, .message_id = "user", .message = &message};
   if (aimee_ir_session_emit(&started) || publish_child(scope, "running"))
      return -1;
   scope->announced = scope->parent[0] != 0;
   if (prompt && aimee_ir_session_emit(&user))
      return -1; /* Caller must finish *out, including failed admission. */
   return 0;
fail:
   cJSON_Delete(scope->learning_binding);
   free(scope);
   return -1;
}
int server_session_log_active(void)
{
   return current != NULL;
}
int server_session_log_history_load(cJSON **out)
{
   if (!out)
      return -1;
   *out = NULL;
   if (!current)
      return 1;
   char *json = NULL;
   int rc = db1_session_ir_history_load(current->session, "primary", current->principal,
                                        current->turn, &json);
   if (rc == 0)
   {
      cJSON *receipts = NULL;
      rc = server_native_history_load(current->session, current->principal, current->turn, 0, out,
                                      &receipts);
      cJSON_Delete(receipts);
   }
   free(json);
   return rc;
}
int server_session_log_history_save(const cJSON *messages)
{
   if (!current)
      return 0;
   char checkpoint[33];
   char *json = cJSON_IsArray(messages) ? cJSON_PrintUnformatted(messages) : NULL;
   int rc = json && !random_id(checkpoint)
                ? db1_session_ir_history_save(current->session, checkpoint, "primary",
                                              current->principal, json)
                : -1;
   free(json);
   if (rc)
   {
      server_session_capture_fail(current->capture);
      aimee_ir_session_fail_capture();
   }
   return rc;
}
int server_session_log_finish(server_session_log_t *scope, int success, const char *error)
{
   if (!scope)
      return 0;
   server_session_capture_counts_t counts = {0};
   int count_failed = server_session_capture_counts(scope->capture, &counts);
   int missing = success && (count_failed || !counts.model_completions);
   if (missing)
   {
      success = 0;
      error = "execution completed without canonical model output";
   }
   if (aimee_ir_session_capture_failed() || server_session_capture_failed(scope->capture))
      success = 0;
   /* No new browser reply may be queued once this execution is finishing.
    * Existing callback leases retain the sealed scope until they stop. */
   server_control_scope_close(scope->controls);
   scope->controls = NULL;
   aimee_session_event_t finished = {.kind = AIMEE_SESSION_EXECUTION,
                                     .execution_status = agent_request_cancelled() ? "cancelled"
                                                         : success                 ? "completed"
                                                                                   : "failed",
                                     .execution_error = error && error[0] ? error : NULL};
   int failed = aimee_ir_session_emit(&finished) != 0 || aimee_ir_session_capture_failed();
   failed = failed || missing || server_session_capture_failed(scope->capture);
   if (scope->announced && publish_child(scope, failed ? "failed" : finished.execution_status))
      failed = 1;
   current = scope->previous;
   if (failed)
      server_session_capture_fail(scope->previous_capture);
   server_session_capture_bind(scope->previous_capture);
   server_session_capture_close(scope->capture);
   aimee_ir_session_restore_sink(scope->previous_sink, scope->previous_context, failed);
   server_control_scope_bind(scope->previous_controls);
   cJSON_Delete(scope->learning_binding);
   free(scope);
   return failed ? -1 : 0;
}

int server_session_log_identity(char *session, size_t capacity, char *principal,
                                size_t principal_capacity)
{
   if (!current)
      return 1;
   if (!session || !principal || strlen(current->session) >= capacity ||
       strlen(current->principal) >= principal_capacity)
      return -1;
   snprintf(session, capacity, "%s", current->session);
   snprintf(principal, principal_capacity, "%s", current->principal);
   return 0;
}
