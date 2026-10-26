/* Canonical ring-buffer delivery log; callers own deep-cloned snapshots. */
#pragma once
#include "gateway_platform.h"
#include <aimee/ir/aimee_ir.h>
#include <time.h>
#define MAX_MIRROR_ENTRIES 256
typedef struct mirror_entry
{
   time_t ts;
   session_source_t source;
   char direction[8];
   aimee_message_t message;
} mirror_entry_t;
int mirror_record_ir(const session_source_t *source, const char *direction,
                     const aimee_message_t *message);
int mirror_get_recent(mirror_entry_t *entries, int max_entries);
void mirror_entries_free(mirror_entry_t *entries, int count);
void mirror_clear(void);
