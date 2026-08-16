#include "modules/db2/c/db2.h"
#include "modules/db2/c/db2_tenant.h"
#include "modules/db2/c/org_model_catalog.h"
#include "kb/kb_bedrock_egress.h"
#include "kb_identity.h"
#include "kb_verifier.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct
{
   size_t callbacks;
   size_t terminal;
   int abort_nonterminal;
} stream_probe_t;

static int consume_delta(const aimee_delta_t *delta, void *context)
{
   stream_probe_t *probe = context;
   probe->callbacks++;
   if (delta->type == AIMEE_DELTA_TURN_STOP)
      probe->terminal++;
   else if (probe->abort_nonterminal)
      return 1;
   return 0;
}

int main(int argc, char **argv)
{
   const char *url = getenv("AIMEE_TEST_PG_URL");
   const char *team_text = getenv("AIMEE_TEST_TEAM_ID");
   if (!url || !*url)
   {
      puts("kb_bedrock_live: SKIP (AIMEE_TEST_PG_URL unset)");
      return 0;
   }
   if (argc < 2 || argc > 3)
   {
      fprintf(stderr, "usage: %s model [buffered|stream|stream-abort]\n", argv[0]);
      return 2;
   }
   int stream_abort = argc == 3 && strcmp(argv[2], "stream-abort") == 0;
   int streaming = argc == 3 && (strcmp(argv[2], "stream") == 0 || stream_abort);
   if (argc == 3 && !streaming && strcmp(argv[2], "buffered") != 0)
      return 2;
   int64_t team = team_text && *team_text ? strtoll(team_text, NULL, 10) : 960001;
   if (team <= 0 || db2_init(url) != 0)
      return 2;

   kb_verify_result_t verified;
   memset(&verified, 0, sizeof(verified));
   snprintf(verified.subject, sizeof(verified.subject), "p6c_member_a");
   kb_principal_t actor;
   if (kb_principal_from_verify(&verified, "test", &actor) != 0 ||
       db2_tenant_scope_begin(&actor, team) != 0)
   {
      db2_shutdown();
      return 2;
   }
   kb_bedrock_authorized_target_t *target = NULL;
   kb_bedrock_result_t resolved = kb_bedrock_authorized_target_resolve(team, argv[1], &target);
   if (db2_tenant_scope_commit() != 0 || resolved != KB_BEDROCK_OK)
   {
      fprintf(stderr, "kb_bedrock_live: target unavailable (%d)\n", resolved);
      kb_bedrock_authorized_target_clear(&target);
      db2_shutdown();
      return 1;
   }

   aimee_block_t block = {.type = AIMEE_BLK_TEXT, .text = "reply with ok"};
   aimee_message_t message = {.role = "user", .blocks = &block, .n_blocks = 1};
   aimee_request_t request = {.model = argv[1], .messages = &message, .n_messages = 1};
   const char *wrong = getenv("AIMEE_TEST_WRONG_SECRET");
   kb_bedrock_credentials_t credentials = {
       .access_key_id = "AKIDEXAMPLE",
       .secret_access_key = wrong && strcmp(wrong, "1") == 0 ? "wrong" : "secret",
       .session_token = "token",
       .amz_date = "20260101T000000Z",
       .date = "20260101"};
   int status = 0;
   kb_bedrock_result_t result;
   if (streaming)
   {
      stream_probe_t probe = {.abort_nonterminal = stream_abort};
      result = kb_bedrock_dispatch_stream(target, &request, &credentials, consume_delta, &probe,
                                          &status);
      if (stream_abort)
      {
         if (result != KB_BEDROCK_CALLBACK_ABORT || status != 0 || probe.callbacks != 1 ||
             probe.terminal != 0)
         {
            fprintf(stderr,
                    "kb_bedrock_live: callback abort mismatch rc=%d status=%d callbacks=%zu "
                    "terminal=%zu\n",
                    result, status, probe.callbacks, probe.terminal);
            kb_bedrock_authorized_target_clear(&target);
            db2_shutdown();
            return 1;
         }
         kb_bedrock_authorized_target_clear(&target);
         db2_shutdown();
         puts("kb_bedrock_live: ok (callback abort)");
         return 0;
      }
      if (result == KB_BEDROCK_OK && !probe.callbacks)
         result = KB_BEDROCK_INCOMPLETE_STREAM;
   }
   else
   {
      aimee_response_t response;
      kb_bedrock_response_init(&response);
      result = kb_bedrock_dispatch_buffered(target, &request, &credentials, &response, &status);
      aimee_response_free(&response);
   }
   if (result != KB_BEDROCK_OK || status != 200)
   {
      fprintf(stderr, "kb_bedrock_live: dispatch failed rc=%d status=%d\n", result, status);
      kb_bedrock_authorized_target_clear(&target);
      db2_shutdown();
      return 1;
   }
   kb_bedrock_authorized_target_clear(&target);
   db2_shutdown();
   puts("kb_bedrock_live: ok");
   return 0;
}
