/* sweep_scope.c: scope→areas partition + caps for the deepening sweep (pure).
 * See headers/sweep.h. */
#include "sweep.h"

#include <stdio.h>
#include <string.h>

void sweep_caps_defaults(sweep_caps_t *caps)
{
   if (!caps)
      return;
   caps->max_areas = 40;
   caps->max_files_per_area = 50;
   caps->max_calls_per_area = 2;
   caps->max_items_per_area = 10;
   caps->wall_area_s = 60;
   caps->wall_sweep_s = 1800;
}

static int glob_match(const char *path, const char *glob)
{
   if (!path || !glob)
      return 0;
   size_t gl = strlen(glob);
   /* a dir followed by slash-star-star (or slash-star) -> directory prefix match */
   if (gl >= 3 && glob[gl - 1] == '*' && glob[gl - 2] == '*' && glob[gl - 3] == '/')
      return strncmp(path, glob, gl - 3) == 0 && (path[gl - 3] == '\0' || path[gl - 3] == '/');
   if (gl >= 1 && glob[gl - 1] == '*')
      return strncmp(path, glob, gl - 1) == 0; /* trailing * -> prefix */
   return strcmp(path, glob) == 0;             /* otherwise exact */
}

int sweep_path_allowed(const char *path, const char *const *globs, int nglobs)
{
   if (!path || !path[0] || !globs)
      return 0;
   for (int i = 0; i < nglobs; i++)
      if (globs[i] && glob_match(path, globs[i]))
         return 1;
   return 0;
}

/* Directory part of a path (everything before the last '/'); "" if none. */
static void path_dir(const char *path, char *out, size_t cap)
{
   out[0] = '\0';
   const char *slash = strrchr(path, '/');
   if (!slash)
      return;
   size_t n = (size_t)(slash - path);
   if (n >= cap)
      n = cap - 1;
   memcpy(out, path, n);
   out[n] = '\0';
}

int sweep_partition(const char *const *paths, int n, int max_files_per_area, int *out_area)
{
   if (!paths || !out_area || n < 0 || max_files_per_area <= 0)
      return -1;
   int area = -1, count = 0;
   char cur[MAX_PATH_LEN] = "";
   char dir[MAX_PATH_LEN];
   for (int i = 0; i < n; i++)
   {
      if (!paths[i])
         return -1;
      path_dir(paths[i], dir, sizeof(dir));
      if (area < 0 || strcmp(dir, cur) != 0 || count >= max_files_per_area)
      {
         area++;
         snprintf(cur, sizeof(cur), "%s", dir);
         count = 0;
      }
      out_area[i] = area;
      count++;
   }
   return area + 1; /* area count (0 when n==0) */
}
