/* kb_memory_embed.c: KB-local memory embedding hook. */
#include "aimee.h"
#include "memory.h"
#include "modules/memory/memory_platform.h"

int platform_memory_background_embed_set_suppressed(int suppressed)
{
   (void)suppressed;
   return 0;
}

void platform_memory_background_embed(int64_t memory_id, const char *command)
{
   (void)memory_embed(memory_id, command);
}
