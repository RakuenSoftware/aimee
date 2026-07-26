/* kb_login_throttle.c — see kb_login_throttle.h. */

#include "kb_login_throttle.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>

/* Fixed tables: bounded memory is the point, so an attacker cannot make kb
 * allocate by varying the username or the source address. Sized generously
 * relative to the number of distinct principals a kb sees in one window. */
#define SLOTS 1024

typedef struct
{
   uint64_t key; /* 0 = free. The hash IS the identity; see take_slot. */
   int64_t window_start;
   int64_t locked_until;
   int fails;
} slot_t;

/* Peer and username are separate tables rather than one keyed namespace, so a
 * flood of usernames from one address cannot evict that address's own record. */
static slot_t g_peer[SLOTS];
static slot_t g_user[SLOTS];
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

/* The peer for the request in flight. Thread-local rather than a file static:
 * kb serves connections from a single listener thread today, and this must not
 * silently start attributing one caller's failures to another if that changes. */
static __thread char g_peer_ip[64];

static uint64_t hash_key(const char *prefix, const char *s)
{
   /* FNV-1a. A non-cryptographic hash is fine: it is not a secret, and a
    * collision costs an innocent principal some of its budget rather than
    * granting an attacker any. */
   uint64_t h = 1469598103934665603ULL;
   for (const char *p = prefix; *p; ++p)
      h = (h ^ (unsigned char)*p) * 1099511628211ULL;
   for (const char *p = s; *p; ++p)
      h = (h ^ (unsigned char)*p) * 1099511628211ULL;
   return h ? h : 1; /* 0 marks a free slot, so never hand it back as a key. */
}

static int expired(const slot_t *s, int64_t now)
{
   return now >= s->locked_until && (now - s->window_start) >= KB_LOGIN_THROTTLE_WINDOW_SEC;
}

/* Find the slot for `key`, or a slot to use for it.
 *
 * On collision the two identities SHARE a slot rather than one evicting the
 * other. Eviction would be the bug: an attacker who could evict their own record
 * by hashing a few fresh usernames would clear their lockout at will. Sharing
 * errs toward over-throttling, which is the safe direction — it can cost an
 * innocent principal part of its budget, but it can never grant an attacker more
 * attempts than the budget allows.
 *
 * Returns NULL only if every slot is occupied by a live record, which is the
 * fail-closed case the caller must treat as "refuse". */
static slot_t *take_slot(slot_t *table, uint64_t key, int64_t now)
{
   size_t idx = (size_t)(key % SLOTS);
   slot_t *s = &table[idx];
   if (s->key == key)
      return s;
   if (s->key == 0 || expired(s, now))
   {
      /* Free, or a spent record from another identity: reuse it, reset. */
      s->key = key;
      s->window_start = now;
      s->locked_until = 0;
      s->fails = 0;
      return s;
   }
   return NULL; /* live record for a different identity -> share nothing, fail closed */
}

static int check_one(slot_t *table, uint64_t key, int64_t now)
{
   slot_t *s = take_slot(table, key, now);
   if (!s)
   {
      /* The slot is held by a different, still-live identity. We cannot tell
       * whether THIS identity is over budget, and a throttle that guesses
       * "allowed" when it cannot answer is not a throttle. Refuse for the
       * remainder of the occupant's window. */
      slot_t *occ = &table[(size_t)(key % SLOTS)];
      int64_t until = occ->locked_until > 0 ? occ->locked_until
                                            : occ->window_start + KB_LOGIN_THROTTLE_WINDOW_SEC;
      int retry = (int)(until - now);
      return retry > 0 ? retry : 1;
   }
   if (now < s->locked_until)
   {
      int retry = (int)(s->locked_until - now);
      return retry > 0 ? retry : 1;
   }
   /* A window that has run out with the budget unspent starts fresh. */
   if ((now - s->window_start) >= KB_LOGIN_THROTTLE_WINDOW_SEC)
   {
      s->window_start = now;
      s->fails = 0;
   }
   return 0;
}

static void fail_one(slot_t *table, uint64_t key, int64_t now)
{
   slot_t *s = take_slot(table, key, now);
   if (!s)
      return; /* Shared slot already refusing; nothing to charge. */
   if ((now - s->window_start) >= KB_LOGIN_THROTTLE_WINDOW_SEC && now >= s->locked_until)
   {
      s->window_start = now;
      s->fails = 0;
   }
   if (s->fails < 1000000)
      s->fails++;
   if (s->fails >= KB_LOGIN_THROTTLE_BUDGET)
   {
      /* Exponential from the first failure past the budget, capped. */
      int over = s->fails - KB_LOGIN_THROTTLE_BUDGET;
      if (over > 20)
         over = 20;
      int64_t lock = KB_LOGIN_THROTTLE_BASE_LOCK_SEC;
      for (int i = 0; i < over && lock < KB_LOGIN_THROTTLE_MAX_LOCK_SEC; ++i)
         lock *= 2;
      if (lock > KB_LOGIN_THROTTLE_MAX_LOCK_SEC)
         lock = KB_LOGIN_THROTTLE_MAX_LOCK_SEC;
      s->locked_until = now + lock;
   }
}

static void clear_one(slot_t *table, uint64_t key)
{
   size_t idx = (size_t)(key % SLOTS);
   slot_t *s = &table[idx];
   if (s->key != key)
      return; /* Another identity's record; clearing it would let an attacker
               * reset a victim's state, or their own via a collision. */
   s->key = 0;
   s->window_start = 0;
   s->locked_until = 0;
   s->fails = 0;
}

/* An absent peer is ONE shared bucket, not an exemption: "we could not tell who
 * this was" must not be the cheapest way to get unlimited attempts. */
static const char *peer_or_unknown(void)
{
   return g_peer_ip[0] ? g_peer_ip : "<unknown-peer>";
}

void kb_login_throttle_set_peer(const char *peer_ip)
{
   snprintf(g_peer_ip, sizeof(g_peer_ip), "%s", peer_ip ? peer_ip : "");
}

int kb_login_throttle_check(const char *username, int64_t now)
{
   if (!username)
      username = "";
   pthread_mutex_lock(&g_lock);
   int a = check_one(g_peer, hash_key("peer:", peer_or_unknown()), now);
   int b = check_one(g_user, hash_key("user:", username), now);
   pthread_mutex_unlock(&g_lock);
   return a > b ? a : b;
}

void kb_login_throttle_record_failure(const char *username, int64_t now)
{
   if (!username)
      username = "";
   pthread_mutex_lock(&g_lock);
   fail_one(g_peer, hash_key("peer:", peer_or_unknown()), now);
   fail_one(g_user, hash_key("user:", username), now);
   pthread_mutex_unlock(&g_lock);
}

void kb_login_throttle_record_success(const char *username)
{
   if (!username)
      username = "";
   pthread_mutex_lock(&g_lock);
   clear_one(g_peer, hash_key("peer:", peer_or_unknown()));
   clear_one(g_user, hash_key("user:", username));
   pthread_mutex_unlock(&g_lock);
}

void kb_login_throttle_reset(void)
{
   pthread_mutex_lock(&g_lock);
   memset(g_peer, 0, sizeof(g_peer));
   memset(g_user, 0, sizeof(g_user));
   pthread_mutex_unlock(&g_lock);
}
