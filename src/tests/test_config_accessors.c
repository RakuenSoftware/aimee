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

   /* Indexed accessors must agree row-for-row, and must refuse an out-of-range
    * index rather than read adjacent memory. */
   for (int i = 0; i < 64; i++)
   {
      if (strcmp(config_workspaces(i), cfg->workspaces[i]) != 0)
      {
         printf("MISMATCH workspaces[%d]: '%s' vs '%s'\n", i, config_workspaces(i),
                cfg->workspaces[i]);
         return 1;
      }
      checked++;
   }
   if (config_workspaces(-1)[0] != 0 || config_workspaces(64)[0] != 0 ||
       config_workspaces(100000)[0] != 0)
   {
      printf("out-of-range index did not return an empty row\n");
      return 1;
   }
   checked += 3;

   /* Setters must round-trip through the config file, not just through a
    * buffer: read a value, change it, read it back, restore it. A setter that
    * wrote somewhere the getter does not read would otherwise look like it
    * worked. */
   {
      int before = config_memory_maintenance_trigger_secs();
      int probe = before == 4242 ? 4243 : 4242;
      if (config_set_memory_maintenance_trigger_secs(probe) == 0)
      {
         if (config_memory_maintenance_trigger_secs() != probe)
         {
            printf("setter did not round-trip: wrote %d, read %d\n", probe,
                   config_memory_maintenance_trigger_secs());
            return 1;
         }
         checked++;
         config_set_memory_maintenance_trigger_secs(before);
         if (config_memory_maintenance_trigger_secs() != before)
         {
            printf("restore failed: wanted %d, got %d\n", before,
                   config_memory_maintenance_trigger_secs());
            return 1;
         }
         checked++;
      }
   }

   /* Struct-array elements: the offsetof arithmetic (base + index*stride +
    * member offset) is the part most likely to be silently wrong, so compare
    * every slot against the struct and check the bounds guard. */
   for (int i = 0; i < CONFIG_MCP_MAX_CLIENTS; i++)
   {
      if (strcmp(config_mcp_client_name(i), cfg->mcp_clients[i].name) != 0 ||
          config_mcp_client_command_count(i) != cfg->mcp_clients[i].command_count)
      {
         printf("MISMATCH mcp_clients[%d]\n", i);
         return 1;
      }
      checked += 2;
   }
   for (int i = 0; i < CRON_JOBS_MAX; i++)
   {
      if (strcmp(config_cron_job_id(i), cfg->cron_jobs[i].id) != 0 ||
          config_cron_job_enabled(i) != cfg->cron_jobs[i].enabled)
      {
         printf("MISMATCH cron_jobs[%d]\n", i);
         return 1;
      }
      checked += 2;
   }
   if (config_mcp_client_name(-1)[0] != 0 ||
       config_mcp_client_name(CONFIG_MCP_MAX_CLIENTS)[0] != 0 || config_cron_job_enabled(-1) != 0 ||
       config_cron_job_enabled(CRON_JOBS_MAX) != 0)
   {
      printf("struct-array bounds guard failed\n");
      return 1;
   }
   checked += 4;

   printf("accessor parity: %d field(s) match the loaded struct\n", checked);
   free(cfg);
   return 0;
}
