/* Exercise the actual provenance consumer with backend-opaque references. */
#include <assert.h>
#include "../kb/kb_service_memory.c"

static cJSON *event, *current, *response;
static int lookup = 1, dispatch = 1;
static const int64_t record_id = INT64_C(9007199254740993);

int kb_store_demotion_retrieval_event_by_turn(const char *turn, char *id, int id_cap, char *payload,
                                              int payload_cap)
{
   assert(!strcmp(turn, "fixture-turn"));
   snprintf(id, (size_t)id_cap, "fixture-event");
   char *json = cJSON_PrintUnformatted(event);
   assert(json && strlen(json) < (size_t)payload_cap);
   snprintf(payload, (size_t)payload_cap, "%s", json);
   free(json);
   return lookup;
}
int kb_store_work_outcomes_for_retrieval_json(const char *id, char *out, int cap)
{
   assert(!strcmp(id, "fixture-event"));
   snprintf(out, (size_t)cap, "[]");
   return 0;
}
int kb_store_code_file_hash(const char *project, const char *path, char *out, int cap)
{
   (void)project;
   (void)path;
   (void)out;
   (void)cap;
   assert(0 && "fixture contains no code references");
   return -1;
}
int aimee_module_commands_dispatch_internal(const char *method, const cJSON *args, cJSON **out)
{
   assert(!strcmp(method, "memory.runtime"));
   assert(!strcmp(jo_cstr(args, "operation"), "record"));
   int64_t id = 0;
   assert(jo_read_i64_exact(cJSON_GetObjectItemCaseSensitive(args, "id"), &id));
   assert(id == record_id);
   assert(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(args, "include_version")));
   *out = cJSON_Duplicate(current, 1);
   return dispatch;
}
int kb_send_error(int fd, const char *message)
{
   (void)fd;
   (void)message;
   return -1;
}
int kb_reply_or_error(int fd, cJSON *reply, const char *message)
{
   (void)fd;
   (void)message;
   assert(!response);
   response = reply;
   return 1;
}
static cJSON *reference(const char *scheme, const char *owner, const char *token)
{
   cJSON *ref = cJSON_CreateObject();
   cJSON_AddNumberToObject(ref, "schema_version", 1);
   cJSON_AddStringToObject(ref, "scheme", scheme);
   cJSON_AddStringToObject(ref, "owner", owner);
   cJSON_AddStringToObject(ref, "record", "9007199254740993");
   cJSON_AddStringToObject(ref, "version", token);
   return ref;
}
static cJSON *prepare(void)
{
   cJSON_Delete(event);
   cJSON_Delete(current);
   cJSON_Delete(response);
   response = NULL;
   lookup = dispatch = 1;
   event = cJSON_CreateObject();
   cJSON_AddItemToArray(cJSON_AddArrayToObject(event, "surfaced_ids"),
                        jo_i64_value_exact(record_id));
   cJSON *item = cJSON_CreateObject();
   cJSON_AddItemToObject(item, "id", jo_i64_value_exact(record_id));
   cJSON_AddItemToObject(item, "source_ref", reference("fixture-source/v1", "owner", "content-a"));
   cJSON_AddItemToObject(item, "state_ref", reference("fixture-state/v1", "owner", "state-a"));
   cJSON_AddItemToArray(cJSON_AddArrayToObject(event, "surfaced_items"), item);
   current = cJSON_CreateObject();
   cJSON_AddStringToObject(current, "status", "ok");
   cJSON *record = cJSON_AddObjectToObject(current, "memory");
   cJSON_AddStringToObject(record, "kind", "world_fact");
   cJSON_AddStringToObject(record, "source_session", "descriptive-session");
   cJSON_AddItemToObject(record, "source_ref",
                         reference("fixture-source/v1", "owner", "content-a"));
   cJSON_AddItemToObject(record, "state_ref", reference("fixture-state/v1", "owner", "state-b"));
   return record;
}
static cJSON *run(void)
{
   cJSON *request = cJSON_CreateObject();
   cJSON_AddStringToObject(request, "turn_id", "fixture-turn");
   assert(kb_handle_evidence_provenance(0, request) == 1);
   cJSON_Delete(request);
   cJSON *source = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(response, "sources"), 0);
   int64_t exact;
   if (lookup == 1)
      assert(jo_read_i64_exact(cJSON_GetObjectItemCaseSensitive(source, "id"), &exact) &&
             exact == record_id);
   return source;
}
static void expect(const cJSON *source, const char *state, int drifted)
{
   assert(!strcmp(jo_cstr(source, "drift_state"), state));
   assert(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(source, "drifted")) == drifted);
}
int main(void)
{
   cJSON *record = prepare();
   cJSON *source = run();
   expect(source, "unchanged", 0);
   assert(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(source, "state_changed")));
   assert(!strcmp(jo_cstr(source, "state_drift_state"), "changed"));
   assert(!strcmp(jo_cstr(source, "version"), "")); /* no fabricated timestamp */

   record = prepare();
   cJSON_ReplaceItemInObjectCaseSensitive(record, "source_ref",
                                          reference("fixture-source/v1", "owner", "content-b"));
   expect(run(), "changed", 1);
   record = prepare();
   cJSON_ReplaceItemInObjectCaseSensitive(
       record, "source_ref", reference("fixture-source/v1", "other-owner", "content-a"));
   expect(run(), "unknown", 0);
   record = prepare();
   cJSON_ReplaceItemInObjectCaseSensitive(record, "source_ref",
                                          reference("other-scheme", "owner", "content-a"));
   expect(run(), "unknown", 0);

   record = prepare();
   cJSON *item = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(event, "surfaced_items"), 0);
   cJSON_DeleteItemFromObjectCaseSensitive(item, "source_ref");
   cJSON_AddStringToObject(item, "v", "old-timestamp");
   expect(run(), "unknown", 0); /* mixed version domains are never compared */
   cJSON_Delete(response);
   response = NULL;
   cJSON_DeleteItemFromObjectCaseSensitive(record, "source_ref");
   cJSON_AddStringToObject(record, "updated_at", "new-timestamp");
   expect(run(), "changed", 1);
   cJSON_Delete(response);
   response = NULL;
   cJSON_ReplaceItemInObjectCaseSensitive(record, "updated_at",
                                          cJSON_CreateString("old-timestamp"));
   expect(run(), "unchanged", 0);

   prepare();
   cJSON_ReplaceItemInObjectCaseSensitive(current, "status", cJSON_CreateString("error"));
   cJSON_AddStringToObject(current, "kind", "not_found");
   source = run();
   expect(source, "unknown", 0);
   assert(cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(source, "present")));
   assert(!cJSON_HasObjectItem(source, "error"));
   assert(!cJSON_HasObjectItem(source, "source_ref"));
   assert(!strcmp(jo_cstr(source, "kind"), ""));
   prepare();
   dispatch = 0;
   source = run();
   expect(source, "unknown", 0);
   assert(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(source, "error")));
   assert(!cJSON_HasObjectItem(source, "source_ref"));
   assert(!strcmp(jo_cstr(source, "source"), ""));
   prepare();
   lookup = -1;
   run();
   assert(!strcmp(jo_cstr(response, "provenance_status"), "evidence_unavailable"));
   cJSON_Delete(event);
   cJSON_Delete(current);
   cJSON_Delete(response);
   return 0;
}
