/* test_pii_gate.c: typed-fact §7 per-attribute PII recall gating. Pure. P5. */
#include "modules/memory/memory_pii_gate.h"
#include "../headers/rel_types.h"
#include <aimee/memory/module_api.h>
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

/* --- the module seam ------------------------------------------------------
 * With a classifier registered the turn verdict comes from the memory module,
 * not the local cue scan. A recorder stands in for that module, so the seam is
 * tested without a running bus. */

static struct
{
   int calls;
   int fail;
   int answer;
   char text[256];
} g_turn_state;

static int recording_turn_classifier(const char *turn_text, int *requests_sensitive)
{
   g_turn_state.calls++;
   snprintf(g_turn_state.text, sizeof(g_turn_state.text), "%s", turn_text ? turn_text : "");
   if (g_turn_state.fail)
      return -1;
   *requests_sensitive = g_turn_state.answer;
   return 0;
}

static void test_registered_turn_classifier_decides(void)
{
   memset(&g_turn_state, 0, sizeof(g_turn_state));
   memory_pii_register_turn_classifier(recording_turn_classifier);

   /* A turn the local scan reads as NOT asking; the module says it does. */
   g_turn_state.answer = 1;
   assert(memory_pii_turn_requests_sensitive("tell me about the weather") == 1);
   assert(g_turn_state.calls == 1);
   assert(strcmp(g_turn_state.text, "tell me about the weather") == 0);

   /* And the converse: a turn the local scan would flag, which the module does
    * not. Both directions matter -- a seam tested in one direction only cannot
    * tell "the module decided" from "the local scan happened to agree". */
   g_turn_state.answer = 0;
   assert(memory_pii_turn_requests_sensitive("what is my password") == 0);
   assert(g_turn_state.calls == 2);

   /* Empty and NULL turns are answered without troubling the module. */
   assert(memory_pii_turn_requests_sensitive("") == 0);
   assert(memory_pii_turn_requests_sensitive(NULL) == 0);
   assert(g_turn_state.calls == 2);

   /* Unregistering restores the local scan. */
   memory_pii_register_turn_classifier(NULL);
   assert(memory_pii_turn_requests_sensitive("what is my password") == 1);
   assert(g_turn_state.calls == 2);
   printf("  PASS: test_registered_turn_classifier_decides\n");
}

static void test_turn_classifier_failure_fails_closed(void)
{
   memset(&g_turn_state, 0, sizeof(g_turn_state));
   g_turn_state.fail = 1;
   memory_pii_register_turn_classifier(recording_turn_classifier);

   /* A turn the local scan would flag as asking. A failure must NOT fall back to
    * that scan (which would hide a broken module) and must NOT assume the turn
    * asked: 0 withholds PII, which is the safe side of this gate. */
   assert(memory_pii_turn_requests_sensitive("what is my password") == 0);
   assert(g_turn_state.calls == 1);

   memory_pii_register_turn_classifier(NULL);
   assert(memory_pii_turn_requests_sensitive("what is my password") == 1);
   printf("  PASS: test_turn_classifier_failure_fails_closed\n");
}

/* The Go stage encodes and decodes these offsets independently; pinning them
 * here is what makes the two halves one contract rather than two guesses. */
static void test_pii_wire_layout(void)
{
   const char *turn = "what is my email";
   uint8_t request[AIMEE_MEMORY_PII_REQUEST_HEADER_LEN + 32];
   assert(aimee_memory_pii_request_size(turn) ==
          AIMEE_MEMORY_PII_REQUEST_HEADER_LEN + strlen(turn));
   assert(aimee_memory_pii_request_encode(turn, request, sizeof(request)) == 0);
   assert(aimee_memory_get_u32(request) == AIMEE_MEMORY_PII_REQUEST_MAGIC);
   assert(aimee_memory_get_u32(request + 4) == AIMEE_MEMORY_WIRE_VERSION);
   assert(aimee_memory_get_u32(request + 8) == (uint32_t)strlen(turn));
   assert(memcmp(request + 12, turn, strlen(turn)) == 0);
   assert(aimee_memory_pii_request_encode(turn, request, 4) == -1);

   uint8_t response[AIMEE_MEMORY_PII_RESPONSE_LEN];
   int answer = -1;
   aimee_memory_put_u32(response, AIMEE_MEMORY_PII_RESPONSE_MAGIC);
   aimee_memory_put_u32(response + 4, 1);
   assert(aimee_memory_pii_response_decode(response, sizeof(response), &answer) == 0);
   assert(answer == 1);
   aimee_memory_put_u32(response + 4, 0);
   assert(aimee_memory_pii_response_decode(response, sizeof(response), &answer) == 0);
   assert(answer == 0);

   /* Anything other than 0 or 1 is a broken module. This decides whether private
    * facts are eligible for a prompt, so a truthy-looking 2 is refused rather
    * than read as "yes, the user asked". */
   aimee_memory_put_u32(response + 4, 2);
   assert(aimee_memory_pii_response_decode(response, sizeof(response), &answer) == -1);
   aimee_memory_put_u32(response + 4, 1);
   aimee_memory_put_u32(response, AIMEE_MEMORY_PII_RESPONSE_MAGIC + 1);
   assert(aimee_memory_pii_response_decode(response, sizeof(response), &answer) == -1);
   aimee_memory_put_u32(response, AIMEE_MEMORY_PII_RESPONSE_MAGIC);
   assert(aimee_memory_pii_response_decode(response, sizeof(response) - 1, &answer) == -1);
   printf("  PASS: test_pii_wire_layout\n");
}

int main(void)
{
   /* turn-requests-sensitive detection. */
   assert(memory_pii_turn_requests_sensitive("what is my address again?") == 1);
   assert(memory_pii_turn_requests_sensitive("remind me of my password") == 1);
   assert(memory_pii_turn_requests_sensitive("what's my email?") == 1);
   assert(memory_pii_turn_requests_sensitive("when is my birthday") == 1);
   assert(memory_pii_turn_requests_sensitive("how are you today?") == 0);
   assert(memory_pii_turn_requests_sensitive("what do i do for work") == 0);
   assert(memory_pii_turn_requests_sensitive("") == 0);
   assert(memory_pii_turn_requests_sensitive(NULL) == 0);

   /* rel_type -> sensitivity. Known: seed lookup. Unknown: default OPEN
    * (SENS_NORMAL) so free-form extracted relations are not all withheld, except
    * names that plainly denote a credential or a regulated PII identifier. */
   assert(memory_pii_rel_sensitivity("works_for") == SENS_NORMAL);
   assert(memory_pii_rel_sensitivity("also_known_as") == SENS_NORMAL);
   assert(memory_pii_rel_sensitivity("age") == SENS_PII);
   assert(memory_pii_rel_sensitivity("totally_unknown_rel") == SENS_NORMAL); /* unknown -> open */
   assert(memory_pii_rel_sensitivity("favorite_food") == SENS_NORMAL);
   assert(memory_pii_rel_sensitivity("") == SENS_NORMAL);
   assert(memory_pii_rel_sensitivity(NULL) == SENS_NORMAL);
   /* unknown but obviously sensitive by name: still gated by the heuristic. */
   assert(memory_pii_rel_sensitivity("home_password") == SENS_SECRET);
   assert(memory_pii_rel_sensitivity("api_key") == SENS_SECRET);
   assert(memory_pii_rel_sensitivity("ssn") == SENS_PII);
   assert(memory_pii_rel_sensitivity("home_address") == SENS_PII);

   /* injection decision. */
   /* NORMAL: passes above the floor regardless of request; withheld below it. */
   assert(memory_pii_should_inject(SENS_NORMAL, 0.5, 0) == 1);
   assert(memory_pii_should_inject(SENS_NORMAL, 0.4, 0) == 1); /* at the floor */
   assert(memory_pii_should_inject(SENS_NORMAL, 0.3, 1) == 0); /* below floor, even if asked */
   /* PII: only when the turn explicitly asks (and above the floor). */
   assert(memory_pii_should_inject(SENS_PII, 0.9, 0) == 0);
   assert(memory_pii_should_inject(SENS_PII, 0.9, 1) == 1);
   assert(memory_pii_should_inject(SENS_PII, 0.3, 1) == 0); /* below floor */
   /* SECRET: never injected, even when asked at full confidence. */
   assert(memory_pii_should_inject(SENS_SECRET, 1.0, 1) == 0);
   /* non-finite confidence fails closed (must not slip past the floor check). */
   assert(memory_pii_should_inject(SENS_NORMAL, NAN, 1) == 0);

   test_registered_turn_classifier_decides();
   test_turn_classifier_failure_fails_closed();
   test_pii_wire_layout();

   printf("pii_gate: all tests passed\n");
   return 0;
}
