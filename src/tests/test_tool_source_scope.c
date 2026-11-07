/* Generic custody/byte-integrity tests. Opaque fixture receipts are not claims
 * of owner signature authority; the private importer separately verifies that. */
#include <aimee/tools/agent_tools.h>
#include <aimee/ir/aimee_ir.h>
#include "aimee_sha256.h"
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static cJSON *source_for(const char *visible)
{
   char digest[65];
   assert(aimee_sha256_hex(visible, strlen(visible), digest) == 0);
   cJSON *source = cJSON_CreateObject();
   assert(source);
   cJSON_AddNumberToObject(source, "schema_version", 1);
   cJSON_AddStringToObject(source, "owner", "81000000-0000-4000-8000-000000000001");
   cJSON_AddStringToObject(source, "visible_sha256", digest);
   cJSON_AddStringToObject(source, "receipt", "{\"opaque_coordinate\":18446744073709551615}");
   return source;
}
static void *other_thread(void *source)
{
   assert(agent_tool_source_attach(source, "abc") == -1);
   return NULL;
}
int main(void)
{
   cJSON *source = source_for("abc"), *owned = NULL;
   assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(source, "visible_sha256")),
                  "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
   assert(aimee_tool_source_visible_valid(source, "abc"));
   assert(!aimee_tool_source_visible_valid(source, "abc\n"));
   assert(agent_tool_source_attach(source, "abc") == -1); /* No trusted result scope. */
   agent_tool_source_scope_t scope = {0};
   assert(!agent_tool_source_scope_begin(&scope));
   assert(!agent_tool_source_scope_finish(&scope, "ordinary legacy output", &owned) && !owned);
   assert(!agent_tool_source_scope_begin(&scope));
   assert(!agent_tool_source_attach(source, "abc"));
   pthread_t thread;
   assert(!pthread_create(&thread, NULL, other_thread, source));
   assert(!pthread_join(thread, NULL));
   assert(!agent_tool_source_scope_finish(&scope, "abc", &owned));
   assert(owned && aimee_tool_source_visible_valid(owned, "abc"));
   assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(owned, "receipt")),
                  "{\"opaque_coordinate\":18446744073709551615}"));
   cJSON_Delete(owned);
   owned = NULL;
   assert(!agent_tool_source_scope_begin(&scope));
   assert(!agent_tool_source_attach(source, "abc"));
   assert(agent_tool_source_check("abc\nannotation") == -1);
   assert(agent_tool_source_scope_finish(&scope, "abc", &owned) == -1 && !owned);
   assert(!agent_tool_source_scope_begin(&scope));
   assert(!agent_tool_source_attach(source, "abc"));
   assert(agent_tool_source_attach(source, "abc") == -1); /* No overwritten proof. */
   assert(agent_tool_source_scope_finish(&scope, "abc", &owned) == -1 && !owned);
   assert(!agent_tool_source_scope_begin(&scope));
   assert(!agent_tool_source_attach(source, "abc"));
   agent_tool_source_scope_t nested = {0};
   cJSON *unicode = source_for("wide 18446744073709551615 界");
   assert(!agent_tool_source_scope_begin(&nested));
   assert(!agent_tool_source_attach(unicode, "wide 18446744073709551615 界"));
   assert(!agent_tool_source_scope_finish(&nested, "wide 18446744073709551615 界", &owned));
   assert(owned);
   cJSON_Delete(owned);
   assert(!agent_tool_source_scope_finish(&scope, "abc", &owned));
   assert(owned && aimee_tool_source_visible_valid(owned, "abc"));
   cJSON_Delete(owned);
   owned = NULL;
   cJSON *untrusted_meta = cJSON_CreateObject();
   cJSON_AddItemToObject(untrusted_meta, "_meta", cJSON_Duplicate(source, 1));
   assert(!agent_tool_source_scope_begin(&scope));
   assert(agent_tool_source_attach(untrusted_meta, "abc") == -1);
   assert(agent_tool_source_scope_finish(&scope, "abc", &owned) == -1 && !owned);
   cJSON_Delete(untrusted_meta);
   cJSON_Delete(unicode);
   cJSON_Delete(source);
   puts("tool source scope: exact bytes, owned lifetime, nested/thread isolation and fail-closed custody passed");
   return 0;
}
