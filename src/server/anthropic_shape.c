/* anthropic_shape.c: Anthropic request-shaping helpers (§3 cache-aware shaping).
 * Pure cJSON, no provider/config dependency, so it is unit-tested directly.
 * See agent_anthropic_set_system in agent_protocol.h. */
#include "aimee.h" /* MAX_PATH_LEN, pulled in before agent_types.h */
#include "agent_protocol.h"
#include "cJSON.h"
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

/* The per-turn, query-derived pre-injection envelope. It is VOLATILE — it must
 * never sit inside a cache_control block, or the prompt cache is busted every
 * turn (and the stable prefix never benefits). The cache boundary goes on the
 * stable text that precedes it. */
#define AIMEE_CONTEXT_MARKER "<aimee-context"

static cJSON *text_block(const char *text, size_t len, int with_cache_control)
{
   cJSON *block = cJSON_CreateObject();
   cJSON_AddStringToObject(block, "type", "text");
   /* cJSON copies; bound the length explicitly so we can pass a substring. */
   char *buf = malloc(len + 1);
   if (!buf)
   {
      cJSON_Delete(block);
      return NULL;
   }
   memcpy(buf, text, len);
   buf[len] = '\0';
   cJSON_AddStringToObject(block, "text", buf);
   free(buf);
   if (with_cache_control)
   {
      cJSON *cc = cJSON_AddObjectToObject(block, "cache_control");
      cJSON_AddStringToObject(cc, "type", "ephemeral");
   }
   return block;
}

void agent_anthropic_set_system(cJSON *req, const char *system_prompt, int cache_marking)
{
   if (!req || !system_prompt || !system_prompt[0])
      return;
   if (!cache_marking)
   {
      cJSON_AddStringToObject(req, "system", system_prompt);
      return;
   }

   /* Split at the volatile <aimee-context> envelope: cache ONLY the stable text
    * before it (persona / system / history), leaving the query-derived context —
    * and everything after it — uncached. */
   const char *ctx = strstr(system_prompt, AIMEE_CONTEXT_MARKER);
   size_t prefix_len = ctx ? (size_t)(ctx - system_prompt) : strlen(system_prompt);

   /* Is there any non-whitespace, cacheable content before the volatile context? */
   int prefix_has_content = 0;
   for (size_t i = 0; i < prefix_len; i++)
   {
      if (!isspace((unsigned char)system_prompt[i]))
      {
         prefix_has_content = 1;
         break;
      }
   }

   if (!ctx)
   {
      /* No volatile context — the whole system is stable; cache it as one block. */
      cJSON *arr = cJSON_CreateArray();
      cJSON *b = text_block(system_prompt, prefix_len, 1);
      if (b)
         cJSON_AddItemToArray(arr, b);
      cJSON_AddItemToObject(req, "system", arr);
      return;
   }
   if (!prefix_has_content)
   {
      /* System is entirely the volatile envelope (no stable prefix to cache);
       * emit the plain string so nothing volatile is marked cacheable. */
      cJSON_AddStringToObject(req, "system", system_prompt);
      return;
   }
   /* Stable prefix + volatile context: cache the prefix only, leave the rest
    * (the <aimee-context> envelope onward) uncached. */
   cJSON *arr = cJSON_CreateArray();
   cJSON *stable = text_block(system_prompt, prefix_len, 1);
   cJSON *volatile_blk = text_block(ctx, strlen(ctx), 0);
   if (stable && volatile_blk)
   {
      cJSON_AddItemToArray(arr, stable);
      cJSON_AddItemToArray(arr, volatile_blk);
      cJSON_AddItemToObject(req, "system", arr);
   }
   else
   {
      /* Allocation failure — fall back to the safe plain string. */
      cJSON_Delete(arr);
      cJSON_Delete(stable);
      cJSON_Delete(volatile_blk);
      cJSON_AddStringToObject(req, "system", system_prompt);
   }
}
