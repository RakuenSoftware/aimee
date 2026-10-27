#ifndef AIMEE_CURATOR_MODEL_FIXTURE_H
#define AIMEE_CURATOR_MODEL_FIXTURE_H
/* Canonical local provider transport, never shell/stdout model authority. */
#include "mock_agent_http.h"
#include "cJSON.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
static const char *curator_fixture_text;
static int curator_fixture_failed;
static int curator_fixture_http(const char *url, const char *auth, const char *body, char **reply,
                                int timeout, const char *headers)
{
   (void)url;
   (void)auth;
   (void)timeout;
   (void)headers;
   cJSON *request = cJSON_Parse(body);
   assert(request && cJSON_IsArray(cJSON_GetObjectItemCaseSensitive(request, "messages")));
   cJSON_Delete(request);
   if (curator_fixture_failed)
      return -1;
   cJSON *root = cJSON_CreateObject(), *choices = cJSON_AddArrayToObject(root, "choices");
   cJSON *choice = cJSON_CreateObject();
   cJSON_AddItemToArray(choices, choice);
   cJSON_AddStringToObject(choice, "finish_reason", "stop");
   cJSON *message = cJSON_AddObjectToObject(choice, "message");
   cJSON_AddStringToObject(message, "role", "assistant");
   cJSON_AddStringToObject(message, "content", curator_fixture_text ? curator_fixture_text : "");
   *reply = cJSON_PrintUnformatted(root);
   cJSON_Delete(root);
   return 200;
}
static inline const char *curator_fixture_reply(const char *text)
{
   curator_fixture_text = text;
   curator_fixture_failed = 0;
   assert(!setenv("SYNTHESIS_ENDPOINT", "http://canonical-fixture/v1", 1));
   assert(!setenv("SYNTHESIS_MODEL", "fixture-model", 1));
   mock_agent_http_reset();
   mock_agent_http_set_post_handler(curator_fixture_http);
   kb_curator_provider_backoff_recovered();
   return ""; /* retired command selector, never executed */
}
static inline const char *curator_fixture_failure(void)
{
   curator_fixture_reply(NULL);
   curator_fixture_failed = 1;
   return "";
}
#endif
