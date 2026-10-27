/* Bounded domain delivery log. Model history lives in the chat owner. */
#include "mirror.h"
#include <pthread.h>
#include <stdio.h>
#include <string.h>

static mirror_entry_t g_entries[MAX_MIRROR_ENTRIES];
static unsigned g_head, g_count;
static pthread_mutex_t g_mutex = PTHREAD_MUTEX_INITIALIZER;

void mirror_record(const char *platform, const char *chat_id, const char *direction,
                   const char *text)
{
   pthread_mutex_lock(&g_mutex);
   mirror_entry_t *entry = g_entries + g_head;
   entry->ts = time(NULL);
   snprintf(entry->platform, sizeof entry->platform, "%s", platform ? platform : "");
   snprintf(entry->chat_id, sizeof entry->chat_id, "%s", chat_id ? chat_id : "");
   snprintf(entry->direction, sizeof entry->direction, "%s", direction ? direction : "");
   snprintf(entry->text, sizeof entry->text, "%s", text ? text : "");
   g_head = (g_head + 1) % MAX_MIRROR_ENTRIES;
   if (g_count < MAX_MIRROR_ENTRIES)
      g_count++;
   pthread_mutex_unlock(&g_mutex);
}

int mirror_get_recent(mirror_entry_t *entries, int max_entries)
{
   if (!entries || max_entries <= 0)
      return 0;
   pthread_mutex_lock(&g_mutex);
   int count = g_count < (unsigned)max_entries ? (int)g_count : max_entries;
   unsigned start = (g_head + MAX_MIRROR_ENTRIES - g_count) % MAX_MIRROR_ENTRIES;
   for (int i = 0; i < count; i++)
      entries[i] = g_entries[(start + (unsigned)i) % MAX_MIRROR_ENTRIES];
   pthread_mutex_unlock(&g_mutex);
   return count;
}
