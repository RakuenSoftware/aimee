/* session_state_stub.c: the store calls a guardrail test drags in, in memory.
 *
 * test_guardrails asserts about guardrail decisions. It reaches the store
 * only through code it links: session state is loaded and saved around a turn,
 * a worktree helper records git ownership, and five calls delete session state
 * as TEARDOWN between cases. Not one assertion in that file is about any of it.
 *
 * The real functions are bus clients now, so linking them would make a test
 * about classification need a running store module.
 *
 * SAVE/LOAD IS A REAL ROUND TRIP, not a pair of no-ops, and that is the whole
 * design decision here. A load that always misses is not a neutral stand-in for
 * a store -- it is a store that forgets, which is a behaviour, and code under
 * test is entitled to read back what it just wrote. One slot is enough: these
 * tests work on one session at a time and delete between cases.
 *
 * The write paths return success because their results are not examined; the
 * read path reports a genuine miss when nothing was saved, so a caller cannot
 * mistake an empty slot for stored data.
 */

#include <stdio.h>
#include <string.h>

#include "db1_client/git_ownership.h"
#include "db1_client/guardrail_events.h"
#include "db1_client/session_state.h"

static char g_sid[DB1_SS_SID_LEN];
static session_state_t g_state;
static int g_have;

int db1_session_state_save(const char *sid, const session_state_t *in)
{
   if (!sid || !in)
   {
      return -1;
   }
   snprintf(g_sid, sizeof(g_sid), "%s", sid);
   g_state = *in;
   g_have = 1;
   return 0;
}

int db1_session_state_load(const char *sid, session_state_t *out)
{
   if (!sid || !out || !g_have || strcmp(sid, g_sid) != 0)
   {
      return -1;
   }
   *out = g_state;
   return 0;
}

int db1_session_state_delete(const char *sid)
{
   if (sid && g_have && strcmp(sid, g_sid) == 0)
   {
      g_have = 0;
   }
   return 0;
}

/* Ownership is recorded by a worktree helper and never read back here. */
int db1_git_ownership_upsert(const char *repo_path, const char *branch_name, const char *session_id)
{
   (void)repo_path;
   (void)branch_name;
   (void)session_id;
   return 0;
}

/* --- guardrail events -------------------------------------------------------
 *
 * COUNTED FOR REAL, because one test genuinely depends on the round trip:
 * test_semantic_advisory_pre_tool_check takes a baseline, drives two tool
 * checks, and asserts prompt == before + 2 with warn, block and dry_run
 * unchanged.
 *
 * That assertion's subject is the GUARDRAIL -- that it classified both actions
 * as prompt and nothing else -- and the store is only the medium it is read
 * through. Counting here in memory keeps the claim intact. Stubbing the insert
 * to a no-op and the count to zeroes would leave the test passing while
 * asserting 0 == 0 + 2, which does not even hold, so it would fail; stubbing
 * both to a fixed value would make it pass while measuring nothing, which is
 * worse.
 *
 * Bucketed by final_action, which is what the real 7-day query groups on. No
 * time window: nothing in this binary can age an event out within a run.
 */

static guardrail_event_counts_t g_counts;

int db1_guardrail_event_insert(const guardrail_event_t *e)
{
   if (!e)
   {
      return -1;
   }
   if (e->dry_run)
   {
      g_counts.dry_run++;
   }
   else if (strcmp(e->final_action, "warn") == 0)
   {
      g_counts.warn++;
   }
   else if (strcmp(e->final_action, "prompt") == 0)
   {
      g_counts.prompt++;
   }
   else if (strcmp(e->final_action, "block") == 0)
   {
      g_counts.block++;
   }
   return 0;
}

int db1_guardrail_event_counts_7d(guardrail_event_counts_t *out)
{
   if (!out)
   {
      return -1;
   }
   *out = g_counts;
   return 0;
}
