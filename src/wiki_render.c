/* Local file transport for the shared Go memory owner's rendered wiki. */
#include "aimee.h"
#include "wiki_render.h"
#include "kb_client.h"
#include "json_fluent.h"
#include "platform_path.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

int wiki_render(const char *out_dir)
{
   if (!out_dir || !out_dir[0])
      return -1;
   cJSON *args = cJSON_CreateObject();
   if (!args)
      return -1;
   kb_client_memory_scope_context_apply(args);
   cJSON_AddStringToObject(args, "format", "wiki");
   char *raw = kb_v1_action_request("memory.list", args);
   cJSON *response = raw ? cJSON_Parse(raw) : NULL;
   free(raw);
   const cJSON *files = cJSON_GetObjectItemCaseSensitive(response, "files");
   int count = cJSON_GetArraySize(files), rc = -1;
   if (strcmp(jo_cstr(response, "status"), "ok") || !cJSON_IsArray(files) || count < 1 ||
       count > 64)
      goto done;

   /* Validate the entire transport envelope before touching existing output.
    * The remote owner supplies basenames, never host paths. */
   const cJSON *file;
   size_t bytes = 0;
   cJSON_ArrayForEach(file, files)
   {
      const char *name = jo_cstr((cJSON *)file, "name");
      const cJSON *text = cJSON_GetObjectItemCaseSensitive(file, "text");
      const cJSON *preserve = cJSON_GetObjectItemCaseSensitive(file, "preserve_existing");
      if (!name[0] || name[0] == '.' || strlen(name) > 255 ||
          strspn(name, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-") !=
              strlen(name) ||
          !cJSON_IsString(text) || (preserve && !cJSON_IsBool(preserve)) ||
          strlen(out_dir) + strlen(name) + 2 > MAX_PATH_LEN)
         goto done;
      bytes += strlen(text->valuestring);
      if (bytes > 16 * 1024 * 1024)
         goto done;
      for (const cJSON *prior = files->child; prior != file; prior = prior->next)
         if (!strcmp(name, jo_cstr((cJSON *)prior, "name")))
            goto done;
   }
   if (platform_mkdir_p(out_dir, 0755) != 0)
      goto done;
   cJSON_ArrayForEach(file, files)
   {
      char path[MAX_PATH_LEN];
      snprintf(path, sizeof(path), "%s/%s", out_dir, jo_cstr((cJSON *)file, "name"));
      int preserve = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(file, "preserve_existing"));
      FILE *output = fopen(path, preserve ? "wbx" : "wb");
      if (!output)
      {
         struct stat st;
         if (preserve && errno == EEXIST && stat(path, &st) == 0 && S_ISREG(st.st_mode))
            continue;
         goto done;
      }
      const char *text = jo_cstr((cJSON *)file, "text");
      size_t length = strlen(text);
      int failed = fwrite(text, 1, length, output) != length;
      if (fclose(output) != 0)
         failed = 1;
      if (failed)
         goto done;
   }
   rc = 0;
done:
   if (rc != 0 && jo_cstr(response, "message")[0])
      fprintf(stderr, "wiki render: %s\n", jo_cstr(response, "message"));
   cJSON_Delete(response);
   return rc;
}
