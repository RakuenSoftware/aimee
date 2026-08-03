/* session_worktree_key.c: see session_worktree_key.h for why this is one
 * implementation shared by the client and the server rather than two.
 *
 * Deliberately freestanding — no logging, no config, no util.h — so the thin
 * client can link it without pulling in aimee-core. */
#include "session_worktree_key.h"
#include <stdio.h>
#include <string.h>

#define SESSION_WORKTREE_KEY_PREFIX_LEN 8
#define SESSION_WORKTREE_KEY_LEGACY_LEN 16

void session_worktree_key(const char *sid, char *out, size_t cap)
{
   if (!out || cap == 0)
      return;
   out[0] = '\0';
   if (!sid || !sid[0])
      return;
   /* The full id, not a prefix of it: this is what makes the key collision-free
    * for ids minted on a shared prefix. */
   unsigned long long h = 1469598103934665603ULL;
   for (const char *p = sid; *p; p++)
   {
      h ^= (unsigned char)*p;
      h *= 1099511628211ULL;
   }
   /* Readability only. Drops anything outside [A-Za-z0-9] rather than mapping it
    * to '_', so the output alphabet stays closed and no id can inject a path
    * separator or a leading '-' that git would read as an option. */
   char pre[SESSION_WORKTREE_KEY_PREFIX_LEN + 1];
   size_t k = 0;
   for (const char *p = sid; *p && k < SESSION_WORKTREE_KEY_PREFIX_LEN; p++)
   {
      char c = *p;
      if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))
         pre[k++] = c;
   }
   pre[k] = '\0';
   if (cap < SESSION_WORKTREE_KEY_MAX)
   {
      /* Never emit a TRUNCATED key: a short buffer would silently reintroduce
       * exactly the prefix collision this function exists to remove. */
      out[0] = '\0';
      return;
   }
   snprintf(out, cap, "%s%s%016llx", pre, k ? "-" : "", h);
}

void session_worktree_key_legacy(const char *sid, char *out, size_t cap)
{
   if (!out || cap == 0)
      return;
   out[0] = '\0';
   if (!sid)
      return;
   size_t n = 0;
   for (size_t i = 0; sid[i] && n < (size_t)SESSION_WORKTREE_KEY_LEGACY_LEN && n + 1 < cap; i++)
   {
      char c = sid[i];
      int ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
               c == '_' || c == '-';
      out[n++] = ok ? c : '_';
   }
   out[n] = '\0';
}

int session_worktree_key_is_key(const char *name)
{
   if (!name || !name[0])
      return 0;
   size_t len = strlen(name);

   /* Current shape: [<= 8 alnum>'-']<16 lowercase hex>. */
   if (len >= SESSION_WORKTREE_KEY_LEGACY_LEN)
   {
      const char *hex = name + (len - SESSION_WORKTREE_KEY_LEGACY_LEN);
      int hex_ok = 1;
      for (int i = 0; i < SESSION_WORKTREE_KEY_LEGACY_LEN; i++)
      {
         char c = hex[i];
         if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
         {
            hex_ok = 0;
            break;
         }
      }
      if (hex_ok)
      {
         size_t pre_len = len - SESSION_WORKTREE_KEY_LEGACY_LEN;
         if (pre_len == 0)
            return 1; /* bare hash, or a legacy key that happens to be all hex */
         if (pre_len <= SESSION_WORKTREE_KEY_PREFIX_LEN + 1 && name[pre_len - 1] == '-')
         {
            for (size_t i = 0; i + 1 < pre_len; i++)
            {
               char c = name[i];
               if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')))
                  return 0;
            }
            return 1;
         }
      }
   }

   /* Legacy shape: exactly 16 chars from the sanitized alphabet. A shorter id
    * produced a shorter key, so accept <= 16 too. */
   if (len <= SESSION_WORKTREE_KEY_LEGACY_LEN)
   {
      for (size_t i = 0; i < len; i++)
      {
         char c = name[i];
         int ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                  c == '_' || c == '-';
         if (!ok)
            return 0;
      }
      return 1;
   }
   return 0;
}
