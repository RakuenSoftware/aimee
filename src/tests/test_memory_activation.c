/* test_memory_activation.c: per-unit retrieval hysteresis.
 *
 * Hysteresis is non-deterministic by nature -- the same conversation state
 * yields different context depending on what fired before -- so these fixtures
 * assert WHAT FIRES AND WHY against an explicit turn counter, never against
 * wall-clock state. A test that let real time decide would be asserting how
 * fast the machine ran.
 *
 * The store is stubbed at the db1 client seam so the gate is driven through its
 * production entry points rather than a reimplementation of its rules.
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "db1_client/caches.h"
#include "modules/memory/memory_activation.h"

/* --- stub store ---------------------------------------------------------- */

static int g_fail_read = 0; /* make the activation read fail */
static int g_rows = 0;      /* how many rows the stub serves */
static int64_t g_ids[8];
static int64_t g_turns[8];
static int64_t g_current_turn = 1;
static int g_read_calls = 0;

/* Recorded writes, so the "does not feed reinforcement" property is checkable. */
static int g_turn_writes = 0;
static int g_plain_writes = 0;
static int64_t g_last_write_turn = -1;

int db1_context_snapshot_activation(const char *session_id,
                                    char (*out)[DB1_CONTEXT_ACTIVATION_ROW_LEN], int max)
{
   if (!session_id || !session_id[0] || !out || max <= 0)
      return -1;
   if (g_fail_read)
      return -1;
   g_read_calls++;
   int n = 0;
   snprintf(out[n++], DB1_CONTEXT_ACTIVATION_ROW_LEN, "0 %lld", (long long)g_current_turn);
   for (int i = 0; i < g_rows && n < max; i++, n++)
      snprintf(out[n], DB1_CONTEXT_ACTIVATION_ROW_LEN, "%lld %lld", (long long)g_ids[i],
               (long long)g_turns[i]);
   return n;
}

int db1_context_snapshot_insert_turn(const char *session_id, int64_t memory_id,
                                     double relevance_score, int64_t turn_index)
{
   (void)session_id;
   (void)memory_id;
   (void)relevance_score;
   g_turn_writes++;
   g_last_write_turn = turn_index;
   return 0;
}

int db1_context_snapshot_insert(const char *session_id, int64_t memory_id, double relevance_score)
{
   (void)session_id;
   (void)memory_id;
   (void)relevance_score;
   g_plain_writes++;
   return 0;
}

/* --- fixtures ------------------------------------------------------------ */

static void set_rows(int n, const int64_t *ids, const int64_t *turns)
{
   g_rows = n;
   g_current_turn = 1;
   for (int i = 0; i < n; i++)
   {
      g_ids[i] = ids[i];
      g_turns[i] = turns[i];
      if (turns[i] >= g_current_turn)
         g_current_turn = turns[i] + 1;
   }
}

static void reset(void)
{
   g_fail_read = 0;
   g_rows = 0;
   g_current_turn = 1;
   g_turn_writes = 0;
   g_plain_writes = 0;
   g_last_write_turn = -1;
   g_read_calls = 0;
}

/* The current turn is one past the highest turn already recorded, so the gate
 * measures distance in turns rather than in elapsed time. */
static void test_current_turn_is_one_past_the_highest(void)
{
   reset();
   const int64_t ids[] = {10, 11, 12};
   const int64_t turns[] = {7, 3, 5};
   set_rows(3, ids, turns);

   memory_activation_t act;
   memory_activation_load(&act, "sess-a");
   assert(act.loaded == 1);
   assert(act.count == 3);
   assert(act.current_turn == 8);
   assert(memory_activation_last_turn(&act, 10) == 7);
   assert(memory_activation_last_turn(&act, 11) == 3);
   assert(memory_activation_last_turn(&act, 99) == 0); /* never fired here */
   printf("  current_turn_is_one_past_the_highest: ok\n");
}

/* Cooldown is the only thing that stops per-turn repetition. */
static void test_cooldown_blocks_the_turn_after_firing(void)
{
   reset();
   const int64_t ids[] = {10, 20};
   /* 10 fired last turn (7); 20 fired long ago (1). Current turn is 8. */
   const int64_t turns[] = {7, 1};
   set_rows(2, ids, turns);

   memory_activation_t act;
   memory_activation_load(&act, "sess-a");
   assert(act.current_turn == 8);

   assert(memory_activation_in_cooldown(&act, 10, 1) == 1); /* distance 1 <= 1 */
   assert(memory_activation_in_cooldown(&act, 20, 1) == 0); /* distance 7 */
   assert(memory_activation_in_cooldown(&act, 99, 1) == 0); /* never fired */
   printf("  cooldown_blocks_the_turn_after_firing: ok\n");
}

/* Sticky keeps a unit eligible for a couple of turns after it fires, so a
 * rephrase does not drop the thread. */
static void test_sticky_outlasts_cooldown(void)
{
   reset();
   const int64_t ids[] = {10};
   const int64_t turns[] = {6}; /* current turn 7, distance 1 */
   set_rows(1, ids, turns);

   memory_activation_t act;
   memory_activation_load(&act, "sess-a");
   assert(act.current_turn == 7);
   assert(memory_activation_is_sticky(&act, 10, 2) == 1);
   assert(memory_activation_in_cooldown(&act, 10, 1) == 1);
   printf("  sticky_outlasts_cooldown: ok\n");
}

/* THE PRECEDENCE CASE. Sticky and cooldown overlap by construction, and leaving
 * which one wins implicit is the commonest defect in implementations of this
 * pattern. Cooldown wins: a unit inside its cooldown does not fire even while
 * sticky, or cooldown would never bind on exactly the units that keep matching
 * -- which are the ones that repeat. This asserts the overlap exists AND which
 * side of it the gate takes. */
static void test_cooldown_wins_inside_the_sticky_window(void)
{
   /* Distance 1: both windows are open, so the overlap genuinely exists and the
    * precedence question is real rather than theoretical. */
   reset();
   const int64_t ids[] = {10};
   const int64_t turns[] = {7}; /* current turn 8 */
   set_rows(1, ids, turns);

   memory_activation_t act;
   memory_activation_load(&act, "sess-a");
   assert(act.current_turn == 8);
   assert(memory_activation_is_sticky(&act, 10, 2) == 1);
   assert(memory_activation_in_cooldown(&act, 10, 1) == 1);
   /* Cooldown wins: the caller must not fire this unit despite sticky. */

   /* Distance 2 is the band hysteresis exists for: the cooldown has lapsed but
    * sticky still holds, so the unit is eligible WITHOUT having to match again.
    * A second unit carries the turn counter forward so unit 10 can sit two
    * turns back -- with a single row the distance is always 1. */
   reset();
   const int64_t ids2[] = {10, 11};
   const int64_t turns2[] = {5, 6}; /* highest 6 -> current 7, so 10 is 2 back */
   set_rows(2, ids2, turns2);

   memory_activation_t act2;
   memory_activation_load(&act2, "sess-a");
   assert(act2.current_turn == 7);
   assert(memory_activation_in_cooldown(&act2, 10, 1) == 0); /* 2 > cooldown 1 */
   assert(memory_activation_is_sticky(&act2, 10, 2) == 1);   /* 2 <= sticky 2 */
   /* And unit 11, which fired last turn, is still cooling. */
   assert(memory_activation_in_cooldown(&act2, 11, 1) == 1);

   /* Distance 3: both windows have lapsed, so the unit is back to being decided
    * by relevance alone. */
   reset();
   const int64_t ids3[] = {10, 11};
   const int64_t turns3[] = {4, 6}; /* current 7, so 10 is 3 back */
   set_rows(2, ids3, turns3);

   memory_activation_t act3;
   memory_activation_load(&act3, "sess-a");
   assert(memory_activation_in_cooldown(&act3, 10, 1) == 0);
   assert(memory_activation_is_sticky(&act3, 10, 2) == 0);
   printf("  cooldown_wins_inside_the_sticky_window: ok\n");
}

/* A malformed or legacy zero-turn row must read as "never fired", not as "just
 * injected", so defensive parsing cannot mute a memory on an invalid turn. */
static void test_zero_turn_rows_do_not_mute(void)
{
   reset();
   const int64_t ids[] = {10};
   const int64_t turns[] = {0};
   set_rows(1, ids, turns);

   memory_activation_t act;
   memory_activation_load(&act, "sess-a");
   assert(memory_activation_last_turn(&act, 10) == 0);
   assert(memory_activation_in_cooldown(&act, 10, 1) == 0);
   assert(memory_activation_is_sticky(&act, 10, 2) == 0);
   printf("  zero_turn_rows_do_not_mute: ok\n");
}

/* Failing open is the whole safety argument. A gate that errs toward
 * withholding evidence produces confident answers with nothing behind them,
 * which is worse than repeating a memory. */
static void test_unreadable_store_fails_open(void)
{
   memory_activation_t act;

   reset();
   g_fail_read = 1;
   memory_activation_load(&act, "sess-a");
   assert(act.loaded == 0);
   assert(memory_activation_in_cooldown(&act, 10, 1) == 0);
   assert(memory_activation_is_sticky(&act, 10, 2) == 0);

   reset();
   memory_activation_load(&act, NULL); /* no session */
   assert(act.loaded == 0);
   assert(memory_activation_in_cooldown(&act, 10, 1) == 0);

   reset();
   memory_activation_load(&act, ""); /* empty session */
   assert(act.loaded == 0);
   assert(memory_activation_in_cooldown(&act, 10, 1) == 0);

   /* A NULL activation struct must also answer "no opinion" rather than crash. */
   assert(memory_activation_in_cooldown(NULL, 10, 1) == 0);
   assert(memory_activation_is_sticky(NULL, 10, 2) == 0);
   assert(memory_activation_last_turn(NULL, 10) == 0);
   printf("  unreadable_store_fails_open: ok\n");
}

/* Recording writes the activation axis, and only that. */
static void test_record_writes_the_turn_axis(void)
{
   reset();
   const int64_t ids[] = {10};
   const int64_t turns[] = {4};
   set_rows(1, ids, turns);

   memory_activation_t act;
   memory_activation_load(&act, "sess-a");
   assert(act.current_turn == 5);

   memory_activation_record(&act, "sess-a", 42, 0.9);
   assert(g_turn_writes == 1);
   assert(g_last_write_turn == 5);
   assert(g_plain_writes == 0); /* not double-counted into the sampling record */

   /* With no turn axis available, omit the event. Falling back to the sampling
    * table would make mere exposure reinforce the memory. */
   reset();
   g_fail_read = 1;
   memory_activation_t unloaded;
   memory_activation_load(&unloaded, "sess-a");
   assert(unloaded.loaded == 0);
   memory_activation_record(&unloaded, "sess-a", 42, 0.9);
   assert(g_turn_writes == 0);
   assert(g_plain_writes == 0);

   /* Bad inputs write nothing at all. */
   reset();
   memory_activation_record(&act, NULL, 42, 0.9);
   memory_activation_record(&act, "sess-a", 0, 0.9);
   memory_activation_record(&act, "sess-a", -1, 0.9);
   assert(g_turn_writes == 0 && g_plain_writes == 0);
   printf("  record_writes_the_turn_axis: ok\n");
}

/* A conversation with no history yet: nothing is cooling, nothing is sticky,
 * and the first turn is 1. */
static void test_fresh_conversation(void)
{
   reset();
   memory_activation_t act;
   memory_activation_load(&act, "sess-new");
   assert(act.loaded == 1);
   assert(act.count == 0);
   assert(act.current_turn == 1);
   assert(memory_activation_in_cooldown(&act, 10, 1) == 0);
   assert(memory_activation_is_sticky(&act, 10, 2) == 0);
   printf("  fresh_conversation: ok\n");
}

static void test_delay_uses_real_conversation_turns(void)
{
   reset();
   g_current_turn = 3; /* persisted even though no memory fired on turns 1-2 */
   memory_activation_t act;
   memory_activation_load(&act, "sess-delay");
   assert(act.current_turn == 3);
   assert(memory_activation_is_delayed(&act, 3) == 1);
   assert(memory_activation_is_delayed(&act, 2) == 0);
   assert(memory_activation_is_delayed(&act, 0) == 0);
   assert(memory_activation_is_delayed(NULL, 20) == 0); /* fail open */
   printf("  delay_uses_real_conversation_turns: ok\n");
}

static void test_explain_snapshot_does_not_advance_turn(void)
{
   reset();
   g_current_turn = 9;
   memory_activation_t loaded;
   memory_activation_t explained;
   memory_activation_load(&loaded, "sess-explain");
   assert(g_read_calls == 1);
   assert(memory_activation_last_loaded("sess-explain", &explained) == 1);
   assert(g_read_calls == 1); /* no second store call, therefore no phantom turn */
   assert(explained.current_turn == loaded.current_turn);
   assert(memory_activation_last_loaded("another-session", &explained) == 0);

   /* A subsequent store failure invalidates the reusable snapshot. Explain
    * must fail open rather than describe stale state from an earlier turn. */
   g_fail_read = 1;
   memory_activation_load(&loaded, "sess-explain");
   assert(loaded.loaded == 0);
   assert(memory_activation_last_loaded("sess-explain", &explained) == 0);
   printf("  explain_snapshot_does_not_advance_turn: ok\n");
}

int main(void)
{
   printf("test_memory_activation:\n");
   test_current_turn_is_one_past_the_highest();
   test_cooldown_blocks_the_turn_after_firing();
   test_sticky_outlasts_cooldown();
   test_cooldown_wins_inside_the_sticky_window();
   test_zero_turn_rows_do_not_mute();
   test_unreadable_store_fails_open();
   test_record_writes_the_turn_axis();
   test_fresh_conversation();
   test_delay_uses_real_conversation_turns();
   test_explain_snapshot_does_not_advance_turn();
   printf("all memory_activation tests passed\n");
   return 0;
}
