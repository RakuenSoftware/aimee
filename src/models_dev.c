/* models_dev.c: models.dev registry ingestion */
#include "aimee.h"
#include "aimee_home.h"
#include "log.h"
#include "models_dev.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define MODELS_DEV_URL         "https://models.dev/api.json"
#define MODELS_DEV_CACHE_REL   ".cache/aimee/models_dev.json"
#define MODELS_DEV_CACHE_TTL_S 86400

#ifndef AIMEE_DATADIR
#define AIMEE_DATADIR "/usr/local/share/aimee"
#endif

/* Resolve the bundled offline snapshot.
 *
 * This used to return the compiled-in AIMEE_DATADIR path and nothing else,
 * while model_registry.c carried a SECOND resolver for the same artifact that
 * honoured $AIMEE_MODELS_DEV_SNAPSHOT and the in-tree data/ directory. The price
 * path goes through this one, so on any uninstalled run - a dev tree, CI, a
 * container where DATADIR was never populated - the snapshot was unreachable,
 * every catalog price resolved to "unknown", and the fleet silently fell back to
 * the heuristic, which publishes no prices at all. Nothing surfaced: the price
 * fields simply fail closed and are omitted.
 *
 * Search the same places, in the same order, as the other resolver. Falls back
 * to the DATADIR path when nothing exists so a diagnostic still names the
 * location an installed build expects. */
const char *models_dev_snapshot_path(void)
{
   static char path[512];
   if (path[0])
      return path;

   const char *env = getenv("AIMEE_MODELS_DEV_SNAPSHOT");
   if (env && env[0])
   {
      snprintf(path, sizeof(path), "%s", env);
      return path;
   }

   const char *candidates[] = {AIMEE_DATADIR "/models_dev_snapshot.json",
                               "data/models_dev_snapshot.json", "../data/models_dev_snapshot.json",
                               NULL};
   for (int i = 0; candidates[i]; i++)
   {
      if (access(candidates[i], R_OK) == 0)
      {
         snprintf(path, sizeof(path), "%s", candidates[i]);
         return path;
      }
   }
   snprintf(path, sizeof(path), "%s", candidates[0]);
   return path;
}

const char *models_dev_overrides_path(void)
{
   static char path[512];
   if (path[0])
      return path;
   const char *base = aimee_home();
   if (!base || !base[0])
      return NULL;
   snprintf(path, sizeof(path), "%s/model_overrides.json", base);
   return path;
}

static int models_dev_cache_path(char *buf, size_t buf_len)
{
   const char *home = getenv("HOME");
   if (!home || !home[0])
      return -1;
   snprintf(buf, buf_len, "%s/%s", home, MODELS_DEV_CACHE_REL);
   return 0;
}

static int models_dev_cache_is_fresh(void)
{
   char path[512];
   if (models_dev_cache_path(path, sizeof(path)) != 0)
      return 0;
   struct stat st;
   if (stat(path, &st) != 0)
      return 0;
   int age = (int)(time(NULL) - st.st_mtime);
   return age >= 0 && age < MODELS_DEV_CACHE_TTL_S;
}

int models_dev_refresh(void)
{
   if (models_dev_cache_is_fresh())
   {
      aimee_log(LOG_INFO, "model_meta", "models.dev cache is fresh, skipping download");
      return 0;
   }

   char cache_path[512];
   if (models_dev_cache_path(cache_path, sizeof(cache_path)) != 0)
   {
      aimee_log(LOG_INFO, "model_meta", "could not determine cache path");
      return -1;
   }

   char parent[512];
   snprintf(parent, sizeof(parent), "%s", cache_path);
   char *slash = strrchr(parent, '/');
   if (slash)
   {
      *slash = '\0';
      mkdir(parent, 0755);
   }

   char tmp_path[560];
   snprintf(tmp_path, sizeof(tmp_path), "%s.tmp.XXXXXX", cache_path);
   int tmp_fd = mkstemp(tmp_path);
   if (tmp_fd < 0)
   {
      aimee_log(LOG_INFO, "model_meta", "could not create temp file for models.dev download");
      return -1;
   }
   close(tmp_fd);

   aimee_log(LOG_INFO, "model_meta", "refreshing models.dev registry from %s", MODELS_DEV_URL);

   char cmd[1024];
   snprintf(cmd, sizeof(cmd), "curl -s --max-time 10 -o '%s' '%s'", tmp_path, MODELS_DEV_URL);
   int rc = system(cmd);
   if (rc != 0)
   {
      aimee_log(LOG_INFO, "model_meta", "models.dev download failed (curl exit %d)", rc);
      unlink(tmp_path);
      return -1;
   }

   if (rename(tmp_path, cache_path) != 0)
   {
      aimee_log(LOG_INFO, "model_meta", "could not atomically rename models.dev cache");
      unlink(tmp_path);
      return -1;
   }

   aimee_log(LOG_INFO, "model_meta", "models.dev registry cached at %s", cache_path);
   return 0;
}

int models_dev_capability_get(const char *provider, const char *model_id, model_capability_t *out)
{
   (void)provider;
   (void)model_id;
   (void)out;
   /* Stub: full ingestion from models.dev cache is future work. */
   return 0;
}
