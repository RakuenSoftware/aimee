/* cmd_init.c: stack detection and .aimee-rules generation for aimee init */
#include "commands.h"
#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define RULES_FILE ".aimee-rules"

static int has_file_in_dir(const char *dir, const char *filename)
{
   char path[MAX_PATH_LEN];
   snprintf(path, sizeof(path), "%s/%s", dir, filename);
   return access(path, F_OK) == 0;
}

static int dir_has_c_files(const char *dir)
{
   DIR *d = opendir(dir);
   if (!d)
      return 0;
   struct dirent *entry;
   while ((entry = readdir(d)) != NULL)
   {
      size_t n = strlen(entry->d_name);
      if (n > 2 && strcmp(entry->d_name + n - 2, ".c") == 0)
      {
         closedir(d);
         return 1;
      }
   }
   closedir(d);
   return 0;
}

int detect_project_stacks(const char *dir, stack_info_t out[], int max_stacks)
{
   int n = 0;

   /* CMake/C++: check before plain C to avoid double-counting */
   if (n < max_stacks && has_file_in_dir(dir, "CMakeLists.txt"))
   {
      out[n].name = "CMake/C++";
      out[n].build_cmd = "cmake --build .";
      out[n].test_cmd = "ctest";
      n++;
   }
   /* C: Makefile + at least one .c file in the root */
   if (n < max_stacks && has_file_in_dir(dir, "Makefile") && dir_has_c_files(dir))
   {
      out[n].name = "C";
      out[n].build_cmd = "make";
      out[n].test_cmd = "make test";
      n++;
   }
   /* Rust */
   if (n < max_stacks && has_file_in_dir(dir, "Cargo.toml"))
   {
      out[n].name = "Rust";
      out[n].build_cmd = "cargo build";
      out[n].test_cmd = "cargo test";
      n++;
   }
   /* Go */
   if (n < max_stacks && has_file_in_dir(dir, "go.mod"))
   {
      out[n].name = "Go";
      out[n].build_cmd = "go build ./...";
      out[n].test_cmd = "go test ./...";
      n++;
   }
   /* TypeScript: package.json + tsconfig.json */
   if (n < max_stacks && has_file_in_dir(dir, "package.json") &&
       has_file_in_dir(dir, "tsconfig.json"))
   {
      out[n].name = "TypeScript";
      out[n].build_cmd = "npm run build";
      out[n].test_cmd = "npm test";
      n++;
   }
   else if (n < max_stacks && has_file_in_dir(dir, "package.json"))
   {
      /* Node.js without TypeScript */
      out[n].name = "Node.js";
      out[n].build_cmd = "npm install";
      out[n].test_cmd = "npm test";
      n++;
   }
   /* Python */
   if (n < max_stacks &&
       (has_file_in_dir(dir, "pyproject.toml") || has_file_in_dir(dir, "setup.py")))
   {
      out[n].name = "Python";
      out[n].build_cmd = "pip install -e .";
      out[n].test_cmd = "pytest";
      n++;
   }

   return n;
}

int write_aimee_rules_file(const char *dir, const stack_info_t *stacks, int count)
{
   char path[MAX_PATH_LEN];
   snprintf(path, sizeof(path), "%s/%s", dir, RULES_FILE);

   /* Idempotent: skip if already exists */
   if (access(path, F_OK) == 0)
      return 1;

   FILE *f = fopen(path, "w");
   if (!f)
      return -1;

   const char *project_name = strrchr(dir, '/');
   project_name = project_name ? project_name + 1 : dir;

   fprintf(f, "# Project: %s\n\n", project_name);
   fprintf(f, "## Detected stack\n");

   if (count > 0)
   {
      for (int i = 0; i < count; i++)
         fprintf(f, "- %s\n", stacks[i].name);
   }
   else
   {
      fprintf(f, "- (none detected)\n");
   }

   fprintf(f, "\n## Verification\n");
   if (count > 0)
   {
      for (int i = 0; i < count; i++)
      {
         fprintf(f, "- Build: `%s`\n", stacks[i].build_cmd);
         fprintf(f, "- Test: `%s`\n", stacks[i].test_cmd);
      }
   }
   else
   {
      fprintf(f, "- Run: (configure in this file)\n");
   }

   fprintf(f, "\n## Working agreement\n"
              "- Prefer small, reviewable changes\n"
              "- Update tests alongside code changes\n");

   fclose(f);
   return 0;
}
