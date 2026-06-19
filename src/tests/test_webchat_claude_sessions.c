/* test_webchat_claude_sessions.c: the per-(principal, tab) Claude --resume
 * binding that stops one webchat tab from resuming — and thereby merging into —
 * another tab's Claude conversation. */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "db1.h"

int main(void)
{
   assert(db1_init(":memory:") == 0);

   char out[128];

   /* Unknown tab -> no binding. */
   assert(db1_webchat_claude_session_get("webuser:alice", "tabA", out, sizeof(out)) == -1);
   assert(out[0] == '\0');

   /* Bind tabA -> csidA, then read it back. */
   assert(db1_webchat_claude_session_bind("webuser:alice", "tabA", "csidA") == 0);
   assert(db1_webchat_claude_session_get("webuser:alice", "tabA", out, sizeof(out)) == 0);
   assert(strcmp(out, "csidA") == 0);

   /* csidA is owned by tabA: another tab may not adopt it, but tabA itself may. */
   assert(db1_webchat_claude_session_owned_by_other("webuser:alice", "tabB", "csidA") == 1);
   assert(db1_webchat_claude_session_owned_by_other("webuser:alice", "tabA", "csidA") == 0);

   /* A cross-wired client value (tabB claiming csidA) must NOT bind or resume. */
   assert(db1_webchat_claude_session_bind("webuser:alice", "tabB", "csidA") == -1);
   assert(db1_webchat_claude_session_get("webuser:alice", "tabB", out, sizeof(out)) == -1);

   /* tabB binds its own session normally. */
   assert(db1_webchat_claude_session_bind("webuser:alice", "tabB", "csidB") == 0);
   assert(db1_webchat_claude_session_get("webuser:alice", "tabB", out, sizeof(out)) == 0);
   assert(strcmp(out, "csidB") == 0);

   /* Re-binding the same tab updates it (Claude rotated the session id); the old
    * id is then free for adoption. */
   assert(db1_webchat_claude_session_bind("webuser:alice", "tabA", "csidA2") == 0);
   assert(db1_webchat_claude_session_get("webuser:alice", "tabA", out, sizeof(out)) == 0);
   assert(strcmp(out, "csidA2") == 0);
   assert(db1_webchat_claude_session_owned_by_other("webuser:alice", "tabB", "csidA") == 0);

   /* Principal isolation: a different principal with the SAME tab name is a
    * distinct binding, and may not adopt another principal's live session. */
   assert(db1_webchat_claude_session_bind("webuser:bob", "tabA", "csidC") == 0);
   assert(db1_webchat_claude_session_get("webuser:bob", "tabA", out, sizeof(out)) == 0);
   assert(strcmp(out, "csidC") == 0);
   assert(db1_webchat_claude_session_get("webuser:alice", "tabA", out, sizeof(out)) == 0);
   assert(strcmp(out, "csidA2") == 0);
   assert(db1_webchat_claude_session_owned_by_other("webuser:bob", "tabA", "csidB") == 1);

   /* No per-tab identity -> get refuses (caller falls back to legacy behavior). */
   assert(db1_webchat_claude_session_get("webuser:alice", "", out, sizeof(out)) == -1);

   printf("test_webchat_claude_sessions: OK\n");
   return 0;
}
