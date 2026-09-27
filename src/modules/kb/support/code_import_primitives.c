#include "db2_code_import.h"

#include <stdio.h>
#include <string.h>

#define DB2_CODE_IMPORT_PATH_MAX 4096

static void slash_normalize(const char *in, char *out, size_t out_cap)
{
   if (!out || out_cap == 0)
      return;
   out[0] = '\0';
   if (!in)
      return;
   while (in[0] == '.' && (in[1] == '/' || in[1] == '\\'))
      in += 2;
   size_t n = 0;
   while (*in && n + 1 < out_cap)
   {
      char c = *in++;
      out[n++] = c == '\\' ? '/' : c;
   }
   out[n] = '\0';
}

int code_path_import_identity(const char *path, char *out, size_t out_cap)
{
   if (!path || !out || out_cap == 0)
      return -1;
   char normalized[DB2_CODE_IMPORT_PATH_MAX];
   slash_normalize(path, normalized, sizeof(normalized));
   size_t len = strlen(normalized);
   if (len > 3 && strcmp(normalized + len - 3, ".py") == 0)
   {
      normalized[len - 3] = '\0';
      len -= 3;
      if (len >= 9 && strcmp(normalized + len - 9, "/__init__") == 0)
         normalized[len - 9] = '\0';
      for (char *p = normalized; *p; p++)
         if (*p == '/')
            *p = '.';
   }
   snprintf(out, out_cap, "%s", normalized);
   return out[0] ? 0 : -1;
}

int code_import_identity(const char *importer_path, const char *raw_import, char *out,
                         size_t out_cap)
{
   if (!raw_import || !out || out_cap == 0)
      return -1;
   out[0] = '\0';
   size_t importer_len = importer_path ? strlen(importer_path) : 0;
   int python = importer_len > 3 && strcmp(importer_path + importer_len - 3, ".py") == 0;
   if (!python || raw_import[0] != '.')
   {
      slash_normalize(raw_import, out, out_cap);
      return out[0] ? 0 : -1;
   }

   char package[DB2_CODE_IMPORT_PATH_MAX];
   slash_normalize(importer_path, package, sizeof(package));
   char *slash = strrchr(package, '/');
   if (slash)
      *slash = '\0';
   else
      package[0] = '\0';
   for (char *p = package; *p; p++)
      if (*p == '/')
         *p = '.';

   size_t dots = 0;
   while (raw_import[dots] == '.')
      dots++;
   for (size_t level = 1; level < dots && package[0]; level++)
   {
      char *last = strrchr(package, '.');
      if (last)
         *last = '\0';
      else
         package[0] = '\0';
   }
   const char *suffix = raw_import + dots;
   if (package[0] && suffix[0])
      snprintf(out, out_cap, "%s.%s", package, suffix);
   else
      snprintf(out, out_cap, "%s%s", package, suffix);
   return out[0] ? 0 : -1;
}

int code_import_resolves_path(const char *importer_path, const char *raw_import,
                              const char *target_path)
{
   if (!importer_path || !raw_import || !target_path)
      return 0;
   char import_id[DB2_CODE_IMPORT_PATH_MAX];
   char target_id[DB2_CODE_IMPORT_PATH_MAX];
   if (code_import_identity(importer_path, raw_import, import_id, sizeof(import_id)) != 0 ||
       code_path_import_identity(target_path, target_id, sizeof(target_id)) != 0)
      return 0;
   if (strcmp(import_id, target_id) == 0)
      return 1;
   size_t target_len = strlen(target_id);
   return target_len + 9 < sizeof(target_id) && strncmp(import_id, target_id, target_len) == 0 &&
          strcmp(import_id + target_len, ".__init__") == 0;
}
