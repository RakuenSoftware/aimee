#include "db2.h"
#include "db2/db2_tenant.h"
#include "db2/org_model_catalog.h"
#include "kb/kb_bedrock_egress.h"
#include "kb_identity.h"
#include "kb_verifier.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int consume_delta(const aimee_delta_t *delta, void *context)
{
   (void)delta;
   size_t *count = context;
   (*count)++;
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
      fprintf(stderr, "usage: %s model [buffered|stream]\n", argv[0]);
      return 2;
   }
   int streaming = argc == 3 && strcmp(argv[2], "stream") == 0;
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
   db2_bedrock_target_t target;
   db2_bedrock_target_result_t resolved =
       db2_model_bedrock_target_resolve(team, argv[1], &target);
   if (db2_tenant_scope_commit() != 0 || resolved != DB2_BEDROCK_TARGET_OK)
   {
      fprintf(stderr, "kb_bedrock_live: target unavailable (%d)\n", resolved);
      db2_shutdown();
      return 1;
   }

   aimee_block_t block = {.type = AIMEE_BLK_TEXT, .text = "reply with ok"};
   aimee_message_t message = {.role = "user", .blocks = &block, .n_blocks = 1};
   aimee_request_t request = {.model = argv[1], .messages = &message, .n_messages = 1};
   const char *wrong = getenv("AIMEE_TEST_WRONG_SECRET");
   kb_bedrock_credentials_t credentials = {.access_key_id = "AKIDEXAMPLE",
                                            .secret_access_key =
                                                wrong && strcmp(wrong, "1") == 0 ? "wrong"
                                                                                 : "secret",
                                            .session_token = "token",
                                            .amz_date = "20260101T000000Z",
                                            .date = "20260101"};
   int status = 0;
   kb_bedrock_result_t result;
   if (streaming)
   {
      size_t deltas = 0;
      result = kb_bedrock_dispatch_stream(&target, &request, &credentials, consume_delta, &deltas,
                                          &status);
      if (result == KB_BEDROCK_OK && !deltas)
         result = KB_BEDROCK_INCOMPLETE_STREAM;
   }
   else
   {
      aimee_response_t response;
      kb_bedrock_response_init(&response);
      result = kb_bedrock_dispatch_buffered(&target, &request, &credentials, &response, &status);
      aimee_response_free(&response);
   }
   if (result != KB_BEDROCK_OK || status != 200)
   {
      fprintf(stderr, "kb_bedrock_live: dispatch failed rc=%d status=%d\n", result, status);
      db2_shutdown();
      return 1;
   }
   db2_shutdown();
   puts("kb_bedrock_live: ok");
   return 0;
}
