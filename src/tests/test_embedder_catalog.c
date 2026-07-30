/* test_embedder_catalog.c — the wizard's embedder list.
 *
 * Two properties carry real consequences and so are pinned here.
 *
 * `local` decides whether the wizard may offer a model for a LOCAL placement. Offering
 * one whose weights cannot be fetched produces a container that refuses to boot, and
 * hiding one that can be is how bekko sat unusable behind a blank-coordinates check. It
 * is source-aware on purpose: an `hf` entry is a pinned file we can checksum ahead of
 * time, while a `release` entry is a GGUF we convert ourselves whose digest cannot exist
 * until the conversion runs.
 *
 * `dim` and `prefixed` are surfaced because they decide the COST of the choice — a width
 * change rebuilds the pgvector columns, and prefixes are part of the vector space. A
 * picker that dropped them would invite a silent re-embed.
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "embedder_catalog.h"

static cJSON *entry_by_id(cJSON *arr, const char *id)
{
   cJSON *e = NULL;
   cJSON_ArrayForEach(e, arr)
   {
      cJSON *got = cJSON_GetObjectItemCaseSensitive(e, "id");
      if (cJSON_IsString(got) && strcmp(got->valuestring, id) == 0)
         return e;
   }
   return NULL;
}

static int is_true(cJSON *obj, const char *field)
{
   return cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(obj, field));
}

static double num(cJSON *obj, const char *field)
{
   cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, field);
   return cJSON_IsNumber(v) ? v->valuedouble : -1;
}

static void test_shapes_every_field_the_picker_needs(void)
{
   const char *raw =
       "{\"embedders\": {"
       " \"nomic\": {\"repo\":\"r\",\"file\":\"f\",\"revision\":\"v\",\"sha256\":\"s\","
       "             \"pooling\":\"mean\",\"dim\":768,\"context\":2048,"
       "             \"prefixes\":{\"query\":\"search_query: \",\"document\":\"search_document: \"}},"
       " \"bekko\": {\"source\":\"release\",\"release_tag\":\"t\",\"file\":\"f\","
       "             \"pooling\":\"mean\",\"dim\":384,\"context\":8192,"
       "             \"prefixes\":{\"query\":\"\",\"document\":\"\"}}"
       "}}";
   const char *err = "unset";
   cJSON *arr = embedder_catalog_build(raw, &err);
   assert(arr && !err);
   assert(cJSON_GetArraySize(arr) == 2);

   cJSON *nomic = entry_by_id(arr, "nomic");
   assert(nomic);
   assert(num(nomic, "dim") == 768);
   assert(num(nomic, "context") == 2048);
   assert(is_true(nomic, "local"));
   assert(is_true(nomic, "prefixed"));
   assert(strcmp(cJSON_GetObjectItemCaseSensitive(nomic, "source")->valuestring, "hf") == 0);

   cJSON *bekko = entry_by_id(arr, "bekko");
   assert(bekko);
   /* 384, not 768: the width is what makes choosing it a schema rebuild. */
   assert(num(bekko, "dim") == 384);
   assert(is_true(bekko, "local")); /* release_tag + file is enough to fetch it */
   assert(!is_true(bekko, "prefixed")); /* its card defines none */
   assert(strcmp(cJSON_GetObjectItemCaseSensitive(bekko, "source")->valuestring, "release") == 0);
   cJSON_Delete(arr);
   printf("  shapes_every_field_the_picker_needs: ok\n");
}

static void test_local_is_source_aware(void)
{
   /* An hf entry missing its checksum is NOT hostable: we could fetch bytes but could
    * not tell whether they are the right ones, and a substituted embedder produces
    * well-formed wrong vectors. */
   const char *no_sha =
       "{\"embedders\": {\"m\": {\"repo\":\"r\",\"file\":\"f\",\"revision\":\"v\",\"sha256\":\"\","
       " \"pooling\":\"mean\",\"dim\":8,\"context\":8,"
       " \"prefixes\":{\"query\":\"\",\"document\":\"\"}}}}";
   cJSON *arr = embedder_catalog_build(no_sha, NULL);
   assert(arr && !is_true(entry_by_id(arr, "m"), "local"));
   cJSON_Delete(arr);

   /* A release entry needs no sha256 — the digest travels with the release — but it does
    * need to say which release and which file. */
   const char *no_tag =
       "{\"embedders\": {\"m\": {\"source\":\"release\",\"file\":\"f\","
       " \"pooling\":\"mean\",\"dim\":8,\"context\":8,"
       " \"prefixes\":{\"query\":\"\",\"document\":\"\"}}}}";
   arr = embedder_catalog_build(no_tag, NULL);
   assert(arr && !is_true(entry_by_id(arr, "m"), "local"));
   cJSON_Delete(arr);

   /* Declared but unhosted: a complete embedder description with no weight source at
    * all. Selectable against an external endpoint, never for a local placement. */
   const char *declared_only =
       "{\"embedders\": {\"m\": {\"pooling\":\"mean\",\"dim\":8,\"context\":8,"
       " \"prefixes\":{\"query\":\"\",\"document\":\"\"}}}}";
   arr = embedder_catalog_build(declared_only, NULL);
   assert(arr && !is_true(entry_by_id(arr, "m"), "local"));
   cJSON_Delete(arr);
   printf("  local_is_source_aware: ok\n");
}

static void test_malformed_registries_are_refused_not_guessed(void)
{
   const char *err = NULL;
   assert(embedder_catalog_build(NULL, &err) == NULL && err);
   err = NULL;
   assert(embedder_catalog_build("not json", &err) == NULL && err);
   err = NULL;
   assert(embedder_catalog_build("{}", &err) == NULL && err);
   err = NULL;
   assert(embedder_catalog_build("{\"embedders\": []}", &err) == NULL && err);
   /* A non-object entry is skipped rather than aborting the whole list: one bad row in an
    * operator overlay should not blank the picker. */
   err = NULL;
   cJSON *arr = embedder_catalog_build("{\"embedders\": {\"bad\": 7}}", &err);
   assert(arr && !err && cJSON_GetArraySize(arr) == 0);
   cJSON_Delete(arr);
   printf("  malformed_registries_are_refused_not_guessed: ok\n");
}

static void test_reads_the_shipped_registry(void)
{
   /* The file the gateway and supervisor read must satisfy this reader too — one
    * declaration drives the picker, the provisioning and the serving flags. */
   char *raw = embedder_registry_read();
   if (!raw)
   {
      printf("  reads_the_shipped_registry: skipped (registry not on any candidate path)\n");
      return;
   }
   const char *err = NULL;
   cJSON *arr = embedder_catalog_build(raw, &err);
   free(raw);
   assert(arr && !err);
   assert(cJSON_GetArraySize(arr) > 0);
   cJSON *e = NULL;
   cJSON_ArrayForEach(e, arr)
   {
      /* Every shipped entry declares a positive width: the wizard states the re-embed
       * cost from it, and a zero would silently read as "no change". */
      assert(num(e, "dim") > 0);
      assert(num(e, "context") > 0);
   }
   cJSON_Delete(arr);
   printf("  reads_the_shipped_registry: ok\n");
}

int main(void)
{
   printf("embedder_catalog:\n");
   test_shapes_every_field_the_picker_needs();
   test_local_is_source_aware();
   test_malformed_registries_are_refused_not_guessed();
   test_reads_the_shipped_registry();
   printf("embedder_catalog: all tests passed\n");
   return 0;
}
