/* pairing.c: in-memory pairing registry with mutex protection */
#include "pairing.h"
#include "log.h"
#include "platform_random.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>

static pairing_record_t g_pairings[MAX_PAIRINGS];
static pthread_mutex_t g_mutex = PTHREAD_MUTEX_INITIALIZER;
static int g_next_slot = 0; /* simple round-robin over array slots */

static int find_by_code_locked(const char *code)
{
   for (int i = 0; i < MAX_PAIRINGS; i++)
   {
      if (g_pairings[i].code[0] != '\0' && strcmp(g_pairings[i].code, code) == 0)
         return i;
   }
   return -1;
}

static int find_by_identity_locked(const char *platform, const char *user_id)
{
   for (int i = 0; i < MAX_PAIRINGS; i++)
   {
      if (g_pairings[i].code[0] != '\0' && strcmp(g_pairings[i].platform, platform) == 0 &&
          strcmp(g_pairings[i].user_id, user_id) == 0)
         return i;
   }
   return -1;
}

static int random_unique_code_locked(char out[16])
{
   for (int attempt = 0; attempt < 128; attempt++)
   {
      uint32_t draw = 0;
      if (platform_random_bytes(&draw, sizeof(draw)) != 0)
         return -1;
      if (draw >= UINT32_C(4294000000))
         continue;
      char candidate[16];
      snprintf(candidate, sizeof(candidate), "%06u", draw % UINT32_C(1000000));
      if (find_by_code_locked(candidate) < 0)
      {
         snprintf(out, 16, "%s", candidate);
         return 0;
      }
   }
   return -1;
}

int pairing_issue(const char *platform, const char *user_id, int ttl_seconds, char *code_out,
                  size_t code_size)
{
   if (!platform || !user_id || !code_out || code_size < 7)
      return -1;

   pthread_mutex_lock(&g_mutex);

   /* check for existing pending code for this identity */
   int slot = find_by_identity_locked(platform, user_id);
   if (slot >= 0)
   {
      /* reuse existing slot */
      snprintf(code_out, code_size, "%s", g_pairings[slot].code);
      g_pairings[slot].expires_at = time(NULL) + ttl_seconds;
      pthread_mutex_unlock(&g_mutex);
      return 0;
   }

   /* find next free slot */
   int start = g_next_slot;
   do
   {
      if (g_pairings[g_next_slot].code[0] == '\0')
         break;
      g_next_slot = (g_next_slot + 1) % MAX_PAIRINGS;
   } while (g_next_slot != start);

   /* all slots full */
   if (g_pairings[g_next_slot].code[0] != '\0')
   {
      pthread_mutex_unlock(&g_mutex);
      LOG_WARN("pairing", "no free slot for new pairing");
      return -1;
   }

   slot = g_next_slot;
   g_next_slot = (g_next_slot + 1) % MAX_PAIRINGS;

   if (random_unique_code_locked(g_pairings[slot].code) != 0)
   {
      pthread_mutex_unlock(&g_mutex);
      LOG_WARN("pairing", "secure random code generation failed");
      return -1;
   }
   snprintf(g_pairings[slot].platform, sizeof(g_pairings[slot].platform), "%s", platform);
   snprintf(g_pairings[slot].user_id, sizeof(g_pairings[slot].user_id), "%s", user_id);
   g_pairings[slot].expires_at = time(NULL) + ttl_seconds;
   g_pairings[slot].approved = 0;

   snprintf(code_out, code_size, "%s", g_pairings[slot].code);
   pthread_mutex_unlock(&g_mutex);
   return 0;
}

int pairing_approve(const char *code)
{
   if (!code)
      return -1;

   pthread_mutex_lock(&g_mutex);
   int slot = find_by_code_locked(code);
   if (slot < 0)
   {
      pthread_mutex_unlock(&g_mutex);
      return -1;
   }
   if (time(NULL) > g_pairings[slot].expires_at)
   {
      pthread_mutex_unlock(&g_mutex);
      return -1;
   }
   g_pairings[slot].approved = 1;
   pthread_mutex_unlock(&g_mutex);
   return 0;
}

int pairing_revoke(const char *platform, const char *user_id)
{
   if (!platform || !user_id)
      return -1;

   pthread_mutex_lock(&g_mutex);
   int slot = find_by_identity_locked(platform, user_id);
   if (slot >= 0)
      g_pairings[slot].approved = -1;
   pthread_mutex_unlock(&g_mutex);
   return slot >= 0 ? 0 : -1;
}

int pairing_is_approved(const char *platform, const char *user_id)
{
   if (!platform || !user_id)
      return 0;

   pthread_mutex_lock(&g_mutex);
   int slot = find_by_identity_locked(platform, user_id);
   if (slot < 0)
   {
      pthread_mutex_unlock(&g_mutex);
      return 0;
   }
   int result =
       (g_pairings[slot].approved == 1 && time(NULL) <= g_pairings[slot].expires_at) ? 1 : 0;
   pthread_mutex_unlock(&g_mutex);
   return result;
}

int pairing_list(pairing_record_t *records, int max_records)
{
   if (!records || max_records <= 0)
      return 0;

   pthread_mutex_lock(&g_mutex);
   int count = 0;
   for (int i = 0; i < MAX_PAIRINGS && count < max_records; i++)
   {
      if (g_pairings[i].code[0] != '\0')
         records[count++] = g_pairings[i];
   }
   pthread_mutex_unlock(&g_mutex);
   return count;
}
