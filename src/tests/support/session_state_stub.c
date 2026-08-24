/* session_state_stub.c: session state, saved and loaded, in memory.
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
