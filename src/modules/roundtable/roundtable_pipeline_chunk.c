/* roundtable_pipeline_chunk.c: budget-sized chunk planning + synthesis assembly
 * for the authoring pipeline. Pure logic over a retained origin string. See
 * roundtable_pipeline_chunk.h. */

#include "roundtable_pipeline_chunk.h"

#include <stdio.h>
#include <string.h>

void rtp_chunk_hash(const char *data, int n, char *out, int out_cap)
{
   unsigned long long h = 1469598103934665603ULL;
   for (int i = 0; i < n; i++)
   {
      h ^= (unsigned long long)(unsigned char)data[i];
      h *= 1099511628211ULL;
   }
   snprintf(out, (size_t)out_cap, "%016llx", h);
}

int rtp_chunk_needed(const char *origin, int budget_bytes)
{
   if (!origin)
      return 0;
   if (budget_bytes <= 0)
      return 0;
   return (int)strlen(origin) > budget_bytes ? 1 : 0;
}

int rtp_chunk_plan(const char *origin, int budget_bytes, rtp_chunk_plan_t *out)
{
   if (!out)
      return -1;
   memset(out, 0, sizeof(*out));
   if (!origin)
      origin = "";
   int len = (int)strlen(origin);
   out->origin_len = len;
   out->budget_bytes = budget_bytes;
   rtp_chunk_hash(origin, len, out->origin_hash, sizeof(out->origin_hash));

   /* Whole artifact fits, or no budget set -> one chunk. */
   if (budget_bytes <= 0 || len <= budget_bytes)
   {
      out->chunks[0].index = 0;
      out->chunks[0].offset = 0;
      out->chunks[0].len = len;
      rtp_chunk_hash(origin, len, out->chunks[0].hash, sizeof(out->chunks[0].hash));
      out->count = 1;
      return 0;
   }

   int pos = 0;
   while (pos < len && out->count < RTP_MAX_CHUNKS)
   {
      int remaining = len - pos;
      int take = remaining < budget_bytes ? remaining : budget_bytes;
      /* Prefer to break on the last newline within the window so a chunk holds
       * whole lines; fall back to a hard cut if a single line is too long. */
      if (take == budget_bytes)
      {
         int brk = -1;
         for (int i = pos + take - 1; i > pos; i--)
         {
            if (origin[i] == '\n')
            {
               brk = i;
               break;
            }
         }
         if (brk > pos)
            take = brk - pos + 1; /* include the newline */
         else
            out->over_budget = 1; /* an indivisible line exceeds the budget */
      }
      rtp_chunk_t *c = &out->chunks[out->count];
      c->index = out->count;
      c->offset = pos;
      c->len = take;
      rtp_chunk_hash(origin + pos, take, c->hash, sizeof(c->hash));
      out->count++;
      pos += take;
   }
   if (pos < len)
      out->truncated = 1; /* ran out of chunk slots */
   return 0;
}

int rtp_chunk_verify(const char *origin, const rtp_chunk_plan_t *plan)
{
   if (!origin || !plan)
      return 0;
   int len = (int)strlen(origin);
   char h[RTP_CHUNK_HASH_LEN];
   rtp_chunk_hash(origin, len, h, sizeof(h));
   if (strcmp(h, plan->origin_hash) != 0)
      return 0;
   for (int i = 0; i < plan->count; i++)
   {
      const rtp_chunk_t *c = &plan->chunks[i];
      if (c->offset < 0 || c->len < 0 || c->offset + c->len > len)
         return 0;
      rtp_chunk_hash(origin + c->offset, c->len, h, sizeof(h));
      if (strcmp(h, c->hash) != 0)
         return 0;
   }
   return 1;
}

int rtp_assembly_build(const rtp_chunk_plan_t *plan, int budget_bytes, rtp_assembly_t *out)
{
   if (!plan || !out)
      return -1;
   memset(out, 0, sizeof(*out));
   out->budget_bytes = budget_bytes;
   for (int i = 0; i < plan->count; i++)
   {
      int sz = plan->chunks[i].len;
      if (budget_bytes > 0 && sz > budget_bytes && out->selected_count == 0)
      {
         /* the very first span already overflows the synthesis budget. */
         out->over_budget = 1;
      }
      if (budget_bytes <= 0 || out->used_bytes + sz <= budget_bytes)
      {
         out->selected[out->selected_count++] = plan->chunks[i].index;
         out->used_bytes += sz;
      }
      else
      {
         out->omitted[out->omitted_count++] = plan->chunks[i].index;
      }
   }
   return 0;
}
