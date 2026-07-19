/* cmd_memory_pack.c: `aimee memory pack` subcommand family.
 *
 * Subcommands:
 *   list     — list available profile packs
 *   show     — print a pack's fields
 *   validate — validate a pack file
 *   use      — set the active pack */

#include "aimee.h"
#include "cmd_memory_internal.h"
#include "commands.h"
#include "memory_profile_pack.h"
#include "cJSON.h"
#include "json_fluent.h"

#include <stdio.h>
#include <string.h>

static void pack_list(app_ctx_t *ctx, int argc, char **argv)
{
   (void)argc;
   (void)argv;
   char names[MEMORY_PROFILE_PACK_MAX_LIST][MEMORY_PROFILE_PACK_NAME_LEN];
   int n = memory_profile_pack_list(names, MEMORY_PROFILE_PACK_MAX_LIST);

   char active[MEMORY_PROFILE_PACK_NAME_LEN];
   memory_profile_pack_active(active, sizeof(active));

   if (ctx->json_output)
   {
      cJSON *obj = cJSON_CreateObject();
      cJSON *arr = cJSON_AddArrayToObject(obj, "packs");
      for (int i = 0; i < n; i++)
      {
         cJSON *p = cJSON_CreateObject();
         cJSON_AddStringToObject(p, "name", names[i]);
         cJSON_AddBoolToObject(p, "active", strcmp(names[i], active) == 0 ? 1 : 0);
         cJSON_AddItemToArray(arr, p);
      }
      cJSON_AddStringToObject(obj, "active", active);
      emit_json_ctx(obj, ctx->json_fields, ctx->response_profile);
      return;
   }

   if (n == 0)
   {
      printf("No profile packs found in %s\n", memory_profile_pack_dir());
      return;
   }
   for (int i = 0; i < n; i++)
      printf("%s%s\n", names[i], strcmp(names[i], active) == 0 ? " (active)" : "");
}

static void pack_show(app_ctx_t *ctx, int argc, char **argv)
{
   if (argc < 1)
      fatal("memory pack show requires <name>");
   const char *name = argv[0];

   char errbuf[256];
   memory_profile_pack_t pack;
   if (memory_profile_pack_load(name, &pack, errbuf, sizeof(errbuf)) != 0)
      fatal("cannot load pack '%s': %s", name, errbuf);

   if (ctx->json_output)
   {
      cJSON *obj = cJSON_CreateObject();
      cJSON_AddStringToObject(obj, "name", pack.name);
      cJSON_AddStringToObject(obj, "description", pack.description);
      if (pack.allowed_tier_count > 0)
      {
         cJSON *at = cJSON_AddArrayToObject(obj, "allowed_tiers");
         for (int i = 0; i < pack.allowed_tier_count; i++)
            cJSON_AddItemToArray(at, cJSON_CreateString(pack.allowed_tiers[i]));
      }
      if (pack.allowed_kind_count > 0)
      {
         cJSON *ak = cJSON_AddArrayToObject(obj, "allowed_kinds");
         for (int i = 0; i < pack.allowed_kind_count; i++)
            cJSON_AddItemToArray(ak, cJSON_CreateString(pack.allowed_kinds[i]));
      }
      if (pack.default_tier[0])
         cJSON_AddStringToObject(obj, "default_tier", pack.default_tier);
      if (pack.default_visibility[0])
         cJSON_AddStringToObject(obj, "default_visibility", pack.default_visibility);
      emit_json_ctx(obj, ctx->json_fields, ctx->response_profile);
      return;
   }

   printf("name:        %s\n", pack.name);
   printf("description: %s\n", pack.description);
   if (pack.allowed_tier_count > 0)
   {
      printf("allowed_tiers:");
      for (int i = 0; i < pack.allowed_tier_count; i++)
         printf(" %s", pack.allowed_tiers[i]);
      printf("\n");
   }
   if (pack.allowed_kind_count > 0)
   {
      printf("allowed_kinds:");
      for (int i = 0; i < pack.allowed_kind_count; i++)
         printf(" %s", pack.allowed_kinds[i]);
      printf("\n");
   }
   if (pack.default_tier[0])
      printf("default_tier: %s\n", pack.default_tier);
   if (pack.default_visibility[0])
      printf("default_visibility: %s\n", pack.default_visibility);
}

static void pack_validate(app_ctx_t *ctx, int argc, char **argv)
{
   if (argc < 1)
      fatal("memory pack validate requires <path>");
   const char *path = argv[0];

   char errbuf[256] = "";
   int rc = memory_profile_pack_validate_file(path, errbuf, sizeof(errbuf));

   if (ctx->json_output)
   {
      cJSON *obj = cJSON_CreateObject();
      cJSON_AddStringToObject(obj, "status", rc == 0 ? "ok" : "error");
      if (rc != 0)
         cJSON_AddStringToObject(obj, "message", errbuf);
      emit_json_ctx(obj, ctx->json_fields, ctx->response_profile);
      if (rc != 0)
         exit(1);
      return;
   }

   if (rc == 0)
      printf("ok: %s\n", path);
   else
   {
      fprintf(stderr, "error: %s\n", errbuf);
      exit(1);
   }
}

static void pack_use(app_ctx_t *ctx, int argc, char **argv)
{
   if (argc < 1)
      fatal("memory pack use requires <name>");
   const char *name = argv[0];

   /* Verify the pack exists before setting it active. */
   char errbuf[256] = "";
   memory_profile_pack_t pack;
   if (memory_profile_pack_load(name, &pack, errbuf, sizeof(errbuf)) != 0)
      fatal("cannot load pack '%s': %s", name, errbuf);

   if (memory_profile_pack_set_active(name) != 0)
      fatal("failed to set active pack to '%s'", name);

   if (ctx->json_output)
   {
      emit_ok_ctx(ctx->json_fields, ctx->response_profile);
      return;
   }
   printf("active pack set to: %s\n", name);
}

static const subcmd_t mem_pack_subs[] = {
    {"list", "list available packs", pack_list},
    {"show", "show a pack", pack_show},
    {"validate", "validate a pack file", pack_validate},
    {"use", "activate a pack", pack_use},
    {NULL, NULL, NULL},
};

void mem_pack(app_ctx_t *ctx, int argc, char **argv)
{
   if (argc < 1)
   {
      fprintf(stderr, "Usage: aimee memory pack <subcommand> [args]\n"
                      "Subcommands: list | show <name> | validate <path> | use <name>\n");
      exit(1);
   }

   const char *sub = argv[0];
   argc--;
   argv++;

   if (subcmd_dispatch(mem_pack_subs, sub, ctx, argc, argv) != 0)
      fatal("unknown memory pack subcommand: %s", sub);
}
