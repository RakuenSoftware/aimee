/* episode_seal.c: sealed work episode (fold §5, P5). See episode_seal.h. */
#include "episode_seal.h"

#include "cJSON.h"
#include <stdlib.h>
#include <string.h>

static char *dup_str(const char *s)
{
   if (!s)
      return NULL;
   size_t n = strlen(s);
   char *c = malloc(n + 1);
   if (c)
      memcpy(c, s, n + 1);
   return c;
}

void episode_seal_init(episode_seal_t *s)
{
   if (!s)
      return;
   s->conclusion = NULL;
   s->files = NULL;
   s->count = 0;
   s->cap = 0;
}

void episode_seal_free(episode_seal_t *s)
{
   if (!s)
      return;
   free(s->conclusion);
   for (size_t i = 0; i < s->count; i++)
      free(s->files[i]);
   free(s->files);
   episode_seal_init(s);
}

int episode_seal_set_conclusion(episode_seal_t *s, const char *text)
{
   if (!s)
      return -1;
   char *c = dup_str(text ? text : "");
   if (!c)
      return -1;
   free(s->conclusion);
   s->conclusion = c;
   return 0;
}

int episode_seal_add_file(episode_seal_t *s, const char *path)
{
   if (!s)
      return -1;
   if (!path || !path[0])
      return 0; /* NULL/empty ignored (not an error) */
   for (size_t i = 0; i < s->count; i++)
      if (strcmp(s->files[i], path) == 0)
         return 0; /* dedup */
   if (s->count == s->cap)
   {
      size_t ncap = s->cap ? s->cap * 2 : 8;
      char **nf = realloc(s->files, ncap * sizeof(*nf));
      if (!nf)
         return -1;
      s->files = nf;
      s->cap = ncap;
   }
   char *c = dup_str(path);
   if (!c)
      return -1;
   s->files[s->count++] = c;
   return 0;
}

int episode_seal_touches(const episode_seal_t *s, const char *path)
{
   if (!s || !path)
      return 0;
   for (size_t i = 0; i < s->count; i++)
      if (strcmp(s->files[i], path) == 0)
         return 1;
   return 0;
}

/* All-or-nothing: any cJSON allocation failure deletes root and returns NULL. */
char *episode_seal_serialize(const episode_seal_t *s)
{
   if (!s)
      return NULL;
   cJSON *root = cJSON_CreateObject();
   if (!root)
      return NULL;
   if (!cJSON_AddStringToObject(root, "conclusion", s->conclusion ? s->conclusion : ""))
   {
      cJSON_Delete(root);
      return NULL;
   }
   cJSON *files = cJSON_AddArrayToObject(root, "files");
   if (!files)
   {
      cJSON_Delete(root);
      return NULL;
   }
   for (size_t i = 0; i < s->count; i++)
   {
      cJSON *f = cJSON_CreateString(s->files[i]);
      if (!f)
      {
         cJSON_Delete(root);
         return NULL;
      }
      cJSON_AddItemToArray(files, f);
   }
   char *out = cJSON_PrintUnformatted(root);
   cJSON_Delete(root);
   return out;
}

/* Validate + build into a temporary; swap into *s only on complete success, so
 * malformed/partial JSON or OOM never destroys the caller's existing seal. */
int episode_seal_parse(episode_seal_t *s, const char *json)
{
   if (!s || !json)
      return -1;
   cJSON *root = cJSON_Parse(json);
   if (!root || !cJSON_IsObject(root))
   {
      cJSON_Delete(root);
      return -1;
   }
   cJSON *files = cJSON_GetObjectItem(root, "files");
   if (files && !cJSON_IsArray(files))
   {
      cJSON_Delete(root);
      return -1;
   }

   episode_seal_t tmp;
   episode_seal_init(&tmp);
   if (episode_seal_set_conclusion(
           &tmp, cJSON_GetStringValue(cJSON_GetObjectItem(root, "conclusion"))) != 0)
   {
      episode_seal_free(&tmp);
      cJSON_Delete(root);
      return -1;
   }
   cJSON *f;
   cJSON_ArrayForEach(f, files)
   {
      if (!cJSON_IsString(f))
         continue;
      if (episode_seal_add_file(&tmp, f->valuestring) != 0)
      {
         episode_seal_free(&tmp);
         cJSON_Delete(root);
         return -1; /* OOM -> caller's seal untouched */
      }
   }
   cJSON_Delete(root);
   episode_seal_free(s);
   *s = tmp; /* swap in only on complete success */
   return 0;
}
