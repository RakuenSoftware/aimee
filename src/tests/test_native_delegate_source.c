/* Real source authority/HMAC/TLS; only storage, registry and workspace owners
 * are replaced at their public dependency seams. No provider or SDK is run. */
#include "server_native_delegate_source.h"
#include "request_context.h"
#include "util.h"
#include "db1_client/server_sessions.h"
#include "db1_client/wfe_store.h"
#include "vault_service.h"
#include "vault_server_key.h"
#include "modules/workspace/workspace_turn.h"
#include "aimee_sha256.h"
#include "cJSON.h"
#include "platform_test_util.h"
#include "config.h"
#include "../server/workflow_control_bus.h"
#include "server.h"
#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static struct
{
   char agent[32], key[64];
   char *wire;
} vault[32];
static int n_vault, custody_fail, storage_fail, registry_fail, workspace_depth, vault_writes;
static db1_server_session_t parents[16];
static int n_parents;
static db1_work_item_t item;
static int item_get_result = 1;
static agent_t registered;
static char root[512], worktree[512];
static int service_owner_enabled, service_owned_calls, rule_count, workflow_enabled;
static trigger_rule_t trigger_rule;
static cJSON *workflow_parameters;
static int workflow_is_trigger = 1, owner_status = 200, owner_registry_error, owner_bad_rows;

int vault_server_kek(uint8_t key[VAULT_KEK_LEN])
{
   /* This test-only key never enters the test inputs or saved context. */
   memset(key, 0xA7, VAULT_KEK_LEN);
   return 0;
}
static int vault_slot(const char *agent, const char *key)
{
   assert(
       (!strcmp(agent, "host-native-source") && !strncmp(key, "scope-", 6) && strlen(key) == 54) ||
       (!strcmp(agent, "host-native-trigger") && !strncmp(key, "trigger-", 8) &&
        strlen(key) == 56));
   for (int i = 0; i < n_vault; i++)
      if (!strcmp(vault[i].agent, agent) && !strcmp(vault[i].key, key))
         return i;
   return -1;
}
vault_status_t vault_service_set_server(const char *agent, const char *key, const char *secret)
{
   int i = vault_slot(agent, key);
   if (storage_fail)
      return VAULT_ERR_IO;
   if (i < 0)
   {
      assert(n_vault < 32);
      i = n_vault++;
      snprintf(vault[i].agent, sizeof vault[i].agent, "%s", agent);
      snprintf(vault[i].key, sizeof vault[i].key, "%s", key);
   }
   free(vault[i].wire);
   vault[i].wire = strdup(secret);
   assert(vault[i].wire);
   vault_writes++;
   return VAULT_OK;
}
vault_status_t vault_service_get_server_principal(const char *agent, const char *key, char *out,
                                                  size_t cap)
{
   int i = vault_slot(agent, key);
   if (i < 0 || !vault[i].wire)
      return VAULT_NO_ENTRY;
   assert(strlen(vault[i].wire) < cap);
   snprintf(out, cap, "%s", vault[i].wire);
   return VAULT_OK;
}
vault_status_t vault_service_delete(const char *principal, const char *agent, const char *key)
{
   assert(!strcmp(principal, VAULT_SERVER_PRINCIPAL));
   int i = vault_slot(agent, key);
   if (i < 0)
      return VAULT_NO_ENTRY;
   free(vault[i].wire);
   vault[i].wire = NULL;
   return VAULT_OK;
}
int db1_server_session_create(const char *id, const char *client, const char *principal)
{
   assert(n_parents < 16);
   db1_server_session_t *p = &parents[n_parents++];
   snprintf(p->id, sizeof p->id, "%s", id);
   snprintf(p->client_type, sizeof p->client_type, "%s", client);
   snprintf(p->principal, sizeof p->principal, "%s", principal);
   return 0;
}
int db1_server_session_get(const char *id, db1_server_session_t *out)
{
   for (int i = 0; i < n_parents; i++)
      if (!strcmp(parents[i].id, id))
      {
         *out = parents[i];
         return 0;
      }
   return -1;
}
int db1_server_session_delete(const char *id)
{
   for (int i = 0; i < n_parents; i++)
      if (!strcmp(parents[i].id, id))
         parents[i].id[0] = 0;
   return 0;
}
int db1_work_item_get(const char *id, db1_work_item_t *out)
{
   if (strcmp(id, item.work_item_id))
      return 0;
   if (item_get_result == 1)
      *out = item;
   return item_get_result;
}
int agent_load_config(agent_config_t *out)
{
   memset(out, 0, sizeof *out);
   return registry_fail ? -1 : 0;
}
agent_t *agent_find(agent_config_t *config, const char *name)
{
   (void)config;
   return !strcmp(name, "native-fixture") ? &registered : NULL;
}
int agent_has_role(const agent_t *agent, const char *role)
{
   assert(agent == &registered);
   return !strcmp(role, "reviewer");
}
struct workspace_turn_scope
{
   int depth;
};
int workspace_turn_scope_begin_background(const char *cwd, workspace_turn_scope_t **out)
{
   *out = NULL;
   if (custody_fail || (strcmp(cwd, root) && strcmp(cwd, worktree)))
      return -1;
   *out = calloc(1, sizeof **out);
   assert(*out);
   (*out)->depth = ++workspace_depth;
   return 1;
}
int workspace_turn_scope_begin_owned_source(const char *cwd, int held_fd,
                                            workspace_turn_scope_t **out)
{
   *out = NULL;
   assert(service_owner_enabled && "human source entered the registered-service owner seam");
   struct stat held, current;
   assert(!fstat(held_fd, &held) && !stat(cwd, &current));
   assert(S_ISDIR(held.st_mode) && held.st_dev == current.st_dev && held.st_ino == current.st_ino);
   service_owned_calls++;
   return workspace_turn_scope_begin_background(cwd, out);
}
static void owner_row(cJSON *rows, const char *origin, const char *source, const char *event,
                      const char *workspace, const char *ref, const char *pipeline,
                      const char *mode, double maximum)
{
   cJSON *row = cJSON_CreateObject();
   cJSON_AddStringToObject(row, "origin", origin);
   cJSON_AddStringToObject(row, "source", source);
   cJSON_AddStringToObject(row, "event", event);
   cJSON_AddStringToObject(row, "workspace", workspace);
   cJSON_AddStringToObject(row, "schedule", ref);
   cJSON_AddStringToObject(row, "template", pipeline);
   cJSON_AddStringToObject(row, "mode", mode);
   cJSON_AddNumberToObject(row, "max_spend_usd", maximum);
   cJSON_AddStringToObject(row, "last_error", "historical operational failure is not registration");
   cJSON_AddItemToArray(rows, row);
}
int workflow_control_request(const char *method, const char *path, const char *query,
                             const char *body, int body_len, const char *principal,
                             int workflow_operator, char *response, int cap)
{
   assert(service_owner_enabled && "human source queried service registrations");
   assert(!strcmp(method, "GET") && !strcmp(path, "/v1/workflow/triggers"));
   assert(!query[0] && !body && !body_len && !strcmp(principal, VAULT_SERVER_PRINCIPAL));
   assert(!workflow_operator);
   if (owner_status != 200)
   {
      snprintf(response, (size_t)cap, "{\"error\":\"owner unavailable\"}");
      return owner_status;
   }
   if (owner_bad_rows == 2)
   {
      snprintf(response, (size_t)cap, "malformed owner JSON");
      return 200;
   }
   cJSON *reply = cJSON_CreateObject(), *rows = cJSON_AddArrayToObject(reply, "triggers");
   if (rule_count == 1)
      owner_row(rows, "config", trigger_rule.source, trigger_rule.event, trigger_rule.workspace,
                trigger_rule.schedule, trigger_rule.pipeline_template, trigger_rule.mode,
                trigger_rule.max_spend_usd);
   if (workflow_enabled && workflow_is_trigger)
      owner_row(
          rows, "workflow", "watch-dir",
          cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(workflow_parameters, "dir")),
          cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(workflow_parameters, "workspace")),
          cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(workflow_parameters, "ref")),
          "configured-workflow",
          cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(workflow_parameters, "mode")),
          cJSON_GetObjectItemCaseSensitive(workflow_parameters, "max_spend_usd")->valuedouble);
   if (owner_registry_error)
      cJSON_AddStringToObject(reply, "registry_error", "configuration owner refused registry");
   if (owner_bad_rows)
      cJSON_ReplaceItemInObjectCaseSensitive(reply, "triggers", cJSON_CreateString("malformed"));
   char *wire = cJSON_PrintUnformatted(reply);
   assert(wire && strlen(wire) < (size_t)cap);
   snprintf(response, (size_t)cap, "%s", wire);
   free(wire);
   cJSON_Delete(reply);
   return 200;
}
int workspace_turn_scope_end(workspace_turn_scope_t *scope)
{
   assert(scope && scope->depth == workspace_depth);
   workspace_depth--;
   free(scope);
   return 0;
}
static int identity(const char *cwd, int *fd, char digest[65])
{
   *fd = -1;
   if (custody_fail || !workspace_depth)
      return -1;
   *fd = open(cwd, O_RDONLY | O_DIRECTORY);
   if (*fd < 0)
      return -1;
   struct stat st;
   assert(!fstat(*fd, &st));
   char value[128];
   snprintf(value, sizeof value, "%llu:%llu", (unsigned long long)st.st_dev,
            (unsigned long long)st.st_ino);
   return aimee_sha256_hex(value, strlen(value), digest);
}
int workspace_turn_source_identity(const char *cwd, int *fd, char digest[65])
{
   return identity(cwd, fd, digest);
}
int workspace_turn_resource_identity(const char *cwd, int *fd, char digest[65])
{
   return identity(cwd, fd, digest);
}

static aimee_block_t block = {.type = AIMEE_BLK_TEXT, .text = "Review the owned source."};
static aimee_message_t message = {.role = "user", .blocks = &block, .n_blocks = 1};
static aimee_request_t request = {.model = "fixture-model", .messages = &message, .n_messages = 1};
static agent_config_t config;
static agent_t selected;
static char error[256];

static int bind(const char *handle, unsigned principal, const char *operation, const char *role,
                int tools, server_native_delegate_source_scope_t **scope)
{
   return server_native_delegate_source_bind(handle, principal, operation, role, "native-fixture",
                                             tools ? worktree : root, &request, tools, 3, 1234,
                                             scope, &config, &selected, error, sizeof error);
}
static void unchanged(const request_context_snapshot_t *context, const agent_request_creds_t *creds,
                      const char *cwd, int depth)
{
   request_context_snapshot_t now;
   request_context_snapshot(&now);
   assert(!memcmp(context, &now, sizeof now));
   agent_request_creds_t current = {0};
   agent_request_creds_snapshot(&current);
   assert(!memcmp(creds, &current, sizeof current));
   assert(!strcmp(run_cmd_get_cwd(), cwd) && workspace_depth == depth);
}
static void denied(const char *handle, unsigned principal, const char *operation, const char *role,
                   int tools)
{
   request_context_snapshot_t context;
   request_context_snapshot(&context);
   agent_request_creds_t creds = {0};
   agent_request_creds_snapshot(&creds);
   char cwd[512];
   snprintf(cwd, sizeof cwd, "%s", run_cmd_get_cwd());
   int depth = workspace_depth;
   server_native_delegate_source_scope_t *scope = (void *)1;
   assert(bind(handle, principal, operation, role, tools, &scope) == -1 && !scope);
   unchanged(&context, &creds, cwd, depth);
}
static void configure_source(const char *actor, const char *parent)
{
   request_context_t context = {0};
   snprintf(context.principal, sizeof context.principal, "tenant");
   snprintf(context.vault_principal, sizeof context.vault_principal, "%s", actor);
   context.vault_transport = ATTEST_WEBCHAT_TRUSTED;
   context.transport = REQ_TRANSPORT_TCP;
   context.trusted = 1;
   context.request_budget_present = 1;
   snprintf(context.request_budget_limits, sizeof context.request_budget_limits, "{\"tokens\":9}");
   request_context_set(&context);
   agent_request_creds_t creds = {0};
   snprintf(creds.session_id, sizeof creds.session_id, "%s", parent);
   snprintf(creds.api_key, sizeof creds.api_key, "outer-secret-never-delegated");
   snprintf(creds.vault_principal, sizeof creds.vault_principal, "%s", actor);
   agent_request_creds_restore(&creds);
   run_cmd_set_cwd(root);
}

static int claims(void)
{
   cJSON *sealed = cJSON_Parse(vault[0].wire);
   cJSON *payload =
       cJSON_Parse(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(sealed, "payload")));
   int count = cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(payload, "claims"));
   cJSON_Delete(payload);
   cJSON_Delete(sealed);
   return count;
}
static void denied_admit(server_native_delegate_source_scope_t *scope,
                         const aimee_request_t *effective)
{
   request_context_snapshot_t context;
   request_context_snapshot(&context);
   agent_request_creds_t creds = {0};
   agent_request_creds_snapshot(&creds);
   char cwd[512];
   snprintf(cwd, sizeof cwd, "%s", run_cmd_get_cwd());
   int depth = workspace_depth, writes = vault_writes;
   assert(server_native_delegate_source_admit(scope, "engineer", effective, error, sizeof error) ==
          -1);
   unchanged(&context, &creds, cwd, depth);
   assert(vault_writes == writes);
}
static void effective_policy_admission(server_native_delegate_source_scope_t *scope)
{
   aimee_block_t policy = {.type = AIMEE_BLK_TEXT, .text = "Host-composed engineer policy."};
   aimee_request_t effective = request;
   effective.system = &policy;
   effective.n_system = 1;
   int original = claims();
   assert(!server_native_delegate_source_admit(scope, "engineer", &effective, error, sizeof error));
   assert(claims() == original + 1 && !request.n_system && !request.system);
   assert(!server_native_delegate_source_admit(scope, "engineer", &effective, error, sizeof error));
   assert(claims() == original + 1);
   assert(!server_native_delegate_source_admit(scope, "auditor", &effective, error, sizeof error));
   assert(claims() == original + 2);
   request_context_t active = *request_context_get(), modified;
   agent_request_creds_t credentials = {0}, other;
   agent_request_creds_snapshot(&credentials);
   modified = active;
   snprintf(modified.vault_principal, sizeof modified.vault_principal, "other-actor");
   request_context_set(&modified);
   denied_admit(scope, &effective);
   modified = active;
   snprintf(modified.principal, sizeof modified.principal, "other-account");
   request_context_set(&modified);
   denied_admit(scope, &effective);
   modified = active;
   modified.vault_transport = ATTEST_NONE;
   request_context_set(&modified);
   denied_admit(scope, &effective);
   modified = active;
   modified.model_execution = 0;
   request_context_set(&modified);
   denied_admit(scope, &effective);
   modified = active;
   modified.operator_script_origin = 1;
   request_context_set(&modified);
   denied_admit(scope, &effective);
   modified = active;
   modified.context_refused = 1;
   request_context_set(&modified);
   denied_admit(scope, &effective);
   request_context_clear();
   denied_admit(scope, &effective);
   request_context_set(&active);
   other = credentials;
   snprintf(other.session_id, sizeof other.session_id, "parent-b");
   agent_request_creds_restore(&other);
   denied_admit(scope, &effective);
   other = credentials;
   snprintf(other.vault_principal, sizeof other.vault_principal, "other-actor");
   agent_request_creds_restore(&other);
   denied_admit(scope, &effective);
   agent_request_creds_restore(&credentials);
   effective.n_messages = -1;
   denied_admit(scope, &effective);
   assert(claims() == original + 2);
}

static const char configuration_hash[] =
    "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
static cJSON *trigger_registration(const char *origin)
{
   cJSON *registration = cJSON_CreateObject();
   cJSON_AddStringToObject(registration, "origin", origin);
   cJSON_AddStringToObject(registration, "source", trigger_rule.source);
   cJSON_AddStringToObject(registration, "event", trigger_rule.event);
   cJSON_AddStringToObject(registration, "workspace", trigger_rule.workspace);
   cJSON_AddStringToObject(registration, "ref", trigger_rule.schedule);
   cJSON_AddStringToObject(registration, "pipeline", trigger_rule.pipeline_template);
   cJSON_AddStringToObject(registration, "mode", trigger_rule.mode);
   cJSON_AddNumberToObject(registration, "max_spend_usd", trigger_rule.max_spend_usd);
   return registration;
}
static void rejected_trigger(const cJSON *registration)
{
   request_context_snapshot_t context;
   request_context_snapshot(&context);
   agent_request_creds_t credentials = {0};
   agent_request_creds_snapshot(&credentials);
   char cwd[512], handle[65] = "must-clear";
   snprintf(cwd, sizeof cwd, "%s", run_cmd_get_cwd());
   int writes = vault_writes, count = n_vault, depth = workspace_depth;
   assert(server_native_delegate_source_trigger(registration, configuration_hash, handle, error,
                                                sizeof error) == -1);
   assert(!handle[0] && vault_writes == writes && n_vault == count);
   unchanged(&context, &credentials, cwd, depth);
}
static int cached_parent_index(const char *handle)
{
   char digest[65], key[64];
   assert(!aimee_sha256_hex(handle, 64, digest));
   snprintf(key, sizeof key, "scope-%.48s", digest);
   int slot = vault_slot("host-native-source", key);
   assert(slot >= 0 && vault[slot].wire);
   cJSON *sealed = cJSON_Parse(vault[slot].wire);
   cJSON *payload =
       cJSON_Parse(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(sealed, "payload")));
   const char *parent = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(payload, "parent"));
   int found = -1;
   for (int i = 0; i < n_parents; i++)
      if (parent && !strcmp(parents[i].id, parent))
         found = i;
   cJSON_Delete(payload);
   cJSON_Delete(sealed);
   assert(found >= 0);
   return found;
}
static void service_trigger_sources(void)
{
   service_owner_enabled = rule_count = 1;
   snprintf(trigger_rule.source, sizeof trigger_rule.source, "watch-dir");
   snprintf(trigger_rule.event, sizeof trigger_rule.event, "docs/proposals/pending");
   assert(strlen(root) < sizeof trigger_rule.workspace);
   memcpy(trigger_rule.workspace, root, strlen(root) + 1);
   snprintf(trigger_rule.schedule, sizeof trigger_rule.schedule, "HEAD");
   snprintf(trigger_rule.pipeline_template, sizeof trigger_rule.pipeline_template,
            "configured-workflow");
   snprintf(trigger_rule.mode, sizeof trigger_rule.mode, "autonomous");
   trigger_rule.max_spend_usd = 3;
   cJSON *registration = trigger_registration("config");
   request_context_snapshot_t outer;
   request_context_snapshot(&outer);
   agent_request_creds_t outer_creds = {0};
   agent_request_creds_snapshot(&outer_creds);
   char handle[65], stable[65], fresh[65];
   assert(!server_native_delegate_source_trigger(registration, configuration_hash, handle, error,
                                                 sizeof error));
   unchanged(&outer, &outer_creds, root, 0);
   assert(service_owned_calls > 0);
   assert(!server_native_delegate_source_trigger(
       registration, "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb", stable,
       error, sizeof error));
   assert(!strcmp(handle, stable)); /* The supplied diagnostic hash is not authority. */
   unchanged(&outer, &outer_creds, root, 0);
   custody_fail = 1;
   rejected_trigger(registration);
   custody_fail = 0;
   int parent_index = cached_parent_index(handle);
   snprintf(parents[parent_index].principal, sizeof parents[parent_index].principal,
            "changed-parent-owner");
   snprintf(item.execution_scope, sizeof item.execution_scope, "%s", handle);
   denied(handle, 76, "work-1", "reviewer", 1);
   assert(!server_native_delegate_source_trigger(registration, configuration_hash, fresh, error,
                                                 sizeof error));
   assert(strcmp(handle, fresh));
   assert(!strcmp(parents[cached_parent_index(fresh)].principal, VAULT_SERVER_PRINCIPAL));
   snprintf(handle, sizeof handle, "%s", fresh);
   unchanged(&outer, &outer_creds, root, 0);
   owner_status = 503;
   rejected_trigger(registration);
   owner_status = 200;
   owner_registry_error = 1;
   rejected_trigger(registration);
   owner_registry_error = 0;
   owner_bad_rows = 1;
   rejected_trigger(registration);
   owner_bad_rows = 2;
   rejected_trigger(registration);
   owner_bad_rows = 0;
   rule_count = 0;
   rejected_trigger(registration);
   rule_count = 1;
   atomic_int cancelled = 1;
   agent_set_request_cancel(&cancelled);
   rejected_trigger(registration);
   agent_set_request_cancel(NULL);
   const char *changed_fields[] = {"origin", "source",   "event", "workspace",
                                   "ref",    "pipeline", "mode"};
   for (size_t i = 0; i < sizeof changed_fields / sizeof changed_fields[0]; i++)
   {
      cJSON *changed = cJSON_Duplicate(registration, 1);
      cJSON_ReplaceItemInObjectCaseSensitive(changed, changed_fields[i],
                                             cJSON_CreateString("not-the-registered-value"));
      rejected_trigger(changed);
      cJSON_Delete(changed);
   }
   cJSON *changed = cJSON_Duplicate(registration, 1);
   cJSON_ReplaceItemInObjectCaseSensitive(changed, "max_spend_usd", cJSON_CreateNumber(4));
   rejected_trigger(changed);
   cJSON_ReplaceItemInObjectCaseSensitive(changed, "max_spend_usd", cJSON_CreateString("3"));
   rejected_trigger(changed);
   cJSON_Delete(changed);
   changed = cJSON_Duplicate(registration, 1);
   cJSON_ReplaceItemInObjectCaseSensitive(changed, "origin", cJSON_CreateString("host-service"));
   rejected_trigger(changed);
   cJSON_Delete(changed);
   changed = cJSON_Duplicate(registration, 1);
   cJSON_ReplaceItemInObjectCaseSensitive(changed, "workspace", cJSON_CreateNumber(1));
   rejected_trigger(changed);
   cJSON_Delete(changed);
   changed = cJSON_Duplicate(registration, 1);
   cJSON_AddStringToObject(changed, "actor", "forged-human");
   rejected_trigger(changed);
   cJSON_Delete(changed);
   changed = cJSON_Duplicate(registration, 1);
   cJSON_AddStringToObject(changed, "event", trigger_rule.event);
   rejected_trigger(changed);
   cJSON_Delete(changed);
   changed = cJSON_Duplicate(registration, 1);
   cJSON_DeleteItemFromObjectCaseSensitive(changed, "mode");
   rejected_trigger(changed);
   cJSON_Delete(changed);
   /* A still-valid signed source does not override the current config owner. */
   snprintf(item.execution_scope, sizeof item.execution_scope, "%s", handle);
   snprintf(trigger_rule.schedule, sizeof trigger_rule.schedule, "changed-ref");
   rejected_trigger(registration);
   denied(handle, 76, "work-1", "reviewer", 1);
   snprintf(trigger_rule.schedule, sizeof trigger_rule.schedule, "HEAD");
   server_native_delegate_source_scope_t *scope = NULL;
   assert(!bind(handle, 76, "work-1", "reviewer", 1, &scope));
   const request_context_t *service = request_context_get();
   assert(!strcmp(service->principal, VAULT_SERVER_PRINCIPAL));
   assert(!strcmp(service->vault_principal, VAULT_SERVER_PRINCIPAL));
   assert(service->vault_transport == ATTEST_NONE && service->model_execution == 1 &&
          !service->trusted && !service->operator_script_origin);
   assert(service->capabilities == (CAPS_READ_ONLY | CAP_DELEGATE | CAP_TOOL_EXECUTE |
                                    CAP_TOOL_BASH | CAP_TOOL_WRITE | CAP_MEMORY_WRITE));
   assert(!strcmp(agent_get_request_vault_principal(), VAULT_SERVER_PRINCIPAL));
   assert(!server_native_delegate_source_admit(scope, "engineer", &request, error, sizeof error));
   server_native_delegate_source_unbind(scope);
   unchanged(&outer, &outer_creds, root, 0);
   assert(!server_native_delegate_source_revoke(handle));
   assert(!server_native_delegate_source_trigger(registration, configuration_hash, fresh, error,
                                                 sizeof error));
   assert(strcmp(handle, fresh)); /* A revoked source is not a reusable cached capability. */
   unchanged(&outer, &outer_creds, root, 0);
   char retired[1024], replaced[65], restored[65];
   snprintf(retired, sizeof retired, "%s.service-cache-retired", root);
   assert(!rename(root, retired) && !mkdir(root, 0700));
   assert(!server_native_delegate_source_trigger(registration, configuration_hash, replaced, error,
                                                 sizeof error));
   assert(strcmp(fresh, replaced)); /* The same configured path is a different source epoch. */
   assert(!rmdir(root) && !rename(retired, root));
   assert(!server_native_delegate_source_trigger(registration, configuration_hash, restored, error,
                                                 sizeof error));
   assert(strcmp(replaced, restored));
   unchanged(&outer, &outer_creds, root, 0);

   /* The workflow definition owner is a separate legitimate registration source. */
   workflow_parameters = cJSON_CreateObject();
   cJSON_AddStringToObject(workflow_parameters, "workspace", root);
   cJSON_AddStringToObject(workflow_parameters, "dir", trigger_rule.event);
   cJSON_AddStringToObject(workflow_parameters, "ref", trigger_rule.schedule);
   cJSON_AddStringToObject(workflow_parameters, "mode", trigger_rule.mode);
   cJSON_AddNumberToObject(workflow_parameters, "max_spend_usd", 3);
   workflow_enabled = 1;
   changed = trigger_registration("workflow");
   assert(!server_native_delegate_source_trigger(changed, configuration_hash, stable, error,
                                                 sizeof error));
   unchanged(&outer, &outer_creds, root, 0);
   workflow_is_trigger = 0;
   rejected_trigger(changed);
   workflow_is_trigger = 1;
   workflow_enabled = 0;
   rejected_trigger(changed);
   cJSON_Delete(changed);
   cJSON_Delete(workflow_parameters);
   workflow_parameters = NULL;
   cJSON_Delete(registration);
}
int main(void)
{
   char path[256];
   assert(snprintf(path, sizeof path, "%s/aimee-sourcecap-unit-XXXXXX", platform_tmpdir()) <
          (int)sizeof path);
   assert(mkdtemp(path));
   snprintf(root, sizeof root, "%s", path);
   snprintf(worktree, sizeof worktree, "%s/child", path);
   assert(!mkdir(worktree, 0700));
   registered.enabled = 1;
   registered.max_turns = 9;
   registered.timeout_ms = 9000;
   snprintf(registered.model, sizeof registered.model, "fixture-model");
   assert(!db1_server_session_create("parent-a", "webchat", "actor-a"));
   assert(!db1_server_session_create("parent-b", "webchat", "actor-b"));
   configure_source("actor-a", "parent-a");
   char handle[65], panel[65], nested[65];
   assert(!server_native_delegate_source_mint_for_workspace("workflow", root, handle, error,
                                                            sizeof error));
   assert(strlen(handle) == 64 && n_vault == 1 && !strstr(vault[0].key, handle));
   char digest[65], expected_key[64];
   assert(!aimee_sha256_hex(handle, 64, digest));
   snprintf(expected_key, sizeof expected_key, "scope-%.48s", digest);
   assert(!strcmp(vault[0].key, expected_key));
   assert(!strstr(vault[0].wire, "outer-secret-never-delegated"));
   snprintf(item.work_item_id, sizeof item.work_item_id, "work-1");
   snprintf(item.repo, sizeof item.repo, "%s", root);
   snprintf(item.worktree, sizeof item.worktree, "%s", worktree);
   snprintf(item.execution_scope, sizeof item.execution_scope, "%s", handle);
   snprintf(item.state, sizeof item.state, "active");
   configure_source("actor-b", "parent-b");
   request_context_snapshot_t outer;
   request_context_snapshot(&outer);
   agent_request_creds_t outer_creds = {0};
   agent_request_creds_snapshot(&outer_creds);
   server_native_delegate_source_scope_t *scope = NULL;
   assert(!bind(handle, 76, "work-1", "reviewer", 1, &scope));
   assert(!strcmp(request_context_get()->vault_principal, "actor-a"));
   assert(request_context_get()->model_execution && !request_context_get()->operator_script_origin);
   agent_request_creds_t inside = {0};
   agent_request_creds_snapshot(&inside);
   assert(!strcmp(inside.session_id, "parent-a") && !inside.api_key[0]);
   assert(selected.max_turns == 3 && selected.timeout_ms == 1234);
   effective_policy_admission(scope);
   /* Nested scopes restore the actual outer host context, not a global default. */
   assert(!server_native_delegate_source_mint_for_workspace("roundtable", root, nested, error,
                                                            sizeof error));
   server_native_delegate_source_scope_t *inner = NULL;
   assert(!bind(nested, 76, "panel-1", "reviewer", 0, &inner));
   server_native_delegate_source_unbind(inner);
   assert(!strcmp(run_cmd_get_cwd(), worktree));
   assert(!strcmp(agent_get_request_session(), "parent-a"));
   server_native_delegate_source_unbind(scope);
   unchanged(&outer, &outer_creds, root, 0);
   denied(handle, 75, "work-1", "reviewer", 1);
   denied("0000000000000000000000000000000000000000000000000000000000000000", 76, "work-1",
          "reviewer", 1);
   denied(handle, 76, "other-work", "reviewer", 1);
   item_get_result = 0;
   denied(handle, 76, "work-1", "reviewer", 1);
   item_get_result = -1;
   denied(handle, 76, "work-1", "reviewer", 1);
   item_get_result = 1;
   denied(handle, 76, "work-1", "unsupported-role", 1);
   request.model = "wrong-model";
   denied(handle, 76, "work-1", "reviewer", 1);
   request.model = "fixture-model";
   registered.primary_only = 1;
   denied(handle, 76, "work-1", "reviewer", 1);
   registered.primary_only = 0;
   registered.enabled = 0;
   denied(handle, 76, "work-1", "reviewer", 1);
   registered.enabled = 1;
   char saved_repo[sizeof item.repo];
   snprintf(saved_repo, sizeof saved_repo, "%s", item.repo);
   snprintf(item.repo, sizeof item.repo, "/other/repository");
   denied(handle, 76, "work-1", "reviewer", 1);
   snprintf(item.repo, sizeof item.repo, "%s", saved_repo);
   registry_fail = 1;
   denied(handle, 76, "work-1", "reviewer", 1);
   registry_fail = 0;
   snprintf(item.state, sizeof item.state, "accepted");
   denied(handle, 76, "work-1", "reviewer", 1);
   snprintf(item.state, sizeof item.state, "active");
   item.worktree[0] = 0;
   denied(handle, 76, "work-1", "reviewer", 1);
   assert(!bind(handle, 76, "work-1", "reviewer", 0, &scope));
   server_native_delegate_source_unbind(scope);
   snprintf(item.worktree, sizeof item.worktree, "%s", worktree);
   storage_fail = 1;
   denied(handle, 76, "work-1", "reviewer", 1);
   storage_fail = 0;
   custody_fail = 1;
   denied(handle, 76, "work-1", "reviewer", 1);
   custody_fail = 0;
   /* Database ownership changes cannot be replaced by claims inside the handle. */
   snprintf(parents[0].principal, sizeof parents[0].principal, "actor-b");
   denied(handle, 76, "work-1", "reviewer", 1);
   snprintf(parents[0].principal, sizeof parents[0].principal, "actor-a");
   char *original = strdup(vault[0].wire);
   cJSON *sealed = cJSON_Parse(vault[0].wire);
   cJSON *payload =
       cJSON_Parse(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(sealed, "payload")));
   cJSON *context = cJSON_GetObjectItemCaseSensitive(payload, "context");
   cJSON_ReplaceItemInObjectCaseSensitive(context, "vault_principal",
                                          cJSON_CreateString("actor-b"));
   char *forged = cJSON_PrintUnformatted(payload);
   cJSON_ReplaceItemInObjectCaseSensitive(sealed, "payload", cJSON_CreateString(forged));
   free(forged);
   free(vault[0].wire);
   vault[0].wire = cJSON_PrintUnformatted(sealed);
   cJSON_Delete(payload);
   cJSON_Delete(sealed);
   denied(handle, 76, "work-1", "reviewer", 1);
   free(vault[0].wire);
   vault[0].wire = original;
   assert(!server_native_delegate_source_mint_for_workspace("roundtable", root, panel, error,
                                                            sizeof error));
   assert(!bind(panel, 76, "panel-A", "reviewer", 0, &scope));
   server_native_delegate_source_unbind(scope);
   denied(panel, 76, "panel-B", "reviewer", 0);
   /* The same authenticated context cannot mint over a different actor's parent. */
   configure_source("actor-a", "parent-b");
   char rejected[65];
   int count = n_vault;
   assert(server_native_delegate_source_mint_for_workspace("workflow", root, rejected, error,
                                                           sizeof error) == -1);
   assert(!rejected[0] && n_vault == count && !workspace_depth);
   request_context_clear();
   assert(server_native_delegate_source_mint_for_workspace("workflow", root, rejected, error,
                                                           sizeof error) == -1);
   configure_source("actor-b", "parent-b");
   assert(!server_native_delegate_source_revoke(handle));
   denied(handle, 76, "work-1", "reviewer", 1);
   service_trigger_sources();
   for (int i = 0; i < n_vault; i++)
      free(vault[i].wire);
   agent_request_creds_t empty = {0};
   agent_request_creds_restore(&empty);
   request_context_clear();
   run_cmd_set_cwd(NULL);
   assert(!workspace_depth && !rmdir(worktree) && !rmdir(root));
   puts("native delegate source: ok");
   return 0;
}
