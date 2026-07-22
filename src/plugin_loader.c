/* plugin_loader.c: four-source plugin discovery with last-writer-wins precedence.
 *
 * Part of the pluggable context engine surface described in
 * docs/proposals/pending/plugin-extension-surface-and-context-engine.md.
 */
#include "headers/plugin_loader.h"
#include "headers/log.h"
#include "config.h"
#include "headers/aimee_home.h"
#include <dirent.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>

/* --- Install prefix state --- */

static char g_install_prefix[MAX_PATH_LEN] = {0};

void plugin_loader_set_install_prefix(const char *prefix)
{
   if (!prefix || !prefix[0])
      return;
   snprintf(g_install_prefix, sizeof(g_install_prefix), "%s", prefix);
}

/* Resolve the bundled plugins directory.
 * Priority: explicit set > $AIMEE_INSTALL_PREFIX/plugins > ./plugins */
static void bundled_plugins_dir(char *out, size_t cap)
{
   if (g_install_prefix[0])
   {
      snprintf(out, cap, "%s/plugins", g_install_prefix);
      return;
   }
   const char *env = getenv("AIMEE_INSTALL_PREFIX");
   if (env && env[0])
   {
      snprintf(out, cap, "%s/plugins", env);
      return;
   }
   /* Fall back to ./plugins relative to cwd (build-tree / test runs) */
   snprintf(out, cap, "%s", "plugins");
}

/* --- scan_dir implementation --- */

int plugin_loader_scan_dir(const char *dir, plugin_t *out, int max_out)
{
   if (!dir || !dir[0] || !out || max_out <= 0)
      return 0;

   DIR *d = opendir(dir);
   if (!d)
      return 0; /* directory absent — not an error */

   int count = 0;
   struct dirent *ent;
   while ((ent = readdir(d)) != NULL && count < max_out)
   {
      if (ent->d_name[0] == '.')
         continue; /* skip . .. and hidden */

      char plugin_dir[MAX_PATH_LEN];
      snprintf(plugin_dir, sizeof(plugin_dir), "%s/%s", dir, ent->d_name);

      /* Must be a directory */
      struct stat st;
      if (stat(plugin_dir, &st) != 0 || !S_ISDIR(st.st_mode))
         continue;

      char err[256];
      plugin_t tmp;
      if (plugin_manifest_parse(plugin_dir, &tmp, err, sizeof(err)) != 0)
      {
         LOG_DEBUG("plugin_loader", "skipping %s: %s", plugin_dir, err);
         continue;
      }

      out[count++] = tmp;
   }
   closedir(d);
   return count;
}

/* --- Name-keyed merge (last-writer-wins) --- */

/* Find the index of a plugin with the given name in the array, or -1. */
static int find_by_name(const plugin_t *arr, int count, const char *name)
{
   for (int i = 0; i < count; i++)
      if (strcmp(arr[i].name, name) == 0)
         return i;
   return -1;
}

/* Merge src[] into dst[]; later calls override earlier ones by name.
 * source_label is "user" or "project" for INFO logging. */
static int merge_plugins(plugin_t *dst, int *dst_count, int dst_max, const plugin_t *src,
                         int src_count, const char *source_label)
{
   for (int i = 0; i < src_count; i++)
   {
      int idx = find_by_name(dst, *dst_count, src[i].name);
      if (idx >= 0)
      {
         LOG_INFO("plugin_loader", "plugin '%s': %s override from %s", src[i].name, source_label,
                  src[i].source_path);
         dst[idx] = src[i];
      }
      else
      {
         if (*dst_count >= dst_max)
         {
            LOG_WARN("plugin_loader", "plugin list full, skipping '%s'", src[i].name);
            continue;
         }
         dst[(*dst_count)++] = src[i];
      }
   }
   return 0;
}

/* --- Required-env check --- */

static int check_required_env(const plugin_t *p)
{
   for (int i = 0; i < p->required_env_count; i++)
   {
      if (!p->required_env[i][0])
         continue;
      if (getenv(p->required_env[i]) == NULL)
      {
         LOG_WARN("plugin_loader", "plugin '%s': skipping — required env var %s is not set",
                  p->name, p->required_env[i]);
         return -1;
      }
   }
   return 0;
}

/* --- discover_all --- */

int plugin_loader_discover_all(char *err_buf, size_t err_len)
{
   /* plugin_t is ~70 KB, so plugin_t[PLUGIN_MAX_PLUGINS] is ~4.3 MB. Two such
    * arrays live at once here (merged + a scratch found buffer); on the stack
    * that overflows the 8 MB main-thread stack and segfaults at function entry.
    * Heap-allocate both. plugin_t is flat POD (only inline char arrays / ints —
    * no owned pointers), so the merged[x]=found[i] value-copy is complete and a
    * single found buffer can be reused across the scan passes (each scan fully
    * populates found[0..n-1], which is all the merge reads). */
   plugin_t *merged = calloc(PLUGIN_MAX_PLUGINS, sizeof(*merged));
   plugin_t *found = calloc(PLUGIN_MAX_PLUGINS, sizeof(*found));
   if (!merged || !found)
   {
      free(merged);
      free(found);
      if (err_buf)
         snprintf(err_buf, err_len, "plugin discovery: out of memory");
      return 0; /* non-fatal: startup continues without plugins */
   }
   int merged_count = 0;

   /* 1. Bundled plugins */
   {
      char bdir[MAX_PATH_LEN];
      bundled_plugins_dir(bdir, sizeof(bdir));

      int n = plugin_loader_scan_dir(bdir, found, PLUGIN_MAX_PLUGINS);
      for (int i = 0; i < n && merged_count < PLUGIN_MAX_PLUGINS; i++)
         merged[merged_count++] = found[i];

      if (n > 0)
         LOG_DEBUG("plugin_loader", "bundled: found %d plugin(s) in %s", n, bdir);
   }

   /* 2. User plugins (<aimee_home>/plugins/) */
   {
      const char *home = aimee_home();
      if (home && home[0])
      {
         char udir[MAX_PATH_LEN];
         snprintf(udir, sizeof(udir), "%s/plugins", home);

         int n = plugin_loader_scan_dir(udir, found, PLUGIN_MAX_PLUGINS);
         if (n > 0)
         {
            LOG_DEBUG("plugin_loader", "user: found %d plugin(s) in %s", n, udir);
            merge_plugins(merged, &merged_count, PLUGIN_MAX_PLUGINS, found, n, "user");
         }
      }
   }

   /* 3. Project plugins (.aimee/plugins/) — gated by env var */
   {
      const char *gate = getenv("AIMEE_ENABLE_PROJECT_PLUGINS");
      if (gate && gate[0] && strcmp(gate, "0") != 0)
      {
         char pdir[MAX_PATH_LEN];
         snprintf(pdir, sizeof(pdir), "%s", ".aimee/plugins");

         int n = plugin_loader_scan_dir(pdir, found, PLUGIN_MAX_PLUGINS);
         if (n > 0)
         {
            LOG_INFO("plugin_loader", "project: found %d plugin(s) in %s", n, pdir);
            merge_plugins(merged, &merged_count, PLUGIN_MAX_PLUGINS, found, n, "project");
         }
      }
      else
      {
         LOG_DEBUG("plugin_loader",
                   "project plugins skipped (set AIMEE_ENABLE_PROJECT_PLUGINS=1 to enable)");
      }
   }

   /* 4. Load each discovered plugin (skip if required env is absent) */
   int failures = 0;
   for (int i = 0; i < merged_count; i++)
   {
      if (!merged[i].enabled)
         continue;
      if (check_required_env(&merged[i]) != 0)
         continue; /* warning already logged */

      char lerr[256] = {0};
      if (plugin_load_and_register(&merged[i], lerr, sizeof(lerr)) != 0)
      {
         LOG_WARN("plugin_loader", "failed to load plugin '%s': %s", merged[i].name,
                  lerr[0] ? lerr : "unknown error");
         failures++;
      }
   }

   if (failures > 0 && err_buf)
      snprintf(err_buf, err_len, "%d plugin(s) failed to load", failures);

   free(merged);
   free(found);
   return 0; /* non-fatal: individual plugin failures do not abort startup */
}
