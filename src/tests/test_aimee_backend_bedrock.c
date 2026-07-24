/* test_aimee_backend_bedrock.c -- Slice P6c-ir: the Bedrock Converse backend
 * build/parse. Headline assertions: (1) build emits the EXACT documented Converse
 * wire shape (system[].text, messages[].content[] toolUse/toolResult typed parts,
 * inferenceConfig/toolConfig, object-wrapped toolChoice) with empty-omission of
 * absent optionals; (2) parse maps output.message + stopReason + usage into the IR
 * with the raw stop string kept verbatim; (3) tool_id/tool_name/thinking round-trip;
 * (4) NO openai/anthropic-shaped keys leak (toolUse not tool_calls, toolResult not a
 * role:"tool" message). The IR is built BY HAND -- no frontend parse -- so the test
 * has no dependency beyond the backend + IR + cJSON. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <aimee/translation/aimee_backend.h>
#include <aimee/ir/aimee_ir.h>
#include "cJSON.h"

static cJSON *obj(const cJSON *o, const char *k)
{
   return cJSON_GetObjectItemCaseSensitive((cJSON *)o, k);
}
static const char *str(const cJSON *o, const char *k)
{
   cJSON *it = obj(o, k);
   return (it && cJSON_IsString(it)) ? it->valuestring : NULL;
}

/* Build one text block. */
static aimee_block_t text_block(char *t)
{
   aimee_block_t b = {0};
   b.type = AIMEE_BLK_TEXT;
   b.text = t;
   return b;
}

static size_t allocation_count, allocation_fail_at;

static void *failing_cjson_malloc(size_t size)
{
   allocation_count++;
   if (allocation_count == allocation_fail_at)
      return NULL;
   return malloc(size);
}

static void test_build_allocation_atomicity(void)
{
   aimee_block_t sys = text_block("system");
   cJSON *tool_input = cJSON_Parse("{\"x\":1}");
   cJSON *tool_result = cJSON_Parse("{\"ok\":true}");
   cJSON *schema =
       cJSON_Parse("{\"type\":\"object\",\"properties\":{\"x\":{\"type\":\"number\"}}}");
   cJSON *choice = cJSON_Parse("{\"type\":\"tool\",\"name\":\"tool\"}");
   cJSON *raw = cJSON_Parse("{\"text\":\"raw\"}");
   assert(tool_input && tool_result && schema && choice && raw);
   aimee_block_t blocks[] = {
       {.type = AIMEE_BLK_TEXT, .text = "text"},
       {.type = AIMEE_BLK_TOOL_USE, .tool_id = "id", .tool_name = "tool", .tool_input = tool_input},
       {.type = AIMEE_BLK_TOOL_RESULT, .tool_id = "id", .tool_result = tool_result},
       {.type = AIMEE_BLK_IMAGE, .media_type = "image/png", .media_ref = "AA=="},
       {.type = AIMEE_BLK_THINKING, .text = "thought", .thinking_signature = "sig"},
       {.type = AIMEE_BLK_UNKNOWN, .raw = raw}};
   aimee_message_t message = {.role = "user", .blocks = blocks, .n_blocks = 6};
   aimee_tool_t tool = {.name = "tool", .description = "desc", .schema = schema};
   char *stops[] = {"STOP"};
   aimee_request_t ir = {.system = &sys,
                         .n_system = 1,
                         .messages = &message,
                         .n_messages = 1,
                         .tools = &tool,
                         .n_tools = 1,
                         .tool_choice = choice,
                         .has_max_tokens = 1,
                         .max_tokens = 10,
                         .has_temperature = 1,
                         .temperature = 0.5,
                         .has_top_p = 1,
                         .top_p = 0.8,
                         .stop_sequences = stops,
                         .n_stop = 1};
   cJSON_Hooks hooks = {.malloc_fn = failing_cjson_malloc, .free_fn = free};
   cJSON_InitHooks(&hooks);
   allocation_count = 0;
   allocation_fail_at = SIZE_MAX;
   cJSON *built = bedrock_converse_build(&ir);
   assert(built != NULL);
   size_t allocations = allocation_count;
   cJSON_Delete(built);
   for (size_t i = 1; i <= allocations; i++)
   {
      allocation_count = 0;
      allocation_fail_at = i;
      assert(bedrock_converse_build(&ir) == NULL);
   }
   cJSON_InitHooks(NULL);
   cJSON_Delete(tool_input);
   cJSON_Delete(tool_result);
   cJSON_Delete(schema);
   cJSON_Delete(choice);
   cJSON_Delete(raw);
}

static void test_build_full(void)
{
   /* system TEXT + user TEXT + assistant TOOL_USE + user TOOL_RESULT + 2 tools +
    * all inference params. */
   aimee_block_t sys = text_block("You are helpful.");

   aimee_block_t user_txt = text_block("read foo.c");

   aimee_block_t tu = {0};
   tu.type = AIMEE_BLK_TOOL_USE;
   tu.tool_id = "toolu_abc";
   tu.tool_name = "Read";
   tu.tool_input = cJSON_Parse("{\"path\":\"foo.c\"}");

   aimee_block_t tr = {0};
   tr.type = AIMEE_BLK_TOOL_RESULT;
   tr.tool_id = "toolu_abc";
   tr.tool_result = cJSON_CreateString("file contents");
   tr.tool_is_error = 0;

   aimee_message_t msgs[3] = {0};
   msgs[0].role = "user";
   msgs[0].blocks = &user_txt;
   msgs[0].n_blocks = 1;
   msgs[1].role = "assistant";
   msgs[1].blocks = &tu;
   msgs[1].n_blocks = 1;
   msgs[2].role = "user";
   msgs[2].blocks = &tr;
   msgs[2].n_blocks = 1;

   aimee_tool_t tools[2] = {0};
   tools[0].name = "Read";
   tools[0].description = "Read a file";
   tools[0].schema =
       cJSON_Parse("{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"}}}");
   tools[1].name = "Write";
   tools[1].description = "Write a file";
   tools[1].schema = cJSON_Parse("{\"type\":\"object\"}");

   const char *stops[1] = {"STOP"};

   aimee_request_t ir = {0};
   ir.model = "anthropic.claude-3-5-sonnet";
   ir.system = &sys;
   ir.n_system = 1;
   ir.messages = msgs;
   ir.n_messages = 3;
   ir.tools = tools;
   ir.n_tools = 2;
   ir.max_tokens = 1024;
   ir.has_max_tokens = 1;
   ir.temperature = 0.7;
   ir.has_temperature = 1;
   ir.top_p = 0.9;
   ir.has_top_p = 1;
   ir.stop_sequences = (char **)stops;
   ir.n_stop = 1;

   cJSON *j = bedrock_converse_build(&ir);
   assert(j);

   /* modelId is NOT in the body. */
   assert(!obj(j, "modelId"));
   assert(!obj(j, "model"));

   /* system[0].text */
   cJSON *system = obj(j, "system");
   assert(cJSON_IsArray(system) && cJSON_GetArraySize(system) == 1);
   assert(strcmp(str(cJSON_GetArrayItem(system, 0), "text"), "You are helpful.") == 0);

   /* messages[].role + content[].text/toolUse/toolResult */
   cJSON *m = obj(j, "messages");
   assert(cJSON_IsArray(m) && cJSON_GetArraySize(m) == 3);

   cJSON *m0 = cJSON_GetArrayItem(m, 0);
   assert(strcmp(str(m0, "role"), "user") == 0);
   cJSON *c0 = obj(m0, "content");
   assert(strcmp(str(cJSON_GetArrayItem(c0, 0), "text"), "read foo.c") == 0);

   cJSON *m1 = cJSON_GetArrayItem(m, 1);
   assert(strcmp(str(m1, "role"), "assistant") == 0);
   cJSON *tuj = obj(cJSON_GetArrayItem(obj(m1, "content"), 0), "toolUse");
   assert(tuj && cJSON_IsObject(tuj));
   assert(strcmp(str(tuj, "toolUseId"), "toolu_abc") == 0);
   assert(strcmp(str(tuj, "name"), "Read") == 0);
   cJSON *tin = obj(tuj, "input");
   assert(tin && cJSON_IsObject(tin));
   assert(strcmp(str(tin, "path"), "foo.c") == 0);
   /* no-catch-all: NOT openai tool_calls, NOT anthropic type:tool_use */
   assert(!obj(cJSON_GetArrayItem(obj(m1, "content"), 0), "tool_calls"));
   assert(!obj(cJSON_GetArrayItem(obj(m1, "content"), 0), "type"));

   cJSON *m2 = cJSON_GetArrayItem(m, 2);
   /* no-catch-all: tool_result stays a content part, NOT a role:"tool" message */
   assert(strcmp(str(m2, "role"), "user") == 0);
   cJSON *trj = obj(cJSON_GetArrayItem(obj(m2, "content"), 0), "toolResult");
   assert(trj && cJSON_IsObject(trj));
   assert(strcmp(str(trj, "toolUseId"), "toolu_abc") == 0);
   assert(strcmp(str(trj, "status"), "success") == 0);
   cJSON *trc = obj(trj, "content");
   assert(cJSON_IsArray(trc) && cJSON_GetArraySize(trc) == 1);
   /* string tool_result -> {text:...} */
   assert(strcmp(str(cJSON_GetArrayItem(trc, 0), "text"), "file contents") == 0);

   /* inferenceConfig.{maxTokens,temperature,topP,stopSequences} */
   cJSON *ic = obj(j, "inferenceConfig");
   assert(ic && cJSON_IsObject(ic));
   assert(obj(ic, "maxTokens")->valueint == 1024);
   assert(cJSON_IsNumber(obj(ic, "temperature")));
   assert(cJSON_IsNumber(obj(ic, "topP")));
   cJSON *ss = obj(ic, "stopSequences");
   assert(cJSON_IsArray(ss) && cJSON_GetArraySize(ss) == 1);
   assert(strcmp(cJSON_GetArrayItem(ss, 0)->valuestring, "STOP") == 0);
   /* top_k never emitted */
   assert(!obj(ic, "topK"));

   /* toolConfig.tools[].toolSpec.{name,description,inputSchema.json} */
   cJSON *tcfg = obj(j, "toolConfig");
   assert(tcfg && cJSON_IsObject(tcfg));
   cJSON *tj = obj(tcfg, "tools");
   assert(cJSON_IsArray(tj) && cJSON_GetArraySize(tj) == 2);
   cJSON *spec0 = obj(cJSON_GetArrayItem(tj, 0), "toolSpec");
   assert(strcmp(str(spec0, "name"), "Read") == 0);
   assert(strcmp(str(spec0, "description"), "Read a file") == 0);
   cJSON *isch = obj(spec0, "inputSchema");
   assert(isch && cJSON_IsObject(isch));
   assert(obj(isch, "json") && cJSON_IsObject(obj(isch, "json")));

   /* stream=0 vs stream=1 -> identical body */
   ir.stream = 1;
   cJSON *j2 = bedrock_converse_build(&ir);
   char *s1 = cJSON_PrintUnformatted(j);
   char *s2 = cJSON_PrintUnformatted(j2);
   assert(strcmp(s1, s2) == 0);
   free(s1);
   free(s2);

   cJSON_Delete(j);
   cJSON_Delete(j2);
   cJSON_Delete(tu.tool_input);
   cJSON_Delete(tr.tool_result);
   cJSON_Delete(tools[0].schema);
   cJSON_Delete(tools[1].schema);
}

static void test_build_minimal(void)
{
   /* NO inference params, NO tools, NO system -> those keys absent. */
   aimee_block_t user_txt = text_block("hi");
   aimee_message_t msg = {0};
   msg.role = "user";
   msg.blocks = &user_txt;
   msg.n_blocks = 1;

   aimee_request_t ir = {0};
   ir.messages = &msg;
   ir.n_messages = 1;

   cJSON *j = bedrock_converse_build(&ir);
   assert(j);
   assert(!obj(j, "inferenceConfig"));
   assert(!obj(j, "toolConfig"));
   assert(!obj(j, "system"));
   assert(obj(j, "messages")); /* messages always present */
   cJSON_Delete(j);
}

static void test_tool_choice(void)
{
   aimee_block_t user_txt = text_block("hi");
   aimee_message_t msg = {0};
   msg.role = "user";
   msg.blocks = &user_txt;
   msg.n_blocks = 1;

   aimee_tool_t tool = {0};
   tool.name = "Read";

   aimee_request_t ir = {0};
   ir.messages = &msg;
   ir.n_messages = 1;
   ir.tools = &tool;
   ir.n_tools = 1;

   /* {type:auto} -> toolChoice == {"auto":{}} (object, NOT a bare string) */
   ir.tool_choice = cJSON_Parse("{\"type\":\"auto\"}");
   cJSON *j = bedrock_converse_build(&ir);
   cJSON *choice = obj(obj(j, "toolConfig"), "toolChoice");
   assert(choice && cJSON_IsObject(choice));
   assert(!cJSON_IsString(choice));
   cJSON *autoo = obj(choice, "auto");
   assert(autoo && cJSON_IsObject(autoo));
   cJSON_Delete(j);
   cJSON_Delete(ir.tool_choice);

   /* {type:tool,name:X} -> {"tool":{"name":X}} */
   ir.tool_choice = cJSON_Parse("{\"type\":\"tool\",\"name\":\"Read\"}");
   j = bedrock_converse_build(&ir);
   choice = obj(obj(j, "toolConfig"), "toolChoice");
   assert(choice && cJSON_IsObject(choice));
   cJSON *toolo = obj(choice, "tool");
   assert(toolo && cJSON_IsObject(toolo));
   assert(strcmp(str(toolo, "name"), "Read") == 0);
   cJSON_Delete(j);
   cJSON_Delete(ir.tool_choice);

   /* unrecognized -> toolChoice OMITTED (tools still present) */
   ir.tool_choice = cJSON_Parse("{\"type\":\"weird\"}");
   j = bedrock_converse_build(&ir);
   assert(obj(j, "toolConfig"));
   assert(!obj(obj(j, "toolConfig"), "toolChoice"));
   cJSON_Delete(j);
   cJSON_Delete(ir.tool_choice);
}

static void test_thinking_roundtrip(void)
{
   /* THINKING block with signature -> reasoningContent.reasoningText.{text,signature} */
   aimee_block_t th = {0};
   th.type = AIMEE_BLK_THINKING;
   th.text = "let me think";
   th.thinking_signature = "sig-xyz";
   aimee_message_t msg = {0};
   msg.role = "assistant";
   msg.blocks = &th;
   msg.n_blocks = 1;
   aimee_request_t ir = {0};
   ir.messages = &msg;
   ir.n_messages = 1;

   cJSON *j = bedrock_converse_build(&ir);
   cJSON *part = cJSON_GetArrayItem(obj(cJSON_GetArrayItem(obj(j, "messages"), 0), "content"), 0);
   cJSON *rc = obj(part, "reasoningContent");
   assert(rc && cJSON_IsObject(rc));
   cJSON *rt = obj(rc, "reasoningText");
   assert(strcmp(str(rt, "text"), "let me think") == 0);
   assert(strcmp(str(rt, "signature"), "sig-xyz") == 0);
   cJSON_Delete(j);
}

static void test_parse_basic(void)
{
   const char *RESP =
       "{\"output\":{\"message\":{\"role\":\"assistant\",\"content\":["
       "{\"text\":\"Here you go\"},"
       "{\"toolUse\":{\"toolUseId\":\"toolu_1\",\"name\":\"Read\",\"input\":{\"path\":\"a.c\"}}}"
       "]}},"
       "\"stopReason\":\"tool_use\","
       "\"usage\":{\"inputTokens\":10,\"outputTokens\":20,"
       "\"cacheReadInputTokens\":3,\"cacheWriteInputTokens\":4}}";
   cJSON *r = cJSON_Parse(RESP);
   assert(r);
   aimee_response_t out;
   char err[128];
   assert(bedrock_converse_parse(r, &out, err, sizeof err) == 0);

   assert(out.n_content == 2);
   assert(out.content[0].type == AIMEE_BLK_TEXT);
   assert(strcmp(out.content[0].text, "Here you go") == 0);
   assert(out.content[1].type == AIMEE_BLK_TOOL_USE);
   assert(strcmp(out.content[1].tool_id, "toolu_1") == 0);
   assert(strcmp(out.content[1].tool_name, "Read") == 0);
   assert(out.content[1].tool_input);
   assert(strcmp(str(out.content[1].tool_input, "path"), "a.c") == 0);

   assert(out.stop_reason == AIMEE_STOP_TOOL_USE);
   assert(strcmp(out.raw_stop_reason, "tool_use") == 0);
   assert(out.usage_in == 10);
   assert(out.usage_out == 20);
   assert(out.usage_cache_read == 3);
   assert(out.usage_cache_write == 4);

   aimee_response_free(&out);
   cJSON_Delete(r);
}

static void test_parse_missing_message(void)
{
   cJSON *r = cJSON_Parse("{\"stopReason\":\"end_turn\"}");
   aimee_response_t out;
   char err[128];
   assert(bedrock_converse_parse(r, &out, err, sizeof err) == -1);
   cJSON_Delete(r);
}

static void test_parse_stop_reasons(void)
{
   /* guardrail_intervened -> CONTENT_FILTER, raw kept verbatim */
   cJSON *r = cJSON_Parse("{\"output\":{\"message\":{\"role\":\"assistant\",\"content\":[]}},"
                          "\"stopReason\":\"guardrail_intervened\"}");
   aimee_response_t out;
   char err[128];
   assert(bedrock_converse_parse(r, &out, err, sizeof err) == 0);
   assert(out.stop_reason == AIMEE_STOP_CONTENT_FILTER);
   assert(strcmp(out.raw_stop_reason, "guardrail_intervened") == 0);
   aimee_response_free(&out);
   cJSON_Delete(r);

   /* unknown stopReason -> UNKNOWN, raw kept */
   r = cJSON_Parse("{\"output\":{\"message\":{\"role\":\"assistant\",\"content\":[]}},"
                   "\"stopReason\":\"context_exceeded\"}");
   assert(bedrock_converse_parse(r, &out, err, sizeof err) == 0);
   assert(out.stop_reason == AIMEE_STOP_UNKNOWN);
   assert(strcmp(out.raw_stop_reason, "context_exceeded") == 0);
   aimee_response_free(&out);
   cJSON_Delete(r);
}

static void test_thinking_parse_roundtrip(void)
{
   /* reasoningContent -> THINKING with text + signature preserved */
   cJSON *r = cJSON_Parse(
       "{\"output\":{\"message\":{\"role\":\"assistant\",\"content\":["
       "{\"reasoningContent\":{\"reasoningText\":{\"text\":\"hmm\",\"signature\":\"sig9\"}}}"
       "]}},\"stopReason\":\"end_turn\"}");
   aimee_response_t out;
   char err[128];
   assert(bedrock_converse_parse(r, &out, err, sizeof err) == 0);
   assert(out.n_content == 1);
   assert(out.content[0].type == AIMEE_BLK_THINKING);
   assert(strcmp(out.content[0].text, "hmm") == 0);
   assert(strcmp(out.content[0].thinking_signature, "sig9") == 0);
   aimee_response_free(&out);
   cJSON_Delete(r);
}

static void test_tool_result_json_and_error(void)
{
   /* structured tool_result -> {json:...}; tool_is_error -> status:"error" */
   aimee_block_t tr = {0};
   tr.type = AIMEE_BLK_TOOL_RESULT;
   tr.tool_id = "t1";
   tr.tool_result = cJSON_Parse("{\"rows\":3}");
   tr.tool_is_error = 1;
   aimee_message_t msg = {0};
   msg.role = "user";
   msg.blocks = &tr;
   msg.n_blocks = 1;
   aimee_request_t ir = {0};
   ir.messages = &msg;
   ir.n_messages = 1;

   cJSON *j = bedrock_converse_build(&ir);
   cJSON *trj =
       obj(cJSON_GetArrayItem(obj(cJSON_GetArrayItem(obj(j, "messages"), 0), "content"), 0),
           "toolResult");
   assert(strcmp(str(trj, "status"), "error") == 0);
   cJSON *part = cJSON_GetArrayItem(obj(trj, "content"), 0);
   assert(obj(part, "json") && cJSON_IsObject(obj(part, "json")));
   assert(!obj(part, "text"));
   assert(obj(obj(part, "json"), "rows"));
   cJSON_Delete(j);
   cJSON_Delete(tr.tool_result);
}

/* image.format must be a Converse enum; an unknown media-type -> the block is
 * OMITTED (never a schema-invalid format). A known type -> emitted with the enum. */
static void test_image_format_enum(void)
{
   /* unknown subtype -> omitted */
   aimee_block_t bimg;
   memset(&bimg, 0, sizeof bimg);
   bimg.type = AIMEE_BLK_IMAGE;
   bimg.media_type = "application/octet-stream";
   bimg.media_ref = "aGVsbG8="; /* base64 */
   aimee_message_t m = {.role = "user", .blocks = &bimg, .n_blocks = 1};
   aimee_request_t ir;
   memset(&ir, 0, sizeof ir);
   ir.messages = &m;
   ir.n_messages = 1;
   cJSON *j = bedrock_converse_build(&ir);
   cJSON *content = obj(cJSON_GetArrayItem(obj(j, "messages"), 0), "content");
   assert(cJSON_GetArraySize(content) == 0); /* no image block emitted */
   cJSON_Delete(j);

   /* jpg alias -> "jpeg" */
   bimg.media_type = "image/jpg";
   j = bedrock_converse_build(&ir);
   content = obj(cJSON_GetArrayItem(obj(j, "messages"), 0), "content");
   assert(cJSON_GetArraySize(content) == 1);
   cJSON *img = obj(cJSON_GetArrayItem(content, 0), "image");
   assert(strcmp(str(img, "format"), "jpeg") == 0);
   cJSON_Delete(j);
}

/* toolUse.input must always be a JSON object even if the IR carried a non-object. */
static void test_tooluse_input_object(void)
{
   aimee_block_t btu;
   memset(&btu, 0, sizeof btu);
   btu.type = AIMEE_BLK_TOOL_USE;
   btu.tool_id = "t1";
   btu.tool_name = "fn";
   btu.tool_input = cJSON_CreateString("not-an-object"); /* opaque non-object */
   aimee_message_t m = {.role = "assistant", .blocks = &btu, .n_blocks = 1};
   aimee_request_t ir;
   memset(&ir, 0, sizeof ir);
   ir.messages = &m;
   ir.n_messages = 1;
   cJSON *j = bedrock_converse_build(&ir);
   cJSON *tu = obj(cJSON_GetArrayItem(obj(cJSON_GetArrayItem(obj(j, "messages"), 0), "content"), 0),
                   "toolUse");
   assert(cJSON_IsObject(obj(tu, "input"))); /* coerced to {}, not a string */
   cJSON_Delete(j);
   cJSON_Delete(btu.tool_input);
}

int main(void)
{
   test_build_allocation_atomicity();
   printf("aimee-backend-bedrock: ");
   test_build_full();
   test_build_minimal();
   test_tool_choice();
   test_thinking_roundtrip();
   test_parse_basic();
   test_parse_missing_message();
   test_parse_stop_reasons();
   test_thinking_parse_roundtrip();
   test_tool_result_json_and_error();
   test_image_format_enum();
   test_tooluse_input_object();
   printf("ok\n");
   return 0;
}
