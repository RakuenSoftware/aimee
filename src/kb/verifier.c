/* verifier.c: the pluggable Verifier seam for aimee-kb auth. See kb_verifier.h.
 * (distributed-mode-auth proposal, Phase 2.)
 *
 * The built-in kb-token verifier reproduces the v1 opaque-bearer check that used
 * to live inline in kb_http_route_ex(), now behind the seam: the presented
 * credential must equal the configured token in full, or its secret part when
 * the configured token is scope-describing. Scope is derived from the verified
 * token only (verify-then-trust). The compare is constant-time so a bearer
 * token cannot be recovered byte-by-byte through response-timing. */
#include "kb_verifier.h"
#include "kb_scope.h"
#include <aimee/core/connection/auth.h>

#include <pthread.h>
#include <stdio.h>
#include <string.h>

/* --- built-in kb-token verifier --- */

#define KB_VERIFIER_TOKEN_MAX 4096

int kb_verifier_kbtoken(const char *presented, const char *configured, kb_verify_result_t *out,
                        void *ctx)
{
   (void)ctx;
   if (!out || !configured || !configured[0])
      return 0;
   if (!presented)
      presented = "";

   char tkind[32] = "", tid[128] = "", tsecret[KB_VERIFIER_TOKEN_MAX + 1] = "";
   kb_scope_token_parse(configured, tkind, sizeof(tkind), tid, sizeof(tid), tsecret,
                        sizeof(tsecret));

   /* Accept the full configured token, or — for a scoped token — its secret
    * part. Constant-time, and the OR is not short-circuited on the secret. */
   int full_match = aimee_core_credential_equal(presented, configured);
   int secret_match = (tsecret[0] != '\0') & aimee_core_credential_equal(presented, tsecret);
   if (!(full_match | secret_match))
      return 0;

   memset(out, 0, sizeof(*out));
   /* Scope derived from the VERIFIED configured token, never from the caller. */
   snprintf(out->scope_kind, sizeof(out->scope_kind), "%s", tkind);
   snprintf(out->scope_id, sizeof(out->scope_id), "%s", tid);
   snprintf(out->subject, sizeof(out->subject), "%s", tid[0] ? tid : "owner");
   out->expiry = 0; /* opaque v1 tokens do not expire */
   return 1;
}

/* --- additive verifier registry --- */

#define KB_VERIFIER_MAX 8

typedef struct
{
   char name[32];
   kb_verifier_fn fn;
   void *ctx;
} verifier_entry_t;

static verifier_entry_t g_verifiers[KB_VERIFIER_MAX];
static int g_verifier_count = 0;
static int g_initialized = 0;
static pthread_mutex_t g_verifier_lock = PTHREAD_MUTEX_INITIALIZER;

/* Seed the registry with the built-in kb-token verifier as entry 0. Caller must
 * hold g_verifier_lock. Idempotent. */
static void ensure_default_locked(void)
{
   if (g_initialized)
      return;
   g_verifiers[0].fn = kb_verifier_kbtoken;
   g_verifiers[0].ctx = NULL;
   snprintf(g_verifiers[0].name, sizeof(g_verifiers[0].name), "kb-token");
   g_verifier_count = 1;
   g_initialized = 1;
}

int kb_verifier_register(const char *name, kb_verifier_fn fn, void *ctx)
{
   if (!name || !fn)
      return -1;
   pthread_mutex_lock(&g_verifier_lock);
   ensure_default_locked();
   /* Replace-by-name: re-registering an existing verifier (e.g. a config reload,
    * or re-adding "oidc" after a kb_verifier_reset) swaps it in place rather than
    * appending a duplicate. Index 0 (the built-in kb-token owner verifier) is
    * never overwritten, so the owner can never be displaced. */
   for (int i = 1; i < g_verifier_count; i++)
   {
      if (strcmp(g_verifiers[i].name, name) == 0)
      {
         g_verifiers[i].fn = fn;
         g_verifiers[i].ctx = ctx;
         pthread_mutex_unlock(&g_verifier_lock);
         return 0;
      }
   }
   if (g_verifier_count >= KB_VERIFIER_MAX)
   {
      pthread_mutex_unlock(&g_verifier_lock);
      return -1;
   }
   verifier_entry_t *e = &g_verifiers[g_verifier_count++];
   e->fn = fn;
   e->ctx = ctx;
   snprintf(e->name, sizeof(e->name), "%s", name);
   pthread_mutex_unlock(&g_verifier_lock);
   return 0;
}

void kb_verifier_reset(void)
{
   pthread_mutex_lock(&g_verifier_lock);
   g_initialized = 0;
   g_verifier_count = 0;
   ensure_default_locked();
   pthread_mutex_unlock(&g_verifier_lock);
}

int kb_verifier_authenticate(const char *presented, const char *configured, kb_verify_result_t *out,
                             char *which, size_t which_cap)
{
   kb_verify_result_t scratch;
   if (!out)
      out = &scratch;

   /* No owner credential configured → auth is open (pre-seam behavior). */
   if (!configured || !configured[0])
   {
      memset(out, 0, sizeof(*out));
      snprintf(out->subject, sizeof(out->subject), "owner");
      if (which && which_cap)
         snprintf(which, which_cap, "open");
      return 1;
   }

   /* Snapshot the registry under the lock, then run verifiers unlocked (a
    * verifier may do slow work, e.g. a future JWKS fetch). */
   pthread_mutex_lock(&g_verifier_lock);
   ensure_default_locked();
   verifier_entry_t snapshot[KB_VERIFIER_MAX];
   int count = g_verifier_count;
   memcpy(snapshot, g_verifiers, sizeof(verifier_entry_t) * (size_t)count);
   pthread_mutex_unlock(&g_verifier_lock);

   for (int i = 0; i < count; i++)
   {
      if (snapshot[i].fn(presented, configured, out, snapshot[i].ctx))
      {
         if (which && which_cap)
            snprintf(which, which_cap, "%s", snapshot[i].name);
         return 1;
      }
   }
   return 0;
}
