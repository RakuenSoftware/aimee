/* msg_session_disable.c: see msg_session_disable.h. Derives the per-identity
 * session key the gateway seam is keyed by.
 *
 * The circuit breaker this file is named after has MOVED: it is state, so it now
 * lives in the Go economizer module (server-go/modules/economizer/breaker.go),
 * where the side that writes it and the side that reads it are the same side of
 * the bus. What remains is the key derivation, which is not state -- it is a pure
 * function of the request's identity material. The file keeps the old name for
 * now; renaming it is a separate, mechanical change. */
#include "msg_session_disable.h"

#include <ctype.h>
#include <stdio.h> /* snprintf — do not rely on a transitive include */
#include <string.h>

#include "harness_memory_common.h" /* hmem_sha256_hex — vendored SHA-256, no OpenSSL */

/* The session key is the first 16 hex of a SHA-256 digest; bind that assumption to
 * the vendored helper's output width so a silent migration to a different digest
 * can't slip a truncated wrong-hash key through. */
_Static_assert(HMEM_HASH_HEX_LEN >= 17, "session key needs >= 16 hex chars + NUL");

static int is_16_lower_hex(const char *s)
{
   if (!s)
      return 0;
   for (int i = 0; i < 16; i++)
   {
      char c = s[i];
      if (c == '\0') /* shorter than 16 — stop before reading past the NUL */
         return 0;
      if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
         return 0;
   }
   return s[16] == '\0'; /* exactly 16 chars, no trailing garbage */
}

msg_session_key_status_t msg_session_key_resolve(const char *hdr_session_id, const char *bearer,
                                                 const char *auth_identity,
                                                 char key[MSG_SESSION_KEY_LEN])
{
   char full[HMEM_HASH_HEX_LEN];
   int header_present_but_bad = 0;

   /* NULL-first: only validate a header when we have an identity to validate it
    * against (never SHA256(NULL)). */
   if (auth_identity && auth_identity[0] && hdr_session_id && hdr_session_id[0])
   {
      if (is_16_lower_hex(hdr_session_id))
      {
         hmem_sha256_hex(auth_identity, strlen(auth_identity), full);
         if (strncmp(hdr_session_id, full, 16) == 0)
         {
            memcpy(key, hdr_session_id, 16);
            key[16] = '\0';
            return MSG_SESSION_KEY_RESOLVED;
         }
      }
      /* Present but malformed or mismatched -> treat as absent, note the anomaly so
       * the caller can WARN (IP-rate-limited). An attacker's forged header for
       * another identity lands here and falls back to the attacker's own bearer. */
      header_present_but_bad = 1;
   }

   if (bearer && bearer[0])
   {
      hmem_sha256_hex(bearer, strlen(bearer), full);
      memcpy(key, full, 16);
      key[16] = '\0';
      return header_present_but_bad ? MSG_SESSION_KEY_BEARER_BAD_HDR : MSG_SESSION_KEY_RESOLVED;
   }

   /* Identity-less: no key, caller must pass through pristine with no disable state. */
   key[0] = '\0';
   return MSG_SESSION_KEY_NONE;
}
