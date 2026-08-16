#include "kb_neardup.h"

#if defined(AIMEE_DB2_DISABLED)
int kb_neardup_propose(const char *project, const char *file_path, const char *match_path,
                       double jaccard)
{
   (void)project;
   (void)file_path;
   (void)match_path;
   (void)jaccard;
   return 0;
}
#else
#include "modules/db2/c/artifacts.h"
#include "log.h"
#include "cJSON.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *kb_neardup_source_id(const char *project, const char *path)
{
   size_t plen = strlen(project);
   size_t path_len = strlen(path);
   if (plen > SIZE_MAX - path_len - 2)
      return NULL;
   size_t len = plen + path_len + 2;
   char *out = malloc(len);
   if (!out)
      return NULL;
   snprintf(out, len, "%s:%s", project, path);
   return out;
}

int kb_neardup_propose(const char *project, const char *file_path, const char *match_path,
                       double jaccard)
{
   if (!project || !*project || !file_path || !*file_path || !match_path || !*match_path ||
       jaccard < 0.92)
      return 0;

   cJSON *payload = cJSON_CreateObject();
   if (!payload)
      return -1;
   cJSON_AddStringToObject(payload, "project", project);
   cJSON_AddStringToObject(payload, "source_path", file_path);
   cJSON_AddStringToObject(payload, "target_path", match_path);
   cJSON_AddStringToObject(payload, "proposed_relation", "supersedes");
   cJSON_AddNumberToObject(payload, "jaccard", jaccard);
   cJSON_AddBoolToObject(payload, "gate_decides", 1);
   char *json = cJSON_PrintUnformatted(payload);
   cJSON_Delete(payload);
   if (!json)
      return -1;

   char id[64];
   db2_artifact_gen_id(id, sizeof(id));
   int rc = db2_artifact_write(id, "kb_near_duplicate", "proposed", "kb_project", project,
                               "kb-ingest", jaccard, json);
   free(json);
   if (rc != 0)
      return rc;

   char *source_id = kb_neardup_source_id(project, file_path);
   char *target_id = kb_neardup_source_id(project, match_path);
   if (!source_id || !target_id)
   {
      free(source_id);
      free(target_id);
      return -1;
   }
   rc = db2_artifact_cite(id, "kb_file", source_id);
   if (rc == 0)
      rc = db2_artifact_cite(id, "kb_file", target_id);
   free(source_id);
   free(target_id);
   if (rc != 0)
      return rc;
   LOG_INFO("kb_neardup", "proposed supersedes: project=%s path=%s match=%s jaccard=%.3f", project,
            file_path, match_path, jaccard);
   return 0;
}
#endif
