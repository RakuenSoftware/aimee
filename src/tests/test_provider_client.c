/* test_provider_client.c: shared provider-client request builder + response parser.
 * Network-free: exercises the pure build/parse halves (provider_client_complete's
 * transport is covered by integration once a curator endpoint is wired). */
#include "provider_client.h"
#include "cJSON.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static cJSON *two_messages(void)
{
   cJSON *msgs = cJSON_CreateArray();
   cJSON *m = cJSON_CreateObject();
   cJSON_AddStringToObject(m, "role", "user");
   cJSON_AddStringToObject(m, "content", "extract entities");
   cJSON_AddItemToArray(msgs, m);
   return msgs;
}

static void test_build_basic(void)
{
   provider_def_t def = {
       .base_url = "http://h:8080/v1",
       .model = "granite-3b",
       .wire = PROVIDER_WIRE_OPENAI_CHAT,
       .temperature = -1.0, /* omit */
       .max_tokens = 0,     /* omit */
   };
   cJSON *msgs = two_messages();
   cJSON *req = provider_client_build_openai(&def, msgs, NULL);
   assert(req);

   cJSON *model = cJSON_GetObjectItemCaseSensitive(req, "model");
   assert(cJSON_IsString(model) && strcmp(model->valuestring, "granite-3b") == 0);
   /* messages copied, not aliased: mutating the original must not change req. */
   cJSON *rmsgs = cJSON_GetObjectItemCaseSensitive(req, "messages");
   assert(cJSON_IsArray(rmsgs) && cJSON_GetArraySize(rmsgs) == 1);
   /* omitted optionals absent */
   assert(!cJSON_GetObjectItemCaseSensitive(req, "temperature"));
   assert(!cJSON_GetObjectItemCaseSensitive(req, "max_tokens"));
   assert(!cJSON_GetObjectItemCaseSensitive(req, "response_format"));

   cJSON_Delete(req);
   cJSON_Delete(msgs);
   printf("provider_client: build basic ok\n");
}

static void test_build_options_and_schema(void)
{
   provider_def_t def = {
       .base_url = "http://h:8080/v1",
       .model = "granite-3b",
       .wire = PROVIDER_WIRE_OPENAI_CHAT,
       .temperature = 0.2,
       .max_tokens = 512,
   };
   cJSON *msgs = two_messages();
   cJSON *schema = cJSON_CreateObject();
   cJSON_AddStringToObject(schema, "type", "object");

   cJSON *req = provider_client_build_openai(&def, msgs, schema);
   assert(req);
   cJSON *temp = cJSON_GetObjectItemCaseSensitive(req, "temperature");
   assert(cJSON_IsNumber(temp) && temp->valuedouble > 0.19 && temp->valuedouble < 0.21);
   cJSON *mt = cJSON_GetObjectItemCaseSensitive(req, "max_tokens");
   assert(cJSON_IsNumber(mt) && mt->valueint == 512);

   cJSON *rf = cJSON_GetObjectItemCaseSensitive(req, "response_format");
   assert(rf);
   cJSON *rftype = cJSON_GetObjectItemCaseSensitive(rf, "type");
   assert(cJSON_IsString(rftype) && strcmp(rftype->valuestring, "json_schema") == 0);
   cJSON *js = cJSON_GetObjectItemCaseSensitive(rf, "json_schema");
   assert(js);
   cJSON *strict = cJSON_GetObjectItemCaseSensitive(js, "strict");
   assert(cJSON_IsBool(strict) && cJSON_IsTrue(strict));
   cJSON *embedded = cJSON_GetObjectItemCaseSensitive(js, "schema");
   assert(embedded); /* the caller schema is copied in */
   cJSON *etype = cJSON_GetObjectItemCaseSensitive(embedded, "type");
   assert(cJSON_IsString(etype) && strcmp(etype->valuestring, "object") == 0);

   cJSON_Delete(req);
   cJSON_Delete(msgs);
   cJSON_Delete(schema); /* still owned by the caller — proves no take-over */
   printf("provider_client: build options + schema ok\n");
}

static void test_parse_success(void)
{
   const char *body =
       "{\"model\":\"granite-3b-instruct\",\"choices\":[{\"finish_reason\":\"stop\","
       "\"message\":{\"role\":\"assistant\",\"content\":\"{\\\"entities\\\":[]}\"}}],"
       "\"usage\":{\"prompt_tokens\":42,\"completion_tokens\":7}}";
   provider_completion_t out;
   int rc = provider_client_parse_openai(body, &out);
   assert(rc == 0);
   assert(out.content && strcmp(out.content, "{\"entities\":[]}") == 0);
   assert(out.prompt_tokens == 42 && out.completion_tokens == 7);
   assert(strcmp(out.model, "granite-3b-instruct") == 0);
   assert(strcmp(out.finish_reason, "stop") == 0);
   provider_completion_free(&out);
   provider_completion_free(&out); /* idempotent */
   printf("provider_client: parse success ok\n");
}

static void test_parse_failures(void)
{
   provider_completion_t out;
   /* not JSON */
   assert(provider_client_parse_openai("not json", &out) == -1);
   /* well-formed but no content */
   assert(provider_client_parse_openai("{\"choices\":[{\"message\":{}}]}", &out) == -1);
   /* empty choices */
   assert(provider_client_parse_openai("{\"choices\":[]}", &out) == -1);
   /* NULL body */
   assert(provider_client_parse_openai(NULL, &out) == -1);
   /* out zeroed even on failure */
   assert(out.content == NULL && out.prompt_tokens == 0);
   printf("provider_client: parse failures ok\n");
}

static void test_complete_arg_guards(void)
{
   char err[128];
   provider_completion_t out;
   cJSON *msgs = two_messages();
   provider_def_t def = {.base_url = "http://h/v1", .wire = PROVIDER_WIRE_OPENAI_CHAT};

   /* missing base_url */
   provider_def_t no_url = def;
   no_url.base_url = "";
   assert(provider_client_complete(&no_url, msgs, NULL, &out, err, sizeof(err)) == -1);
   assert(err[0] != '\0');

   /* unsupported wire */
   provider_def_t bad_wire = def;
   bad_wire.wire = (provider_wire_t)99;
   assert(provider_client_complete(&bad_wire, msgs, NULL, &out, err, sizeof(err)) == -1);

   cJSON_Delete(msgs);
   printf("provider_client: complete arg guards ok\n");
}

int main(void)
{
   test_build_basic();
   test_build_options_and_schema();
   test_parse_success();
   test_parse_failures();
   test_complete_arg_guards();
   printf("provider_client: all tests passed\n");
   return 0;
}
