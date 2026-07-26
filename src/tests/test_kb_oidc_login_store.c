/* test_kb_oidc_login_store.c — custody of in-flight OIDC logins.
 *
 * The three properties the store exists for, each asserted as an outcome rather
 * than an implementation detail:
 *
 *   SINGLE USE — a callback replayed with the correct state finds nothing, which
 *     is what makes a callback URL in browser history or a proxy log useless.
 *   EXPIRING — a login not completed within its TTL is gone, and the clock is a
 *     parameter so this is tested rather than slept through.
 *   BOUNDED — the table fills and then REFUSES, and a full table never evicts a
 *     live login (a flood must not be able to knock out a real user).
 */
#include "kb_oidc_login_store.h"

#include "kb_oidc_login.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static int random_failure;

/* Every call must produce a DISTINCT block. This test fills all 64 slots, which
 * is 192 separate 32-byte draws; any sawtooth keyed only on a byte counter
 * repeats and the store then (correctly) refuses the duplicate state, which
 * would read as a store bug rather than a harness one. Stamping the call number
 * into the first four bytes makes uniqueness structural. */
int platform_random_bytes(void *buf, size_t len)
{
   if (random_failure)
      return -1;
   static uint32_t call_no = 1;
   unsigned char *out = buf;
   assert(len >= sizeof(call_no));
   for (size_t i = 0; i < len; ++i)
      out[i] = (unsigned char)(i * 7u + 3u);
   memcpy(out, &call_no, sizeof(call_no));
   call_no++;
   return 0;
}

static const int64_t T0 = 1780000000;

static kb_oidc_login_config_t good_config(void)
{
   kb_oidc_login_config_t cfg;
   memset(&cfg, 0, sizeof(cfg));
   snprintf(cfg.issuer, sizeof(cfg.issuer), "%s", "https://idp.example");
   snprintf(cfg.client_id, sizeof(cfg.client_id), "%s", "aimee-kb");
   snprintf(cfg.authorize_url, sizeof(cfg.authorize_url), "%s", "https://idp.example/authorize");
   snprintf(cfg.token_url, sizeof(cfg.token_url), "%s", "https://idp.example/token");
   snprintf(cfg.redirect_uri, sizeof(cfg.redirect_uri), "%s", "https://kb.example/oidc/callback");
   return cfg;
}

/* A real pending login, drawn the way the production path draws it. */
static kb_oidc_login_pending_t start_login(void)
{
   kb_oidc_login_config_t cfg = good_config();
   kb_oidc_login_pending_t p;
   char url[KB_OIDC_LOGIN_URL_MAX];
   assert(kb_oidc_login_start(&cfg, "srv-a", &p, url, sizeof(url)) == KB_OIDC_LOGIN_OK);
   return p;
}

static void test_round_trip_is_single_use(void)
{
   kb_oidc_login_store_reset();
   kb_oidc_login_pending_t p = start_login();
   assert(kb_oidc_login_store_put(&p, T0, 300) == KB_OIDC_LOGIN_STORE_OK);
   assert(kb_oidc_login_store_count(T0) == 1);

   kb_oidc_login_pending_t got;
   assert(kb_oidc_login_store_take(p.state, T0 + 10, &got) == KB_OIDC_LOGIN_STORE_OK);
   /* Everything the exchange needs comes back intact: the verifier it must send,
    * the nonce it must compare, and the redirect_uri it must repeat. */
   assert(!strcmp(got.state, p.state));
   assert(!strcmp(got.code_verifier, p.code_verifier));
   assert(!strcmp(got.nonce, p.nonce));
   assert(!strcmp(got.redirect_uri, p.redirect_uri));

   /* THE property: the same correct state a second time finds nothing. */
   kb_oidc_login_pending_t again;
   memset(&again, 0xaa, sizeof(again));
   assert(kb_oidc_login_store_take(p.state, T0 + 11, &again) == KB_OIDC_LOGIN_STORE_NOT_FOUND);
   kb_oidc_login_pending_t zero;
   memset(&zero, 0, sizeof(zero));
   assert(!memcmp(&again, &zero, sizeof(again)));
   assert(kb_oidc_login_store_count(T0 + 11) == 0);
}

static void test_unknown_and_malformed_states(void)
{
   kb_oidc_login_store_reset();
   kb_oidc_login_pending_t p = start_login();
   assert(kb_oidc_login_store_put(&p, T0, 300) == KB_OIDC_LOGIN_STORE_OK);

   kb_oidc_login_pending_t got;
   /* A different login's state. */
   kb_oidc_login_pending_t other = start_login();
   assert(kb_oidc_login_store_take(other.state, T0, &got) == KB_OIDC_LOGIN_STORE_NOT_FOUND);

   /* One flipped character. */
   char wrong[KB_OIDC_LOGIN_SECRET_LEN + 1];
   memcpy(wrong, p.state, sizeof(wrong));
   wrong[5] = (char)(wrong[5] == 'A' ? 'B' : 'A');
   assert(kb_oidc_login_store_take(wrong, T0, &got) == KB_OIDC_LOGIN_STORE_NOT_FOUND);

   /* Malformed lengths are answered identically, so a callback handler cannot
    * distinguish "malformed" from "unknown". */
   assert(kb_oidc_login_store_take("", T0, &got) == KB_OIDC_LOGIN_STORE_NOT_FOUND);
   assert(kb_oidc_login_store_take("short", T0, &got) == KB_OIDC_LOGIN_STORE_NOT_FOUND);
   char longer[KB_OIDC_LOGIN_SECRET_LEN + 3];
   snprintf(longer, sizeof(longer), "%sxx", p.state);
   assert(kb_oidc_login_store_take(longer, T0, &got) == KB_OIDC_LOGIN_STORE_NOT_FOUND);

   /* A prefix of a live state must not match it. */
   char prefix[KB_OIDC_LOGIN_SECRET_LEN];
   memcpy(prefix, p.state, sizeof(prefix) - 1);
   prefix[sizeof(prefix) - 1] = '\0';
   assert(kb_oidc_login_store_take(prefix, T0, &got) == KB_OIDC_LOGIN_STORE_NOT_FOUND);

   /* None of those misses consumed the real login. */
   assert(kb_oidc_login_store_count(T0) == 1);
   assert(kb_oidc_login_store_take(p.state, T0, &got) == KB_OIDC_LOGIN_STORE_OK);

   assert(kb_oidc_login_store_take(NULL, T0, &got) == KB_OIDC_LOGIN_STORE_INVALID);
   assert(kb_oidc_login_store_take(p.state, T0, NULL) == KB_OIDC_LOGIN_STORE_INVALID);
}

static void test_expiry(void)
{
   kb_oidc_login_store_reset();
   kb_oidc_login_pending_t p = start_login();
   assert(kb_oidc_login_store_put(&p, T0, 60) == KB_OIDC_LOGIN_STORE_OK);

   kb_oidc_login_pending_t got;
   /* Alive right up to the boundary. */
   assert(kb_oidc_login_store_count(T0 + 59) == 1);
   /* Gone AT the expiry instant, not one second after: the boundary is
    * inclusive, so a login is never usable at the moment it expires. */
   assert(kb_oidc_login_store_count(T0 + 60) == 0);
   assert(kb_oidc_login_store_take(p.state, T0 + 60, &got) == KB_OIDC_LOGIN_STORE_NOT_FOUND);

   /* And an expired login stays gone even if the clock is asked again earlier —
    * the failed take swept it. */
   assert(kb_oidc_login_store_take(p.state, T0, &got) == KB_OIDC_LOGIN_STORE_NOT_FOUND);

   /* A default TTL is applied for a non-positive one. */
   kb_oidc_login_pending_t q = start_login();
   assert(kb_oidc_login_store_put(&q, T0, 0) == KB_OIDC_LOGIN_STORE_OK);
   assert(kb_oidc_login_store_count(T0 + KB_OIDC_LOGIN_STORE_TTL_DEFAULT - 1) == 1);
   assert(kb_oidc_login_store_count(T0 + KB_OIDC_LOGIN_STORE_TTL_DEFAULT) == 0);

   /* An over-long TTL is REFUSED, not clamped: silently shortening an operator's
    * stated lifetime would be a surprise, and pinning secrets forever is not an
    * option either. */
   kb_oidc_login_store_reset();
   kb_oidc_login_pending_t r = start_login();
   assert(kb_oidc_login_store_put(&r, T0, KB_OIDC_LOGIN_STORE_TTL_MAX) == KB_OIDC_LOGIN_STORE_OK);
   kb_oidc_login_store_reset();
   assert(kb_oidc_login_store_put(&r, T0, KB_OIDC_LOGIN_STORE_TTL_MAX + 1) ==
          KB_OIDC_LOGIN_STORE_INVALID);
   assert(kb_oidc_login_store_count(T0) == 0);
}

static void test_sweep(void)
{
   kb_oidc_login_store_reset();
   kb_oidc_login_pending_t a = start_login();
   kb_oidc_login_pending_t b = start_login();
   assert(kb_oidc_login_store_put(&a, T0, 60) == KB_OIDC_LOGIN_STORE_OK);
   assert(kb_oidc_login_store_put(&b, T0, 600) == KB_OIDC_LOGIN_STORE_OK);

   /* A periodic sweep zeroes abandoned secrets without waiting for the next
    * login attempt, and leaves the live one alone. */
   assert(kb_oidc_login_store_sweep(T0 + 10) == 0);
   assert(kb_oidc_login_store_sweep(T0 + 60) == 1);
   assert(kb_oidc_login_store_count(T0 + 60) == 1);
   kb_oidc_login_pending_t got;
   assert(kb_oidc_login_store_take(b.state, T0 + 60, &got) == KB_OIDC_LOGIN_STORE_OK);
   assert(!strcmp(got.code_verifier, b.code_verifier));
}

static void test_bounded_and_never_evicts_a_live_login(void)
{
   kb_oidc_login_store_reset();
   kb_oidc_login_pending_t first = start_login();
   assert(kb_oidc_login_store_put(&first, T0, 300) == KB_OIDC_LOGIN_STORE_OK);
   for (int i = 1; i < KB_OIDC_LOGIN_STORE_SLOTS; ++i)
   {
      kb_oidc_login_pending_t p = start_login();
      assert(kb_oidc_login_store_put(&p, T0, 300) == KB_OIDC_LOGIN_STORE_OK);
   }
   assert(kb_oidc_login_store_count(T0) == KB_OIDC_LOGIN_STORE_SLOTS);

   /* Full: refuse rather than make room. */
   kb_oidc_login_pending_t overflow = start_login();
   assert(kb_oidc_login_store_put(&overflow, T0, 300) == KB_OIDC_LOGIN_STORE_FULL);

   /* THE property that matters about being full: the flood did not cost the
    * first user their in-flight login. */
   kb_oidc_login_pending_t got;
   assert(kb_oidc_login_store_take(first.state, T0 + 1, &got) == KB_OIDC_LOGIN_STORE_OK);
   assert(!strcmp(got.code_verifier, first.code_verifier));

   /* And the freed slot is immediately reusable. */
   assert(kb_oidc_login_store_put(&overflow, T0 + 1, 300) == KB_OIDC_LOGIN_STORE_OK);

   /* Once the flood expires, capacity returns without operator action. */
   assert(kb_oidc_login_store_count(T0 + 300) == 1); /* only `overflow` is younger */
   kb_oidc_login_pending_t after = start_login();
   assert(kb_oidc_login_store_put(&after, T0 + 300, 300) == KB_OIDC_LOGIN_STORE_OK);
}

static void test_rejects_unstarted_and_duplicate(void)
{
   kb_oidc_login_store_reset();
   /* A pending login that was never started has no secrets to keep, and storing
    * one would create a slot whose state is the empty string. */
   kb_oidc_login_pending_t empty;
   memset(&empty, 0, sizeof(empty));
   assert(kb_oidc_login_store_put(&empty, T0, 300) == KB_OIDC_LOGIN_STORE_INVALID);
   assert(kb_oidc_login_store_put(NULL, T0, 300) == KB_OIDC_LOGIN_STORE_INVALID);
   assert(kb_oidc_login_store_count(T0) == 0);

   /* A partially-drawn record is refused too. */
   kb_oidc_login_pending_t partial = start_login();
   memset(partial.nonce, 0, sizeof(partial.nonce));
   assert(kb_oidc_login_store_put(&partial, T0, 300) == KB_OIDC_LOGIN_STORE_INVALID);

   /* Storing the same pending record twice is refused rather than shadowing the
    * first: the earlier login has to stay single-use. */
   kb_oidc_login_pending_t p = start_login();
   assert(kb_oidc_login_store_put(&p, T0, 300) == KB_OIDC_LOGIN_STORE_OK);
   assert(kb_oidc_login_store_put(&p, T0, 300) == KB_OIDC_LOGIN_STORE_INVALID);
   assert(kb_oidc_login_store_count(T0) == 1);
}

static void test_reset_clears(void)
{
   kb_oidc_login_store_reset();
   kb_oidc_login_pending_t p = start_login();
   assert(kb_oidc_login_store_put(&p, T0, 300) == KB_OIDC_LOGIN_STORE_OK);
   kb_oidc_login_store_reset();
   assert(kb_oidc_login_store_count(T0) == 0);
   kb_oidc_login_pending_t got;
   assert(kb_oidc_login_store_take(p.state, T0, &got) == KB_OIDC_LOGIN_STORE_NOT_FOUND);
}

int main(void)
{
   test_round_trip_is_single_use();
   test_unknown_and_malformed_states();
   test_expiry();
   test_sweep();
   test_bounded_and_never_evicts_a_live_login();
   test_rejects_unstarted_and_duplicate();
   test_reset_clears();
   kb_oidc_login_store_reset();
   printf("test_kb_oidc_login_store: ok\n");
   return 0;
}
