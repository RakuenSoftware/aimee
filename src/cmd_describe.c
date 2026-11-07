/* cmd_describe.c: auto-describe projects via agent delegation */
#include "aimee.h"
#include "platform_path.h"
#include "commands.h"
#include <aimee/workspace/workspace.h>
#include "agent.h"
#include "config.h"
#include "cmd_describe_platform.h"
#include "kb_client.h"
#include "cJSON.h"
#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>
#include <time.h>

/* --- description file paths --- */

static void describe_path(const char *project_name, char *buf, size_t len)
{
   snprintf(buf, len, "%s/projects/%s.md", config_default_dir(), project_name);
}

/* --- read existing description --- */

char *describe_read(const char *project_name)
{
   char path[MAX_PATH_LEN];
   describe_path(project_name, path, sizeof(path));

   FILE *f = fopen(path, "r");
   if (!f)
      return NULL;

   fseek(f, 0, SEEK_END);
   long sz = ftell(f);
   fseek(f, 0, SEEK_SET);

   if (sz <= 0 || sz > MAX_FILE_SIZE)
   {
      fclose(f);
      return NULL;
   }

   char *buf = malloc((size_t)sz + 1);
   if (!buf)
   {
      fclose(f);
      return NULL;
   }

   size_t n = fread(buf, 1, (size_t)sz, f);
   buf[n] = '\0';
   fclose(f);
   return buf;
}

/* --- extract commit from frontmatter --- */

/* --- get HEAD commit for a project --- */

/* --- check if project has structural changes since last describe --- */

/* --- check if project needs describing (returns 1 if work needed) --- */

/* --- describe prompt --- */

/* --- describe a single project (runs in child process) --- */

/* --- style analysis --- */

/* --- parallel execution with concurrency cap --- */

/* describe_job_t is defined in cmd_describe_platform.h */

/* --- cmd_describe --- */
