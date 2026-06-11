/* test_session_credentials.c: the in-RAM, session-scoped credential keyring. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "session_credentials.h"

int main(void)
{
   char out[8192];
   char acct[128];

   /* set / get, scoped by session. */
   session_creds_set("sess-A", "minimax", "key-aaa");
   session_creds_set("sess-A", "mistral", "key-bbb");
   session_creds_set("sess-B", "minimax", "key-ccc");
   assert(session_creds_get("sess-A", "minimax", out, sizeof(out)) && strcmp(out, "key-aaa") == 0);
   assert(session_creds_get("sess-A", "mistral", out, sizeof(out)) && strcmp(out, "key-bbb") == 0);
   assert(session_creds_get("sess-B", "minimax", out, sizeof(out)) && strcmp(out, "key-ccc") == 0);
   /* Cross-session / unknown agent miss. */
   assert(session_creds_get("sess-A", "mimo", out, sizeof(out)) == 0);
   assert(session_creds_get("sess-Z", "minimax", out, sizeof(out)) == 0);

   /* Overwrite. */
   session_creds_set("sess-A", "minimax", "key-new");
   assert(session_creds_get("sess-A", "minimax", out, sizeof(out)) && strcmp(out, "key-new") == 0);

   /* Empty/NULL key removes. */
   session_creds_set("sess-A", "mistral", "");
   assert(session_creds_get("sess-A", "mistral", out, sizeof(out)) == 0);
   /* minimax still present after removing mistral. */
   assert(session_creds_get("sess-A", "minimax", out, sizeof(out)) && strcmp(out, "key-new") == 0);

   /* Codex creds. */
   session_creds_set_codex("sess-A", "tok-123", "acct-9");
   assert(session_creds_get_codex("sess-A", out, sizeof(out), acct, sizeof(acct)));
   assert(strcmp(out, "tok-123") == 0 && strcmp(acct, "acct-9") == 0);
   assert(session_creds_get_codex("sess-Z", out, sizeof(out), acct, sizeof(acct)) == 0);

   /* ingest_json. */
   int n = session_creds_ingest_json(
       "sess-C", "{\"agents\":{\"gemini\":\"gk\",\"openai\":\"ok\"},\"codex_oauth_token\":\"ct\","
                 "\"codex_account_id\":\"ca\"}");
   assert(n == 3);
   assert(session_creds_get("sess-C", "gemini", out, sizeof(out)) && strcmp(out, "gk") == 0);
   assert(session_creds_get("sess-C", "openai", out, sizeof(out)) && strcmp(out, "ok") == 0);
   assert(session_creds_get_codex("sess-C", out, sizeof(out), acct, sizeof(acct)) &&
          strcmp(out, "ct") == 0 && strcmp(acct, "ca") == 0);
   assert(session_creds_ingest_json("sess-C", "not json") == -1);

   /* clear drops everything for the session. */
   session_creds_clear("sess-A");
   assert(session_creds_get("sess-A", "minimax", out, sizeof(out)) == 0);
   assert(session_creds_get_codex("sess-A", out, sizeof(out), acct, sizeof(acct)) == 0);
   /* Other sessions unaffected. */
   assert(session_creds_get("sess-B", "minimax", out, sizeof(out)) && strcmp(out, "key-ccc") == 0);

   printf("session_credentials: all tests passed\n");
   return 0;
}
