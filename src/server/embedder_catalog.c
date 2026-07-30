/* embedder_catalog.c — the selectable-embedder list behind GET /v1/embedders.
 *
 * The setup wizard needs this BEFORE anything is deployed, so it cannot come from a
 * running gateway's /embedders — at page-2 time there may be no gateway at all. Reading
 * the same scripts/embedders.json the gateway and supervisor read keeps one source of
 * truth without requiring one of them to be up.
 *
 * Split out of server_state.c so the shaping is testable without linking the server:
 * every field returned changes the vectors, and `local` decides whether the wizard may
 * offer a model for a local placement at all, so this is logic worth pinning.
 */
#include "embedder_catalog.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"

/* Most-specific first. The env override exists for tests and for an operator running the
 * server outside the image layout. */
static const char *EMBEDDER_REGISTRY_PATHS[] = {
    "/opt/aimee/embedders.json", /* the image layout */
    "scripts/embedders.json",    /* a source checkout, from the repo root */
    "../scripts/embedders.json", /* ...or from src/, where the build and tests run */
};

char *embedder_registry_read(void)
{
   const char *override = getenv("AIMEE_EMBEDDERS_FILE");
   size_t count = sizeof(EMBEDDER_REGISTRY_PATHS) / sizeof(*EMBEDDER_REGISTRY_PATHS);
   for (size_t i = 0; i <= count; i++)
   {
      const char *path = (i == 0) ? override : EMBEDDER_REGISTRY_PATHS[i - 1];
      if (!path || !path[0])
         continue;
      FILE *f = fopen(path, "rb");
      if (!f)
         continue;
      if (fseek(f, 0, SEEK_END) != 0)
      {
         fclose(f);
         continue;
      }
      long size = ftell(f);
      /* Bounded: a small hand-maintained descriptor, not a user upload. */
      if (size <= 0 || size > (1 << 20) || fseek(f, 0, SEEK_SET) != 0)
      {
         fclose(f);
         continue;
      }
      char *buf = malloc((size_t)size + 1);
      if (!buf)
      {
         fclose(f);
         return NULL;
      }
      size_t got = fread(buf, 1, (size_t)size, f);
      fclose(f);
      buf[got] = '\0';
      return buf;
   }
   return NULL;
}

/* A field counts as SET only when it is a non-empty string. The registry uses "" to mean
 * "deliberately not declared", which is exactly the difference between an embedder this
 * deployment can host and one it can only reach at an endpoint. */
static int field_set(cJSON *entry, const char *field)
{
   cJSON *v = cJSON_GetObjectItemCaseSensitive(entry, field);
   return cJSON_IsString(v) && v->valuestring && v->valuestring[0];
}

int embedder_entry_is_local(cJSON *entry)
{
   cJSON *source = cJSON_GetObjectItemCaseSensitive(entry, "source");
   const char *src =
       (cJSON_IsString(source) && source->valuestring[0]) ? source->valuestring : "hf";
   if (strcmp(src, "release") == 0)
   {
      /* A GGUF we convert and publish: the digest cannot be pinned here because the
       * artifact postdates the entry, so it travels with the release instead. */
      return field_set(entry, "release_tag") && field_set(entry, "file");
   }
   /* A pinned Hub file: verifiable ahead of time, so demand the checksum. */
   return field_set(entry, "repo") && field_set(entry, "file") &&
          field_set(entry, "revision") && field_set(entry, "sha256");
}

cJSON *embedder_catalog_build(const char *raw, const char **err)
{
   if (err)
      *err = NULL;
   if (!raw)
   {
      if (err)
         *err = "embedder registry not found";
      return NULL;
   }
   cJSON *doc = cJSON_Parse(raw);
   if (!doc)
   {
      if (err)
         *err = "embedder registry is not valid JSON";
      return NULL;
   }
   cJSON *table = cJSON_GetObjectItemCaseSensitive(doc, "embedders");
   if (!cJSON_IsObject(table))
   {
      cJSON_Delete(doc);
      if (err)
         *err = "embedder registry declares no embedders";
      return NULL;
   }

   cJSON *arr = cJSON_CreateArray();
   cJSON *entry = NULL;
   cJSON_ArrayForEach(entry, table)
   {
      if (!cJSON_IsObject(entry) || !entry->string)
         continue;
      cJSON *dim = cJSON_GetObjectItemCaseSensitive(entry, "dim");
      cJSON *ctxlen = cJSON_GetObjectItemCaseSensitive(entry, "context");
      cJSON *pooling = cJSON_GetObjectItemCaseSensitive(entry, "pooling");
      cJSON *prefixes = cJSON_GetObjectItemCaseSensitive(entry, "prefixes");
      cJSON *source = cJSON_GetObjectItemCaseSensitive(entry, "source");

      int prefixed = 0;
      if (cJSON_IsObject(prefixes))
      {
         cJSON *q = cJSON_GetObjectItemCaseSensitive(prefixes, "query");
         cJSON *d = cJSON_GetObjectItemCaseSensitive(prefixes, "document");
         prefixed = (cJSON_IsString(q) && q->valuestring[0]) ||
                    (cJSON_IsString(d) && d->valuestring[0]);
      }

      cJSON *out = cJSON_CreateObject();
      cJSON_AddStringToObject(out, "id", entry->string);
      cJSON_AddNumberToObject(out, "dim", cJSON_IsNumber(dim) ? dim->valuedouble : 0);
      cJSON_AddNumberToObject(out, "context", cJSON_IsNumber(ctxlen) ? ctxlen->valuedouble : 0);
      cJSON_AddStringToObject(out, "pooling", cJSON_IsString(pooling) ? pooling->valuestring : "");
      cJSON_AddStringToObject(
          out, "source",
          (cJSON_IsString(source) && source->valuestring[0]) ? source->valuestring : "hf");
      cJSON_AddBoolToObject(out, "local", embedder_entry_is_local(entry));
      cJSON_AddBoolToObject(out, "prefixed", prefixed);
      cJSON_AddItemToArray(arr, out);
   }
   cJSON_Delete(doc);
   return arr;
}
