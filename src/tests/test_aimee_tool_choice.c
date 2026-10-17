/* test_aimee_tool_choice.c -- tool_choice and parallel tool use cross the IR as typed
 * values. Before this, the IR carried the client's tool_choice JSON verbatim, so a
 * Claude Code {type:any} reached an OpenAI-family provider unchanged and was
 * rejected, and a Responses client's choice was dropped at parse. Each case parses
 * one wire, builds every other, and checks the provider spelling. */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include <aimee/translation/aimee_backend.h>
#include <aimee/translation/aimee_frontend.h>
#include <aimee/ir/aimee_ir.h>
#include "cJSON.h"

#define TOOLS_ANTHROPIC "\"tools\":[{\"name\":\"Read\",\"input_schema\":{\"type\":\"object\"}}]"
#define TOOLS_OPENAI                                                                               \
   "\"tools\":[{\"type\":\"function\",\"function\":{\"name\":\"Read\",\"parameters\":{\"type\":"   \
   "\"object\"}}}]"
#define TOOLS_RESPONSES                                                                            \
   "\"tools\":[{\"type\":\"function\",\"name\":\"Read\",\"parameters\":{\"type\":\"object\"}}]"

typedef int (*parse_fn)(const cJSON *, aimee_request_t *, char *, size_t);

static void parse(parse_fn fn, const char *json, aimee_request_t *ir)
{
   char err[128] = "";
   cJSON *j = cJSON_Parse(json);
   assert(j);
   int rc = fn(j, ir, err, sizeof err);
   if (rc != 0)
      fprintf(stderr, "parse failed: %s\n", err);
   assert(rc == 0);
   cJSON_Delete(j);
}

static int parse_fails(parse_fn fn, const char *json)
{
   char err[128] = "";
   aimee_request_t ir;
   cJSON *j = cJSON_Parse(json);
   assert(j);
   int rc = fn(j, &ir, err, sizeof err);
   cJSON_Delete(j);
   if (rc == 0)
      aimee_request_free(&ir);
   return rc != 0 && strstr(err, "tool_choice") != NULL;
}

static const char *str(const cJSON *o, const char *k)
{
   const cJSON *v = cJSON_GetObjectItemCaseSensitive((cJSON *)o, k);
   return cJSON_IsString(v) ? v->valuestring : NULL;
}

static const cJSON *obj(const cJSON *o, const char *k)
{
   return cJSON_GetObjectItemCaseSensitive((cJSON *)o, k);
}

/* Anthropic {type:any} is "must call some tool": OpenAI and Responses say "required". */
static void anthropic_any_to_openai_family(void)
{
   aimee_request_t ir;
   parse(anthropic_frontend_parse,
         "{\"model\":\"m\",\"max_tokens\":8,\"messages\":[{\"role\":\"user\",\"content\":\"hi\"}]"
         "," TOOLS_ANTHROPIC ",\"tool_choice\":{\"type\":\"any\"}}",
         &ir);
   assert(ir.tool_choice == AIMEE_TOOL_CHOICE_ANY);
   assert(!ir.has_parallel_tool_calls);

   cJSON *chat = openai_backend_build(&ir);
   assert(chat && str(chat, "tool_choice") && strcmp(str(chat, "tool_choice"), "required") == 0);
   assert(!obj(chat, "parallel_tool_calls"));
   cJSON_Delete(chat);

   cJSON *resp = responses_backend_build(&ir);
   assert(resp && str(resp, "tool_choice") && strcmp(str(resp, "tool_choice"), "required") == 0);
   cJSON_Delete(resp);

   /* same-protocol: Anthropic out is the Anthropic spelling it came in as */
   cJSON *anth = anthropic_backend_build(&ir);
   const cJSON *tc = obj(anth, "tool_choice");
   assert(cJSON_IsObject(tc) && strcmp(str(tc, "type"), "any") == 0);
   assert(cJSON_GetArraySize((cJSON *)tc) == 1);
   cJSON_Delete(anth);
   aimee_request_free(&ir);
}

/* A forced named tool in each spelling, and disable_parallel_tool_use <->
 * parallel_tool_calls:false. */
static void named_tool_and_parallel_round_trip(void)
{
   aimee_request_t ir;
   parse(anthropic_frontend_parse,
         "{\"model\":\"m\",\"max_tokens\":8,\"messages\":[{\"role\":\"user\",\"content\":\"hi\"}]"
         "," TOOLS_ANTHROPIC ",\"tool_choice\":{\"type\":\"tool\",\"name\":\"Read\","
         "\"disable_parallel_tool_use\":true}}",
         &ir);
   assert(ir.tool_choice == AIMEE_TOOL_CHOICE_TOOL);
   assert(strcmp(ir.tool_choice_name, "Read") == 0);
   assert(ir.has_parallel_tool_calls && !ir.parallel_tool_calls);

   cJSON *chat = openai_backend_build(&ir);
   const cJSON *tc = obj(chat, "tool_choice");
   assert(cJSON_IsObject(tc) && strcmp(str(tc, "type"), "function") == 0);
   assert(strcmp(str(obj(tc, "function"), "name"), "Read") == 0);
   assert(cJSON_IsFalse(obj(chat, "parallel_tool_calls")));

   /* ...and the OpenAI request parses back to the same IR. */
   aimee_request_t back;
   char *chat_s = cJSON_PrintUnformatted(chat);
   parse(openai_frontend_parse, chat_s, &back);
   assert(back.tool_choice == AIMEE_TOOL_CHOICE_TOOL);
   assert(strcmp(back.tool_choice_name, "Read") == 0);
   assert(back.has_parallel_tool_calls && !back.parallel_tool_calls);
   cJSON_free(chat_s);
   cJSON_Delete(chat);
   aimee_request_free(&back);

   cJSON *resp = responses_backend_build(&ir);
   tc = obj(resp, "tool_choice");
   assert(cJSON_IsObject(tc) && strcmp(str(tc, "type"), "function") == 0);
   assert(strcmp(str(tc, "name"), "Read") == 0 && !obj(tc, "function"));
   assert(cJSON_IsFalse(obj(resp, "parallel_tool_calls")));
   cJSON_Delete(resp);
   aimee_request_free(&ir);
}

/* OpenAI and Responses spellings parse to the same typed values and render as
 * Anthropic, including a bare parallel_tool_calls:false with no choice. */
static void openai_family_to_anthropic(void)
{
   aimee_request_t ir;
   parse(openai_frontend_parse,
         "{\"model\":\"m\",\"messages\":[{\"role\":\"user\",\"content\":\"hi\"}]," TOOLS_OPENAI
         ",\"tool_choice\":\"required\"}",
         &ir);
   assert(ir.tool_choice == AIMEE_TOOL_CHOICE_ANY);
   cJSON *anth = anthropic_backend_build(&ir);
   assert(strcmp(str(obj(anth, "tool_choice"), "type"), "any") == 0);
   cJSON_Delete(anth);
   aimee_request_free(&ir);

   parse(responses_frontend_parse,
         "{\"model\":\"m\",\"input\":\"hi\"," TOOLS_RESPONSES
         ",\"tool_choice\":{\"type\":\"function\",\"name\":\"Read\"}}",
         &ir);
   assert(ir.tool_choice == AIMEE_TOOL_CHOICE_TOOL && strcmp(ir.tool_choice_name, "Read") == 0);
   anth = anthropic_backend_build(&ir);
   const cJSON *tc = obj(anth, "tool_choice");
   assert(strcmp(str(tc, "type"), "tool") == 0 && strcmp(str(tc, "name"), "Read") == 0);
   cJSON_Delete(anth);
   aimee_request_free(&ir);

   parse(responses_frontend_parse,
         "{\"model\":\"m\",\"input\":\"hi\"," TOOLS_RESPONSES ",\"parallel_tool_calls\":false}",
         &ir);
   assert(ir.tool_choice == AIMEE_TOOL_CHOICE_UNSET);
   assert(ir.has_parallel_tool_calls && !ir.parallel_tool_calls);
   anth = anthropic_backend_build(&ir);
   tc = obj(anth, "tool_choice");
   assert(strcmp(str(tc, "type"), "auto") == 0);
   assert(cJSON_IsTrue(obj(tc, "disable_parallel_tool_use")));
   cJSON_Delete(anth);
   aimee_request_free(&ir);

   parse(openai_frontend_parse,
         "{\"model\":\"m\",\"messages\":[{\"role\":\"user\",\"content\":\"hi\"}]," TOOLS_OPENAI
         ",\"tool_choice\":\"none\"}",
         &ir);
   assert(ir.tool_choice == AIMEE_TOOL_CHOICE_NONE);
   anth = anthropic_backend_build(&ir);
   assert(strcmp(str(obj(anth, "tool_choice"), "type"), "none") == 0);
   cJSON_Delete(anth);
   aimee_request_free(&ir);
}

/* A choice the neutral IR has no value for is refused, not downgraded. */
static void unsupported_choices_refused(void)
{
   assert(parse_fails(
       responses_frontend_parse,
       "{\"model\":\"m\",\"input\":\"hi\",\"tool_choice\":{\"type\":\"file_search\"}}"));
   assert(parse_fails(openai_frontend_parse,
                      "{\"model\":\"m\",\"messages\":[{\"role\":\"user\",\"content\":\"hi\"}],"
                      "\"tool_choice\":\"sometimes\"}"));
   assert(parse_fails(anthropic_frontend_parse,
                      "{\"model\":\"m\",\"max_tokens\":8,\"messages\":[{\"role\":\"user\","
                      "\"content\":\"hi\"}],\"tool_choice\":{\"type\":\"tool\"}}"));
}

int main(void)
{
   anthropic_any_to_openai_family();
   named_tool_and_parallel_round_trip();
   openai_family_to_anthropic();
   unsupported_choices_refused();
   printf("aimee-tool-choice: ok\n");
   return 0;
}
