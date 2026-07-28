/* Do the generated accessors agree with a directly-loaded config_t?
 * A generated accessor that compiles but reads the wrong offset would be worse
 * than the leak it replaces, so compare field-by-field against the struct. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "config.h"

int main(void)
{
   config_t *cfg = calloc(1, sizeof(*cfg));
   assert(cfg);
   config_load(cfg);

   int checked = 0;
#define CK_INT(f)                                                                                  \
   do                                                                                              \
   {                                                                                               \
      if (config_##f() != cfg->f)                                                                  \
      {                                                                                            \
         printf("MISMATCH int %s: %d vs %d\n", #f, config_##f(), cfg->f);                          \
         return 1;                                                                                 \
      }                                                                                            \
      checked++;                                                                                   \
   } while (0)
#define CK_STR(f)                                                                                  \
   do                                                                                              \
   {                                                                                               \
      if (strcmp(config_##f(), cfg->f) != 0)                                                       \
      {                                                                                            \
         printf("MISMATCH str %s: '%s' vs '%s'\n", #f, config_##f(), cfg->f);                      \
         return 1;                                                                                 \
      }                                                                                            \
      checked++;                                                                                   \
   } while (0)

   /* Generated accessors */
   CK_INT(workspace_count);
   CK_INT(subagent_ban_enabled);
   CK_INT(embedding_dim);
   CK_INT(memory_maintenance_enabled);
   CK_INT(memory_maintenance_trigger_secs);
   CK_STR(db1_path);
   CK_STR(provider);
   CK_STR(default_persona);
   CK_STR(claude_model);
   CK_STR(openai_endpoint);

   /* Hand-written accessors the generator deliberately skipped: it must not
    * shadow logic that applies precedence or defaulting the raw field lacks. */
   CK_INT(memory_routing_enabled);
   CK_INT(typed_facts_enabled);
   CK_INT(audit_worm_enabled);

   printf("accessor parity: %d field(s) match the loaded struct\n", checked);
   free(cfg);
   return 0;
}
