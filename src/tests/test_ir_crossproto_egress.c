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

   printf("all cross-protocol egress byte-identity checks passed\n");
   return 0;
}
