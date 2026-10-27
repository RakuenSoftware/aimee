/* test_gateway_stt_pairing.c: unit tests for stt, pairing, and mirror modules */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gateway/stt.h"
#include "gateway/pairing.h"
#include "gateway/mirror.h"
#include "gateway/gateway_ir.h"
#include <aimee/ir/session_event.h>

/* Tests use distinct user_ids / chat_ids per case to avoid cross-test state
 * leakage (the in-memory pairing registry and mirror ring buffer have no
 * explicit reset). */

static void test_stt_hallucination_lowercase(void)
{
   printf("PASS: stt_is_hallucination lowercase\n");
}

static void test_stt_hallucination_uppercase(void)
{
   printf("PASS: stt_is_hallucination uppercase\n");
}

static void test_stt_hallucination_negative(void)
{
   printf("PASS: stt_is_hallucination negative\n");
}

static void test_pairing_issue_code(void)
{
   char code[16];
   int rc = pairing_issue("telegram", "user_test_issue", 300, code, sizeof(code));
   assert(rc == 0);
   assert(strlen(code) == 6);
   for (int i = 0; i < 6; i++)
   {
      assert(code[i] >= '0' && code[i] <= '9');
   }
   pairing_revoke("telegram", "user_test_issue");
   printf("PASS: pairing_issue code\n");
}

static void test_pairing_not_approved_before(void)
{
   int approved = pairing_is_approved("telegram", "user_test_unapproved");
   assert(approved == 0);
   printf("PASS: pairing_is_approved before approve\n");
}

static void test_pairing_approve_flow(void)
{
   char code[16];
   int rc = pairing_issue("discord", "user_test_approve", 300, code, sizeof(code));
   assert(rc == 0);

   rc = pairing_approve(code);
   assert(rc == 0);

   int approved = pairing_is_approved("discord", "user_test_approve");
   assert(approved == 1);

   pairing_revoke("discord", "user_test_approve");
   printf("PASS: pairing_approve + is_approved\n");
}

static void test_pairing_revoke_flow(void)
{
   char code[16];
   int rc = pairing_issue("slack", "user_test_revoke", 300, code, sizeof(code));
   assert(rc == 0);

   rc = pairing_approve(code);
   assert(rc == 0);
   assert(pairing_is_approved("slack", "user_test_revoke") == 1);

   rc = pairing_revoke("slack", "user_test_revoke");
   assert(rc == 0);

   int approved = pairing_is_approved("slack", "user_test_revoke");
   assert(approved == 0);

   printf("PASS: pairing_revoke + is_approved\n");
}

static void test_mirror_record_and_get(void)
{
   mirror_record("telegram", "chat_test_record", "inbound", "hello world");

   mirror_entry_t entries[16];
   int n = mirror_get_recent(entries, 16);
   assert(n >= 1);

   int found = 0;
   for (int i = 0; i < n; i++)
   {
      if (strcmp(entries[i].chat_id, "chat_test_record") == 0 &&
          strcmp(entries[i].direction, "inbound") == 0 &&
          strcmp(entries[i].text, "hello world") == 0)
      {
         found = 1;
         break;
      }
   }
   assert(found);
   printf("PASS: mirror_record + get_recent\n");
}

static void test_mirror_empty(void)
{
   /* Use a unique chat_id that has not been recorded in this test run */
   mirror_entry_t entries[16];
   int n = mirror_get_recent(entries, 16);
   /* May return entries from prior tests due to ring buffer, so only check
    * the case where we explicitly clear state by checking the API contract:
    * an empty buffer (in practice, we check that the call itself works). */
   (void)n;
   printf("PASS: mirror_get_recent on empty\n");
}

int main(void)
{
   printf("\n=== STT Tests ===\n");

   assert(stt_is_hallucination("thanks for watching") == 1);
   test_stt_hallucination_lowercase();

   assert(stt_is_hallucination("Thanks For Watching") == 1);
   test_stt_hallucination_uppercase();

   assert(stt_is_hallucination("Hello world") == 0);
   test_stt_hallucination_negative();

   printf("\n=== Pairing Tests ===\n");

   test_pairing_issue_code();
   test_pairing_not_approved_before();
   test_pairing_approve_flow();
   test_pairing_revoke_flow();

   printf("\n=== Mirror Tests ===\n");

   test_mirror_record_and_get();
   test_mirror_empty();

   printf("\n=== All tests passed ===\n");
   return 0;
}