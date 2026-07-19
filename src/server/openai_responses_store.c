/* openai_responses_store.c: in-process store for OpenAI responses conversation transcripts. */
#include "openai_responses_store.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define OPENAI_RESPONSES_STORE_MAX 256
#define OPENAI_RESPONSES_ID_MAX    128

static struct
{
   char id[OPENAI_RESPONSES_ID_MAX];
   char *transcript;
} g_entries[OPENAI_RESPONSES_STORE_MAX];
static int g_count = 0;
static int g_evict = 0; /* next slot to reuse once full (oldest-first, round-robin) */
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

void openai_responses_store_put(const char *resp_id, const char *transcript)
{
   if (!resp_id || !resp_id[0] || !transcript || !transcript[0])
      return;
   pthread_mutex_lock(&g_lock);
   for (int i = 0; i < g_count; i++)
   {
      if (strcmp(g_entries[i].id, resp_id) == 0)
      {
         free(g_entries[i].transcript);
         g_entries[i].transcript = strdup(transcript);
         pthread_mutex_unlock(&g_lock);
         return;
      }
   }
   int slot;
   if (g_count < OPENAI_RESPONSES_STORE_MAX)
   {
      slot = g_count++;
   }
   else
   {
      /* Full: evict the oldest entry (round-robin in insertion order), matching
       * openai_runs_store's oldest-slot reuse. Previously every insert past the
       * cap clobbered slot 0, so slots 1..MAX-1 went permanently stale and only
       * the newest transcript plus the 255 oldest stayed retrievable. */
      slot = g_evict;
      g_evict = (g_evict + 1) % OPENAI_RESPONSES_STORE_MAX;
   }
   free(g_entries[slot].transcript);
   snprintf(g_entries[slot].id, sizeof(g_entries[slot].id), "%s", resp_id);
   g_entries[slot].transcript = strdup(transcript);
   pthread_mutex_unlock(&g_lock);
}

int openai_responses_store_get(const char *resp_id, char *out, size_t out_n)
{
   if (out && out_n)
      out[0] = '\0';
   if (!resp_id || !resp_id[0] || !out || !out_n)
      return 0;
   int found = 0;
   pthread_mutex_lock(&g_lock);
   for (int i = 0; i < g_count; i++)
   {
      if (strcmp(g_entries[i].id, resp_id) == 0)
      {
         if (g_entries[i].transcript)
            snprintf(out, out_n, "%s", g_entries[i].transcript);
         found = 1;
         break;
      }
   }
   pthread_mutex_unlock(&g_lock);
   return found;
}

void openai_responses_store_reset(void)
{
   pthread_mutex_lock(&g_lock);
   for (int i = 0; i < g_count; i++)
   {
      free(g_entries[i].transcript);
      g_entries[i].transcript = NULL;
      g_entries[i].id[0] = '\0';
   }
   g_count = 0;
   g_evict = 0;
   pthread_mutex_unlock(&g_lock);
}
