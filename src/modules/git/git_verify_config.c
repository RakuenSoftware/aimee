#include "git_verify_internal.h"

#include <dirent.h>
#include <stdlib.h>
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

static int generated_verify_local_step(const verify_config_t *cfg)
{
   return cfg && cfg->count == 1 && strcmp(cfg->steps[0].name, "verify-local") == 0 &&
          strstr(cfg->steps[0].run, "AIMEE_VERIFY_MAKE_JOBS") != NULL &&
          strstr(cfg->steps[0].run, "verify-local") != NULL;
}

static int safe_module_dir(const char *name)
{
   if (!name || !name[0] || strlen(name) > 48)
      return 0;
   for (const unsigned char *p = (const unsigned char *)name; *p; p++)
      if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') ||
            *p == '_' || *p == '-' || *p == '.'))
         return 0;
   return strcmp(name, ".") != 0 && strcmp(name, "..") != 0;
}

static int compare_module_dirs(const void *a, const void *b)
{
   return strcmp((const char *)a, (const char *)b);
}

/* Auto-generated plans are repository discovery, not user policy. Keep them
 * complete when a repository contains top-level Go modules that its Makefile
 * gate does not know about yet. This also repairs already-generated project.yaml
 * files, which are intentionally never rewritten. Explicit/custom plans are
 * left alone by the caller's generated-plan check. */
static void append_go_module_steps(const char *project_root, verify_config_t *cfg)
{
   char modules[MAX_VERIFY_STEPS][MAX_STEP_NAME];
   int module_count = 0;
   char path[MAX_PATH_LEN];
   struct stat st;

   snprintf(path, sizeof(path), "%s/go.mod", project_root);
   if (stat(path, &st) == 0 && S_ISREG(st.st_mode))
      snprintf(modules[module_count++], MAX_STEP_NAME, "%s", ".");

   DIR *dir = opendir(project_root);
   if (dir)
   {
      struct dirent *entry;
      while ((entry = readdir(dir)) != NULL && module_count < MAX_VERIFY_STEPS)
      {
         if (!safe_module_dir(entry->d_name))
            continue;
         snprintf(path, sizeof(path), "%s/%s", project_root, entry->d_name);
         if (stat(path, &st) != 0 || !S_ISDIR(st.st_mode))
            continue;
         snprintf(path, sizeof(path), "%s/%s/go.mod", project_root, entry->d_name);
         if (stat(path, &st) != 0 || !S_ISREG(st.st_mode))
            continue;
         snprintf(modules[module_count++], MAX_STEP_NAME, "%s", entry->d_name);
      }
      closedir(dir);
   }

   qsort(modules, (size_t)module_count, sizeof(modules[0]), compare_module_dirs);
   for (int i = 0; i < module_count && cfg->count < MAX_VERIFY_STEPS; i++)
   {
      verify_step_t *step = &cfg->steps[cfg->count++];
      const char *label = strcmp(modules[i], ".") == 0 ? "root" : modules[i];
      snprintf(step->name, MAX_STEP_NAME, "go-test-%s", label);
      snprintf(step->run, MAX_STEP_CMD,
               "cd %s && go_bin=$(command -v go 2>/dev/null || true); "
               "if [ -z \"$go_bin\" ] && [ -x /usr/local/go/bin/go ]; then "
               "go_bin=/usr/local/go/bin/go; fi; [ -n \"$go_bin\" ] || { "
               "echo 'go test: Go toolchain unavailable'; exit 1; }; "
               "unset AIMEE_WFE_ENGINE AIMEE_WFE_HTTP_SOCKET; \"$go_bin\" test ./...",
               modules[i]);
   }
}

void verify_config_prefer_verify_local(const char *project_root, verify_config_t *cfg)
{
   const char *root = (project_root && project_root[0]) ? project_root : ".";
   char subdir[MAX_PATH_LEN];
   int generated = generated_verify_local_step(cfg);
   if (generated_make_step_set(cfg) &&
       makefile_has_target(root, "verify-local", subdir, sizeof(subdir)))
   {
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
      generated = 1;
   }
   if (generated)
      append_go_module_steps(root, cfg);
}
