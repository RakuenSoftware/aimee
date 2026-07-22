/* workspace_manifest.c — parse aimee.workspace.yaml into workspace_manifest_t.
 *
 * Supported manifest shape (block YAML only):
 *
 *   repos:
 *     - url: https://github.com/example/repo.git
 *       path: ./repos/repo        # optional clone destination
 *
 *   dependencies:
 *     c:
 *       apt:
 *         - libssl-dev
 *         - cmake
 *     python:
 *       pip:
 *         - requests
 *         - flask
 *     js:
 *       npm:
 *         - install               # bare "install" → npm install
 *
 *   secrets:
 *     - name: GITHUB_TOKEN
 *       description: PAT for private repos
 *
 *   quickstart:
 *     index: true
 *     generate_rules: true
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "workspace_manifest.h"
#include "headers/yaml.h"
#include "vendor/headers/cJSON.h"

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

/* Read the entire file at path into a malloc'd NUL-terminated buffer.
 * Returns NULL on error. Caller frees. */
static char *read_file(const char *path)
{
   FILE *fp = fopen(path, "r");
   if (!fp)
      return NULL;
   if (fseek(fp, 0, SEEK_END) != 0)
   {
      fclose(fp);
      return NULL;
   }
   long len = ftell(fp);
   rewind(fp);
   if (len < 0)
   {
      fclose(fp);
      return NULL;
   }
   char *buf = malloc((size_t)len + 1);
   if (!buf)
   {
      fclose(fp);
      return NULL;
   }
   size_t nread = fread(buf, 1, (size_t)len, fp);
   fclose(fp);
   buf[nread] = '\0';
   return buf;
}

/* Append pkg to a space-separated command being built in buf[buflen]. */
static void append_pkg(char *buf, size_t buflen, const char *pkg)
{
   size_t cur = strlen(buf);
   if (cur > 0 && cur + 1 < buflen)
   {
      buf[cur++] = ' ';
      buf[cur] = '\0';
   }
   size_t plen = strlen(pkg);
   if (cur + plen < buflen)
   {
      memcpy(buf + cur, pkg, plen + 1);
   }
}

/* ------------------------------------------------------------------ */
/* Dependency section parser                                           */
/* ------------------------------------------------------------------ */

/* Build install command(s) from a single language object, e.g.:
 *   c:
 *     apt:
 *       - libssl-dev
 * lang_obj is the value under the language key (an object of pkg-mgr → list).
 */
static void parse_lang_deps(const cJSON *lang_obj, workspace_manifest_t *out)
{
   if (!cJSON_IsObject(lang_obj))
      return;

   const cJSON *pm_item = NULL;
   cJSON_ArrayForEach(pm_item, lang_obj)
   {
      if (!pm_item->string)
         continue;
      const char *pm = pm_item->string; /* e.g. "apt", "pip", "npm", "cargo" */

      /* Build the prefix for this package manager */
      char prefix[128] = "";
      if (strcmp(pm, "apt") == 0 || strcmp(pm, "apt-get") == 0)
         snprintf(prefix, sizeof(prefix), "apt-get install -y");
      else if (strcmp(pm, "pip") == 0 || strcmp(pm, "pip3") == 0)
         snprintf(prefix, sizeof(prefix), "pip install");
      else if (strcmp(pm, "npm") == 0)
         snprintf(prefix, sizeof(prefix), "npm install");
      else if (strcmp(pm, "cargo") == 0)
         snprintf(prefix, sizeof(prefix), "cargo add");
      else if (strcmp(pm, "brew") == 0)
         snprintf(prefix, sizeof(prefix), "brew install");
      else
         snprintf(prefix, sizeof(prefix), "%s install", pm);

      /* Collect packages from the array */
      char cmd[512];
      snprintf(cmd, sizeof(cmd), "%s", prefix);

      int has_pkg = 0;
      if (cJSON_IsArray(pm_item))
      {
         const cJSON *pkg = NULL;
         cJSON_ArrayForEach(pkg, pm_item)
         {
            if (!cJSON_IsString(pkg))
               continue;
            const char *pname = pkg->valuestring;
            /* npm: bare "install" means just "npm install" with no extra pkg */
            if (strcmp(pm, "npm") == 0 && strcmp(pname, "install") == 0)
               continue;
            append_pkg(cmd, sizeof(cmd), pname);
            has_pkg = 1;
         }
      }

      /* npm with no explicit packages → "npm install" is sufficient on its own */
      if (strcmp(pm, "npm") == 0 && !has_pkg)
      {
         snprintf(cmd, sizeof(cmd), "npm install");
         has_pkg = 1;
      }

      /* Only record if we have something to run */
      if ((has_pkg || strlen(cmd) > strlen(prefix)) && out->dep_cmd_count < MANIFEST_MAX_DEP_CMDS)
      {
         snprintf(out->dep_cmds[out->dep_cmd_count++].cmd, sizeof(out->dep_cmds[0].cmd), "%s", cmd);
      }
   }
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

int workspace_manifest_load(const char *dir_or_path, workspace_manifest_t *out)
{
   if (!dir_or_path || !out)
      return -1;

   /* Resolve the file path */
   char path[MAX_PATH_LEN];
   struct stat st;
   if (stat(dir_or_path, &st) == 0 && S_ISDIR(st.st_mode))
      snprintf(path, sizeof(path), "%s/%s", dir_or_path, WORKSPACE_MANIFEST_FILENAME);
   else
      snprintf(path, sizeof(path), "%s", dir_or_path);

   if (access(path, F_OK) != 0)
      return -1;

   char *buf = read_file(path);
   if (!buf)
      return -1;

   cJSON *root = yaml_parse(buf);
   free(buf);
   if (!root)
      return -2;

   memset(out, 0, sizeof(*out));
   /* Defaults */
   out->index = 1;
   out->generate_rules = 1;

   /* --- repos --- */
   const cJSON *repos = cJSON_GetObjectItemCaseSensitive(root, "repos");
   if (cJSON_IsArray(repos))
   {
      const cJSON *entry = NULL;
      cJSON_ArrayForEach(entry, repos)
      {
         if (out->repo_count >= MANIFEST_MAX_REPOS)
            break;
         if (!cJSON_IsObject(entry))
            continue;

         const cJSON *url_item = cJSON_GetObjectItemCaseSensitive(entry, "url");
         if (!cJSON_IsString(url_item) || !url_item->valuestring[0])
            continue;

         manifest_repo_t *r = &out->repos[out->repo_count++];
         snprintf(r->url, sizeof(r->url), "%s", url_item->valuestring);

         const cJSON *path_item = cJSON_GetObjectItemCaseSensitive(entry, "path");
         if (cJSON_IsString(path_item) && path_item->valuestring[0])
            snprintf(r->path, sizeof(r->path), "%s", path_item->valuestring);
      }
   }

   /* --- dependencies --- */
   const cJSON *deps = cJSON_GetObjectItemCaseSensitive(root, "dependencies");
   if (cJSON_IsObject(deps))
   {
      const cJSON *lang_item = NULL;
      cJSON_ArrayForEach(lang_item, deps) parse_lang_deps(lang_item, out);
   }

   /* --- secrets --- */
   const cJSON *secrets = cJSON_GetObjectItemCaseSensitive(root, "secrets");
   if (cJSON_IsArray(secrets))
   {
      const cJSON *entry = NULL;
      cJSON_ArrayForEach(entry, secrets)
      {
         if (out->secret_count >= MANIFEST_MAX_SECRETS)
            break;
         if (!cJSON_IsObject(entry))
            continue;

         const cJSON *name_item = cJSON_GetObjectItemCaseSensitive(entry, "name");
         if (!cJSON_IsString(name_item) || !name_item->valuestring[0])
            continue;

         manifest_secret_t *s = &out->secrets[out->secret_count++];
         snprintf(s->name, sizeof(s->name), "%s", name_item->valuestring);

         const cJSON *desc_item = cJSON_GetObjectItemCaseSensitive(entry, "description");
         if (cJSON_IsString(desc_item) && desc_item->valuestring[0])
            snprintf(s->description, sizeof(s->description), "%s", desc_item->valuestring);
      }
   }

   /* --- quickstart --- */
   const cJSON *qs = cJSON_GetObjectItemCaseSensitive(root, "quickstart");
   if (cJSON_IsObject(qs))
   {
      const cJSON *idx = cJSON_GetObjectItemCaseSensitive(qs, "index");
      if (cJSON_IsBool(idx))
         out->index = cJSON_IsTrue(idx) ? 1 : 0;

      const cJSON *gr = cJSON_GetObjectItemCaseSensitive(qs, "generate_rules");
      if (cJSON_IsBool(gr))
         out->generate_rules = cJSON_IsTrue(gr) ? 1 : 0;
   }

   cJSON_Delete(root);
   return 0;
}
