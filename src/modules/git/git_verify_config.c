#include "git_verify_internal.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

static int find_makefile_subdir_local(const char *project_root, char *subdir, size_t subdir_len)
{
   char path[MAX_PATH_LEN];
   struct stat st;
   snprintf(path, sizeof(path), "%s/Makefile", project_root);
   if (stat(path, &st) == 0 && S_ISREG(st.st_mode))
   {
      snprintf(subdir, subdir_len, "%s", "");
      return 0;
   }
   snprintf(path, sizeof(path), "%s/src/Makefile", project_root);
   if (stat(path, &st) == 0 && S_ISREG(st.st_mode))
   {
      snprintf(subdir, subdir_len, "%s", "src");
      return 0;
   }
   return -1;
}

static int makefile_has_target(const char *project_root, const char *target, char *subdir,
                               size_t subdir_len)
{
   if (find_makefile_subdir_local(project_root, subdir, subdir_len) != 0)
      return 0;

   char path[MAX_PATH_LEN];
   if (subdir[0])
      snprintf(path, sizeof(path), "%s/%s/Makefile", project_root, subdir);
   else
      snprintf(path, sizeof(path), "%s/Makefile", project_root);

   FILE *f = fopen(path, "r");
   if (!f)
      return 0;

   char line[1024];
   size_t tlen = strlen(target);
   int found = 0;
   while (fgets(line, sizeof(line), f))
      if (strncmp(line, target, tlen) == 0 && line[tlen] == ':')
      {
         found = 1;
         break;
      }
   fclose(f);
   return found;
}

static int generated_make_run(const char *run)
{
   return strstr(run, "make -j$(nproc | awk '{print ($1>8)?8:$1}')") ||
          strstr(run, "make -j${AIMEE_VERIFY_MAKE_JOBS:-2}");
}

static int generated_make_step_set(const verify_config_t *cfg)
{
   if (!cfg || cfg->count < 3 || cfg->count > 4)
      return 0;

   int has_lint = 0, has_build = 0, has_test = 0, has_integrity = 0;
   for (int i = 0; i < cfg->count; i++)
   {
      const verify_step_t *s = &cfg->steps[i];
      if (!generated_make_run(s->run))
         return 0;
      if (strcmp(s->name, "lint") == 0)
         has_lint = 1;
      else if (strcmp(s->name, "build") == 0)
         has_build = 1;
      else if (strcmp(s->name, "unit-tests") == 0 || strcmp(s->name, "test") == 0)
         has_test = 1;
      else if (strcmp(s->name, "build-integrity") == 0)
         has_integrity = 1;
      else
         return 0;
   }
   return has_lint && has_build && has_test && (cfg->count == 3 || has_integrity);
}

void verify_config_prefer_verify_local(const char *project_root, verify_config_t *cfg)
{
   char subdir[MAX_PATH_LEN];
   if (!generated_make_step_set(cfg) ||
       !makefile_has_target(project_root, "verify-local", subdir, sizeof(subdir)))
      return;

   int enforce = cfg->enforce;
   memset(cfg->steps, 0, sizeof(cfg->steps));
   cfg->count = 1;
   cfg->enforce = enforce;
   snprintf(cfg->steps[0].name, MAX_STEP_NAME, "%s", "verify-local");
   if (subdir[0])
      snprintf(cfg->steps[0].run, MAX_STEP_CMD,
               "cd %s && make -j${AIMEE_VERIFY_MAKE_JOBS:-2} "
               "TEST_RUN_JOBS=${AIMEE_VERIFY_TEST_JOBS:-1} verify-local",
               subdir);
   else
      snprintf(cfg->steps[0].run, MAX_STEP_CMD,
               "make -j${AIMEE_VERIFY_MAKE_JOBS:-2} "
               "TEST_RUN_JOBS=${AIMEE_VERIFY_TEST_JOBS:-1} verify-local");
}
