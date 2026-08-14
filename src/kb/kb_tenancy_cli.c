#include "kb_tenancy_cli.h"

#include "cJSON.h"
#include "project.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static int project_attribute_failed(void)
{
   fprintf(stderr,
           "project attribute failed (org-admin required; both exact projects must exist)\n");
   return 1;
}

int kb_tenancy_cli_project_attribute(const char *code_project, const char *kb_project_text)
{
   char *end = NULL;
   errno = 0;
   unsigned long long project_id = strtoull(kb_project_text, &end, 10);
   if (errno != 0 || end == kb_project_text || *end != '\0' || project_id == 0 ||
       project_id > INT64_MAX)
      return project_attribute_failed();

   if (db2_project_attribute_code(code_project, (int64_t)project_id) != 0)
      return project_attribute_failed();

   cJSON *encoded = cJSON_CreateString(code_project);
   char *quoted = encoded ? cJSON_PrintUnformatted(encoded) : NULL;
   cJSON_Delete(encoded);
   if (!quoted)
      return project_attribute_failed();

   printf("{\"code_project\":%s,\"kb_project\":%llu}\n", quoted, project_id);
   free(quoted);
   return 0;
}
