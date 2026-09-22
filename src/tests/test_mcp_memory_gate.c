/* Host capability enforcement and transport conformance for Go-owned MCP policy. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "aimee.h"
#include "cJSON.h"
#include "server.h" /* CAP_* bits, server_capability_for_method */
#include "server_mcp_memory_gate.h"

/* Every mutate verb maps to the RPC method that does the same thing, so the MCP
 * door inherits the grade the NDJSON/HTTP door already had. `forget` is the one
 * that matters: it must land on memory.delete, whose capability is deliberately
 * NOT the one memory.store carries. */
static void test_verb_methods(void)
{
   assert(strcmp(mcp_mutate_verb_method("store"), "memory.store") == 0);
   assert(strcmp(mcp_mutate_verb_method("update"), "memory.update") == 0);
   assert(strcmp(mcp_mutate_verb_method("supersede"), "memory.supersede") == 0);
   assert(strcmp(mcp_mutate_verb_method("forget"), "memory.delete") == 0);
   assert(strcmp(mcp_mutate_verb_method("affirm"), "memory.touch") == 0);
   assert(strcmp(mcp_mutate_verb_method("reject"), "memory.reject") == 0);

   /* Unknown and NULL verbs return NULL so the caller falls through to
    * tool_memory_mutate's own "unknown verb" error rather than being gated on a
    * method nobody named. */
   assert(mcp_mutate_verb_method("wipe") == NULL);
   assert(mcp_mutate_verb_method("") == NULL);
   assert(mcp_mutate_verb_method(NULL) == NULL);
   printf("  PASS: mutate verbs map to their RPC method twins\n");
}

/* The mapping is only worth anything if the methods it names still grade the way
 * the fix intended -- a verb correctly routed to memory.delete buys nothing if
 * memory.delete drifts back to CAP_MEMORY_WRITE. Pin the composition. */
static void test_verb_grades_are_what_the_fix_intended(void)
{
   assert(server_capability_for_method(mcp_mutate_verb_method("forget")) == CAP_MEMORY_ADMIN);
   assert(server_capability_for_method(mcp_mutate_verb_method("store")) == CAP_MEMORY_WRITE);
   assert(server_capability_for_method(mcp_mutate_verb_method("update")) == CAP_MEMORY_WRITE);

   /* Destroying and writing must not be the same grant. */
   assert(server_capability_for_method(mcp_mutate_verb_method("forget")) !=
          server_capability_for_method(mcp_mutate_verb_method("store")));

   /* update must not have fallen back to the memory.* read prefix, which is
    * where it sat before -- a content overwrite gated as a read. */
   assert(server_capability_for_method(mcp_mutate_verb_method("update")) != CAP_MEMORY_READ);
   printf("  PASS: forget grades as memory:admin, store/update as memory:write\n");
}

/* The gate alone cannot stop a model from bulk-deleting, and this is the reason:
 * reaching ANY MCP tool needs CAP_TOOL_EXECUTE, which exists only in
 * CAPS_AUTHENTICATED and CAPS_ALL -- and both of those also carry
 * CAP_MEMORY_ADMIN. So every caller that can invoke memory_maintain at all
 * already clears an admin-graded gate. Pin the property, because it is the
 * justification for stripping prune rather than merely grading it: if the
 * capability sets are ever separated, that is a deliberate change and this
 * assertion should be revisited, not silently invalidated. */
static void test_every_mcp_caller_already_clears_the_admin_gate(void)
{
   uint32_t mcp = server_capability_for_method("mcp.call");
   assert(mcp == CAP_TOOL_EXECUTE);

   /* The only cap sets a connection can hold are CAPS_ALL, CAPS_AUTHENTICATED,
    * and the read-only set (see server_http_conn_caps). */
   assert((CAPS_READ_ONLY & CAP_TOOL_EXECUTE) == 0); /* read-only cannot reach MCP at all */
   assert((CAPS_AUTHENTICATED & CAP_TOOL_EXECUTE) == CAP_TOOL_EXECUTE);
   assert((CAPS_ALL & CAP_TOOL_EXECUTE) == CAP_TOOL_EXECUTE);
   /* ...and both MCP-capable sets already hold the destroy grant. */
   assert((CAPS_AUTHENTICATED & CAP_MEMORY_ADMIN) == CAP_MEMORY_ADMIN);
   assert((CAPS_ALL & CAP_MEMORY_ADMIN) == CAP_MEMORY_ADMIN);
   printf("  PASS: every MCP-capable caller already holds memory:admin\n");
}

/* Neither gate may be satisfiable by a read-only caller. */
static void test_no_gate_is_read_only(void)
{
   assert((CAPS_READ_ONLY & CAP_MEMORY_ADMIN) == 0);
   assert((CAPS_READ_ONLY & CAP_MEMORY_WRITE) == 0);
   assert((CAPS_READ_ONLY & server_capability_for_method(mcp_mutate_verb_method("forget"))) == 0);
   printf("  PASS: no memory gate is satisfied by a read-only caller\n");
}

/* Capability answers "may they", attestation answers "who are they", and the two
 * memory verbs that cannot be undone need the second question asked as well.
 *
 * CAP_MEMORY_ADMIN sits inside CAPS_AUTHENTICATED, so a bearer clears it — over
 * TCP too, under remote_writes=DATA/FULL. That is the right answer for "may this
 * caller delete", and the wrong one for "is this caller the user", which is what
 * decides whether memory.delete destroys the row or retires it and whether a
 * stored note may later mint Class-A facts. The person is the ACCOUNT. */
static void test_authenticated_identity_is_not_capability(void)
{
   /* Every account form is a person, and none of them names its transport. A
    * host account PAM accepted, an OIDC subject, an enrolled client cert and a
    * webuser are the same answer to "who is this" whichever socket carried the
    * request. */
   assert(server_account_is_person("alice") == 1);        /* PAM host account */
   assert(server_account_is_person("oidc:sub-123") == 1); /* OIDC subject */
   assert(server_account_is_person("cert:thin-client") == 1);
   assert(server_account_is_person("webuser:alice") == 1);

   /* A bare bearer authorizes the call but names nobody, so it cannot speak as
    * the user. Empty is also the zero value: a missed hop never becomes one. */
   assert(server_account_is_person("") == 0);
   assert(server_account_is_person(NULL) == 0);

   assert(server_account_memory_authority("alice") == MEMORY_AUTHORITY_USER);
   assert(server_account_memory_authority("oidc:sub-123") == MEMORY_AUTHORITY_USER);
   /* The regression this replaces: an mTLS client was MODEL purely because its
    * bytes arrived over TCP, while aimee-kb called the same caller a person. */
   assert(server_account_memory_authority("cert:thin-client") == MEMORY_AUTHORITY_USER);
   assert(server_account_memory_authority("") == MEMORY_AUTHORITY_MODEL);
   assert(server_account_memory_authority(NULL) == MEMORY_AUTHORITY_MODEL);

   /* The point of the split: a caller can hold the destructive capability and
    * still not be a person, which is exactly the case the mapping must catch. */
   assert((CAPS_AUTHENTICATED & CAP_MEMORY_ADMIN) != 0);
   assert(server_account_is_person("") == 0);
   printf("  PASS: authenticated account is asked separately from capability\n");
}

static const char *owner_plan, *kb_reply;
static int plan_calls, kb_calls;

cJSON *server_invoke_module_operation(const char *method, const char *operation, const cJSON *args,
                                      const char *unavailable)
{
   assert(strcmp(method, "memory.runtime") == 0);
   assert(strcmp(operation, "maintenance-model-plan") == 0);
   assert(cJSON_IsObject(args) && unavailable);
   plan_calls++;
   return owner_plan ? cJSON_Parse(owner_plan) : NULL;
}

cJSON *server_error_kind_json(const char *kind, const char *message, const char *request_id)
{
   (void)request_id;
   cJSON *reply = cJSON_CreateObject();
   cJSON_AddStringToObject(reply, "status", "error");
   cJSON_AddStringToObject(reply, "kind", kind);
   cJSON_AddStringToObject(reply, "message", message);
   return reply;
}

char *kb_v1_action_request(const char *method, cJSON *request)
{
   assert(strcmp(method, "memory.maintenance_run") == 0);
   assert(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(request, "model_policy")));
   assert(cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(request, "modes")) == 3);
   assert(!cJSON_HasObjectItem(request, "actor") && !cJSON_HasObjectItem(request, "authority"));
   assert(!cJSON_HasObjectItem(request, "capabilities") &&
          !cJSON_HasObjectItem(request, "operation"));
   kb_calls++;
   cJSON_Delete(request);
   return kb_reply ? strdup(kb_reply) : NULL;
}

static void assert_reply_kind(cJSON *reply, const char *kind)
{
   assert(reply && strcmp(cJSON_GetObjectItemCaseSensitive(reply, "kind")->valuestring, kind) == 0);
   cJSON_Delete(reply);
}

static void test_maintenance_owner_transport(void)
{
   cJSON *args = cJSON_Parse("{\"modes\":\"prune\",\"actor\":\"operator\",\"authority\":40,"
                             "\"capabilities\":4294967295,\"operation\":\"delete\"}");
   const char *run = "{\"status\":\"ok\",\"execute\":true,\"required_capability\":\"write\","
                     "\"request\":{\"modes\":3,\"model_policy\":true,\"view\":\"model\"}}";
   owner_plan = run;
   kb_reply = "{\"status\":\"ok\",\"text\":\"owner receipt: 9007199254740993\"}";
   assert_reply_kind(server_mcp_memory_maintain_command(0, args), SERVER_ERR_PERMISSION_DENIED);
   assert_reply_kind(server_mcp_memory_maintain_command(CAPS_READ_ONLY, args),
                     SERVER_ERR_PERMISSION_DENIED);
   assert(kb_calls == 0);
   cJSON *reply = server_mcp_memory_maintain_command(CAP_MEMORY_WRITE, args);
   assert(strcmp(cJSON_GetObjectItemCaseSensitive(reply, "text")->valuestring,
                 "owner receipt: 9007199254740993") == 0);
   assert(kb_calls == 1);
   cJSON_Delete(reply);
   owner_plan = "{\"status\":\"ok\",\"execute\":false,\"required_capability\":\"admin\",\"text\":"
                "\"nothing run\"}";
   assert_reply_kind(server_mcp_memory_maintain_command(CAP_MEMORY_WRITE, args),
                     SERVER_ERR_PERMISSION_DENIED);
   reply = server_mcp_memory_maintain_command(CAP_MEMORY_ADMIN, args);
   assert(strcmp(cJSON_GetObjectItemCaseSensitive(reply, "text")->valuestring, "nothing run") == 0);
   cJSON_Delete(reply);
   assert(kb_calls == 1);
   const char *bad_plans[] = {
       NULL,
       "{}",
       "[]",
       "{\"status\":\"error\"}",
       "{\"status\":\"ok\",\"execute\":1,\"required_capability\":\"write\"}",
       "{\"status\":\"ok\",\"execute\":true,\"required_capability\":\"read\"}",
       "{\"status\":\"ok\",\"execute\":true,\"required_capability\":\"write\",\"request\":{\"model_"
       "policy\":false}}",
       "{\"status\":\"ok\",\"execute\":true,\"required_capability\":\"write\"}",
       "{\"status\":\"ok\",\"execute\":false,\"required_capability\":\"admin\"}",
       "{\"status\":\"ok\",\"execute\":false,\"required_capability\":\"admin\",\"text\":"
       "\"ambiguous\",\"request\":{}}"};
   for (size_t i = 0; i < sizeof(bad_plans) / sizeof(bad_plans[0]); ++i)
   {
      owner_plan = bad_plans[i];
      assert_reply_kind(server_mcp_memory_maintain_command(CAPS_ALL, args), SERVER_ERR_UNAVAILABLE);
   }
   assert(kb_calls == 1);
   owner_plan = run;
   const char *bad_replies[] = {NULL,
                                "{}",
                                "[]",
                                "{\"status\":\"error\",\"text\":\"false success\"}",
                                "{\"status\":\"ok\"}",
                                "{\"status\":\"ok\",\"text\":\"x\"} trailing"};
   for (size_t i = 0; i < sizeof(bad_replies) / sizeof(bad_replies[0]); ++i)
   {
      kb_reply = bad_replies[i];
      assert_reply_kind(server_mcp_memory_maintain_command(CAP_MEMORY_WRITE, args),
                        SERVER_ERR_UNAVAILABLE);
   }
   assert(plan_calls > kb_calls);
   cJSON_Delete(args);
   printf("  PASS: Go plans require host capabilities; malformed plans and failures never execute "
          "or look successful\n");
}

int main(void)
{
   const char *index_methods[] = {
       "memory.embed",  "memory.reembed_start", "memory.reembed_cutover", "memory.reembed_rollback",
       "memory.repair", "memory.rebuild",       "memory.reindex"};
   for (size_t i = 0; i < sizeof(index_methods) / sizeof(index_methods[0]); ++i)
      assert(server_capability_for_method(index_methods[i]) == CAP_INDEX_ADMIN);
   assert(server_capability_for_method("memory.reembed_status") == CAP_MEMORY_READ);
   printf("mcp_memory_gate:\n");
   test_authenticated_identity_is_not_capability();
   test_verb_methods();
   test_verb_grades_are_what_the_fix_intended();
   test_every_mcp_caller_already_clears_the_admin_gate();
   test_no_gate_is_read_only();
   test_maintenance_owner_transport();
   printf("mcp_memory_gate: all tests passed\n");
   return 0;
}
