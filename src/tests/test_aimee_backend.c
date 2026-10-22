/* test_aimee_backend.c -- Slice 2: the Anthropic backend build/parse. The headline
 * assertions: (1) same-protocol round-trip through the IR is STABLE
 * (parse->IR->build->parse == equal IR); (2) CROSS-protocol build works — an
 * OpenAI-parsed IR built into an Anthropic request yields the SAME IR an Anthropic
 * client would (proving "no direct translation": OpenAI client -> IR -> Anthropic
 * backend needs zero Anthropic<->OpenAI code). */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <aimee/translation/aimee_backend.h>
#include <aimee/translation/aimee_frontend.h>
#include <aimee/ir/aimee_ir.h>
#include "cJSON.h"

static const char *ANTHROPIC =
    "{\"model\":\"claude-3-5-sonnet-20241022\",\"max_tokens\":1024,"
    "\"system\":[{\"type\":\"text\",\"text\":\"You are a helpful coding assistant.\"}],"
    "\"messages\":[{\"role\":\"user\",\"content\":[{\"type\":\"text\",\"text\":\"read foo.c\"}]}],"
    "\"tools\":[{\"name\":\"Read\",\"description\":\"Read a file\",\"input_schema\":"
    "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"}},\"required\":[\"path\"]}}"
    "]}";

static const char *OPENAI =
    "{\"model\":\"gpt-4o\",\"max_tokens\":1024,"
    "\"messages\":[{\"role\":\"system\",\"content\":\"You are a helpful coding assistant.\"},"
    "{\"role\":\"user\",\"content\":\"read foo.c\"}],"
    "\"tools\":[{\"type\":\"function\",\"function\":{\"name\":\"Read\",\"description\":\"Read a "
    "file\","
    "\"parameters\":{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"}},"
    "\"required\":[\"path\"]}}}]}";

/* parse an Anthropic request JSON into the IR */
static void parse_anthropic(const char *json, aimee_request_t *ir)
{
   char err[128];
   cJSON *j = cJSON_Parse(json);
   assert(j);
   assert(anthropic_frontend_parse(j, ir, err, sizeof err) == 0);
   cJSON_Delete(j);
}

static cJSON *refusal_response(const char *details)
{
   char buffer[8192];
   snprintf(buffer, sizeof buffer,
            "{\"role\":\"assistant\",\"content\":[],\"stop_reason\":\"refusal\","
            "\"stop_details\":%s,\"usage\":{\"input_tokens\":1,\"output_tokens\":0}}",
            details);
   return cJSON_Parse(buffer);
}

static void test_refusal_details_shapes(void)
{
   const char *valid[] = {
       "{\"type\":\"refusal\",\"category\":null,\"explanation\":null}",
       "{\"type\":\"refusal\",\"category\":\"\",\"explanation\":\"\"}",
       "{\"type\":\"refusal\",\"category\":\"future\",\"explanation\":null,"
       "\"fallback_credit_token\":\"do-not-retain\",\"recommended_model\":\"not-authority\"}",
       "{\"type\":\"refusal\"}"};
   char error[128];
   for (size_t i = 0; i < sizeof valid / sizeof *valid; i++)
   {
      cJSON *input = refusal_response(valid[i]);
      aimee_response_t response = {0};
      assert(input && !anthropic_backend_parse(input, &response, error, sizeof error));
      cJSON *output = anthropic_frontend_render(&response);
      cJSON *details = cJSON_GetObjectItemCaseSensitive(output, "stop_details");
      assert(cJSON_IsObject(details) && cJSON_GetArraySize(details) == 3);
      assert(cJSON_GetObjectItemCaseSensitive(details, "category"));
      assert(cJSON_GetObjectItemCaseSensitive(details, "explanation"));
      char *raw = cJSON_PrintUnformatted(response.raw);
      assert(raw && !strstr(raw, "fallback_credit_token") && !strstr(raw, "recommended_model"));
      free(raw);
      cJSON_Delete(output);
      aimee_response_free(&response);
      cJSON_Delete(input);
   }
   cJSON *error_with_details = cJSON_Parse(
       "{\"error\":{\"type\":\"api_error\",\"message\":\"upstream error\"},"
       "\"stop_details\":{\"type\":\"refusal\",\"fallback_credit_token\":\"do-not-retain\"}}");
   aimee_response_t failed_response = {0};
   assert(error_with_details &&
          anthropic_backend_parse(error_with_details, &failed_response, error, sizeof error) == -1);
   assert(!failed_response.raw && !failed_response.refusal);
   aimee_response_free(&failed_response);
   assert(cJSON_ReplaceItemInObjectCaseSensitive(error_with_details, "stop_details",
                                                 cJSON_CreateNull()));
   assert(!anthropic_backend_parse(error_with_details, &failed_response, error, sizeof error));
   assert(failed_response.stop_reason == AIMEE_STOP_ERROR && failed_response.error_message);
   aimee_response_free(&failed_response);
   cJSON_DeleteItemFromObjectCaseSensitive(error_with_details, "stop_details");
   assert(!anthropic_backend_parse(error_with_details, &failed_response, error, sizeof error));
   assert(failed_response.stop_reason == AIMEE_STOP_ERROR && failed_response.error_message);
   aimee_response_free(&failed_response);
   cJSON_Delete(error_with_details);
   const char *invalid[] = {"[]",
                            "5",
                            "{\"type\":\"other\"}",
                            "{\"type\":null}",
                            "{\"type\":\"refusal\",\"category\":1}",
                            "{\"type\":\"refusal\",\"explanation\":{}}",
                            "{\"type\":\"refusal\",\"category\":null,\"category\":\"duplicate\"}"};
   for (size_t i = 0; i < sizeof invalid / sizeof *invalid; i++)
   {
      cJSON *input = refusal_response(invalid[i]);
      aimee_response_t response = {0};
      assert(input && anthropic_backend_parse(input, &response, error, sizeof error) == -1);
      aimee_response_free(&response);
      cJSON_Delete(input);
   }
   for (int field = 0; field < 2; field++)
   {
      size_t bound = field ? 4096 : 64;
      char *text = malloc(bound + 2);
      assert(text);
      memset(text, 'x', bound + 1);
      for (int overflow = 0; overflow < 2; overflow++)
      {
         text[bound + overflow] = 0;
         cJSON *input =
             refusal_response("{\"type\":\"refusal\",\"category\":null,\"explanation\":null}");
         cJSON *details = cJSON_GetObjectItemCaseSensitive(input, "stop_details");
         assert(cJSON_ReplaceItemInObjectCaseSensitive(details, field ? "explanation" : "category",
                                                       cJSON_CreateString(text)));
         aimee_response_t response = {0};
         assert(anthropic_backend_parse(input, &response, error, sizeof error) ==
                (overflow ? -1 : 0));
         aimee_response_free(&response);
         cJSON_Delete(input);
         text[bound] = 'x';
      }
      free(text);
   }
   cJSON *bad_utf8 = refusal_response("{\"type\":\"refusal\"}");
   cJSON *details = cJSON_GetObjectItemCaseSensitive(bad_utf8, "stop_details");
   cJSON_AddStringToObject(details, "explanation", "\xff");
   aimee_response_t response = {0};
   assert(anthropic_backend_parse(bad_utf8, &response, error, sizeof error) == -1);
   aimee_response_free(&response);
   cJSON_Delete(bad_utf8);
   aimee_refusal_details_t details_view = {.category = "future", .explanation = "Diagnostic"};
   aimee_response_t contradictory = {
       .role = "assistant", .stop_reason = AIMEE_STOP_END_TURN, .refusal = &details_view};
   assert(!anthropic_frontend_render(&contradictory));
   contradictory.stop_reason = AIMEE_STOP_ERROR;
   contradictory.error_message = "existing error";
   assert(!anthropic_frontend_render(&contradictory));
   contradictory.stop_reason = AIMEE_STOP_CONTENT_FILTER;
   assert(!anthropic_frontend_render(&contradictory));
   contradictory.error_message = NULL;
   details_view.category = "\xff";
   assert(!anthropic_frontend_render(&contradictory));

   aimee_response_t filtered = {.role = "assistant", .stop_reason = AIMEE_STOP_CONTENT_FILTER};
   cJSON *filtered_wire = anthropic_frontend_render(&filtered);
   assert(
       !strcmp(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(filtered_wire, "stop_reason")),
               "refusal"));
   cJSON_Delete(filtered_wire);
   filtered.raw_stop_reason = "contradictory-raw-value";
   filtered_wire = anthropic_frontend_render(&filtered);
   assert(
       !strcmp(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(filtered_wire, "stop_reason")),
               "refusal"));
   cJSON_Delete(filtered_wire);
   for (int present = 0; present < 2; present++)
   {
      cJSON *input = refusal_response("null");
      if (!present)
         cJSON_DeleteItemFromObjectCaseSensitive(input, "stop_details");
      assert(!anthropic_backend_parse(input, &response, error, sizeof error));
      cJSON *output = anthropic_frontend_render(&response);
      assert(!cJSON_GetObjectItemCaseSensitive(output, "stop_details"));
      cJSON_Delete(output);
      aimee_response_free(&response);
      cJSON_Delete(input);
   }
}

static void test_refusal_details_codec(void)
{
   const char *wire = "{\"id\":\"refusal-codec\",\"model\":\"model\",\"role\":\"assistant\","
                      "\"content\":[],\"stop_reason\":\"refusal\","
                      "\"stop_details\":{\"type\":\"refusal\",\"category\":\"future-category\","
                      "\"explanation\":\"Provider explanation\"},"
                      "\"usage\":{\"input_tokens\":1,\"output_tokens\":0}}";
   cJSON *input = cJSON_Parse(wire);
   aimee_response_t response = {0};
   char error[128];
   assert(input && !anthropic_backend_parse(input, &response, error, sizeof error));
   cJSON *output = anthropic_frontend_render(&response);
   const cJSON *details = cJSON_GetObjectItemCaseSensitive(output, "stop_details");
   assert(cJSON_IsObject(details));
   assert(
       !strcmp(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(details, "type")), "refusal"));
   assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(details, "category")),
                  "future-category"));
   assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(details, "explanation")),
                  "Provider explanation"));
   cJSON_Delete(output);
   aimee_response_free(&response);
   cJSON_Delete(input);
}

int main(void)
{
   test_refusal_details_codec();
   test_refusal_details_shapes();
   printf("aimee-backend: ");
   char err[128];

   const char *fallbacks[] = {"selected", "fallback", "fallback"};
   aimee_openai_host_profile_t host_profile = {.min_p = 0.2,
                                               .repeat_penalty = 1.1,
                                               .disable_thinking = 1,
                                               .split_reasoning = 1,
                                               .fallback_models = fallbacks,
                                               .n_fallback_models = 3};
   aimee_request_t host_ir = {.model = strdup("selected"), .frontend = AIMEE_WIRE_OPENAI_CHAT};
   assert(!openai_ir_set_host_profile(&host_ir, &host_profile));
   cJSON *host_native = openai_backend_build(&host_ir);
   assert(host_native);
   const cJSON *host_models = cJSON_GetObjectItemCaseSensitive(host_native, "models");
   assert(cJSON_GetArraySize(host_models) == 2);
   assert(!strcmp(cJSON_GetArrayItem(host_models, 0)->valuestring, "selected"));
   assert(!strcmp(cJSON_GetArrayItem(host_models, 1)->valuestring, "fallback"));
   assert(cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(
       cJSON_GetObjectItemCaseSensitive(host_native, "chat_template_kwargs"), "enable_thinking")));
   assert(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(host_native, "reasoning_split")));
   cJSON *entry = host_ir.raw;
   assert(openai_ir_set_host_profile(&host_ir, &host_profile) == -1 && host_ir.raw == entry);
   cJSON_Delete(host_native);
   aimee_request_free(&host_ir);
   host_ir = (aimee_request_t){.model = strdup("selected"), .frontend = AIMEE_WIRE_RESPONSES};
   assert(openai_ir_set_host_profile(&host_ir, &host_profile) == -1 && !host_ir.raw);
   aimee_request_free(&host_ir);

   /* Only declared same-wire extensions survive; typed edits and removals
    * cannot be overwritten by the entry snapshot or private root metadata. */
   cJSON *profile = cJSON_Parse(
       "{\"model\":\"old\",\"messages\":[],\"temperature\":0.5,"
       "\"min_p\":0.1,\"repeat_penalty\":1.1,\"chat_template_kwargs\":{\"enable_thinking\":false},"
       "\"route\":\"fallback\",\"models\":[\"old\",\"fallback\"],\"reasoning_split\":true,"
       "\"_host_authority\":\"private\",\"unregistered_extension\":true}");
   aimee_request_t profile_ir = {0};
   assert(profile && !openai_frontend_parse(profile, &profile_ir, err, sizeof err));
   profile_ir.has_temperature = 0;
   profile_ir.mutated = 1;
   cJSON *projected = openai_backend_build(&profile_ir);
   assert(projected);
   assert(!strcmp(cJSON_GetObjectItemCaseSensitive(projected, "model")->valuestring, "old"));
   assert(!cJSON_GetObjectItemCaseSensitive(projected, "temperature"));
   assert(!cJSON_GetObjectItemCaseSensitive(projected, "_host_authority"));
   assert(!cJSON_GetObjectItemCaseSensitive(projected, "unregistered_extension"));
   const char *extensions[] = {"min_p", "repeat_penalty", "chat_template_kwargs",
                               "route", "models",         "reasoning_split"};
   for (size_t i = 0; i < sizeof extensions / sizeof *extensions; i++)
      assert(cJSON_Compare(cJSON_GetObjectItemCaseSensitive(profile, extensions[i]),
                           cJSON_GetObjectItemCaseSensitive(projected, extensions[i]), 1));
   cJSON_Delete(projected);
   free(profile_ir.model);
   profile_ir.model = strdup("new");
   projected = openai_backend_build(&profile_ir);
   assert(projected && !cJSON_GetObjectItemCaseSensitive(projected, "models"));
   assert(!cJSON_GetObjectItemCaseSensitive(projected, "reasoning_split"));
   assert(!strcmp(cJSON_GetObjectItemCaseSensitive(projected, "model")->valuestring, "new"));
   cJSON_Delete(projected);
   free(profile_ir.model);
   profile_ir.model = strdup("old");
   profile_ir.frontend = AIMEE_WIRE_RESPONSES;
   projected = openai_backend_build(&profile_ir);
   assert(projected && !cJSON_GetObjectItemCaseSensitive(projected, "route"));
   cJSON_Delete(projected);
   aimee_request_free(&profile_ir);
   cJSON_Delete(profile);

   /* Native Anthropic replay preserves client forms and cache markers. Repeated
    * rendering and parse/render remain deterministic. */
   aimee_request_t air;
   parse_anthropic(ANTHROPIC, &air);
   cJSON *built = anthropic_backend_build(&air);
   assert(built);
   char *b1 = cJSON_PrintUnformatted(built);
   cJSON *built2 = anthropic_backend_build(&air); /* determinism: a second build matches */
   char *b2 = cJSON_PrintUnformatted(built2);
   assert(b1 && b2 && strcmp(b1, b2) == 0);
   aimee_request_t air2;
   assert(anthropic_frontend_parse(built, &air2, err, sizeof err) == 0);
   cJSON *rebuilt = anthropic_backend_build(&air2); /* idempotent round-trip */
   char *b3 = cJSON_PrintUnformatted(rebuilt);
   assert(b3 && strcmp(b1, b3) == 0);
   free(b1);
   free(b2);
   free(b3);
   cJSON_Delete(built);
   cJSON_Delete(built2);
   cJSON_Delete(rebuilt);

   /* --- (1b) ir->mutated forces the typed rebuild; MODELED top-level fields
    *          (top_p, top_k, metadata) survive it -- required so the canonical egress
    *          is byte-faithful once the raw sidecar is retired. --- */
   {
      cJSON *uj = cJSON_Parse("{\"model\":\"claude-3-5-sonnet\",\"max_tokens\":8,\"messages\":[{"
                              "\"role\":\"user\",\"content\":[{\"type\":\"text\",\"text\":\"hi\"}]}"
                              "],\"top_p\":0.9,\"top_k\":40,\"metadata\":{\"user_id\":\"u1\"},"
                              "\"service_tier\":\"standard_only\","
                              "\"thinking\":{\"type\":\"enabled\",\"budget_tokens\":1024}}");
      aimee_request_t uir;
      assert(anthropic_frontend_parse(uj, &uir, err, sizeof err) == 0);
      cJSON_Delete(uj);
      cJSON *clean = anthropic_backend_build(&uir); /* mutated=0 -> raw passthrough */
      assert(clean && cJSON_GetObjectItem(clean, "top_p") != NULL); /* preserved */
      uir.mutated = 1;
      cJSON *rebuilt = anthropic_backend_build(&uir); /* typed rebuild */
      cJSON *tp = cJSON_GetObjectItem(rebuilt, "top_p");
      cJSON *tk = cJSON_GetObjectItem(rebuilt, "top_k");
      cJSON *md = cJSON_GetObjectItem(rebuilt, "metadata");
      cJSON *st = cJSON_GetObjectItem(rebuilt, "service_tier");
      cJSON *th = cJSON_GetObjectItem(rebuilt, "thinking");
      assert(tp && tp->valuedouble == 0.9); /* modeled -> survives the rebuild */
      assert(tk && tk->valueint == 40);     /* modeled */
      assert(md && cJSON_IsObject(md));     /* modeled (opaque, preserved) */
      assert(st && cJSON_IsString(st) && strcmp(st->valuestring, "standard_only") == 0);
      assert(th && cJSON_IsObject(th)); /* thinking config (opaque, preserved) */
      cJSON_Delete(clean);
      cJSON_Delete(rebuilt);
      aimee_request_free(&uir);
   }

   /* --- (2) CROSS-protocol BYTE-IDENTITY: the Anthropic egress of the SAME logical
    *         content is byte-identical whether sourced from OpenAI-wire or
    *         Anthropic-wire (Anthropic prompt-caches on exact bytes). The served model
    *         is overridden to the target agent's in the real flow, so align it here;
    *         force air through the typed rebuild (mutated) instead of the raw sidecar. --- */
   cJSON *oj = cJSON_Parse(OPENAI);
   aimee_request_t oir;
   assert(openai_frontend_parse(oj, &oir, err, sizeof err) == 0);
   cJSON_Delete(oj);
   free(oir.model);
   oir.model = strdup("claude-3-5-sonnet");
   free(air.model);
   air.model = strdup("claude-3-5-sonnet");
   air.mutated = 1;
   air.cache_policy = AIMEE_CACHE_STABLE_PREFIX;
   oir.cache_policy = AIMEE_CACHE_STABLE_PREFIX;
   cJSON *xbuilt = anthropic_backend_build(&oir); /* openai -> IR -> anthropic egress */
   cJSON *abuilt = anthropic_backend_build(&air); /* anthropic -> IR -> anthropic egress */
   assert(xbuilt && abuilt);
   char *xs = cJSON_PrintUnformatted(xbuilt);
   char *as = cJSON_PrintUnformatted(abuilt);
   assert(xs && as && strcmp(xs, as) == 0); /* cross-protocol egress is BYTE-IDENTICAL */
   /* the built Anthropic request has system as a top-level field, not a message */
   assert(cJSON_GetObjectItem(xbuilt, "system") != NULL);
   assert(cJSON_GetArraySize(cJSON_GetObjectItem(xbuilt, "messages")) == 1);
   free(xs);
   free(as);
   cJSON_Delete(xbuilt);
   cJSON_Delete(abuilt);

   aimee_request_free(&air);
   aimee_request_free(&air2);
   aimee_request_free(&oir);

   /* --- (3) response parse: Anthropic response -> IR --- */
   const char *RESP =
       "{\"id\":\"msg_1\",\"model\":\"claude-3-5-sonnet\",\"role\":\"assistant\","
       "\"content\":[{\"type\":\"text\",\"text\":\"hi\"},"
       "{\"type\":\"tool_use\",\"id\":\"toolu_2\",\"name\":\"Read\",\"input\":{\"path\":\"x\"}}],"
       "\"stop_reason\":\"tool_use\",\"usage\":{\"input_tokens\":12,\"output_tokens\":7}}";
   cJSON *rj = cJSON_Parse(RESP);
   aimee_response_t resp;
   assert(anthropic_backend_parse(rj, &resp, err, sizeof err) == 0);
   assert(strcmp(resp.id, "msg_1") == 0);
   assert(resp.stop_reason == AIMEE_STOP_TOOL_USE);
   assert(resp.n_content == 2 && resp.content[1].type == AIMEE_BLK_TOOL_USE);
   assert(strcmp(resp.content[1].tool_id, "toolu_2") == 0 && resp.content[1].tool_input);
   assert(resp.usage_in == 12 && resp.usage_out == 7);
   aimee_response_free(&resp);
   cJSON_Delete(rj);

   /* --- (3b) a thinking response must survive parse -> render with its SIGNATURE.
    *          Anthropic rejects a resubmitted thinking block whose signature does not
    *          match, so dropping it anywhere on the response path yields a turn that
    *          cannot be replayed. The request direction already modelled this; the
    *          response direction dropped it at BOTH ends (parse and render). --- */
   const char *THINK_RESP =
       "{\"id\":\"msg_2\",\"model\":\"claude-3-5-sonnet\",\"role\":\"assistant\","
       "\"content\":[{\"type\":\"thinking\",\"thinking\":\"let me check\","
       "\"signature\":\"ErUBCkYIAxgCIkA\"},{\"type\":\"text\",\"text\":\"done\"}],"
       "\"stop_reason\":\"end_turn\",\"usage\":{\"input_tokens\":3,\"output_tokens\":4}}";
   cJSON *tj = cJSON_Parse(THINK_RESP);
   aimee_response_t tresp;
   assert(anthropic_backend_parse(tj, &tresp, err, sizeof err) == 0);
   assert(tresp.n_content == 2 && tresp.content[0].type == AIMEE_BLK_THINKING);
   assert(strcmp(tresp.content[0].text, "let me check") == 0);
   assert(tresp.content[0].thinking_signature &&
          strcmp(tresp.content[0].thinking_signature, "ErUBCkYIAxgCIkA") == 0);

   cJSON *trendered = anthropic_frontend_render(&tresp);
   assert(trendered);
   cJSON *tc = cJSON_GetObjectItem(trendered, "content");
   cJSON *tblk = cJSON_GetArrayItem(tc, 0);
   assert(strcmp(cJSON_GetObjectItem(tblk, "type")->valuestring, "thinking") == 0);
   assert(strcmp(cJSON_GetObjectItem(tblk, "thinking")->valuestring, "let me check") == 0);
   assert(cJSON_GetObjectItem(tblk, "signature") &&
          strcmp(cJSON_GetObjectItem(tblk, "signature")->valuestring, "ErUBCkYIAxgCIkA") == 0);
   cJSON_Delete(trendered);

   /* An unsigned/foreign thought stays canonical but cannot be replayed as
    * fabricated Anthropic reasoning with a missing signature. */
   free(tresp.content[0].thinking_signature);
   tresp.content[0].thinking_signature = NULL;
   cJSON *tunsigned = anthropic_frontend_render(&tresp);
   assert(tunsigned);
   const cJSON *unsigned_content = cJSON_GetObjectItem(tunsigned, "content");
   assert(cJSON_GetArraySize(unsigned_content) == 1);
   assert(!strcmp(
       cJSON_GetStringValue(cJSON_GetObjectItem(cJSON_GetArrayItem(unsigned_content, 0), "text")),
       "done"));
   assert(tresp.n_content == 2 && !strcmp(tresp.content[0].text, "let me check"));
   cJSON_Delete(tunsigned);

   aimee_response_free(&tresp);
   cJSON_Delete(tj);

   /* --- (4) OpenAI backend: Anthropic client -> IR -> OpenAI request -> re-parse
    *         == equal IR (cross-protocol build via OpenAI). --- */
   aimee_request_t bir;
   parse_anthropic(ANTHROPIC, &bir);
   cJSON *obuilt = openai_backend_build(&bir);
   assert(obuilt);
   aimee_request_t bir2;
   assert(openai_frontend_parse(obuilt, &bir2, err, sizeof err) == 0);
   assert(aimee_ir_request_equal(&bir, &bir2));
   cJSON_Delete(obuilt);
   aimee_request_free(&bir);
   aimee_request_free(&bir2);

   /* OpenAI response -> IR */
   const char *ORESP =
       "{\"id\":\"cmpl_1\",\"model\":\"gpt-4o\",\"choices\":[{\"index\":0,\"message\":"
       "{\"role\":\"assistant\",\"content\":null,\"tool_calls\":[{\"id\":\"call_5\","
       "\"type\":\"function\",\"function\":{\"name\":\"Read\",\"arguments\":\"{\\\"path\\\":"
       "\\\"y\\\"}\"}}]},"
       "\"finish_reason\":\"tool_calls\"}],\"usage\":{\"prompt_tokens\":20,\"completion_tokens\":8}"
       "}";
   cJSON *orj = cJSON_Parse(ORESP);
   aimee_response_t oresp;
   assert(openai_backend_parse(orj, &oresp, err, sizeof err) == 0);
   assert(oresp.stop_reason == AIMEE_STOP_TOOL_USE);
   assert(oresp.n_content == 1 && oresp.content[0].type == AIMEE_BLK_TOOL_USE);
   assert(strcmp(oresp.content[0].tool_id, "call_5") == 0 && oresp.content[0].tool_input);
   assert(oresp.usage_in == 20 && oresp.usage_out == 8);
   aimee_response_free(&oresp);
   cJSON_Delete(orj);

   /* CACHED PROMPT TOKENS on the openai wire. They arrive in a SIBLING object,
    * usage.prompt_tokens_details.cached_tokens, not as a flat counter -- so a
    * parser reading only prompt_tokens/completion_tokens loses them silently and
    * reports cache_read=0 forever. Measured before the fix: 158 calls and 3.1M
    * prompt tokens across a whole day, cache_read_tokens=0, which reads as
    * "prompt caching is not working" when it may have been working throughout.
    * The Anthropic arm read its equivalent all along, which is why only
    * Anthropic-egress ever showed cache numbers. */
   const char *OCACHE =
       "{\"id\":\"cmpl_2\",\"model\":\"gpt-4o\",\"choices\":[{\"index\":0,\"message\":"
       "{\"role\":\"assistant\",\"content\":\"hi\"},\"finish_reason\":\"stop\"}],"
       "\"usage\":{\"prompt_tokens\":100,\"completion_tokens\":5,"
       "\"prompt_tokens_details\":{\"cached_tokens\":64}}}";
   cJSON *ocj = cJSON_Parse(OCACHE);
   aimee_response_t ocresp;
   assert(openai_backend_parse(ocj, &ocresp, err, sizeof err) == 0);
   assert(ocresp.usage_in == 100 && ocresp.usage_out == 5);
   assert(ocresp.usage_cache_read == 64);
   aimee_response_free(&ocresp);
   cJSON_Delete(ocj);

   /* Same on the responses wire, under a different name for the same shape:
    * usage.input_tokens_details.cached_tokens. */
   const char *RCACHE =
       "{\"id\":\"resp_2\",\"model\":\"gpt-5\",\"output\":[{\"type\":\"message\","
       "\"role\":\"assistant\",\"content\":[{\"type\":\"output_text\",\"text\":\"hi\"}]}],"
       "\"usage\":{\"input_tokens\":200,\"output_tokens\":6,"
       "\"input_tokens_details\":{\"cached_tokens\":128}}}";
   cJSON *rcj = cJSON_Parse(RCACHE);
   aimee_response_t rcresp;
   assert(responses_backend_parse(rcj, &rcresp, err, sizeof err) == 0);
   assert(rcresp.usage_in == 200 && rcresp.usage_out == 6);
   assert(rcresp.usage_cache_read == 128);
   aimee_response_free(&rcresp);
   cJSON_Delete(rcj);

   /* Reasoning models on the openai wire embed chain-of-thought inline. The IR STORES
    * it as a THINKING block (not discarded) and keeps the answer as TEXT; the content
    * accessor excludes THINKING so callers see only the answer. */
   const char *OTHINK =
       "{\"choices\":[{\"finish_reason\":\"stop\",\"message\":{\"role\":\"assistant\","
       "\"content\":\"<think>hidden reasoning</think>the answer\"}}]}";
   cJSON *otj = cJSON_Parse(OTHINK);
   aimee_response_t othink;
   assert(openai_backend_parse(otj, &othink, err, sizeof err) == 0);
   int n_think = 0, n_text = 0;
   for (int i = 0; i < othink.n_content; i++)
   {
      if (othink.content[i].type == AIMEE_BLK_THINKING)
      {
         n_think++;
         assert(strcmp(othink.content[i].text, "hidden reasoning") == 0); /* STORED */
      }
      if (othink.content[i].type == AIMEE_BLK_TEXT)
      {
         n_text++;
         assert(strcmp(othink.content[i].text, "the answer") == 0);
      }
   }
   assert(n_think == 1 && n_text == 1);
   char buf[64];
   aimee_ir_response_text(&othink, buf, sizeof buf);
   assert(strcmp(buf, "the answer") == 0); /* accessor excludes THINKING */
   aimee_response_free(&othink);
   cJSON_Delete(otj);

   /* Content-parts array (mistral): text parts -> answer, thinking parts -> stored. */
   const char *OARR =
       "{\"choices\":[{\"finish_reason\":\"stop\",\"message\":{\"role\":\"assistant\","
       "\"content\":[{\"type\":\"thinking\",\"thinking\":[{\"type\":\"text\",\"text\":\"why\"}]},"
       "{\"type\":\"text\",\"text\":\"final\"}]}}]}";
   cJSON *oaj = cJSON_Parse(OARR);
   aimee_response_t oarr;
   assert(openai_backend_parse(oaj, &oarr, err, sizeof err) == 0);
   aimee_ir_response_text(&oarr, buf, sizeof buf);
   assert(strcmp(buf, "final") == 0);
   int arr_think = 0;
   for (int i = 0; i < oarr.n_content; i++)
      if (oarr.content[i].type == AIMEE_BLK_THINKING)
         arr_think++;
   assert(arr_think == 1); /* thinking part stored, not dropped */
   aimee_response_free(&oarr);
   cJSON_Delete(oaj);

   /* --- (5) Responses (codex) backend: Anthropic IR -> Responses request; a
    *         Responses response (message + function_call output items) -> IR. --- */
   aimee_request_t cir;
   parse_anthropic(ANTHROPIC, &cir);
   cJSON *cbuilt = responses_backend_build(&cir);
   assert(cbuilt);
   assert(cJSON_GetObjectItem(cbuilt, "instructions")); /* system -> instructions */
   assert(cJSON_GetArraySize(cJSON_GetObjectItem(cbuilt, "input")) >= 1); /* messages -> items */
   assert(cJSON_GetObjectItem(cbuilt, "tools"));
   cJSON_Delete(cbuilt);
   aimee_request_free(&cir);

   const char *CRESP = "{\"id\":\"resp_1\",\"status\":\"completed\",\"output\":["
                       "{\"type\":\"message\",\"role\":\"assistant\",\"content\":[{\"type\":"
                       "\"output_text\",\"text\":\"done\"}]},"
                       "{\"type\":\"function_call\",\"call_id\":\"fc_1\",\"name\":\"Read\","
                       "\"arguments\":\"{\\\"path\\\":\\\"z\\\"}\"}],"
                       "\"usage\":{\"input_tokens\":30,\"output_tokens\":9}}";
   cJSON *crj = cJSON_Parse(CRESP);
   aimee_response_t cresp;
   assert(responses_backend_parse(crj, &cresp, err, sizeof err) == 0);
   assert(cresp.stop_reason == AIMEE_STOP_TOOL_USE); /* a function_call was emitted */
   assert(cresp.n_content == 2);
   assert(cresp.content[0].type == AIMEE_BLK_TEXT && strcmp(cresp.content[0].text, "done") == 0);
   assert(cresp.content[1].type == AIMEE_BLK_TOOL_USE &&
          strcmp(cresp.content[1].tool_id, "fc_1") == 0 && cresp.content[1].tool_input);
   assert(cresp.usage_in == 30 && cresp.usage_out == 9);
   aimee_response_free(&cresp);
   cJSON_Delete(crj);

   /* --- (5b) Responses `reasoning` items: BOTH summary shapes yield the text.
    *          The wire's shape is not settled here -- a bare string appears in some
    *          payloads, an array of typed parts (the same idiom this file already
    *          reads for a message's `content`) in others -- and the parser must not
    *          bet on one. Betting wrong drops reasoning silently: ostr() on an array
    *          returns NULL, leaving a THINKING block with no text at all. --- */
   {
      /* array of parts */
      const char *R_ARR = "{\"id\":\"r1\",\"status\":\"completed\",\"output\":["
                          "{\"type\":\"reasoning\",\"id\":\"rs_1\",\"summary\":["
                          "{\"type\":\"summary_text\",\"text\":\"first thought\"},"
                          "{\"type\":\"summary_text\",\"text\":\"second thought\"}]},"
                          "{\"type\":\"message\",\"role\":\"assistant\",\"content\":["
                          "{\"type\":\"output_text\",\"text\":\"answer\"}]}]}";
      cJSON *j = cJSON_Parse(R_ARR);
      aimee_response_t r;
      assert(responses_backend_parse(j, &r, err, sizeof err) == 0);
      assert(r.n_content == 2 && r.content[0].type == AIMEE_BLK_THINKING);
      assert(r.content[0].text &&
             strcmp(r.content[0].text, "first thought\n\nsecond thought") == 0);
      /* the reasoning must not be confused with the answer */
      assert(r.content[1].type == AIMEE_BLK_TEXT && strcmp(r.content[1].text, "answer") == 0);
      aimee_response_free(&r);
      cJSON_Delete(j);
   }
   {
      /* bare string */
      const char *R_STR = "{\"id\":\"r2\",\"status\":\"completed\",\"output\":["
                          "{\"type\":\"reasoning\",\"summary\":\"a single thought\"}]}";
      cJSON *j = cJSON_Parse(R_STR);
      aimee_response_t r;
      assert(responses_backend_parse(j, &r, err, sizeof err) == 0);
      assert(r.n_content == 1 && r.content[0].type == AIMEE_BLK_THINKING);
      assert(r.content[0].text && strcmp(r.content[0].text, "a single thought") == 0);
      aimee_response_free(&r);
      cJSON_Delete(j);
   }
   {
      /* an absent or empty summary yields NO text rather than an empty thought */
      const char *R_EMPTY = "{\"id\":\"r3\",\"status\":\"completed\",\"output\":["
                            "{\"type\":\"reasoning\",\"summary\":[]},"
                            "{\"type\":\"reasoning\"}]}";
      cJSON *j = cJSON_Parse(R_EMPTY);
      aimee_response_t r;
      assert(responses_backend_parse(j, &r, err, sizeof err) == 0);
      assert(r.n_content == 2);
      assert(r.content[0].type == AIMEE_BLK_THINKING && r.content[0].text == NULL);
      assert(r.content[1].type == AIMEE_BLK_THINKING && r.content[1].text == NULL);
      aimee_response_free(&r);
      cJSON_Delete(j);
   }

   /* --- (6) tool_result SPLIT (grouping ruling, Option A): an Anthropic tool
    *         conversation -> IR -> OpenAI emits a role:tool message + Responses a
    *         function_call_output, both carrying the tool_id VERBATIM. --- */
   const char *CONVO =
       "{\"model\":\"claude-3-5-sonnet\",\"max_tokens\":1024,\"messages\":["
       "{\"role\":\"user\",\"content\":[{\"type\":\"text\",\"text\":\"read foo.c\"}]},"
       "{\"role\":\"assistant\",\"content\":[{\"type\":\"tool_use\",\"id\":\"toolu_1\",\"name\":"
       "\"Read\",\"input\":{\"path\":\"foo.c\"}}]},"
       "{\"role\":\"user\",\"content\":[{\"type\":\"tool_result\",\"tool_use_id\":\"toolu_1\","
       "\"content\":\"file contents\"}]}]}";
   aimee_request_t conv;
   parse_anthropic(CONVO, &conv);
   /* IR: the 3rd message is a user message with a single TOOL_RESULT block */
   assert(conv.n_messages == 3);
   assert(conv.messages[2].n_blocks == 1 &&
          conv.messages[2].blocks[0].type == AIMEE_BLK_TOOL_RESULT &&
          strcmp(conv.messages[2].blocks[0].tool_id, "toolu_1") == 0);

   /* OpenAI backend: a role:"tool" message with tool_call_id=toolu_1 */
   cJSON *ob = openai_backend_build(&conv);
   cJSON *om = cJSON_GetObjectItem(ob, "messages");
   int found_tool = 0;
   for (int k = 0; k < cJSON_GetArraySize(om); k++)
   {
      cJSON *mm = cJSON_GetArrayItem(om, k);
      cJSON *role = cJSON_GetObjectItem(mm, "role");
      if (role && strcmp(role->valuestring, "tool") == 0)
      {
         assert(strcmp(cJSON_GetObjectItem(mm, "tool_call_id")->valuestring, "toolu_1") == 0);
         assert(strcmp(cJSON_GetObjectItem(mm, "content")->valuestring, "file contents") == 0);
         found_tool = 1;
      }
   }
   assert(found_tool);
   /* full round-trip: Anthropic tool convo -> IR -> OpenAI wire (SPLIT) -> IR
    * (MERGE) == the original IR. The split (backend) and merge (frontend) are
    * inverses over the common subset. */
   aimee_request_t conv_rt;
   assert(openai_frontend_parse(ob, &conv_rt, err, sizeof err) == 0);
   assert(aimee_ir_request_equal(&conv, &conv_rt));
   aimee_request_free(&conv_rt);
   cJSON_Delete(ob);

   /* Responses backend: a function_call_output item with call_id=toolu_1 */
   cJSON *cb = responses_backend_build(&conv);
   cJSON *ci = cJSON_GetObjectItem(cb, "input");
   int found_fco = 0;
   for (int k = 0; k < cJSON_GetArraySize(ci); k++)
   {
      cJSON *it = cJSON_GetArrayItem(ci, k);
      cJSON *ty = cJSON_GetObjectItem(it, "type");
      if (ty && strcmp(ty->valuestring, "function_call_output") == 0)
      {
         assert(strcmp(cJSON_GetObjectItem(it, "call_id")->valuestring, "toolu_1") == 0);
         found_fco = 1;
      }
   }
   assert(found_fco);
   cJSON_Delete(cb);
   aimee_request_free(&conv);

   printf("ok\n");
   return 0;
}
