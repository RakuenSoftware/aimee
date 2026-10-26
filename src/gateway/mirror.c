#include "mirror.h"
#include "gateway_ir.h"
#include <pthread.h>
#include <stdio.h>
#include <string.h>
static mirror_entry_t g_entries[MAX_MIRROR_ENTRIES];
static unsigned g_head, g_count;
static pthread_mutex_t g_mutex = PTHREAD_MUTEX_INITIALIZER;
int mirror_record_ir(const session_source_t *source, const char *direction,
                     const aimee_message_t *message)
{
   aimee_message_t owned = {0};
   if (!source || !direction || gateway_message_clone(message, &owned))
      return -1;
   pthread_mutex_lock(&g_mutex);
   mirror_entry_t *e = g_entries + g_head;
   aimee_message_free(&e->message);
   *e = (mirror_entry_t){.ts = time(NULL), .source = *source, .message = owned};
   snprintf(e->direction, sizeof e->direction, "%s", direction);
   g_head = (g_head + 1) % MAX_MIRROR_ENTRIES;
   if (g_count < MAX_MIRROR_ENTRIES)
      g_count++;
   pthread_mutex_unlock(&g_mutex);
   return 0;
}
void mirror_entries_free(mirror_entry_t *entries, int count)
{
   for (int i = 0; entries && i < count; i++)
      aimee_message_free(&entries[i].message);
}
int mirror_get_recent(mirror_entry_t *entries, int max_entries)
{
   if (!entries || max_entries <= 0)
      return 0;
   pthread_mutex_lock(&g_mutex);
   int n = g_count < (unsigned)max_entries ? (int)g_count : max_entries;
   unsigned start = (g_head + MAX_MIRROR_ENTRIES - g_count) % MAX_MIRROR_ENTRIES;
   int i = 0;
   for (; i < n; i++)
   {
      const mirror_entry_t *source = g_entries + (start + (unsigned)i) % MAX_MIRROR_ENTRIES;
      entries[i] = (mirror_entry_t){.ts = source->ts, .source = source->source};
      memcpy(entries[i].direction, source->direction, sizeof source->direction);
      if (gateway_message_clone(&source->message, &entries[i].message))
         break;
   }
   pthread_mutex_unlock(&g_mutex);
   if (i != n)
   {
      mirror_entries_free(entries, i);
      return -1;
   }
   return n;
}
void mirror_clear(void)
{
   pthread_mutex_lock(&g_mutex);
   mirror_entries_free(g_entries, MAX_MIRROR_ENTRIES);
   memset(g_entries, 0, sizeof g_entries);
   g_head = g_count = 0;
   pthread_mutex_unlock(&g_mutex);
}
