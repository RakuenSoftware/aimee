/* server_workflow_api.c -- /v1/workflow read+author surface (W7 web composer).
 * A thin HTTP layer over the wfe_ definition model (CORE: parse / validate /
 * canonical / version) and the DB1 work-item store (run-state). Definitions are
 * YAML files under $AIMEE_HOME/workflows; save canonical-normalizes and applies
 * an optimistic lock on the on-disk version. See server_workflow_api.h. */
#include "server_workflow_api.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifndef _WIN32
#include <dirent.h>
#endif

#include "aimee_home.h"
#include "cJSON.h"
#include "wfe_def.h"
#include "wfe_iface.h"
#include "wfe_store.h"

/* ── helpers ─────────────────────────────────────────────────────────────── */

/* Serialize `root` (consumed) into resp; 200 on success, 500 if it overflows the
 * response buffer. */
static int emit(cJSON *root, char *resp, int cap)
{
   char *s = cJSON_PrintUnformatted(root);
   cJSON_Delete(root);
   if (!s)
   {
      snprintf(resp, (size_t)cap, "{\"error\":\"out of memory\"}");
      return 500;
   }
   int rc = 200;
   if ((int)strlen(s) >= cap)
   {
      snprintf(resp, (size_t)cap, "{\"error\":\"response too large\"}");
      rc = 500;
   }
   else
   {
      snprintf(resp, (size_t)cap, "%s", s);
   }
   free(s);
   return rc;
}

static int err(char *resp, int cap, int status, const char *msg)
{
   cJSON *o = cJSON_CreateObject();
   cJSON_AddStringToObject(o, "error", msg);
   char *s = cJSON_PrintUnformatted(o);
   cJSON_Delete(o);
   if (s)
   {
      snprintf(resp, (size_t)cap, "%s", s);
      free(s);
   }
   else
   {
      snprintf(resp, (size_t)cap, "{\"error\":\"%s\"}", "error");
   }
   return status;
}

/* A workflow name must be a single safe path component: [A-Za-z0-9._-], no "..",
 * non-empty, < 64 bytes. The route layer already strips slashes (one path
 * segment), but we re-check here so save/get cannot be coerced into traversal. */
static int safe_name(const char *name)
{
   if (!name || !name[0] || strlen(name) >= 64)
      return 0;
   if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
      return 0;
   for (const char *p = name; *p; p++)
   {
      char c = *p;
      int ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
               c == '.' || c == '_' || c == '-';
      if (!ok)
         return 0;
   }
   if (strstr(name, ".."))
      return 0;
   return 1;
}

static void def_path(const char *name, char *out, size_t cap)
{
   snprintf(out, cap, "%s/workflows/%s.yaml", aimee_home(), name);
}

/* Build a structured node-graph JSON for the editor to render (block names +
 * typed output + input bindings + control edges + params). */
static cJSON *def_to_json(const wfe_def_t *def)
{
   cJSON *o = cJSON_CreateObject();
   cJSON_AddStringToObject(o, "name", def->name);
   cJSON_AddStringToObject(o, "start", def->start);
   cJSON *nodes = cJSON_AddArrayToObject(o, "nodes");
   for (int i = 0; i < def->n_nodes; i++)
   {
      const wfe_node_t *nd = &def->nodes[i];
      cJSON *jn = cJSON_CreateObject();
      cJSON_AddStringToObject(jn, "id", nd->id);
      cJSON_AddStringToObject(
          jn, "block", nd->block == WFE_BLK_CUSTOM ? nd->custom_name : wfe_block_name(nd->block));
      cJSON_AddBoolToObject(jn, "custom", nd->block == WFE_BLK_CUSTOM);
      cJSON_AddStringToObject(jn, "produces", wfe_artifact_name(wfe_node_output(nd)));
      cJSON *ins = cJSON_AddArrayToObject(jn, "in");
      for (int b = 0; b < nd->n_ins; b++)
      {
         cJSON *bd = cJSON_CreateObject();
         cJSON_AddStringToObject(bd, "input", nd->ins[b].input_name);
         cJSON_AddStringToObject(bd, "producer", nd->ins[b].producer_id);
         cJSON_AddStringToObject(bd, "output", nd->ins[b].output_name);
         cJSON_AddItemToArray(ins, bd);
      }
      if (nd->next[0])
         cJSON_AddStringToObject(jn, "next", nd->next);
      if (nd->on_pass[0])
         cJSON_AddStringToObject(jn, "on_pass", nd->on_pass);
      if (nd->on_fail[0])
         cJSON_AddStringToObject(jn, "on_fail", nd->on_fail);
      if (nd->params)
         cJSON_AddItemToObject(jn, "params", cJSON_Duplicate(nd->params, 1));
      cJSON_AddItemToArray(nodes, jn);
   }
   return o;
}

/* Fill {valid,error?,name,version,canonical,def} for an already-parsed def. */
static void add_def_report(cJSON *o, wfe_def_t *def)
{
   char verr[256] = "";
   int rc = wfe_def_validate(def, verr, sizeof verr);
   cJSON_AddBoolToObject(o, "valid", rc == 0);
   cJSON_AddStringToObject(o, "name", def->name);
   char ver[65] = "";
   wfe_def_compute_version(def, ver);
   cJSON_AddStringToObject(o, "version", ver);
   char *canon = wfe_def_canonical(def);
   cJSON_AddStringToObject(o, "canonical", canon ? canon : "");
   free(canon);
   cJSON_AddItemToObject(o, "def", def_to_json(def));
   if (rc != 0)
      cJSON_AddStringToObject(o, "error", verr);
}

/* ── handlers ────────────────────────────────────────────────────────────── */

int wf_api_blocks(char *resp, int cap)
{
   char e[256] = "";
   wfe_custom_registry_ensure(e, sizeof e); /* best-effort; built-ins always listed */
   cJSON *o = cJSON_CreateObject();
   cJSON *arr = cJSON_AddArrayToObject(o, "blocks");
   /* built-ins: every catalog entry except UNKNOWN/CUSTOM sentinels */
   for (wfe_block_type_t t = WFE_BLK_UNKNOWN + 1; t < WFE_BLK_CUSTOM; t++)
   {
      cJSON *b = cJSON_CreateObject();
      cJSON_AddStringToObject(b, "name", wfe_block_name(t));
      cJSON_AddStringToObject(b, "produces", wfe_artifact_name(wfe_block_output(t)));
      cJSON_AddBoolToObject(b, "custom", 0);
      cJSON_AddBoolToObject(b, "requires_input", wfe_block_requires_input(t));
      cJSON *acc = cJSON_AddArrayToObject(b, "accepts");
      for (wfe_artifact_type_t a = WFE_ART_NONE; a < WFE_ART__COUNT; a++)
         if (wfe_block_accepts_input(t, a))
            cJSON_AddItemToArray(acc, cJSON_CreateString(wfe_artifact_name(a)));
      cJSON_AddItemToArray(arr, b);
   }
   /* config-defined custom blocks */
   for (int i = 0; i < wfe_custom_count(); i++)
   {
      const wfe_custom_block_t *c = wfe_custom_at(i);
      if (!c)
         continue;
      cJSON *b = cJSON_CreateObject();
      cJSON_AddStringToObject(b, "name", c->name);
      cJSON_AddStringToObject(b, "produces", wfe_artifact_name(c->produces));
      cJSON_AddBoolToObject(b, "custom", 1);
      cJSON_AddStringToObject(b, "executor",
                              c->executor == WFE_EXEC_COMMAND ? "command" : "delegate");
      cJSON_AddBoolToObject(b, "requires_input", c->consumes != WFE_ART_NONE);
      cJSON *acc = cJSON_AddArrayToObject(b, "accepts");
      if (c->consumes != WFE_ART_NONE)
         cJSON_AddItemToArray(acc, cJSON_CreateString(wfe_artifact_name(c->consumes)));
      cJSON_AddItemToArray(arr, b);
   }
   return emit(o, resp, cap);
}

int wf_api_list(char *resp, int cap)
{
   cJSON *o = cJSON_CreateObject();
   cJSON *arr = cJSON_AddArrayToObject(o, "defs");
#ifndef _WIN32
   char dir[1024];
   snprintf(dir, sizeof dir, "%s/workflows", aimee_home());
   DIR *d = opendir(dir);
   if (d)
   {
      struct dirent *ent;
      while ((ent = readdir(d)))
      {
         const char *dot = strrchr(ent->d_name, '.');
         if (!dot || strcmp(dot, ".yaml") != 0)
            continue;
         char name[128];
         size_t nlen = (size_t)(dot - ent->d_name);
         if (nlen >= sizeof name)
            continue;
         memcpy(name, ent->d_name, nlen);
         name[nlen] = '\0';
         char path[2048];
         snprintf(path, sizeof path, "%s/%s", dir, ent->d_name);
         char lerr[256] = "";
         wfe_def_t *def = wfe_def_load_file(path, lerr, sizeof lerr);
         cJSON *row = cJSON_CreateObject();
         cJSON_AddStringToObject(row, "name", name);
         int valid = def && wfe_def_validate(def, lerr, sizeof lerr) == 0;
         cJSON_AddBoolToObject(row, "valid", valid);
         char ver[65] = "";
         if (valid)
            wfe_def_compute_version(def, ver);
         cJSON_AddStringToObject(row, "version", ver);
         if (def)
            wfe_def_free(def);
         cJSON_AddItemToArray(arr, row);
      }
      closedir(d);
   }
#endif
   return emit(o, resp, cap);
}

int wf_api_get(const char *name, char *resp, int cap)
{
   if (!safe_name(name))
      return err(resp, cap, 400, "invalid workflow name");
   char path[2048];
   def_path(name, path, sizeof path);
   char lerr[256] = "";
   wfe_def_t *def = wfe_def_load_file(path, lerr, sizeof lerr);
   if (!def)
      return err(resp, cap, 404, "workflow not found");
   cJSON *o = cJSON_CreateObject();
   add_def_report(o, def);
   wfe_def_free(def);
   return emit(o, resp, cap);
}

int wf_api_validate(const char *body, char *resp, int cap)
{
   cJSON *req = (body && body[0]) ? cJSON_Parse(body) : NULL;
   const cJSON *jy = req ? cJSON_GetObjectItemCaseSensitive(req, "yaml") : NULL;
   if (!cJSON_IsString(jy))
   {
      cJSON_Delete(req);
      return err(resp, cap, 400, "expected {\"yaml\":\"...\"}");
   }
   char perr[256] = "";
   wfe_def_t *def = wfe_def_parse(jy->valuestring, perr, sizeof perr);
   if (!def)
   {
      cJSON_Delete(req);
      cJSON *o = cJSON_CreateObject();
      cJSON_AddBoolToObject(o, "valid", 0);
      cJSON_AddStringToObject(o, "error", perr);
      return emit(o, resp, cap);
   }
   cJSON *o = cJSON_CreateObject();
   add_def_report(o, def);
   wfe_def_free(def);
   cJSON_Delete(req);
   return emit(o, resp, cap);
}

int wf_api_save(const char *body, char *resp, int cap)
{
   cJSON *req = (body && body[0]) ? cJSON_Parse(body) : NULL;
   const cJSON *jn = req ? cJSON_GetObjectItemCaseSensitive(req, "name") : NULL;
   const cJSON *jy = req ? cJSON_GetObjectItemCaseSensitive(req, "yaml") : NULL;
   const cJSON *jp = req ? cJSON_GetObjectItemCaseSensitive(req, "prev_version") : NULL;
   if (!cJSON_IsString(jn) || !cJSON_IsString(jy))
   {
      cJSON_Delete(req);
      return err(resp, cap, 400, "expected {\"name\",\"yaml\",\"prev_version\"}");
   }
   if (!safe_name(jn->valuestring))
   {
      cJSON_Delete(req);
      return err(resp, cap, 400, "invalid workflow name");
   }
   const char *prev = cJSON_IsString(jp) ? jp->valuestring : "";

   /* parse + validate before touching disk */
   char perr[256] = "";
   wfe_def_t *def = wfe_def_parse(jy->valuestring, perr, sizeof perr);
   if (!def)
   {
      cJSON_Delete(req);
      return err(resp, cap, 400, perr);
   }
   if (wfe_def_validate(def, perr, sizeof perr) != 0)
   {
      wfe_def_free(def);
      cJSON_Delete(req);
      return err(resp, cap, 400, perr);
   }

   char path[2048];
   def_path(jn->valuestring, path, sizeof path);

   /* optimistic lock: compare prev_version against the on-disk version.
    * (Single-editor v1: a tiny check->rename race is accepted, per the slice
    * spec; a multi-writer lock is out of scope.) */
   /* existence is decided by stat (NOT parse success): a present-but-corrupt
    * file must still block a create (empty prev_version), so we never silently
    * overwrite an unparseable workflow. cur stays "" when the file won't parse,
    * so any non-empty prev_version then correctly fails the version match. */
   struct stat stx;
   int exists = (stat(path, &stx) == 0);
   char cur[65] = "";
   wfe_def_t *existing = wfe_def_load_file(path, perr, sizeof perr);
   if (existing)
   {
      wfe_def_compute_version(existing, cur);
      wfe_def_free(existing);
   }
   if (!prev[0] && exists)
   {
      wfe_def_free(def);
      cJSON_Delete(req);
      return err(resp, cap, 409, "workflow already exists (supply prev_version to overwrite)");
   }
   if (prev[0] && (!exists || strcmp(prev, cur) != 0))
   {
      wfe_def_free(def);
      cJSON_Delete(req);
      cJSON *o = cJSON_CreateObject();
      cJSON_AddStringToObject(o, "error", "version conflict");
      cJSON_AddStringToObject(o, "current_version", cur);
      char *s = cJSON_PrintUnformatted(o);
      cJSON_Delete(o);
      if (s)
      {
         snprintf(resp, (size_t)cap, "%s", s);
         free(s);
      }
      return 409;
   }

   /* normalize-on-save: write the canonical form (byte-stable round-trip) */
   char *canon = wfe_def_canonical(def);
   char ver[65] = "";
   wfe_def_compute_version(def, ver);
   wfe_def_free(def);
   if (!canon)
   {
      cJSON_Delete(req);
      return err(resp, cap, 500, "canonicalization failed");
   }

   char dir[1024];
   snprintf(dir, sizeof dir, "%s/workflows", aimee_home());
   mkdir(dir, 0755); /* best-effort; ok if it exists */

   /* atomic write: temp file in the same dir + rename over the target */
   char tmp[2100];
   snprintf(tmp, sizeof tmp, "%s/.%s.yaml.tmp", dir, jn->valuestring);
   FILE *f = fopen(tmp, "wb");
   if (!f)
   {
      free(canon);
      cJSON_Delete(req);
      return err(resp, cap, 500, "cannot write workflow");
   }
   size_t clen = strlen(canon);
   int wok = fwrite(canon, 1, clen, f) == clen;
   free(canon);
   if (fclose(f) != 0 || !wok || rename(tmp, path) != 0)
   {
      remove(tmp);
      cJSON_Delete(req);
      return err(resp, cap, 500, "cannot persist workflow");
   }

   cJSON_Delete(req);
   cJSON *o = cJSON_CreateObject();
   cJSON_AddStringToObject(o, "name", jn->valuestring);
   cJSON_AddStringToObject(o, "version", ver);
   return emit(o, resp, cap);
}

/* shared: serialize one work-item row */
static cJSON *item_to_json(const db1_work_item_t *wi)
{
   cJSON *o = cJSON_CreateObject();
   cJSON_AddStringToObject(o, "id", wi->work_item_id);
   cJSON_AddStringToObject(o, "workflow", wi->workflow_name);
   cJSON_AddStringToObject(o, "version", wi->workflow_version);
   cJSON_AddStringToObject(o, "stage", wi->current_stage);
   cJSON_AddStringToObject(o, "state", wi->state);
   cJSON_AddStringToObject(o, "mode", wi->mode);
   cJSON_AddStringToObject(o, "pause_reason", wi->pause_reason);
   cJSON_AddStringToObject(o, "repo", wi->repo);
   return o;
}

int wf_api_items(char *resp, int cap)
{
   cJSON *o = cJSON_CreateObject();
   cJSON *arr = cJSON_AddArrayToObject(o, "items");
   db1_work_item_t *rows = NULL;
   int n = db1_work_item_list(&rows);
   for (int i = 0; i < n; i++)
      cJSON_AddItemToArray(arr, item_to_json(&rows[i]));
   free(rows);
   return emit(o, resp, cap);
}

int wf_api_item(const char *id, char *resp, int cap)
{
   if (!id || !id[0])
      return err(resp, cap, 400, "missing work item id");
   db1_work_item_t wi;
   if (db1_work_item_get(id, &wi) != 1)
      return err(resp, cap, 404, "work item not found");
   return emit(item_to_json(&wi), resp, cap);
}
