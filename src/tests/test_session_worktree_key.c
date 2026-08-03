/* test_session_worktree_key.c: the one derivation of a session's worktree and
 * branch key.
 *
 * These are regression tests for a live defect, not abstract properties. The
 * previous derivation truncated the session id to 16 characters, so ids minted
 * on a shared prefix mapped to ONE key — one worktree, one branch — and
 * concurrent writers overwrote each other. The prefix cases below are the ones
 * that actually collided. */
#include "session_worktree_key.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

/* Ids differing only past the old 16-char window. Every one of these pairs
 * collapsed onto a single worktree before the rekey. */
static void test_shared_prefix_ids_do_not_collide(void)
{
   const char *pairs[][2] = {
       /* the real shape: "aimee-task-" spends 11 of the 16 characters */
       {"aimee-task-abcdef0123456789", "aimee-task-abcdef0123456789-retry"},
       {"aimee-task-0000000000000001", "aimee-task-0000000000000002"},
       /* several models under one project, distinguished by a trailing tag */
       {"proj-alpha-run-model-a", "proj-alpha-run-model-b"},
       /* ids identical for far longer than the old window */
       {"session-2026-08-03-0900-north", "session-2026-08-03-0900-south"},
   };
   for (size_t i = 0; i < sizeof(pairs) / sizeof(pairs[0]); i++)
   {
      char a[SESSION_WORKTREE_KEY_MAX], b[SESSION_WORKTREE_KEY_MAX];
      session_worktree_key(pairs[i][0], a, sizeof a);
      session_worktree_key(pairs[i][1], b, sizeof b);
      assert(a[0] && b[0]);
      assert(strcmp(a, b) != 0);

      /* And confirm the OLD derivation really did collide on these, so this
       * test is pinning a fix rather than restating something always true. */
      char la[SESSION_WORKTREE_KEY_MAX], lb[SESSION_WORKTREE_KEY_MAX];
      session_worktree_key_legacy(pairs[i][0], la, sizeof la);
      session_worktree_key_legacy(pairs[i][1], lb, sizeof lb);
      assert(strcmp(la, lb) == 0);
   }
   printf("  shared-prefix ids no longer collide (and did before): ok\n");
}

static void test_key_is_stable(void)
{
   /* A session that resumes, compacts or reconnects must land on the SAME
    * worktree; the key cannot drift between calls or processes. */
   const char *sid = "4e2f8b9e-4d46-4744-b08b-1cdc7623f121";
   char a[SESSION_WORKTREE_KEY_MAX], b[SESSION_WORKTREE_KEY_MAX];
   session_worktree_key(sid, a, sizeof a);
   session_worktree_key(sid, b, sizeof b);
   assert(strcmp(a, b) == 0);
   /* Pinned literal: this is a persisted on-disk layout. If this value changes,
    * every live worktree moves — which is exactly what the reclaim path exists
    * to handle, so the change must be deliberate. */
   assert(strcmp(a, "4e2f8b9e-752dfcbbee8ce090") == 0);
   printf("  stable across calls, and pinned to the on-disk layout: ok\n");
}

static void test_key_is_path_safe(void)
{
   /* Session ids can arrive in a request body (webchat git panel, editors) and
    * are spliced straight into a path and a branch name. */
   const char *hostile[] = {
       "../../etc/passwd", "..",           "/absolute/path", "a/b/c", "-rf", "--upload-pack=evil",
       "id with spaces",   "quote'inject", "semi;colon",
   };
   for (size_t i = 0; i < sizeof(hostile) / sizeof(hostile[0]); i++)
   {
      char k[SESSION_WORKTREE_KEY_MAX];
      session_worktree_key(hostile[i], k, sizeof k);
      assert(k[0]);
      assert(strstr(k, "..") == NULL);
      assert(strchr(k, '/') == NULL);
      assert(k[0] != '-'); /* git would read a leading '-' as an option */
      for (const char *p = k; *p; p++)
      {
         int ok = (*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') ||
                  (*p >= '0' && *p <= '9') || *p == '-';
         assert(ok);
      }
   }
   /* Distinct hostile inputs still map to distinct keys. */
   char a[SESSION_WORKTREE_KEY_MAX], b[SESSION_WORKTREE_KEY_MAX];
   session_worktree_key("../../a", a, sizeof a);
   session_worktree_key("../../b", b, sizeof b);
   assert(strcmp(a, b) != 0);
   printf("  hostile session ids cannot escape the worktrees dir: ok\n");
}

static void test_short_buffer_never_truncates(void)
{
   /* A truncated key would silently reintroduce prefix collisions, so a buffer
    * too small must yield NOTHING rather than a short key. */
   char small[8];
   session_worktree_key("some-session-id", small, sizeof small);
   assert(small[0] == '\0');

   char exact[SESSION_WORKTREE_KEY_MAX];
   session_worktree_key("some-session-id", exact, sizeof exact);
   assert(exact[0]);
   printf("  a short buffer yields no key rather than a truncated one: ok\n");
}

static void test_empty_and_null(void)
{
   char k[SESSION_WORKTREE_KEY_MAX];
   session_worktree_key(NULL, k, sizeof k);
   assert(k[0] == '\0');
   session_worktree_key("", k, sizeof k);
   assert(k[0] == '\0');
   session_worktree_key("x", NULL, 0); /* must not crash */
   printf("  null/empty ids yield no key: ok\n");
}

static void test_is_key_guards_rekeying(void)
{
   /* Callers that recover a key from a worktree PATH must be able to tell that
    * what they hold is already a key. Re-deriving was harmless when the key was
    * a 16-char truncation (truncating 16 chars is a no-op) and became silently
    * wrong once it carried a hash — the GC would target a path that never
    * existed and remove nothing. */
   char k[SESSION_WORKTREE_KEY_MAX];
   session_worktree_key("4e2f8b9e-4d46-4744-b08b-1cdc7623f121", k, sizeof k);
   assert(session_worktree_key_is_key(k) == 1);

   char legacy[SESSION_WORKTREE_KEY_MAX];
   session_worktree_key_legacy("4e2f8b9e-4d46-4744-b08b-1cdc7623f121", legacy, sizeof legacy);
   assert(session_worktree_key_is_key(legacy) == 1);

   /* A raw session id is NOT a key. */
   assert(session_worktree_key_is_key("4e2f8b9e-4d46-4744-b08b-1cdc7623f121") == 0);
   assert(session_worktree_key_is_key("") == 0);
   assert(session_worktree_key_is_key(NULL) == 0);
   printf("  is_key distinguishes a key from a session id: ok\n");
}

static void test_legacy_derivation_preserved(void)
{
   /* Reclaim depends on reproducing the OLD key exactly; if this drifts, live
    * worktrees are stranded instead of recycled. */
   char k[SESSION_WORKTREE_KEY_MAX];
   session_worktree_key_legacy("4e2f8b9e-4d46-4744-b08b-1cdc7623f121", k, sizeof k);
   assert(strcmp(k, "4e2f8b9e-4d46-47") == 0);

   /* It sanitized rather than dropped: non-alnum mapped to '_'. */
   session_worktree_key_legacy("a/b", k, sizeof k);
   assert(strcmp(k, "a_b") == 0);

   /* Ids shorter than the window mapped to themselves. */
   session_worktree_key_legacy("abc123", k, sizeof k);
   assert(strcmp(k, "abc123") == 0);
   printf("  legacy derivation reproduced exactly (reclaim depends on it): ok\n");
}

int main(void)
{
   printf("session worktree key\n");
   test_shared_prefix_ids_do_not_collide();
   test_key_is_stable();
   test_key_is_path_safe();
   test_short_buffer_never_truncates();
   test_empty_and_null();
   test_is_key_guards_rekeying();
   test_legacy_derivation_preserved();
   printf("all session worktree key tests passed\n");
   return 0;
}
