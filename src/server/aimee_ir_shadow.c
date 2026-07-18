/* aimee_ir_shadow.c -- see aimee_ir_shadow.h. */
#include "aimee.h" /* MAX_PATH_LEN et al: agent_types.h depends on it */

#include "aimee_ir_shadow.h"

#include "agent_protocol.h" /* parsed_response_t (response comparison) */
#include "aimee_backend.h"
#include "aimee_frontend.h"
#include "aimee_ir_metrics.h"
#include "cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Cap the mismatch/failure log so a systematic bug can't flood server.log. */
static int g_logged;
#define SHADOW_LOG_CAP 20

static int shadow_enabled(void)
{
   const char *v = getenv("AIMEE_IR_SHADOW");
   return v && v[0] && v[0] != '0';
}

int aimee_ir_shadow_enabled(void)
{
   return shadow_enabled();
}

void aimee_ir_shadow_compare_bodies(const char *ir_body, const char *legacy_body,
                                    aimee_wire_t frontend)
{
   if (!shadow_enabled())
      return;
   /* Compare the two provider bodies SEMANTICALLY (parsed JSON), not byte-for-byte.
    * The IR and legacy legitimately serialize the same request with keys in a
    * different order ({"model","max_tokens","messages"} vs
    * {"model","messages","max_tokens"}); a strcmp flags that as a mismatch even
    * though the provider sees identical requests. cJSON_Compare ignores key order
    * but still catches a genuine divergence -- a missing/extra field or a different
    * value (e.g. legacy injecting a temperature default the IR omitted). A NULL on
    * either side is a divergence, not a skip: "the IR could not build it" is exactly
    * the case that forces a legacy fallback in production. */
   cJSON *ir_json = ir_body ? cJSON_Parse(ir_body) : NULL;
   cJSON *legacy_json = legacy_body ? cJSON_Parse(legacy_body) : NULL;
   int equal = ir_json && legacy_json && cJSON_Compare(ir_json, legacy_json, 1);
   cJSON_Delete(ir_json);
   cJSON_Delete(legacy_json);
   if (equal)
   {
      aimee_ir_metric_inc(AIMEE_IR_M_BODY_MATCH, frontend);
      return;
   }
   aimee_ir_metric_inc(AIMEE_IR_M_BODY_MISMATCH, frontend);
   if (g_logged < SHADOW_LOG_CAP)
   {
      g_logged++;
      /* Truncated on purpose: bodies carry user content, and the roundtable ruling
       * on this refactor was explicit that no raw request bytes may reach the log.
       * The lengths + a short prefix are enough to identify WHICH field diverged;
       * reproduce the full diff offline from the shape, not from production logs. */
      fprintf(stderr,
              "aimee_ir_shadow: provider-body mismatch (wire=%d) ir_len=%zu legacy_len=%zu\n"
              "  ir[0:80]     =%.80s\n"
              "  legacy[0:80] =%.80s\n",
              (int)frontend, ir_body ? strlen(ir_body) : 0, legacy_body ? strlen(legacy_body) : 0,
              ir_body ? ir_body : "(null)", legacy_body ? legacy_body : "(null)");
   }
}

/* Whitespace-insensitive text equality: the two parsers may differ only in leading
 * or trailing trim, which is not a semantic divergence. Both NULL/empty == equal. */
static int text_equal_trimmed(const char *a, const char *b)
{
   if (!a)
      a = "";
   if (!b)
      b = "";
   while (*a == ' ' || *a == '\n' || *a == '\t' || *a == '\r')
      a++;
   while (*b == ' ' || *b == '\n' || *b == '\t' || *b == '\r')
      b++;
   size_t la = strlen(a), lb = strlen(b);
   while (la > 0 &&
          (a[la - 1] == ' ' || a[la - 1] == '\n' || a[la - 1] == '\t' || a[la - 1] == '\r'))
      la--;
   while (lb > 0 &&
          (b[lb - 1] == ' ' || b[lb - 1] == '\n' || b[lb - 1] == '\t' || b[lb - 1] == '\r'))
      lb--;
   return la == lb && memcmp(a, b, la) == 0;
}

/* Do the IR's TOOL_USE blocks match the legacy calls[] one-for-one: same count,
 * same names in order, same argument JSON (semantic, via cJSON_Compare)? */
static int tool_calls_equal(const parsed_response_t *legacy, const aimee_response_t *ir)
{
   int ir_n = 0;
   for (int i = 0; i < ir->n_content; i++)
      if (ir->content[i].type == AIMEE_BLK_TOOL_USE)
         ir_n++;
   if (ir_n != legacy->call_count)
      return 0;

   int k = 0;
   for (int i = 0; i < ir->n_content; i++)
   {
      const aimee_block_t *b = &ir->content[i];
      if (b->type != AIMEE_BLK_TOOL_USE)
         continue;
      const char *ir_name = b->tool_name ? b->tool_name : "";
      const char *lg_name = legacy->calls[k].name;
      if (strcmp(ir_name, lg_name) != 0)
         return 0;
      /* Arguments: compare the parsed JSON, not the string form, so key order and
       * spacing don't read as a divergence. Legacy stores args as a JSON string. */
      cJSON *lg_args = legacy->calls[k].arguments ? cJSON_Parse(legacy->calls[k].arguments) : NULL;
      /* Both sides argument-less counts as equal; otherwise a missing side diverges.
       * cJSON_Compare(x, NULL) is false, so handle the both-NULL case explicitly. */
      int args_ok = (!b->tool_input && !lg_args) ? 1 : cJSON_Compare(b->tool_input, lg_args, 1);
      cJSON_Delete(lg_args);
      if (!args_ok)
         return 0;
      k++;
   }
   return 1;
}

void aimee_ir_shadow_compare_response(const struct parsed_response *legacy, const cJSON *resp_json,
                                      aimee_wire_t wire)
{
   if (!shadow_enabled() || !legacy || !resp_json)
      return;

   aimee_response_t ir;
   memset(&ir, 0, sizeof(ir));
   char err[128] = {0};
   int rc;
   if (wire == AIMEE_WIRE_ANTHROPIC)
      rc = anthropic_backend_parse(resp_json, &ir, err, sizeof(err));
   else if (wire == AIMEE_WIRE_OPENAI_CHAT)
      rc = openai_backend_parse(resp_json, &ir, err, sizeof(err));
   else if (wire == AIMEE_WIRE_RESPONSES)
      /* Responses (codex) is SSE, not JSON, so the caller must hand us the response
       * OBJECT it extracted from the stream (the response.completed event's payload);
       * responses_backend_parse then reads its `output` items, same as legacy pass 2. */
      rc = responses_backend_parse(resp_json, &ir, err, sizeof(err));
   else
      return; /* unknown wire */

   /* A parse failure is itself a divergence: legacy produced a result, the IR could
    * not. That is exactly the case that must reach zero before we trust the IR. */
   if (rc != 0)
   {
      aimee_ir_metric_inc(AIMEE_IR_M_RESP_MISMATCH, wire);
      if (g_logged < SHADOW_LOG_CAP)
      {
         g_logged++;
         fprintf(stderr, "aimee_ir_shadow: response parse FAILED in IR (wire=%d): %s\n", (int)wire,
                 err[0] ? err : "(no detail)");
      }
      aimee_response_free(&ir);
      return;
   }

   /* Size the buffer to the full concatenated TEXT rather than a fixed 8 KB: a
    * fixed buffer truncated the IR text on long responses and every response longer
    * than the buffer was a FALSE mismatch (legacy content full-length vs IR text cut
    * at the cap). Sum the TEXT blocks and allocate exactly. */
   size_t ir_text_cap = 1;
   for (int i = 0; i < ir.n_content; i++)
      if (ir.content[i].type == AIMEE_BLK_TEXT && ir.content[i].text)
         ir_text_cap += strlen(ir.content[i].text);
   char *ir_text = malloc(ir_text_cap);
   if (!ir_text)
   {
      aimee_response_free(&ir);
      return;
   }
   aimee_ir_response_text(&ir, ir_text, ir_text_cap);
   int ir_has_tool = aimee_ir_response_has_tool_use(&ir);

   int same = (legacy->is_tool_call ? 1 : 0) == (ir_has_tool ? 1 : 0) &&
              tool_calls_equal(legacy, &ir) && text_equal_trimmed(legacy->content, ir_text);

   if (same)
      aimee_ir_metric_inc(AIMEE_IR_M_RESP_MATCH, wire);
   else
   {
      aimee_ir_metric_inc(AIMEE_IR_M_RESP_MISMATCH, wire);
      if (g_logged < SHADOW_LOG_CAP)
      {
         g_logged++;
         /* Shape only, no content: which axis diverged and the counts, never the
          * text or arguments (same roundtable ruling as compare_bodies). */
         fprintf(stderr,
                 "aimee_ir_shadow: response mismatch (wire=%d) "
                 "legacy{tool=%d calls=%d text=%zu} ir{tool=%d calls=%d text=%zu}\n",
                 (int)wire, legacy->is_tool_call ? 1 : 0, legacy->call_count,
                 legacy->content ? strlen(legacy->content) : 0, ir_has_tool ? 1 : 0, ir.n_content,
                 strlen(ir_text));
      }
   }
   free(ir_text);
   aimee_response_free(&ir);
}

void aimee_ir_shadow_observe_request(const cJSON *req, aimee_wire_t frontend)
{
   if (!shadow_enabled() || !req)
      return;
   /* Slice 3 starts with the Anthropic frontend (Claude Code, the primary case). */
   if (frontend != AIMEE_WIRE_ANTHROPIC)
      return;

   aimee_request_t ir;
   char err[128];
   if (anthropic_frontend_parse(req, &ir, err, sizeof err) != 0)
   {
      aimee_ir_metric_inc(AIMEE_IR_M_PARSE_FAIL, frontend);
      if (g_logged < SHADOW_LOG_CAP)
      {
         fprintf(stderr, "[ir-shadow] anthropic parse failed: %s\n", err);
         g_logged++;
      }
      return;
   }
   aimee_ir_metric_inc(AIMEE_IR_M_IR_PATH, frontend);

   /* The byte-parity gate was retired with the raw sidecar (the canonical egress now
    * INTENTIONALLY re-renders every request, so byte-parity vs the client's raw bytes
    * is meaningless). But the residual risk it implicitly covered -- a top-level field
    * the IR does not model being silently DROPPED from the canonical egress -- remains.
    * So the shadow is re-purposed to FIELD-COVERAGE detection: every top-level key the
    * client sent must be one the IR models. A request using only modeled keys scores
    * REBUILD_MATCH; one carrying an unmodeled top-level key scores REBUILD_MISMATCH and
    * logs the key NAME (never its value -- no request content in logs), flagging a
    * modeling gap to close. Cheap: a key-name scan, no rebuild. */
   static const char *const MODELED[] = {
       "model",        "max_tokens",  "messages", "system",         "tools",
       "tool_choice",  "temperature", "top_p",    "top_k",          "metadata",
       "service_tier", "thinking",    "stream",   "stop_sequences", NULL};
   const char *unmodeled = NULL;
   for (const cJSON *k = req->child; k; k = k->next)
   {
      if (!k->string)
         continue;
      int modeled = 0;
      for (const char *const *m = MODELED; *m; m++)
         if (strcmp(k->string, *m) == 0)
         {
            modeled = 1;
            break;
         }
      if (!modeled)
      {
         unmodeled = k->string;
         break;
      }
   }
   if (!unmodeled)
   {
      aimee_ir_metric_inc(AIMEE_IR_M_REBUILD_MATCH, frontend);
   }
   else
   {
      aimee_ir_metric_inc(AIMEE_IR_M_REBUILD_MISMATCH, frontend);
      if (g_logged < SHADOW_LOG_CAP)
      {
         g_logged++;
         fprintf(stderr, "[ir-shadow] anthropic canonical egress drops unmodeled top-level '%s'\n",
                 unmodeled);
      }
   }
   aimee_request_free(&ir);
}
