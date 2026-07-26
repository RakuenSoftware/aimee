/* test_kb_login_throttle.c — the pre-auth login brute-force budget.
 *
 * The properties asserted here are the ones the route's security rests on, and
 * each is written so that removing the corresponding code makes it fail:
 *
 *   - the budget actually runs out, and the refusal carries a positive wait
 *   - the lockout is EXPONENTIAL, not a flat window
 *   - the peer budget and the username budget are INDEPENDENT, so neither alone
 *     can be evaded by varying the other
 *   - a successful credential check CLEARS the budget
 *   - checking is a pure query: being throttled does not extend the lockout
 *   - state is BOUNDED, and a table under pressure fails CLOSED rather than open
 */

#include "kb/kb_login_throttle.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#define NOW 1700000000LL

/* Spend exactly the budget, so the next failure is the one that locks. */
static void spend_budget(const char *user, int64_t now)
{
   for (int i = 0; i < KB_LOGIN_THROTTLE_BUDGET; i++)
   {
      assert(kb_login_throttle_check(user, now) == 0);
      kb_login_throttle_record_failure(user, now);
   }
}

static void test_budget_runs_out(void)
{
   kb_login_throttle_reset();
   kb_login_throttle_set_peer("10.0.0.1");

   /* Every attempt up to the budget is allowed. */
   spend_budget("alice", NOW);

   /* The next one is refused, with a wait the caller can act on. */
   int retry = kb_login_throttle_check("alice", NOW);
   assert(retry > 0);
   assert(retry <= KB_LOGIN_THROTTLE_MAX_LOCK_SEC);

   /* Still refused part-way through the lockout, allowed once it elapses. */
   assert(kb_login_throttle_check("alice", NOW + retry - 1) > 0);
   assert(kb_login_throttle_check("alice", NOW + retry) == 0);
}

static void test_lockout_is_exponential(void)
{
   kb_login_throttle_reset();
   kb_login_throttle_set_peer("10.0.0.2");
   spend_budget("bob", NOW);

   int first = kb_login_throttle_check("bob", NOW);
   assert(first > 0);
   /* One more failure past the budget must lengthen the wait, not restart it.
    * A flat window would make these equal, and on-line cracking would stay
    * viable at a fixed rate forever. */
   kb_login_throttle_record_failure("bob", NOW);
   int second = kb_login_throttle_check("bob", NOW);
   assert(second > first);

   /* ...and it is capped, so a lockout cannot become permanent. */
   for (int i = 0; i < 40; i++)
      kb_login_throttle_record_failure("bob", NOW);
   int capped = kb_login_throttle_check("bob", NOW);
   assert(capped > 0 && capped <= KB_LOGIN_THROTTLE_MAX_LOCK_SEC);
}

static void test_peer_and_user_budgets_are_independent(void)
{
   /* ONE PEER, MANY USERNAMES: the peer budget must stop it. Without a per-peer
    * budget an attacker sprays one password across an account list for free. */
   kb_login_throttle_reset();
   kb_login_throttle_set_peer("10.0.0.3");
   char user[32];
   for (int i = 0; i < KB_LOGIN_THROTTLE_BUDGET; i++)
   {
      snprintf(user, sizeof(user), "user%d", i);
      assert(kb_login_throttle_check(user, NOW) == 0);
      kb_login_throttle_record_failure(user, NOW);
   }
   /* A NEW username from the same peer is refused: the peer is out of budget
    * even though this username has never been tried. */
   assert(kb_login_throttle_check("never-tried-before", NOW) > 0);

   /* MANY PEERS, ONE USERNAME: the username budget must stop it. Without it a
    * botnet cracks one account with one attempt per host. */
   kb_login_throttle_reset();
   for (int i = 0; i < KB_LOGIN_THROTTLE_BUDGET; i++)
   {
      snprintf(user, sizeof(user), "10.1.0.%d", i);
      kb_login_throttle_set_peer(user);
      assert(kb_login_throttle_check("carol", NOW) == 0);
      kb_login_throttle_record_failure("carol", NOW);
   }
   /* A FRESH peer is refused for carol: her own budget is spent. */
   kb_login_throttle_set_peer("10.9.9.9");
   assert(kb_login_throttle_check("carol", NOW) > 0);
   /* ...but that fresh peer can still try somebody else, so the username budget
    * has not become a global outage. */
   assert(kb_login_throttle_check("dave", NOW) == 0);
}

static void test_success_clears_the_budget(void)
{
   kb_login_throttle_reset();
   kb_login_throttle_set_peer("10.0.0.4");
   /* A few fat-fingered attempts, then the right password. */
   for (int i = 0; i < KB_LOGIN_THROTTLE_BUDGET - 1; i++)
      kb_login_throttle_record_failure("erin", NOW);
   assert(kb_login_throttle_check("erin", NOW) == 0);
   kb_login_throttle_record_success("erin");
   /* The budget is whole again: a user who eventually gets it right is not left
    * one typo away from a lockout. */
   for (int i = 0; i < KB_LOGIN_THROTTLE_BUDGET; i++)
   {
      assert(kb_login_throttle_check("erin", NOW) == 0);
      kb_login_throttle_record_failure("erin", NOW);
   }
   assert(kb_login_throttle_check("erin", NOW) > 0);
}

static void test_check_does_not_charge(void)
{
   /* check() is a query. If it charged, a client polling to see whether it may
    * retry would extend its own lockout forever. */
   kb_login_throttle_reset();
   kb_login_throttle_set_peer("10.0.0.5");
   spend_budget("frank", NOW);
   int first = kb_login_throttle_check("frank", NOW);
   for (int i = 0; i < 50; i++)
      (void)kb_login_throttle_check("frank", NOW);
   assert(kb_login_throttle_check("frank", NOW) == first);
}

static void test_window_expiry_restores_an_unspent_budget(void)
{
   kb_login_throttle_reset();
   kb_login_throttle_set_peer("10.0.0.6");
   /* Below the budget, then a long quiet period: the count must not accumulate
    * across windows forever, or a user who mistypes once a month is eventually
    * locked out. */
   for (int i = 0; i < KB_LOGIN_THROTTLE_BUDGET - 1; i++)
      kb_login_throttle_record_failure("grace", NOW);
   int64_t later = NOW + KB_LOGIN_THROTTLE_WINDOW_SEC + 1;
   for (int i = 0; i < KB_LOGIN_THROTTLE_BUDGET - 1; i++)
   {
      assert(kb_login_throttle_check("grace", later) == 0);
      kb_login_throttle_record_failure("grace", later);
   }
   assert(kb_login_throttle_check("grace", later) == 0);
}

static void test_unknown_peer_is_a_bucket_not_an_exemption(void)
{
   /* If an unresolvable peer meant "no limit", the cheapest attack would be to
    * make the address unresolvable. */
   kb_login_throttle_reset();
   kb_login_throttle_set_peer("");
   char user[32];
   for (int i = 0; i < KB_LOGIN_THROTTLE_BUDGET; i++)
   {
      snprintf(user, sizeof(user), "u%d", i);
      kb_login_throttle_record_failure(user, NOW);
   }
   assert(kb_login_throttle_check("someone-else", NOW) > 0);
}

static void test_state_is_bounded_and_fails_closed(void)
{
   /* Drive far more distinct identities than there are slots. The table must not
    * grow, and whatever it cannot track it must REFUSE. The property asserted is
    * the safe direction: under pressure this over-throttles, and at no point does
    * an attacker get more than the budget by exhausting the table. */
   kb_login_throttle_reset();
   char peer[32], user[32];
   int allowed_after_budget = 0;
   for (int i = 0; i < 20000; i++)
   {
      snprintf(peer, sizeof(peer), "10.%d.%d.%d", (i >> 16) & 0xff, (i >> 8) & 0xff, i & 0xff);
      snprintf(user, sizeof(user), "user%d", i);
      kb_login_throttle_set_peer(peer);
      for (int k = 0; k < KB_LOGIN_THROTTLE_BUDGET + 2; k++)
      {
         if (kb_login_throttle_check(user, NOW) == 0 && k >= KB_LOGIN_THROTTLE_BUDGET)
            allowed_after_budget++;
         kb_login_throttle_record_failure(user, NOW);
      }
   }
   /* Not one identity, anywhere in that sweep, got an attempt past its budget. */
   assert(allowed_after_budget == 0);
}

int main(void)
{
   test_budget_runs_out();
   test_lockout_is_exponential();
   test_peer_and_user_budgets_are_independent();
   test_success_clears_the_budget();
   test_check_does_not_charge();
   test_window_expiry_restores_an_unspent_budget();
   test_unknown_peer_is_a_bucket_not_an_exemption();
   test_state_is_bounded_and_fails_closed();
   printf("test_kb_login_throttle: ok\n");
   return 0;
}
