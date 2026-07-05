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
#include <errno.h>
#include <fcntl.h>
#include <limits.h> /* PATH_MAX for the confined project-file browser */
#include <unistd.h>
#endif

#include "aimee_home.h"
#include "cJSON.h"
#include "server_http_identity.h" /* server_http_identity_principal — ownership scoping */
#include "wfe_def.h"
#include "yaml.h" /* yaml_emit — write blocks.yaml */
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
      cJSON_AddStringToObject(b, "consumes", wfe_artifact_name(c->consumes));
      cJSON_AddBoolToObject(b, "custom", 1);
      cJSON_AddStringToObject(b, "executor",
                              c->executor == WFE_EXEC_COMMAND ? "command" : "delegate");
      /* delegate blocks carry the editable persona + prompt (the UI edits these);
       * command blocks stay operator-only, so their argv is not surfaced. */
      if (c->executor == WFE_EXEC_DELEGATE)
      {
         cJSON_AddStringToObject(b, "persona", c->persona);
         cJSON_AddStringToObject(b, "prompt", c->prompt ? c->prompt : "");
      }
      cJSON_AddBoolToObject(b, "requires_input", c->consumes != WFE_ART_NONE);
      cJSON *acc = cJSON_AddArrayToObject(b, "accepts");
      if (c->consumes != WFE_ART_NONE)
         cJSON_AddItemToArray(acc, cJSON_CreateString(wfe_artifact_name(c->consumes)));
      cJSON_AddItemToArray(arr, b);
   }
   return emit(o, resp, cap);
}

/* ---- custom block editor (delegate-executor blocks only) ----------------------
 * The web UI can create/edit reusable DELEGATE custom blocks (a persona + prompt
 * step) the same way it edits personas/roles. It NEVER writes command-executor
 * blocks: those run arbitrary argv and stay operator-only via the blocks.yaml
 * file, so the editor cannot become a remote-code-execution surface. Existing
 * command blocks are preserved verbatim on every write. */

static int valid_artifact_name(const char *s)
{
   for (wfe_artifact_type_t a = WFE_ART_NONE; a < WFE_ART__COUNT; a++)
      if (strcmp(wfe_artifact_name(a), s) == 0)
         return 1;
   return 0;
}

/* Serialize the current custom-block registry to a cJSON blocks array, preserving
 * every block (including command blocks) so a write round-trips them untouched. */
static cJSON *registry_blocks_json(void)
{
   char e[256] = "";
   wfe_custom_registry_ensure(e, sizeof e);
   cJSON *arr = cJSON_CreateArray();
   for (int i = 0; i < wfe_custom_count(); i++)
   {
      const wfe_custom_block_t *c = wfe_custom_at(i);
      if (!c)
         continue;
      cJSON *b = cJSON_CreateObject();
      cJSON_AddStringToObject(b, "name", c->name);
      cJSON_AddStringToObject(b, "consumes", wfe_artifact_name(c->consumes));
      cJSON_AddStringToObject(b, "produces", wfe_artifact_name(c->produces));
      if (c->executor == WFE_EXEC_COMMAND)
      {
         cJSON_AddStringToObject(b, "executor", "command");
         cJSON *cmd = cJSON_AddArrayToObject(b, "command");
         for (int j = 0; c->argv[j]; j++)
            cJSON_AddItemToArray(cmd, cJSON_CreateString(c->argv[j]));
      }
      else
      {
         cJSON_AddStringToObject(b, "executor", "delegate");
         cJSON_AddStringToObject(b, "persona", c->persona);
         cJSON_AddStringToObject(b, "prompt", c->prompt ? c->prompt : "");
      }
      cJSON_AddItemToArray(arr, b);
   }
   return arr;
}

/* Emit {blocks: arr} to $AIMEE_HOME/workflows/blocks.yaml atomically (temp +
 * rename, operator-owned 0600) and reload the registry. Returns 0 or -1+err. */
static int write_blocks_and_reload(cJSON *arr, char *err_buf, size_t errlen)
{
   cJSON *root = cJSON_CreateObject();
   /* Preserve the operator's top-level settings so a UI delegate-block write can't
    * disable existing command blocks by dropping their gate. */
   if (wfe_custom_commands_allowed())
      cJSON_AddBoolToObject(root, "allow_command", 1);
   cJSON_AddNumberToObject(root, "command_timeout_ms", wfe_custom_command_timeout_ms());
   cJSON_AddItemToObject(root, "blocks", arr); /* takes ownership of arr */
   char *yaml = yaml_emit(root);
   cJSON_Delete(root);
   if (!yaml)
   {
      snprintf(err_buf, errlen, "yaml emit failed");
      return -1;
   }
   char dir[1024], path[1100], tmp[1160];
   snprintf(dir, sizeof dir, "%s/workflows", aimee_home());
   mkdir(dir, 0700);
   snprintf(path, sizeof path, "%s/blocks.yaml", dir);
   snprintf(tmp, sizeof tmp, "%s.tmp", path);
   int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0600);
   if (fd < 0)
   {
      free(yaml);
      snprintf(err_buf, errlen, "cannot open blocks.yaml for write");
      return -1;
   }
   size_t len = strlen(yaml);
   ssize_t wr = write(fd, yaml, len);
   close(fd);
   free(yaml);
   if (wr < 0 || (size_t)wr != len)
   {
      unlink(tmp);
      snprintf(err_buf, errlen, "blocks.yaml write failed");
      return -1;
   }
   if (rename(tmp, path) != 0)
   {
      unlink(tmp);
      snprintf(err_buf, errlen, "blocks.yaml rename failed");
      return -1;
   }
   return wfe_custom_registry_load(path, err_buf, errlen);
}

int wf_api_block_put(const char *name, const char *body, char *resp, int cap)
{
   if (!name || !name[0] || !safe_name(name))
      return err(resp, cap, 400, "invalid block name");
   if (wfe_block_from_name(name) != WFE_BLK_UNKNOWN)
      return err(resp, cap, 409, "name shadows a built-in block");

   cJSON *req = body ? cJSON_Parse(body) : NULL;
   if (!req)
      return err(resp, cap, 400, "invalid JSON body");
   const cJSON *jc = cJSON_GetObjectItemCaseSensitive(req, "consumes");
   const cJSON *jp = cJSON_GetObjectItemCaseSensitive(req, "produces");
   const cJSON *jper = cJSON_GetObjectItemCaseSensitive(req, "persona");
   const cJSON *jpr = cJSON_GetObjectItemCaseSensitive(req, "prompt");
   const char *consumes = cJSON_IsString(jc) && jc->valuestring[0] ? jc->valuestring : "none";
   const char *produces = cJSON_IsString(jp) && jp->valuestring[0] ? jp->valuestring : "none";
   const char *persona = cJSON_IsString(jper) ? jper->valuestring : "";
   const char *prompt = cJSON_IsString(jpr) ? jpr->valuestring : "";

   int bad = 0;
   const char *why = "";
   if (strcmp(produces, "branch") != 0 && strcmp(produces, "none") != 0)
   {
      bad = 1;
      why = "produces must be 'branch' or 'none'";
   }
   else if (!valid_artifact_name(consumes))
   {
      bad = 1;
      why = "unknown consumes type";
   }
   else if (!persona[0] || !prompt[0])
   {
      bad = 1;
      why = "delegate block needs a persona and a prompt";
   }
   if (bad)
   {
      cJSON_Delete(req);
      return err(resp, cap, 400, why);
   }

   cJSON *arr = registry_blocks_json();
   for (int i = cJSON_GetArraySize(arr) - 1; i >= 0; i--)
   {
      cJSON *b = cJSON_GetArrayItem(arr, i);
      const cJSON *n = cJSON_GetObjectItemCaseSensitive(b, "name");
      if (cJSON_IsString(n) && strcmp(n->valuestring, name) == 0)
         cJSON_DeleteItemFromArray(arr, i);
   }
   cJSON *nb = cJSON_CreateObject();
   cJSON_AddStringToObject(nb, "name", name);
   cJSON_AddStringToObject(nb, "consumes", consumes);
   cJSON_AddStringToObject(nb, "produces", produces);
   cJSON_AddStringToObject(nb, "executor", "delegate");
   cJSON_AddStringToObject(nb, "persona", persona);
   cJSON_AddStringToObject(nb, "prompt", prompt);
   cJSON_AddItemToArray(arr, nb);
   cJSON_Delete(req);

   char e[256] = "";
   if (write_blocks_and_reload(arr, e, sizeof e) != 0)
      return err(resp, cap, 500, e[0] ? e : "could not save block");
   return wf_api_blocks(resp, cap);
}

int wf_api_block_delete(const char *name, char *resp, int cap)
{
   if (!name || !name[0] || !safe_name(name))
      return err(resp, cap, 400, "invalid block name");
   const wfe_custom_block_t *ex = wfe_custom_lookup(name);
   if (!ex)
      return err(resp, cap, 404, "custom block not found");
   if (ex->executor == WFE_EXEC_COMMAND)
      return err(resp, cap, 403, "command blocks are operator-managed (edit blocks.yaml)");

   cJSON *arr = registry_blocks_json();
   for (int i = cJSON_GetArraySize(arr) - 1; i >= 0; i--)
   {
      cJSON *b = cJSON_GetArrayItem(arr, i);
      const cJSON *n = cJSON_GetObjectItemCaseSensitive(b, "name");
      if (cJSON_IsString(n) && strcmp(n->valuestring, name) == 0)
         cJSON_DeleteItemFromArray(arr, i);
   }
   char e[256] = "";
   if (write_blocks_and_reload(arr, e, sizeof e) != 0)
      return err(resp, cap, 500, e[0] ? e : "could not delete block");
   return wf_api_blocks(resp, cap);
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

/* The basename of a path (last '/'-segment), or the whole string if none. Used
 * to expose proposal_name without leaking the server FS path. */
static const char *path_basename(const char *p)
{
   const char *slash = p ? strrchr(p, '/') : NULL;
   return slash ? slash + 1 : (p ? p : "");
}

/* Ownership: true iff the item's submitter equals the calling principal (string
 * compare; both must be non-empty). A NULL/empty submitter (CLI/legacy/system
 * rows) is owned by nobody, so owner-only reads refuse it — fail-closed. */
static int wf_owns(const db1_work_item_t *wi)
{
   const char *principal = server_http_identity_principal();
   return wi->submitter[0] && principal && principal[0] && strcmp(principal, wi->submitter) == 0;
}

/* shared: serialize one work-item row. The base keys (id..repo) are unchanged so
 * existing consumers keep working; the rest are additive Proposals-page fields. */
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
   /* additive: Proposals status fields (cheap DB columns only — no file IO). */
   if (wi->proposal_path[0])
      cJSON_AddStringToObject(o, "proposal_name", path_basename(wi->proposal_path));
   cJSON_AddStringToObject(o, "pr_ref", wi->pr_ref);
   cJSON_AddStringToObject(o, "submitter", wi->submitter);
   cJSON_AddNumberToObject(o, "cum_cost_usd", wi->cum_cost_usd);
   cJSON_AddNumberToObject(o, "work_item_max_cost_usd", wi->work_item_max_cost_usd);
   cJSON_AddNumberToObject(o, "override_count", wi->override_count);
   return o;
}

/* Shared list builder: emit every row for which `keep` returns true. */
static int wf_items_filtered(char *resp, int cap, int (*keep)(const db1_work_item_t *))
{
   cJSON *o = cJSON_CreateObject();
   cJSON *arr = cJSON_AddArrayToObject(o, "items");
   db1_work_item_t *rows = NULL;
   int n = db1_work_item_list(&rows);
   for (int i = 0; i < n; i++)
      if (!keep || keep(&rows[i]))
         cJSON_AddItemToArray(arr, item_to_json(&rows[i]));
   free(rows);
   return emit(o, resp, cap);
}

int wf_api_items(char *resp, int cap)
{
   /* Owner-scoped: only the caller's own proposals (closes the list IDOR). */
   return wf_items_filtered(resp, cap, wf_owns);
}

int wf_api_items_all(char *resp, int cap)
{
   /* Unscoped operator view; route-gated by CAP_WORKFLOW_ADMIN. */
   return wf_items_filtered(resp, cap, NULL);
}

int wf_api_item(const char *id, char *resp, int cap)
{
   if (!id || !id[0])
      return err(resp, cap, 400, "missing work item id");
   db1_work_item_t wi;
   if (db1_work_item_get(id, &wi) != 1)
      return err(resp, cap, 404, "work item not found");
   if (!wf_owns(&wi))
      return err(resp, cap, 403, "not your work item");
   return emit(item_to_json(&wi), resp, cap);
}

int wf_api_events(const char *id, long after, int limit, char *resp, int cap)
{
   if (!id || !id[0])
      return err(resp, cap, 400, "missing work item id");
   db1_work_item_t wi;
   if (db1_work_item_get(id, &wi) != 1)
      return err(resp, cap, 404, "work item not found");
   if (!wf_owns(&wi))
      return err(resp, cap, 403, "not your work item");
   if (limit <= 0 || limit > 200)
      limit = 200;

   db1_lifecycle_event_t *evs = NULL;
   int n = db1_lifecycle_event_list(id, &evs);
   if (n < 0)
      return err(resp, cap, 500, "could not read events");

   cJSON *o = cJSON_CreateObject();
   cJSON *arr = cJSON_AddArrayToObject(o, "events");
   /* Events are id-ascending; take the first `limit` with id>after. next_after is
    * the id of the LAST event actually returned (so the next poll continues from
    * there, never re-reading or skipping — the item has a single writer). */
   long next_after = after;
   int emitted = 0;
   for (int i = 0; i < n && emitted < limit; i++)
   {
      if (evs[i].id <= after)
         continue;
      cJSON *e = cJSON_CreateObject();
      cJSON_AddNumberToObject(e, "id", (double)evs[i].id);
      cJSON_AddStringToObject(e, "stage", evs[i].stage);
      cJSON_AddStringToObject(e, "kind", evs[i].kind);
      cJSON_AddStringToObject(e, "actor", evs[i].actor);
      cJSON_AddStringToObject(e, "detail", evs[i].detail);
      cJSON_AddNumberToObject(e, "cost_usd", evs[i].cost_usd);
      cJSON_AddStringToObject(e, "created_at", evs[i].created_at);
      cJSON_AddItemToArray(arr, e);
      next_after = evs[i].id;
      emitted++;
   }
   free(evs);
   cJSON_AddNumberToObject(o, "next_after", (double)next_after);
   return emit(o, resp, cap);
}

/* Max proposal markdown served back (submit caps the body at 1 MB; we enforce the
 * same and flag `truncated` rather than reject, so an oversized file still renders. */
#define WF_PROPOSAL_MAX (1 * 1024 * 1024)

int wf_api_proposal(const char *id, char *resp, int cap)
{
   if (!id || !id[0])
      return err(resp, cap, 400, "missing work item id");
   db1_work_item_t wi;
   if (db1_work_item_get(id, &wi) != 1)
      return err(resp, cap, 404, "work item not found");
   if (!wf_owns(&wi))
      return err(resp, cap, 403, "not your work item");
   if (!wi.proposal_path[0])
      return err(resp, cap, 404, "no proposal file");

   /* proposal_path is a server-minted flat file in $AIMEE_HOME/workflows/proposals.
    * Confine race-free: open the fixed dir, then openat the BASENAME with O_NOFOLLOW.
    * A basename with any '/' (or . / ..) is rejected — there are no mid-path
    * components to race, so this is TOCTOU/symlink safe. */
   const char *base = path_basename(wi.proposal_path);
   if (!base[0] || strchr(base, '/') || strcmp(base, ".") == 0 || strcmp(base, "..") == 0)
      return err(resp, cap, 403, "unsafe proposal path");

   const char *home = aimee_home();
   char dir[1024];
   snprintf(dir, sizeof dir, "%s/workflows/proposals", home ? home : "/tmp");
   int dfd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
   if (dfd < 0)
      return err(resp, cap, 404, "proposals dir unavailable");
   int fd = openat(dfd, base, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
   int oerr = errno;
   close(dfd);
   if (fd < 0)
      /* O_NOFOLLOW makes a symlinked entry fail ELOOP — refuse it as forbidden
       * (never followed) rather than reporting a plain not-found. */
      return err(resp, cap, oerr == ELOOP ? 403 : 404,
                 oerr == ELOOP ? "proposal not a regular file" : "proposal file not found");

   struct stat stt;
   if (fstat(fd, &stt) != 0 || !S_ISREG(stt.st_mode))
   {
      close(fd);
      return err(resp, cap, 403, "proposal not a regular file");
   }

   char *buf = malloc(WF_PROPOSAL_MAX + 1);
   if (!buf)
   {
      close(fd);
      return err(resp, cap, 500, "out of memory");
   }
   size_t total = 0;
   ssize_t r;
   while (total < WF_PROPOSAL_MAX && (r = read(fd, buf + total, WF_PROPOSAL_MAX - total)) > 0)
      total += (size_t)r;
   close(fd);
   buf[total] = '\0';
   int truncated = (off_t)total < stt.st_size;

   cJSON *o = cJSON_CreateObject();
   cJSON_AddStringToObject(o, "proposal_md", buf);
   cJSON_AddBoolToObject(o, "truncated", truncated);
   free(buf);
   return emit(o, resp, cap);
}

/* ── lifecycle mutations (start / pause / stop / delete) ─────────────────────
 * All share the same access rule: the item's submitter OR an operator. */

static int item_terminal(const db1_work_item_t *wi)
{
   return strcmp(wi->state, "accepted") == 0 || strcmp(wi->state, "rejected") == 0 ||
          strcmp(wi->state, "abandoned") == 0;
}

/* The attested principal driving this mutation (for the audit event actor). */
static const char *wf_actor(void)
{
   const char *p = server_http_identity_principal();
   return (p && p[0]) ? p : "operator";
}

/* Resolve + owner/operator-gate an item for a mutation. On success fills `wi` and
 * returns 0; otherwise writes an error envelope and returns the HTTP status. */
static int wf_load_for_mutation(const char *id, int is_operator, db1_work_item_t *wi, char *resp,
                                int cap)
{
   if (!id || !id[0])
      return err(resp, cap, 400, "missing work item id");
   if (db1_work_item_get(id, wi) != 1)
      return err(resp, cap, 404, "work item not found");
   if (!is_operator && !wf_owns(wi))
      return err(resp, cap, 403, "not your work item");
   return 0;
}

int wf_api_item_pause(const char *id, int is_operator, char *resp, int cap)
{
   db1_work_item_t wi;
   int rc = wf_load_for_mutation(id, is_operator, &wi, resp, cap);
   if (rc)
      return rc;
   if (item_terminal(&wi))
      return err(resp, cap, 409, "run already finished");
   if (wi.pause_reason[0])
      return err(resp, cap, 409, "run is already paused");
   if (db1_work_item_set_pause(id, "operator_paused", wi.current_stage) != 0)
      return err(resp, cap, 500, "could not pause run");
   db1_lifecycle_event_add(id, wi.current_stage, "pause", wf_actor(), "paused by operator", "", 0);
   if (db1_work_item_get(id, &wi) != 1)
      return err(resp, cap, 500, "paused but could not reload");
   return emit(item_to_json(&wi), resp, cap);
}

int wf_api_item_resume(const char *id, int is_operator, char *resp, int cap)
{
   db1_work_item_t wi;
   int rc = wf_load_for_mutation(id, is_operator, &wi, resp, cap);
   if (rc)
      return rc;
   if (item_terminal(&wi))
      return err(resp, cap, 409, "run already finished");
   if (!wi.pause_reason[0])
      return err(resp, cap, 409, "run is not paused");
   /* A run parked at a human gate must be decided via Approve/Reject so the
    * signed approval is recorded — never silently un-paused here. */
   if (strcmp(wi.pause_reason, "pending_human") == 0)
      return err(resp, cap, 409, "run is at a human gate — use Approve or Reject");
   if (db1_work_item_clear_pause(id) != 0)
      return err(resp, cap, 500, "could not resume run");
   db1_lifecycle_event_add(id, wi.current_stage, "resume", wf_actor(), "resumed by operator", "",
                           0);
   if (db1_work_item_get(id, &wi) != 1)
      return err(resp, cap, 500, "resumed but could not reload");
   return emit(item_to_json(&wi), resp, cap);
}

int wf_api_item_stop(const char *id, int is_operator, char *resp, int cap)
{
   db1_work_item_t wi;
   int rc = wf_load_for_mutation(id, is_operator, &wi, resp, cap);
   if (rc)
      return rc;
   if (item_terminal(&wi))
      return err(resp, cap, 409, "run already finished");
   if (db1_work_item_set_terminal(id, "abandoned") != 0)
      return err(resp, cap, 500, "could not stop run");
   db1_lifecycle_event_add(id, wi.current_stage, "abandon", wf_actor(), "stopped by operator", "",
                           0);
   if (db1_work_item_get(id, &wi) != 1)
      return err(resp, cap, 500, "stopped but could not reload");
   return emit(item_to_json(&wi), resp, cap);
}

int wf_api_item_delete(const char *id, int is_operator, char *resp, int cap)
{
   db1_work_item_t wi;
   int rc = wf_load_for_mutation(id, is_operator, &wi, resp, cap);
   if (rc)
      return rc;
   /* Auto-stop an in-flight run first: mark it terminal so the scheduler's
    * orphan-sweep reclaims its worktree before the row disappears. */
   if (!item_terminal(&wi))
      db1_work_item_set_terminal(id, "abandoned");

   /* Best-effort unlink the server-minted proposal artifact, confined to
    * $AIMEE_HOME/workflows/proposals via a dirfd + a slash-free basename (same
    * race-free confinement as wf_api_proposal). A missing/odd file is ignored —
    * the row removal is the operation of record. */
   if (wi.proposal_path[0])
   {
      const char *base = path_basename(wi.proposal_path);
      if (base[0] && !strchr(base, '/') && strcmp(base, ".") != 0 && strcmp(base, "..") != 0)
      {
         const char *home = aimee_home();
         char dir[1024];
         snprintf(dir, sizeof dir, "%s/workflows/proposals", home ? home : "/tmp");
         int dfd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
         if (dfd >= 0)
         {
            unlinkat(dfd, base, 0); /* best effort */
            close(dfd);
         }
      }
   }

   if (db1_work_item_delete(id) != 0)
      return err(resp, cap, 500, "could not delete run");
   cJSON *o = cJSON_CreateObject();
   cJSON_AddStringToObject(o, "id", id);
   cJSON_AddBoolToObject(o, "deleted", 1);
   return emit(o, resp, cap);
}

/* ── project file browser (composer "load a proposal from the project") ──────
 * Read-only, confined to the server's local checkout root. */

/* The project root the browser is rooted at: the workflow repo if configured,
 * else the server's cwd. Returns the realpath'd root in `out` (>= PATH_MAX), or
 * NULL on failure. */
static char *wf_repo_root(char resolved[PATH_MAX])
{
   const char *root = getenv("AIMEE_WORKFLOW_REPO");
   if (!root || !root[0])
      root = ".";
   return realpath(root, resolved);
}

/* Resolve `rel` under the project root and confirm the result stays inside it
 * (blocks `..` and symlink escape). Writes the resolved absolute path to `out`
 * and returns 0; on escape/nonexistence returns an HTTP status via `err`. */
static int wf_resolve_confined(const char *rel, char out[PATH_MAX], char *resp, int cap)
{
   char root[PATH_MAX];
   if (!wf_repo_root(root))
      return err(resp, cap, 500, "project root unavailable");
   char cand[PATH_MAX];
   if (!rel || !rel[0])
      snprintf(cand, sizeof cand, "%s", root);
   else if ((int)snprintf(cand, sizeof cand, "%s/%s", root, rel) >= (int)sizeof cand)
      return err(resp, cap, 400, "path too long");
   if (!realpath(cand, out))
      return err(resp, cap, 404, "path not found");
   /* Confinement: `out` must equal root or sit strictly beneath it (root + '/'). */
   size_t rl = strlen(root);
   if (strncmp(out, root, rl) != 0 || (out[rl] != '\0' && out[rl] != '/'))
      return err(resp, cap, 400, "path escapes project root");
   return 0;
}

/* Relative path of `abs` beneath realpath'd `root` (for echoing back to the UI).
 * "" at the root; never leading '/'. */
static const char *wf_rel_of(const char *abs, const char *root)
{
   size_t rl = strlen(root);
   if (strncmp(abs, root, rl) != 0)
      return "";
   const char *r = abs + rl;
   while (*r == '/')
      r++;
   return r;
}

static int str_ends_with(const char *s, const char *suf)
{
   size_t ls = strlen(s), lf = strlen(suf);
   return ls >= lf && strcmp(s + ls - lf, suf) == 0;
}

int wf_api_repo_tree(const char *rel, char *resp, int cap)
{
   char root[PATH_MAX];
   if (!wf_repo_root(root))
      return err(resp, cap, 500, "project root unavailable");
   char abs[PATH_MAX];
   int rc = wf_resolve_confined(rel, abs, resp, cap);
   if (rc)
      return rc;

   DIR *d = opendir(abs);
   if (!d)
      return err(resp, cap, 404, "not a directory");
   cJSON *o = cJSON_CreateObject();
   cJSON_AddStringToObject(o, "path", wf_rel_of(abs, root));
   cJSON *arr = cJSON_AddArrayToObject(o, "entries");
   struct dirent *e;
   while ((e = readdir(d)) != NULL)
   {
      const char *name = e->d_name;
      if (name[0] == '.')
         continue; /* skip . / .. and hidden (incl. .git) */
      if (strcmp(name, "node_modules") == 0)
         continue;
      char child[PATH_MAX];
      if ((int)snprintf(child, sizeof child, "%s/%s", abs, name) >= (int)sizeof child)
         continue;
      struct stat stt;
      if (stat(child, &stt) != 0)
         continue;
      int is_dir = S_ISDIR(stt.st_mode);
      if (!is_dir && !str_ends_with(name, ".md"))
         continue; /* files: markdown only */
      cJSON *it = cJSON_CreateObject();
      cJSON_AddStringToObject(it, "name", name);
      cJSON_AddStringToObject(it, "type", is_dir ? "dir" : "file");
      cJSON_AddItemToArray(arr, it);
   }
   closedir(d);
   return emit(o, resp, cap);
}

int wf_api_repo_file(const char *rel, char *resp, int cap)
{
   char root[PATH_MAX];
   if (!wf_repo_root(root))
      return err(resp, cap, 500, "project root unavailable");
   if (!rel || !rel[0])
      return err(resp, cap, 400, "missing path");
   if (!str_ends_with(rel, ".md"))
      return err(resp, cap, 400, "only .md files are readable");
   char abs[PATH_MAX];
   int rc = wf_resolve_confined(rel, abs, resp, cap);
   if (rc)
      return rc;

   int fd = open(abs, O_RDONLY | O_CLOEXEC);
   if (fd < 0)
      return err(resp, cap, 404, "file not found");
   struct stat stt;
   if (fstat(fd, &stt) != 0 || !S_ISREG(stt.st_mode))
   {
      close(fd);
      return err(resp, cap, 404, "not a regular file");
   }
   char *buf = malloc(WF_PROPOSAL_MAX + 1);
   if (!buf)
   {
      close(fd);
      return err(resp, cap, 500, "out of memory");
   }
   size_t total = 0;
   ssize_t r;
   while (total < WF_PROPOSAL_MAX && (r = read(fd, buf + total, WF_PROPOSAL_MAX - total)) > 0)
      total += (size_t)r;
   close(fd);
   buf[total] = '\0';
   int truncated = (off_t)total < stt.st_size;

   cJSON *o = cJSON_CreateObject();
   cJSON_AddStringToObject(o, "path", wf_rel_of(abs, root));
   cJSON_AddStringToObject(o, "content", buf);
   cJSON_AddBoolToObject(o, "truncated", truncated);
   free(buf);
   return emit(o, resp, cap);
}
