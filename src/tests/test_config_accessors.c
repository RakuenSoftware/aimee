/* Do the generated accessors agree with a directly-loaded config_t?
 * A generated accessor that compiles but reads the wrong offset would be worse
 * than the leak it replaces, so compare field-by-field against the struct. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/stat.h>
#include "config.h"
#include "platform_test_util.h" /* platform_tmpdir: honour TMPDIR, do not leak into /tmp */

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
   CK_INT(embedder_dims);
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

   /* A read that FAILS must yield the field's DECLARED DEFAULT, not a zero seed.
    * config_field_read used to copy only when config_load returned 0, so every accessor
    * answered 0 on a failure — inverting every default-ON dial. For subagent_ban_enabled
    * (default ON) that turned a fail-closed guard fail-OPEN precisely when config was
    * broken.
    *
    * A merely MISSING file does not exercise this: config_load_file returns 0 ("defaults
    * are fine"). The reachable failure is strict mode + a validation error, which returns
    * -1 from config_load_file with defaults applied and field parsing not yet reached. */
   {
      char tmpl[256];
      snprintf(tmpl, sizeof tmpl, "%s/aimee-accessor-default-XXXXXX", platform_tmpdir());
      const char *dir = mkdtemp(tmpl);
      assert(dir);
      char cfgdir[512], cfgpath[600];
      snprintf(cfgdir, sizeof cfgdir, "%s/.config", dir);
      mkdir(cfgdir, 0755);
      snprintf(cfgdir, sizeof cfgdir, "%s/.config/aimee", dir);
      mkdir(cfgdir, 0755);
      snprintf(cfgpath, sizeof cfgpath, "%s/aimee.yaml", cfgdir);
      FILE *f = fopen(cfgpath, "w");
      assert(f);
      /* A type error the schema rejects -> issues > 0 -> strict mode aborts the load. */
      fputs("memory:\n  citations:\n    mode: 12345\n", f);
      fclose(f);

      /* The config path derives from HOME (AIMEE_HOME must be unset), as in test_config.c. */
      char *saved_home = getenv("HOME") ? strdup(getenv("HOME")) : NULL;
      char *saved_ahome = getenv("AIMEE_HOME") ? strdup(getenv("AIMEE_HOME")) : NULL;
      setenv("HOME", dir, 1);
      unsetenv("AIMEE_HOME");
      setenv("AIMEE_NO_CACHE", "1", 1); /* defeat the mtime/ino config cache */
      int saved_strict = g_config_strict;
      g_config_strict = 1;

      int ban = config_subagent_ban_enabled();
      int git = config_require_aimee_git();
      int wt = config_require_session_worktree();

      g_config_strict = saved_strict;
      unsetenv("AIMEE_NO_CACHE");
      if (saved_home)
      {
         setenv("HOME", saved_home, 1);
         free(saved_home);
      }
      else
         unsetenv("HOME");
      if (saved_ahome)
      {
         setenv("AIMEE_HOME", saved_ahome, 1);
         free(saved_ahome);
      }
      unlink(cfgpath);
      rmdir(cfgdir);
      snprintf(cfgdir, sizeof cfgdir, "%s/.config", dir);
      rmdir(cfgdir);
      rmdir(dir);

      if (ban != 1 || git != 1 || wt != 1)
      {
         printf("fail-open regression: a default-ON enforcement dial read as 0 when "
                "config_load failed (subagent_ban=%d require_aimee_git=%d "
                "require_session_worktree=%d); all three default ON\n",
                ban, git, wt);
         return 1;
      }
      printf("accessor defaults: default-ON dials stay ON when config_load fails\n");
   }

   free(cfg);
   return 0;
}
