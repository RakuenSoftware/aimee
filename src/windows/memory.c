/* windows/memory.c: Windows memory stubs; regex is unavailable, so gates are permissive. */
#include "aimee.h"
#include "memory.h"
#include "modules/memory/memory_platform.h"
#include <io.h>

int gate_check_sensitive(const char *content, char *redacted, size_t redacted_cap)
{
   (void)content;
   (void)redacted;
   (void)redacted_cap;
   return 0;
}

int gate_check_ephemeral(const char *content)
{
   (void)content;
   return 0;
}

int gate_has_evidence_markers(const char *content)
{
   (void)content;
   return 0;
}

const char *memory_scan_content(char *content, size_t content_len)
{
   (void)content;
   (void)content_len;
   return "normal";
}

void platform_memory_background_embed(int64_t memory_id, const char *command)
{
   /* No background fork on Windows; embedding is skipped. */
   (void)memory_id;
   (void)command;
}

int platform_memory_background_embed_set_suppressed(int suppressed)
{
   (void)suppressed;
   return 0;
}
