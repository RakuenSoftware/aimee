/* Compose the actual source issuer/admitter with the actual runtime owner gate.
 * Reuse the existing runtime dependency fixture under private test names; its
 * source/credential substitutes cannot satisfy the production source owner.
 * No SDK/provider or complete runtime is started: a deliberate DB miss proves
 * whether the real constructor crossed its owner gate. */
#include "server_native_delegate_source.h"
#include "agent_exec.h"
#include "db1_client/server_sessions.h"
__typeof__(agent_get_request_session) composition_mock_agent_get_request_session;
#define agent_get_request_session composition_mock_agent_get_request_session
__typeof__(agent_get_request_cancel) composition_mock_agent_get_request_cancel;
#define agent_get_request_cancel composition_mock_agent_get_request_cancel
__typeof__(agent_get_request_vault_principal) composition_mock_agent_get_request_vault_principal;
#define agent_get_request_vault_principal composition_mock_agent_get_request_vault_principal
__typeof__(agent_request_cancel_scope_bind) composition_mock_agent_request_cancel_scope_bind;
#define agent_request_cancel_scope_bind composition_mock_agent_request_cancel_scope_bind
__typeof__(agent_request_cancel_scope_current) composition_mock_agent_request_cancel_scope_current;
#define agent_request_cancel_scope_current composition_mock_agent_request_cancel_scope_current
__typeof__(agent_request_cancelled) composition_mock_agent_request_cancelled;
#define agent_request_cancelled composition_mock_agent_request_cancelled
__typeof__(agent_request_creds_restore) composition_mock_agent_request_creds_restore;
#define agent_request_creds_restore composition_mock_agent_request_creds_restore
__typeof__(agent_request_creds_snapshot) composition_mock_agent_request_creds_snapshot;
#define agent_request_creds_snapshot composition_mock_agent_request_creds_snapshot
__typeof__(agent_set_request_api_credentials_for)
    composition_mock_agent_set_request_api_credentials_for;
#define agent_set_request_api_credentials_for composition_mock_agent_set_request_api_credentials_for
__typeof__(agent_set_request_cancel) composition_mock_agent_set_request_cancel;
#define agent_set_request_cancel composition_mock_agent_set_request_cancel
__typeof__(agent_set_request_claude_creds_for) composition_mock_agent_set_request_claude_creds_for;
#define agent_set_request_claude_creds_for composition_mock_agent_set_request_claude_creds_for
__typeof__(agent_set_request_codex_creds_for) composition_mock_agent_set_request_codex_creds_for;
#define agent_set_request_codex_creds_for composition_mock_agent_set_request_codex_creds_for
__typeof__(agent_set_request_session) composition_mock_agent_set_request_session;
#define agent_set_request_session composition_mock_agent_set_request_session
__typeof__(db1_server_session_owner_get) composition_mock_db1_server_session_owner_get;
#define db1_server_session_owner_get composition_mock_db1_server_session_owner_get
__typeof__(server_native_delegate_source_runtime_consume)
    composition_mock_server_native_delegate_source_runtime_consume;
#define server_native_delegate_source_runtime_consume                                              \
   composition_mock_server_native_delegate_source_runtime_consume
#define request  composition_runtime_request
#define identity composition_runtime_identity
#define main     composition_runtime_unused_main
#include "test_server_native_runtime.c"
#undef main
#undef identity
#undef request
#undef agent_get_request_session
#undef agent_get_request_cancel
#undef agent_get_request_vault_principal
#undef agent_request_cancel_scope_bind
#undef agent_request_cancel_scope_current
#undef agent_request_cancelled
#undef agent_request_creds_restore
#undef agent_request_creds_snapshot
#undef agent_set_request_api_credentials_for
#undef agent_set_request_cancel
#undef agent_set_request_claude_creds_for
#undef agent_set_request_codex_creds_for
#undef agent_set_request_session
#undef db1_server_session_owner_get
#undef server_native_delegate_source_runtime_consume

static int probing_constructor, constructor_owner_queries, service_probes;
static char service_handle[65];
static server_native_delegate_source_scope_t *probe_scope;
static void service_runtime_probe(server_native_delegate_source_scope_t *scope);
static int composition_source_admit(server_native_delegate_source_scope_t *scope,
                                    const char *persona, const aimee_request_t *effective,
                                    char *error, size_t capacity);
__typeof__(db1_server_session_owner_get) composition_source_session_get;
__typeof__(server_native_delegate_source_bind) composition_source_bind;
#define server_native_delegate_source_bind  composition_source_bind
#define db1_server_session_owner_get        composition_source_session_get
#define server_native_delegate_source_admit composition_source_admit
#define main                                composition_source_suite_main
#include "test_native_delegate_source.c"
#undef main
#undef server_native_delegate_source_admit
#undef db1_server_session_owner_get
#undef server_native_delegate_source_bind

int db1_server_session_owner_get(const char *session, db1_server_session_owner_t *out)
{
   if (probing_constructor)
   {
      constructor_owner_queries++;
      return -1; /* The auth test ends here; no callback/provider work follows. */
   }
   return composition_source_session_get(session, out);
}
int composition_source_bind(const char *handle, unsigned principal, const char *operation,
                            const char *role, const char *registration, const char *cwd,
                            const aimee_request_t *effective, int tools, int max_turns,
                            long timeout_ms, server_native_delegate_source_scope_t **scope,
                            agent_config_t *config, agent_t *agent, char *detail, size_t capacity)
{
   int rc = server_native_delegate_source_bind(handle, principal, operation, role, registration,
                                               cwd, effective, tools, max_turns, timeout_ms, scope,
                                               config, agent, detail, capacity);
   const request_context_t *owner = request_context_get();
   if (!rc && owner && owner->vault_transport == ATTEST_NONE)
      snprintf(service_handle, sizeof service_handle, "%s", handle);
   return rc;
}
static int composition_source_admit(server_native_delegate_source_scope_t *scope,
                                    const char *persona, const aimee_request_t *effective,
                                    char *detail, size_t capacity)
{
   int rc = server_native_delegate_source_admit(scope, persona, effective, detail, capacity);
   const request_context_t *owner = request_context_get();
   if (!rc && owner && owner->vault_transport == ATTEST_NONE &&
       !strcmp(owner->vault_principal, VAULT_SERVER_PRINCIPAL))
      service_runtime_probe(scope);
   return rc;
}
static void ctor(const agent_t *model, const aimee_request_t *profile, int admitted)
{
   assert(!server_native_runtime_forward_service_admitted());
   constructor_owner_queries = 0;
   probing_constructor = 1;
   assert(!server_native_runtime_open_bound_request(model, request_context_get(),
                                                    agent_get_request_session(), profile, NULL, 0));
   probing_constructor = 0;
   assert(!server_native_runtime_forward_service_admitted());
   assert(constructor_owner_queries == admitted);
}
static void readmit(void)
{
   assert(!server_native_delegate_source_admit(probe_scope, "engineer", &request, error,
                                               sizeof error));
}
static void prepare(const aimee_request_t *profile)
{
   readmit();
   assert(!server_native_delegate_source_runtime_prepare("reviewer", &selected, &selected, &request,
                                                         profile, agent_get_request_session()));
}
static void service_runtime_probe(server_native_delegate_source_scope_t *scope)
{
   probe_scope = scope;
   service_probes++;
   aimee_request_t profile = {0};
   assert(!aimee_ir_request_clone(&request, &profile));
   profile.mutated = 1;
   profile.has_max_tokens = 1;
   profile.max_tokens = 32;
   /* A nested unadmitted scope hides, and cannot consume, the outer ticket. */
   prepare(&profile);
   server_native_delegate_source_scope_t *nested = NULL;
   agent_config_t nested_config;
   agent_t nested_selected;
   assert(!server_native_delegate_source_bind(
       service_handle, 76, "work-1", "reviewer", "native-fixture", worktree, &request, 1, 3, 1234,
       &nested, &nested_config, &nested_selected, error, sizeof error));
   request_context_snapshot_t inner_context;
   request_context_snapshot(&inner_context);
   agent_request_creds_t inner_creds;
   agent_request_creds_snapshot(&inner_creds);
   int inner_depth = workspace_depth;
   server_native_delegate_source_unbind(scope); /* LIFO violation must be inert. */
   unchanged(&inner_context, &inner_creds, worktree, inner_depth);
   assert(server_native_delegate_source_runtime_prepare("reviewer", &selected, &selected, &request,
                                                        &profile, agent_get_request_session()));
   ctor(&selected, &profile, 0);
   server_native_delegate_source_unbind(nested);
   ctor(&selected, &profile, 1);
   ctor(&selected, &profile, 0);
   assert(server_native_delegate_source_runtime_prepare(
       "reviewer", &selected, &selected, &request, &profile,
       agent_get_request_session())); /* No reissue after consumption. */
   prepare(&profile);                 /* A fresh actual admit/HMAC claim is required. */
   ctor(&selected, &profile, 1);
   ctor(&selected, &profile, 0);
   prepare(&profile);
   profile.max_tokens++;
   ctor(&selected, &profile, 0);
   profile.max_tokens--;
   prepare(&profile);
   agent_t foreign = selected;
   snprintf(foreign.model, sizeof foreign.model, "foreign-model");
   ctor(&foreign, &profile, 0);
   prepare(&profile);
   request_context_snapshot_t before;
   request_context_snapshot(&before);
   request_context_t changed = *request_context_get();
   changed.vault_transport = ATTEST_TCP_BEARER;
   request_context_set(&changed);
   ctor(&selected, &profile, 0);
   request_context_restore(&before);
   ctor(&selected, &profile, 0); /* A failed owner attempt also consumes it. */
   prepare(&profile);
   changed = *request_context_get();
   snprintf(changed.vault_principal, sizeof changed.vault_principal, "foreign");
   request_context_set(&changed);
   ctor(&selected, &profile, 0);
   request_context_restore(&before);
   readmit();
   assert(server_native_delegate_source_runtime_prepare(
       "foreign-role", &selected, &selected, &request, &profile, agent_get_request_session()));
   aimee_request_t other = {0};
   assert(!aimee_ir_request_clone(&request, &other));
   other.has_max_tokens = 1;
   other.max_tokens = 99;
   readmit();
   assert(server_native_delegate_source_runtime_prepare("reviewer", &selected, &selected, &other,
                                                        &profile, agent_get_request_session()));
   aimee_request_free(&other);
   ctor(&selected, &profile, 0);
   /* Failed admission revokes the ticket and requires a new actual admission. */
   prepare(&profile);
   aimee_request_t invalid = request;
   invalid.n_messages = -1;
   assert(server_native_delegate_source_admit(scope, "engineer", &invalid, error, sizeof error));
   ctor(&selected, &profile, 0);
   assert(server_native_delegate_source_runtime_prepare("reviewer", &selected, &selected, &request,
                                                        &profile, agent_get_request_session()));
   /* A C-created child may differ from the admitted source parent, but it must
    * already be a real session owned by that same actor. */
   agent_request_creds_t parent_creds;
   agent_request_creds_snapshot(&parent_creds);
   char child_id[64], foreign_child[64];
   snprintf(child_id, sizeof child_id, "source-child-%d", service_probes);
   snprintf(foreign_child, sizeof foreign_child, "foreign-child-%d", service_probes);
   assert(!db1_server_session_create(child_id, "delegate", VAULT_SERVER_PRINCIPAL));
   assert(!db1_server_session_create(foreign_child, "delegate", "foreign-owner"));
   readmit();
   agent_set_request_session(child_id);
   assert(!server_native_delegate_source_runtime_prepare("reviewer", &selected, &selected, &request,
                                                         &profile, child_id));
   ctor(&selected, &profile, 1);
   agent_request_creds_restore(&parent_creds);
   readmit();
   agent_set_request_session(foreign_child);
   assert(server_native_delegate_source_runtime_prepare("reviewer", &selected, &selected, &request,
                                                        &profile, foreign_child));
   ctor(&selected, &profile, 0);
   agent_request_creds_restore(&parent_creds);
   /* The real delegate tightens timeout after admission to the remaining bus
    * deadline. Both identity and every finite upper bound stay authoritative. */
   agent_t tight = selected;
   assert(tight.timeout_ms > 2 && tight.max_turns > 1);
   tight.timeout_ms--;
   tight.max_turns--;
   readmit();
   assert(!server_native_delegate_source_runtime_prepare("reviewer", &tight, &tight, &request,
                                                         &profile, agent_get_request_session()));
   ctor(&tight, &profile, 1);
   assert(server_native_delegate_source_runtime_prepare("reviewer", &tight, &tight, &request,
                                                        &profile, agent_get_request_session()));
   const int bad_timeouts[] = {0, -1, selected.timeout_ms + 1};
   for (size_t i = 0; i < sizeof bad_timeouts / sizeof bad_timeouts[0]; i++)
   {
      agent_t wider = selected;
      wider.timeout_ms = bad_timeouts[i];
      readmit();
      assert(server_native_delegate_source_runtime_prepare("reviewer", &wider, &wider, &request,
                                                           &profile, agent_get_request_session()));
      ctor(&wider, &profile, 0);
   }
   const int bad_turns[] = {0, -1, selected.max_turns + 1};
   for (size_t i = 0; i < sizeof bad_turns / sizeof bad_turns[0]; i++)
   {
      agent_t wider = selected;
      wider.max_turns = bad_turns[i];
      readmit();
      assert(server_native_delegate_source_runtime_prepare("reviewer", &wider, &wider, &request,
                                                           &profile, agent_get_request_session()));
      ctor(&wider, &profile, 0);
   }
   readmit();
   changed = *request_context_get();
   changed.capabilities |= CAP_WORKFLOW_ADMIN;
   request_context_set(&changed);
   assert(server_native_delegate_source_runtime_prepare("reviewer", &selected, &selected, &request,
                                                        &profile, agent_get_request_session()));
   request_context_restore(&before);
   ctor(&selected, &profile, 0);
   aimee_request_free(&profile);
}
int main(void)
{
   assert(!composition_source_suite_main());
   assert(service_probes > 0);
   request_context_t raw = {
       .vault_transport = ATTEST_NONE, .model_execution = 1, .capabilities = CAP_CHAT};
   snprintf(raw.principal, sizeof raw.principal, "%s", VAULT_SERVER_PRINCIPAL);
   snprintf(raw.vault_principal, sizeof raw.vault_principal, "%s", VAULT_SERVER_PRINCIPAL);
   request_context_set(&raw);
   aimee_request_t profile = {0};
   ctor(&selected, &profile, 0); /* Raw server/NONE and an unbound scope do not mint. */
   assert(server_native_delegate_source_runtime_prepare("reviewer", &selected, &selected, &request,
                                                        &profile, "forged-session"));
   request_context_clear();
   puts("actual service source -> native constructor capability: ok");
   return 0;
}
