/* mirror.h: ring-buffer delivery log */
#pragma once
#include <time.h>

#define MAX_MIRROR_ENTRIES 256

typedef struct mirror_entry
{
   time_t ts;
   char platform[32];
   char chat_id[128];
   char direction[8];
   char text[512];
} mirror_entry_t;

/* Record a message delivery event. Overwrites oldest entry when full. */
void mirror_record(const char *platform, const char *chat_id, const char *direction,
                   const char *text);

/* Copy the most recent N entries into the provided array.
 * Entries are returned in chronological order (oldest first).
 * Returns the number of entries written (0 if empty). */
int mirror_get_recent(mirror_entry_t *entries, int max_entries);