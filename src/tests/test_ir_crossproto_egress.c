/* test_ir_crossproto_egress.c -- the IR-funnel HARD requirement: outbound requests
 * to the Anthropic Messages API must be BYTE-IDENTICAL for the same logical
 * conversation regardless of the client's source protocol, because Anthropic
 * prompt-caches on exact bytes. So anthropic->IR->anthropic and openai->IR->anthropic
 * must serialize identically. This is achieved by making the Anthropic egress a pure,
 * deterministic function of the typed IR (no raw sidecar) with a uniform aimee
 * cache_control policy applied at egress (client markers normalized away). */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "aimee_ir.h"
#include "aimee_frontend.h"
#include "aimee_backend.h"
#include "cJSON.h"

/* Build the Anthropic egress bytes for a request expressed in `wire`. */
static char *egress(const char *body, int frontend /* 0=anthropic,1=openai */)
{
   cJSON *req = cJSON_Parse(body);
   assert(req);
   aimee_request_t ir;
   char err[128];
   int rc = (frontend == 0) ? anthropic_frontend_parse(req, &ir, err, sizeof err)
                            : openai_frontend_parse(req, &ir, err, sizeof err);
   assert(rc == 0);
   cJSON *out = anthropic_backend_build(&ir);
   assert(out);
   char *s = cJSON_PrintUnformatted(out);
   cJSON_Delete(out);
   aimee_request_free(&ir);
   cJSON_Delete(req);
   return s;
}

static void same(const char *label, const char *anth_wire, const char *oai_wire)
{
   char *a = egress(anth_wire, 0);
   char *o = egress(oai_wire, 1);
   if (!a || !o || strcmp(a, o) != 0)
   {
      printf("  CROSS-PROTO DRIFT [%s]\n    anthropic-src: %s\n    openai-src   : %s\n", label,
             a ? a : "(null)", o ? o : "(null)");
      free(a);
      free(o);
      exit(1);
   }
   printf("  %s: byte-identical OK\n", label);
   free(a);
   free(o);
}

int main(void)
{
   printf("cross-protocol anthropic egress byte-identity:\n");

   /* Text-only turn: same served model, system, and user content in each wire.
    * The anthropic client sends its own cache_control; the openai client sends none.
    * A uniform egress cache policy must normalize both to identical bytes. */
   same("text",
        "{\"model\":\"claude-3-5-sonnet\",\"max_tokens\":100,"
        "\"system\":[{\"type\":\"text\",\"text\":\"You are helpful.\","
        "\"cache_control\":{\"type\":\"ephemeral\"}}],"
        "\"messages\":[{\"role\":\"user\",\"content\":[{\"type\":\"text\",\"text\":\"hi\"}]}]}",
        "{\"model\":\"claude-3-5-sonnet\",\"max_tokens\":100,"
        "\"messages\":[{\"role\":\"system\",\"content\":\"You are helpful.\"},"
        "{\"role\":\"user\",\"content\":\"hi\"}]}");

   /* Tool-bearing multi-turn: system + user + assistant tool_use + user tool_result,
    * plus a tool definition. Exercises tool_input, tool_result shape, and tool schema
    * canonicalization across the frontends. */
   same(
       "tools",
       "{\"model\":\"claude-3-5-sonnet\",\"max_tokens\":100,"
       "\"system\":[{\"type\":\"text\",\"text\":\"sys\"}],"
       "\"messages\":[{\"role\":\"user\",\"content\":[{\"type\":\"text\",\"text\":\"read foo\"}]},"
       "{\"role\":\"assistant\",\"content\":[{\"type\":\"tool_use\",\"id\":\"t1\",\"name\":"
       "\"Read\","
       "\"input\":{\"path\":\"foo\"}}]},"
       "{\"role\":\"user\",\"content\":[{\"type\":\"tool_result\",\"tool_use_id\":\"t1\","
       "\"content\":\"file body\"}]}],"
       "\"tools\":[{\"name\":\"Read\",\"description\":\"Read a file\",\"input_schema\":"
       "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"}}}}]}",
       "{\"model\":\"claude-3-5-sonnet\",\"max_tokens\":100,"
       "\"messages\":[{\"role\":\"system\",\"content\":\"sys\"},"
       "{\"role\":\"user\",\"content\":\"read foo\"},"
       "{\"role\":\"assistant\",\"content\":null,\"tool_calls\":[{\"id\":\"t1\","
       "\"type\":\"function\",\"function\":{\"name\":\"Read\",\"arguments\":\"{\\\"path\\\":"
       "\\\"foo\\\"}\"}}]},"
       "{\"role\":\"tool\",\"tool_call_id\":\"t1\",\"content\":\"file body\"}],"
       "\"tools\":[{\"type\":\"function\",\"function\":{\"name\":\"Read\",\"description\":\"Read a "
       "file\",\"parameters\":{\"type\":\"object\",\"properties\":{\"path\":{\"type\":"
       "\"string\"}}}}}]}");

   /* Inline base64 image: Anthropic sends a structured base64 source; OpenAI sends a
    * data: URL. The openai frontend must decompose the data: URL to match. */
   same("image-b64",
        "{\"model\":\"claude-3-5-sonnet\",\"max_tokens\":8,"
        "\"messages\":[{\"role\":\"user\",\"content\":[{\"type\":\"image\",\"source\":"
        "{\"type\":\"base64\",\"media_type\":\"image/png\",\"data\":\"AAAA\"}}]}]}",
        "{\"model\":\"claude-3-5-sonnet\",\"max_tokens\":8,"
        "\"messages\":[{\"role\":\"user\",\"content\":[{\"type\":\"image_url\",\"image_url\":"
        "{\"url\":\"data:image/png;base64,AAAA\"}}]}]}");

   /* tool_result as an Anthropic block-array vs an OpenAI string: the egress collapses
    * a single text block to the string form so both match. */
   same("tool-result-array",
        "{\"model\":\"claude-3-5-sonnet\",\"max_tokens\":8,"
        "\"messages\":[{\"role\":\"user\",\"content\":[{\"type\":\"tool_result\","
        "\"tool_use_id\":\"t1\",\"content\":[{\"type\":\"text\",\"text\":\"body\"}]}]}]}",
        "{\"model\":\"claude-3-5-sonnet\",\"max_tokens\":8,"
        "\"messages\":[{\"role\":\"tool\",\"tool_call_id\":\"t1\",\"content\":\"body\"}]}");

   /* Mixed multi-part user content: text + inline base64 image in one message. */
   same("mixed-content",
        "{\"model\":\"claude-3-5-sonnet\",\"max_tokens\":8,"
        "\"messages\":[{\"role\":\"user\",\"content\":[{\"type\":\"text\",\"text\":\"see this\"},"
        "{\"type\":\"image\",\"source\":{\"type\":\"base64\",\"media_type\":\"image/png\","
        "\"data\":\"AAAA\"}}]}]}",
        "{\"model\":\"claude-3-5-sonnet\",\"max_tokens\":8,"
        "\"messages\":[{\"role\":\"user\",\"content\":[{\"type\":\"text\",\"text\":\"see this\"},"
        "{\"type\":\"image_url\",\"image_url\":{\"url\":\"data:image/png;base64,AAAA\"}}]}]}");

   /* Regression: a data: URL where ";base64," appears only AFTER the first comma (i.e.
    * inside the payload, not as the RFC-2397 header marker) must NOT be decomposed --
    * it is kept as a verbatim url reference, not misparsed into a base64 source. */
   {
      char *e = egress("{\"model\":\"m\",\"max_tokens\":8,\"messages\":[{\"role\":\"user\","
                       "\"content\":[{\"type\":\"image_url\",\"image_url\":"
                       "{\"url\":\"data:image/png,payload;base64,tail\"}}]}]}",
                       1);
      assert(e);
      assert(strstr(e, "\"type\":\"url\"") != NULL);                   /* kept as url ref */
      assert(strstr(e, "data:image/png,payload;base64,tail") != NULL); /* verbatim */
      assert(strstr(e, "\"type\":\"base64\"") == NULL);                /* NOT decomposed */
      printf("  data-url-earlier-comma-passthrough OK\n");
      free(e);
   }

   /* Thinking block signature: Anthropic requires the opaque signature echoed back
    * verbatim on a resubmitted assistant thinking turn. With the raw sidecar retired,
    * the canonical rebuild must preserve it (it is modeled, not carried via raw). */
   {
      char *e = egress("{\"model\":\"m\",\"max_tokens\":8,\"messages\":[{\"role\":\"assistant\","
                       "\"content\":[{\"type\":\"thinking\",\"thinking\":\"reasoning\","
                       "\"signature\":\"sig123\"}]}]}",
                       0);
      assert(e);
      assert(strstr(e, "\"signature\":\"sig123\"") != NULL); /* preserved through rebuild */
      printf("  thinking-signature-preserved OK\n");
      free(e);
   }

   /* redacted_thinking blocks (opaque base64 data) have no typed IR arm, so they are
    * preserved verbatim via the UNKNOWN/raw replay path -- confirm the canonical
    * rebuild does not drop them (required to resubmit an extended-thinking turn). */
   {
      char *e = egress("{\"model\":\"m\",\"max_tokens\":8,\"messages\":[{\"role\":\"assistant\","
                       "\"content\":[{\"type\":\"redacted_thinking\",\"data\":\"Er0Bopaque\"}]}]}",
                       0);
      assert(e);
      assert(strstr(e, "\"redacted_thinking\"") != NULL);
      assert(strstr(e, "Er0Bopaque") != NULL); /* opaque data preserved verbatim */
      printf("  redacted-thinking-preserved OK\n");
      free(e);
   }

   printf("all cross-protocol egress byte-identity checks passed\n");
   return 0;
}
