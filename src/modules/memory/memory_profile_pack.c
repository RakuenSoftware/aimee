/* memory_profile_pack.c: operator-authored memory profile pack support.
 *
 * Pack format (JSON):
 * {
 *   "name": "myproject",
 *   "description": "Memory profile for myproject",
 *   "validation_rules": {
 *     "allowed_tiers": ["L0","L1","L2"],
 *     "allowed_kinds": ["fact","preference","decision"]
 *   },
 *   "scope_defaults": {
 *     "tier": "L1",
 *     "visibility": "default"
 *   }
 * }
 *
 * Required fields: name, description.
 * Unknown top-level keys are rejected. */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "aimee.h"
#include "aimee_home.h"
#include "memory_profile_pack.h"
#include "cJSON.h"
#include "platform_path.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define ACTIVE_PACK_FILE "active-pack"

static const char *s_known_tiers[] = {"L0", "L1", "L2", "L3", NULL};
static const char *s_known_top_keys[] = {
    "name",           "description", "extraction_hints", "validation_rules", "scope_defaults",
    "explain_labels", NULL};

const char *memory_profile_pack_dir(void)
{
   static char dir[MAX_PATH_LEN];
   const char *env = getenv("AIMEE_PACK_DIR");
   if (env && env[0])
   {
      snprintf(dir, sizeof(dir), "%s", env);
      return dir;
   }
   const char *base = aimee_home();
   if (!base)
      base = "/tmp/.config/aimee";
   snprintf(dir, sizeof(dir), "%s/packs", base);
   return dir;
}

static int is_known_tier(const char *tier)
{
   for (int i = 0; s_known_tiers[i]; i++)
      if (strcmp(tier, s_known_tiers[i]) == 0)
         return 1;
   return 0;
}

int memory_profile_pack_validate_file(const char *path, char *errbuf, size_t errlen)
{
   if (!path || !path[0])
   {
      snprintf(errbuf, errlen, "no path given");
      return -1;
   }

   FILE *f = fopen(path, "r");
   if (!f)
   {
      snprintf(errbuf, errlen, "cannot open %s", path);
      return -1;
   }

   fseek(f, 0, SEEK_END);
   long sz = ftell(f);
   rewind(f);
   if (sz <= 0 || sz > 65536)
   {
      fclose(f);
      snprintf(errbuf, errlen, "file size out of range (%ld bytes)", sz);
      return -1;
   }

   char *buf = malloc((size_t)sz + 1);
   if (!buf)
   {
      fclose(f);
      snprintf(errbuf, errlen, "out of memory");
      return -1;
   }
   size_t n = fread(buf, 1, (size_t)sz, f);
   fclose(f);
   buf[n] = '\0';

   cJSON *root = cJSON_Parse(buf);
   free(buf);
   if (!root)
   {
      snprintf(errbuf, errlen, "invalid JSON");
      return -1;
   }

   /* Required: name */
   cJSON *name_j = cJSON_GetObjectItemCaseSensitive(root, "name");
   if (!cJSON_IsString(name_j) || !name_j->valuestring[0])
   {
      cJSON_Delete(root);
      snprintf(errbuf, errlen, "missing required field: name");
      return -1;
   }

   /* Required: description */
   cJSON *desc_j = cJSON_GetObjectItemCaseSensitive(root, "description");
   if (!cJSON_IsString(desc_j))
   {
      cJSON_Delete(root);
      snprintf(errbuf, errlen, "missing required field: description");
      return -1;
   }

   /* Unknown top-level keys */
   cJSON *it = NULL;
   cJSON_ArrayForEach(it, root)
   {
      int known = 0;
      for (int i = 0; s_known_top_keys[i]; i++)
         if (strcmp(it->string, s_known_top_keys[i]) == 0)
         {
            known = 1;
            break;
         }
      if (!known)
      {
         cJSON_Delete(root);
         snprintf(errbuf, errlen, "unknown key: %s", it->string);
         return -1;
      }
   }

   /* Optional: validation_rules */
   cJSON *vr = cJSON_GetObjectItemCaseSensitive(root, "validation_rules");
   if (vr)
   {
      if (!cJSON_IsObject(vr))
      {
         cJSON_Delete(root);
         snprintf(errbuf, errlen, "validation_rules must be an object");
         return -1;
      }
      cJSON *at = cJSON_GetObjectItemCaseSensitive(vr, "allowed_tiers");
      if (at)
      {
         if (!cJSON_IsArray(at))
         {
            cJSON_Delete(root);
            snprintf(errbuf, errlen, "validation_rules.allowed_tiers must be an array");
            return -1;
         }
         cJSON *tier_item = NULL;
         cJSON_ArrayForEach(tier_item, at)
         {
            if (!cJSON_IsString(tier_item) || !is_known_tier(tier_item->valuestring))
            {
               cJSON_Delete(root);
               snprintf(errbuf, errlen, "unknown tier: %s",
                        cJSON_IsString(tier_item) ? tier_item->valuestring : "(non-string)");
               return -1;
            }
         }
      }
      cJSON *ak = cJSON_GetObjectItemCaseSensitive(vr, "allowed_kinds");
      if (ak && !cJSON_IsArray(ak))
      {
         cJSON_Delete(root);
         snprintf(errbuf, errlen, "validation_rules.allowed_kinds must be an array");
         return -1;
      }
   }

   /* Optional: scope_defaults */
   cJSON *sd = cJSON_GetObjectItemCaseSensitive(root, "scope_defaults");
   if (sd && !cJSON_IsObject(sd))
   {
      cJSON_Delete(root);
      snprintf(errbuf, errlen, "scope_defaults must be an object");
      return -1;
   }

   /* Optional: extraction_hints, explain_labels — must be objects if present */
   cJSON *eh = cJSON_GetObjectItemCaseSensitive(root, "extraction_hints");
   if (eh && !cJSON_IsObject(eh))
   {
      cJSON_Delete(root);
      snprintf(errbuf, errlen, "extraction_hints must be an object");
      return -1;
   }
   cJSON *el = cJSON_GetObjectItemCaseSensitive(root, "explain_labels");
   if (el && !cJSON_IsObject(el))
   {
      cJSON_Delete(root);
      snprintf(errbuf, errlen, "explain_labels must be an object");
      return -1;
   }

   cJSON_Delete(root);
   return 0;
}

int memory_profile_pack_load(const char *name, memory_profile_pack_t *out, char *errbuf,
                             size_t errlen)
{
   if (!name || !name[0] || !out)
   {
      if (errbuf)
         snprintf(errbuf, errlen, "invalid arguments");
      return -1;
   }

   char path[MAX_PATH_LEN];
   snprintf(path, sizeof(path), "%s/%s.json", memory_profile_pack_dir(), name);

   char valerr[256] = "";
   if (memory_profile_pack_validate_file(path, valerr, sizeof(valerr)) != 0)
   {
      if (errbuf)
         snprintf(errbuf, errlen, "%s", valerr);
      return -1;
   }

   FILE *f = fopen(path, "r");
   if (!f)
   {
      if (errbuf)
         snprintf(errbuf, errlen, "cannot open %s", path);
      return -1;
   }
   fseek(f, 0, SEEK_END);
   long sz = ftell(f);
   rewind(f);
   char *buf = malloc((size_t)sz + 1);
   if (!buf)
   {
      fclose(f);
      if (errbuf)
         snprintf(errbuf, errlen, "out of memory");
      return -1;
   }
   size_t nr = fread(buf, 1, (size_t)sz, f);
   fclose(f);
   buf[nr] = '\0';

   cJSON *root = cJSON_Parse(buf);
   free(buf);
   if (!root)
   {
      if (errbuf)
         snprintf(errbuf, errlen, "invalid JSON");
      return -1;
   }

   memset(out, 0, sizeof(*out));

   cJSON *j;
   j = cJSON_GetObjectItemCaseSensitive(root, "name");
   if (cJSON_IsString(j))
      snprintf(out->name, sizeof(out->name), "%s", j->valuestring);

   j = cJSON_GetObjectItemCaseSensitive(root, "description");
   if (cJSON_IsString(j))
      snprintf(out->description, sizeof(out->description), "%s", j->valuestring);

   cJSON *vr = cJSON_GetObjectItemCaseSensitive(root, "validation_rules");
   if (vr)
   {
      cJSON *at = cJSON_GetObjectItemCaseSensitive(vr, "allowed_tiers");
      if (cJSON_IsArray(at))
      {
         cJSON *item = NULL;
         cJSON_ArrayForEach(item, at)
         {
            if (cJSON_IsString(item) && out->allowed_tier_count < MEMORY_PROFILE_PACK_MAX_TIERS)
            {
               snprintf(out->allowed_tiers[out->allowed_tier_count], sizeof(out->allowed_tiers[0]),
                        "%s", item->valuestring);
               out->allowed_tier_count++;
            }
         }
      }
      cJSON *ak = cJSON_GetObjectItemCaseSensitive(vr, "allowed_kinds");
      if (cJSON_IsArray(ak))
      {
         cJSON *item = NULL;
         cJSON_ArrayForEach(item, ak)
         {
            if (cJSON_IsString(item) && out->allowed_kind_count < MEMORY_PROFILE_PACK_MAX_KINDS)
            {
               snprintf(out->allowed_kinds[out->allowed_kind_count], sizeof(out->allowed_kinds[0]),
                        "%s", item->valuestring);
               out->allowed_kind_count++;
            }
         }
      }
   }

   cJSON *sd = cJSON_GetObjectItemCaseSensitive(root, "scope_defaults");
   if (sd)
   {
      j = cJSON_GetObjectItemCaseSensitive(sd, "tier");
      if (cJSON_IsString(j))
         snprintf(out->default_tier, sizeof(out->default_tier), "%s", j->valuestring);
      j = cJSON_GetObjectItemCaseSensitive(sd, "visibility");
      if (cJSON_IsString(j))
         snprintf(out->default_visibility, sizeof(out->default_visibility), "%s", j->valuestring);
   }

   cJSON_Delete(root);
   return 0;
}

int memory_profile_pack_list(char names[][MEMORY_PROFILE_PACK_NAME_LEN], int max)
{
   const char *dir = memory_profile_pack_dir();
   DIR *d = opendir(dir);
   if (!d)
      return 0;

   int count = 0;
   struct dirent *ent;
   while ((ent = readdir(d)) != NULL && count < max)
   {
      const char *n = ent->d_name;
      size_t nlen = strlen(n);
      if (nlen < 5 || strcmp(n + nlen - 5, ".json") != 0)
         continue;
      /* Strip .json suffix */
      size_t base_len = nlen - 5;
      if (base_len >= MEMORY_PROFILE_PACK_NAME_LEN)
         continue;
      if (names)
      {
         memcpy(names[count], n, base_len);
         names[count][base_len] = '\0';
      }
      count++;
   }
   closedir(d);
   return count;
}

void memory_profile_pack_active(char *out, size_t len)
{
   char path[MAX_PATH_LEN];
   snprintf(path, sizeof(path), "%s/%s", memory_profile_pack_dir(), ACTIVE_PACK_FILE);

   FILE *f = fopen(path, "r");
   if (!f)
   {
      snprintf(out, len, "default");
      return;
   }
   char buf[MEMORY_PROFILE_PACK_NAME_LEN] = "";
   if (fgets(buf, sizeof(buf), f))
   {
      /* Strip trailing newline */
      size_t blen = strlen(buf);
      while (blen > 0 && (buf[blen - 1] == '\n' || buf[blen - 1] == '\r'))
         buf[--blen] = '\0';
   }
   fclose(f);
   snprintf(out, len, "%s", buf[0] ? buf : "default");
}

int memory_profile_pack_set_active(const char *name)
{
   if (!name || !name[0])
      return -1;
   const char *dir = memory_profile_pack_dir();
   platform_mkdir_p(dir, 0700);

   char path[MAX_PATH_LEN];
   snprintf(path, sizeof(path), "%s/%s", dir, ACTIVE_PACK_FILE);

   FILE *f = fopen(path, "w");
   if (!f)
      return -1;
   fprintf(f, "%s\n", name);
   fclose(f);
   return 0;
}
