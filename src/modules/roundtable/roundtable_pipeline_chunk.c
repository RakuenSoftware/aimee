/* roundtable_pipeline_chunk.c: budget-sized chunk planning + synthesis assembly
 * for the authoring pipeline. Pure logic over a retained origin string. See
 * roundtable_pipeline_chunk.h. */

#include "roundtable_pipeline_chunk.h"

#include "cJSON.h"
#include "headers/module_json_call.h"

#include <aimee/roundtable/module_api.h>
#include <stdlib.h>

/* A 16 MiB artifact is the largest thing this carries. */
#define RTP_CHUNK_MAX_BODY (16u * 1024u * 1024u)
/* Off the interactive path, but a pipeline pass waits on it. */
#define RTP_CHUNK_TIMEOUT_MS 15000

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

/* Ask the module where the artifact splits, then fill in the digests here.
 *
 * Fail-closed is a single chunk covering the whole origin: that is what the C
 * code did for an artifact that fits, it never loses content, and the caller's
 * over_budget/truncated handling still applies. Returning zero chunks would make
 * an unreachable module look like an empty artifact. */
static int chunk_plan_via_module(const char *origin, int len, int budget_bytes, int assembly_budget,
                                 rtp_chunk_plan_t *out, rtp_assembly_t *asm_out)
{
   cJSON *request = cJSON_CreateObject();
   if (!request)
      return -1;
   cJSON_AddStringToObject(request, "origin", origin);
   cJSON_AddNumberToObject(request, "budget_bytes", budget_bytes);
   cJSON_AddNumberToObject(request, "assembly_budget", assembly_budget);

   cJSON *reply =
       aimee_module_json_call(AIMEE_ROUNDTABLE_EVENT_CHUNK_PLAN, AIMEE_ROUNDTABLE_STAGE_CHUNK_PLAN,
                              request, RTP_CHUNK_MAX_BODY, RTP_CHUNK_TIMEOUT_MS, NULL);
   if (!reply)
      return -1;

   const cJSON *plan = cJSON_GetObjectItemCaseSensitive(reply, "plan");
   const cJSON *spans = plan ? cJSON_GetObjectItemCaseSensitive(plan, "chunks") : NULL;
   if (!cJSON_IsArray(spans))
   {
      cJSON_Delete(reply);
      return -1;
   }

   out->over_budget = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(plan, "over_budget"));
   out->truncated = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(plan, "truncated"));

   const cJSON *span = NULL;
   cJSON_ArrayForEach(span, spans)
   {
      if (out->count >= RTP_MAX_CHUNKS)
         break;
      const cJSON *off = cJSON_GetObjectItemCaseSensitive(span, "offset");
      const cJSON *slen = cJSON_GetObjectItemCaseSensitive(span, "len");
      if (!cJSON_IsNumber(off) || !cJSON_IsNumber(slen))
      {
         cJSON_Delete(reply);
         return -1;
      }
      int o = off->valueint, l = slen->valueint;
      /* A span the module reports must lie inside the artifact we sent. A bad
       * offset would otherwise hash and slice out of bounds. */
      if (o < 0 || l < 0 || o > len || l > len - o)
      {
         cJSON_Delete(reply);
         return -1;
      }
      rtp_chunk_t *c = &out->chunks[out->count];
      c->index = out->count;
      c->offset = o;
      c->len = l;
      rtp_chunk_hash(origin + o, l, c->hash, sizeof(c->hash));
      out->count++;
   }
   if (out->count == 0)
   {
      cJSON_Delete(reply);
      return -1;
   }

   if (asm_out)
   {
      const cJSON *a = cJSON_GetObjectItemCaseSensitive(reply, "assembly");
      const cJSON *sel = a ? cJSON_GetObjectItemCaseSensitive(a, "selected") : NULL;
      const cJSON *omi = a ? cJSON_GetObjectItemCaseSensitive(a, "omitted") : NULL;
      memset(asm_out, 0, sizeof(*asm_out));
      asm_out->budget_bytes = assembly_budget;
      asm_out->over_budget = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(a, "over_budget"));
      const cJSON *it = NULL;
      cJSON_ArrayForEach(it, sel)
      {
         if (asm_out->selected_count >= RTP_MAX_CHUNKS || !cJSON_IsNumber(it))
            break;
         int idx = it->valueint;
         if (idx < 0 || idx >= out->count)
            continue;
         asm_out->selected[asm_out->selected_count++] = idx;
         asm_out->used_bytes += out->chunks[idx].len;
      }
      cJSON_ArrayForEach(it, omi)
      {
         if (asm_out->omitted_count >= RTP_MAX_CHUNKS || !cJSON_IsNumber(it))
            break;
         int idx = it->valueint;
         if (idx < 0 || idx >= out->count)
            continue;
         asm_out->omitted[asm_out->omitted_count++] = idx;
      }
   }

   cJSON_Delete(reply);
   return 0;
}

/* Plan with the module; on any failure fall back to the single whole-artifact
 * chunk described above. */
static void chunk_plan_whole(const char *origin, int len, rtp_chunk_plan_t *out)
{
   out->count = 1;
   out->chunks[0].index = 0;
   out->chunks[0].offset = 0;
   out->chunks[0].len = len;
   rtp_chunk_hash(origin, len, out->chunks[0].hash, sizeof(out->chunks[0].hash));
}

int rtp_chunk_plan(const char *origin, int budget_bytes, rtp_chunk_plan_t *out)
{
   return rtp_chunk_plan_with_assembly(origin, budget_bytes, 0, out, NULL);
}

int rtp_chunk_plan_with_assembly(const char *origin, int budget_bytes, int assembly_budget,
                                 rtp_chunk_plan_t *out, rtp_assembly_t *asm_out)
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

   if (chunk_plan_via_module(origin, len, budget_bytes, assembly_budget, out, asm_out) != 0)
   {
      memset(out->chunks, 0, sizeof(out->chunks));
      out->count = 0;
      out->over_budget = 0;
      out->truncated = 0;
      chunk_plan_whole(origin, len, out);
      if (asm_out)
         rtp_assembly_build(out, assembly_budget, asm_out);
   }
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
