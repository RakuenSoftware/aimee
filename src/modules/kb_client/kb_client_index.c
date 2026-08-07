/* kb_client_index.c: thin client wrappers for the aimee-kb /v1 code-index API */
#include "kb_client.h"
#include "kb_client_internal.h"
#include "code_collect.h"
#include "cJSON.h"
#include "log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Scan timeout is generous because canonical scans of large monorepos can take
 * tens of seconds. The shared v1 helpers choose remote HTTP when configured and
 * otherwise tunnel the same /v1 route over the local UDS transport. */
#define KB_CLIENT_INDEX_SCAN_TIMEOUT_DEFAULT_MS (5 * 60 * 1000)

static int kb_index_find_parse(cJSON *resp, term_hit_t *out, int max)
{
   if (!resp)
      return 0;
   int count = 0;
   cJSON *hits = cJSON_GetObjectItemCaseSensitive(resp, "hits");
   if (cJSON_IsArray(hits))
   {
      cJSON *h;
      cJSON_ArrayForEach(h, hits)
      {
         if (count >= max)
            break;
         cJSON *p = cJSON_GetObjectItemCaseSensitive(h, "project");
         cJSON *f = cJSON_GetObjectItemCaseSensitive(h, "file_path");
         cJSON *l = cJSON_GetObjectItemCaseSensitive(h, "line");
         cJSON *k = cJSON_GetObjectItemCaseSensitive(h, "kind");
         if (cJSON_IsString(p))
            snprintf(out[count].project, sizeof(out[count].project), "%s", p->valuestring);
         if (cJSON_IsString(f))
            snprintf(out[count].file_path, sizeof(out[count].file_path), "%s", f->valuestring);
         out[count].line = cJSON_IsNumber(l) ? (int)l->valuedouble : 0;
         cJSON *le = cJSON_GetObjectItemCaseSensitive(h, "line_end");
         out[count].line_end = cJSON_IsNumber(le) ? (int)le->valuedouble : 0;
         if (cJSON_IsString(k))
            snprintf(out[count].kind, sizeof(out[count].kind), "%s", k->valuestring);
         count++;
      }
   }
   return count;
}

static int kb_index_project_list_parse(cJSON *resp, project_info_t *out, int max)
{
   if (!resp)
      return -1;
   int count = 0;
   cJSON *projects = cJSON_GetObjectItemCaseSensitive(resp, "projects");
   if (!cJSON_IsArray(projects))
      return -1;
   if (cJSON_IsArray(projects))
   {
      cJSON *p;
      cJSON_ArrayForEach(p, projects)
      {
         if (count >= max)
            break;
         cJSON *n = cJSON_GetObjectItemCaseSensitive(p, "name");
         cJSON *r = cJSON_GetObjectItemCaseSensitive(p, "root");
         cJSON *s = cJSON_GetObjectItemCaseSensitive(p, "scanned_at");
         if (cJSON_IsString(n))
            snprintf(out[count].name, sizeof(out[count].name), "%s", n->valuestring);
         if (cJSON_IsString(r))
            snprintf(out[count].root, sizeof(out[count].root), "%s", r->valuestring);
         if (cJSON_IsString(s))
            snprintf(out[count].scanned_at, sizeof(out[count].scanned_at), "%s", s->valuestring);
         count++;
      }
   }
   return count;
}

static int kb_index_code_search_parse(cJSON *resp, code_search_hit_t *out, int max)
{
   if (!resp)
      return 0;
   int count = 0;
   cJSON *hits = cJSON_GetObjectItemCaseSensitive(resp, "hits");
   if (cJSON_IsArray(hits))
   {
      cJSON *h;
      cJSON_ArrayForEach(h, hits)
      {
         if (count >= max)
            break;
         cJSON *p = cJSON_GetObjectItemCaseSensitive(h, "project");
         cJSON *f = cJSON_GetObjectItemCaseSensitive(h, "file_path");
         cJSON *s = cJSON_GetObjectItemCaseSensitive(h, "snippet");
         cJSON *r = cJSON_GetObjectItemCaseSensitive(h, "rank");
         cJSON *ln = cJSON_GetObjectItemCaseSensitive(h, "line");
         if (cJSON_IsString(p))
            snprintf(out[count].project, sizeof(out[count].project), "%s", p->valuestring);
         if (cJSON_IsString(f))
            snprintf(out[count].file_path, sizeof(out[count].file_path), "%s", f->valuestring);
         if (cJSON_IsString(s))
            snprintf(out[count].snippet, sizeof(out[count].snippet), "%s", s->valuestring);
         out[count].rank = cJSON_IsNumber(r) ? r->valuedouble : 0.0;
         /* P1b span enrichment: matched line, present only when the search
          * enriched (absent -> 0). */
         out[count].line = (cJSON_IsNumber(ln) && ln->valueint > 0) ? ln->valueint : 0;
         count++;
      }
   }
   return count;
}

static int kb_index_find_callers_parse(cJSON *resp, caller_hit_t *out, int max)
{
   if (!resp)
      return 0;
   int count = 0;
   cJSON *hits = cJSON_GetObjectItemCaseSensitive(resp, "hits");
   if (cJSON_IsArray(hits))
   {
      cJSON *h;
      cJSON_ArrayForEach(h, hits)
      {
         if (count >= max)
            break;
         cJSON *p = cJSON_GetObjectItemCaseSensitive(h, "project");
         cJSON *f = cJSON_GetObjectItemCaseSensitive(h, "file_path");
         cJSON *cf = cJSON_GetObjectItemCaseSensitive(h, "caller");
         cJSON *l = cJSON_GetObjectItemCaseSensitive(h, "line");
         if (cJSON_IsString(p))
            snprintf(out[count].project, sizeof(out[count].project), "%s", p->valuestring);
         if (cJSON_IsString(f))
            snprintf(out[count].file_path, sizeof(out[count].file_path), "%s", f->valuestring);
         if (cJSON_IsString(cf))
            snprintf(out[count].caller, sizeof(out[count].caller), "%s", cf->valuestring);
         out[count].line = cJSON_IsNumber(l) ? (int)l->valuedouble : 0;
         count++;
      }
   }
   return count;
}

int kb_client_code_scan_push(const char *name, const char *root, int force, void *files_arr_v,
                             kb_client_index_scan_result_t *out)
{
   cJSON *files_arr = (cJSON *)files_arr_v;

   if (!name || !name[0] || !root || !root[0])
   {
      if (files_arr)
         cJSON_Delete(files_arr);
      if (out)
      {
         out->skipped = 1;
         snprintf(out->reason, sizeof(out->reason), "missing_project_root");
         snprintf(out->message, sizeof(out->message),
                  "code index scan requires explicit project and root");
      }
      return -1;
   }

   cJSON *req = cJSON_CreateObject();
   if (!req)
   {
      if (files_arr)
         cJSON_Delete(files_arr);
      return kb_client_index_scan_apply_response(NULL, out);
   }
   cJSON_AddStringToObject(req, "project", name);
   cJSON_AddStringToObject(req, "root_path", root);
   if (force)
      cJSON_AddBoolToObject(req, "force", 1);
   /* Caller-supplied {"rel_path","content"} entries (adopted) let the kb index
    * without filesystem access — used both for a remote kb and for files the
    * thin client pushes for a workspace the server cannot see. */
   if (files_arr)
      cJSON_AddItemToObject(req, "files", files_arr);

   int timeout_ms = kb_client_index_scan_timeout_ms();
   int http_status = 0;
   char *json = kb_client_v1_post_json("/v1/code/scan", req, timeout_ms, &http_status);
   cJSON_Delete(req);
   if (!json)
   {
      /* Say which failure this was. Collapsing every empty reply into "knowledge
       * service unavailable" hid a scan that simply outran its timeout while the
       * kb was healthy and still working -- the status stayed green, the health
       * endpoint kept answering, and the operator had nothing to act on. */
      int rc = kb_client_index_scan_apply_response(NULL, out);
      if (out)
      {
         snprintf(out->reason, sizeof(out->reason), "error");
         if (http_status >= 400)
            snprintf(out->message, sizeof(out->message),
                     "code index scan rejected by the knowledge service (HTTP %d)", http_status);
         else
            snprintf(out->message, sizeof(out->message),
                     "code index scan got no reply within %ds — the knowledge service may still "
                     "be scanning; raise AIMEE_KB_SCAN_TIMEOUT_MS for a tree this size",
                     timeout_ms / 1000);
      }
      return rc;
   }

   cJSON *resp = cJSON_Parse(json);
   free(json);
   int rc = kb_client_index_scan_apply_response(resp, out);
   if (rc == 0 && out && out->projects == 0)
      out->projects = 1;
   if (resp)
      cJSON_Delete(resp);
   return rc;
}

#ifdef AIMEE_POSIX
/* Per-batch content budget for pushed-file scans. Batching keeps client
 * memory to ~one batch regardless of tree size, gives per-batch progress
 * against the scan timeout, and stays under the 1 MB request-body cap of
 * pre-KB_HTTP_BODY_MAX aimee-kb images (which silently truncated bigger
 * bodies into 400 "invalid json"). The kb scan upserts per project+path, so
 * batches accumulate — the same contract the thin client's /v1/index/ingest
 * streamer relies on. */
#define KB_CLIENT_SCAN_BATCH_BYTES (600 * 1024)

/* Streaming scan-push state: code_collect_files_cb hands over one file at a
 * time; we accumulate byte-bounded batches and POST each the moment it fills,
 * keeping memory to ~one batch regardless of tree size. */
typedef struct
{
   const char *name;
   const char *root;
   int force;
   cJSON *batch; /* current open batch, or NULL */
   size_t batch_bytes;
   int batches;   /* batches pushed */
   int failed_rc; /* rc of the first failed batch push; 0 while all succeed */
   kb_client_index_scan_result_t fail_res; /* result of that failed push */
   kb_client_index_scan_result_t agg;      /* aggregate of successful pushes */
} kb_scan_push_ctx_t;

/* Push the current batch (kb_client_code_scan_push adopts/frees it) and fold
 * the per-batch result into the aggregate. */
static void kb_scan_push_flush(kb_scan_push_ctx_t *s)
{
   if (!s->batch)
      return;
   if (cJSON_GetArraySize(s->batch) == 0 || s->failed_rc != 0)
   {
      cJSON_Delete(s->batch);
      s->batch = NULL;
      s->batch_bytes = 0;
      return;
   }
   kb_client_index_scan_result_t res;
   memset(&res, 0, sizeof(res));
   int rc = kb_client_code_scan_push(s->name, s->root, s->force, s->batch, &res);
   s->batch = NULL;
   s->batch_bytes = 0;
   s->batches++;
   if (rc == 0 && !res.skipped)
   {
      s->agg.projects = 1;
      s->agg.files += res.files;
      s->agg.inspected += res.inspected;
   }
   else
   {
      s->failed_rc = rc != 0 ? rc : -1;
      s->fail_res = res;
   }
}

static int kb_scan_push_collect_cb(const char *rel_path, const char *content, void *ctx)
{
   kb_scan_push_ctx_t *s = (kb_scan_push_ctx_t *)ctx;
   size_t flen = strlen(content) + strlen(rel_path) + 64;

   if (s->batch && cJSON_GetArraySize(s->batch) > 0 &&
       s->batch_bytes + flen > KB_CLIENT_SCAN_BATCH_BYTES)
      kb_scan_push_flush(s);
   if (s->failed_rc != 0)
      return 1; /* a batch failed — stop the walk, report the failure */

   if (!s->batch)
   {
      s->batch = cJSON_CreateArray();
      if (!s->batch)
         return 1; /* OOM — stop; the batches so far are already indexed */
   }
   cJSON *entry = cJSON_CreateObject();
   if (!entry)
      return 0; /* skip this file, keep walking */
   cJSON_AddStringToObject(entry, "rel_path", rel_path);
   cJSON_AddStringToObject(entry, "content", content);
   cJSON_AddItemToArray(s->batch, entry);
   s->batch_bytes += flen;
   return 0;
}
#endif

static int kb_client_index_scan_v1(const char *name, const char *root, int force,
                                   kb_client_index_scan_result_t *out)
{
#ifdef AIMEE_POSIX
   /* When aimee-kb is remote (AIMEE_KB_API_URL set), the container cannot reach
    * the host filesystem via root_path.  Enumerate and push file contents so
    * the handler can index without filesystem access — in byte-bounded batches,
    * since the kb caps request bodies at 1 MB. */
   if (name && name[0] && root && root[0] && kb_client_v1_base_url())
   {
      kb_scan_push_ctx_t s;
      memset(&s, 0, sizeof(s));
      s.name = name;
      s.root = root;
      s.force = force;
      code_collect_files_cb(root, kb_scan_push_collect_cb, &s);
      kb_scan_push_flush(&s); /* flush the trailing partial batch */
      if (s.failed_rc != 0)
      {
         if (out)
            *out = s.fail_res;
         return s.failed_rc;
      }
      if (s.batches > 0)
      {
         if (out)
            *out = s.agg;
         return 0;
      }
      /* No indexable files collected: push an empty files array so the kb
       * still registers the project + queues curation (prior behavior),
       * rather than falling back to a root_path scan of a filesystem the kb
       * cannot see. */
      return kb_client_code_scan_push(name, root, force, cJSON_CreateArray(), out);
   }
#endif
   return kb_client_code_scan_push(name, root, force, NULL, out);
}

int kb_client_index_scan(const char *name, const char *root, int force,
                         kb_client_index_scan_result_t *out)
{
   if (out)
      memset(out, 0, sizeof(*out));

   return kb_client_index_scan_v1(name, root, force, out);
}

char *kb_client_index_project_lifecycle_json(const char *operation, const char *project,
                                             const char *confirm_hash, const char *reason,
                                             int retention_days, int *http_status)
{
   if (http_status)
      *http_status = 0;
   if (!operation || !operation[0] || !project || !project[0] ||
       (strcmp(operation, "detach") != 0 && strcmp(operation, "purge") != 0 &&
        strcmp(operation, "gc") != 0))
      return NULL;
   char path[96];
   snprintf(path, sizeof(path), "/v1/code/project/%s", operation);
   cJSON *req = cJSON_CreateObject();
   if (!req)
      return NULL;
   cJSON_AddStringToObject(req, "project", project);
   if (confirm_hash && confirm_hash[0])
      cJSON_AddStringToObject(req, "confirm_hash", confirm_hash);
   if (reason && reason[0])
      cJSON_AddStringToObject(req, "reason", reason);
   if (strcmp(operation, "gc") == 0)
      cJSON_AddNumberToObject(req, "retention_days", retention_days);
   char *json = kb_client_v1_post_json(path, req, kb_client_index_read_timeout_ms(), http_status);
   cJSON_Delete(req);
   return json;
}

int kb_client_index_find_scoped(const char *project, int all_projects, const char *identifier,
                                term_hit_t *out, int max)
{
   if (!identifier || !identifier[0] || !out || max <= 0)
      return 0;
   memset(out, 0, sizeof(*out) * (size_t)max);

   char *encoded = kb_client_query_escape(identifier);
   if (!encoded)
      return 0;
   char *encoded_project = NULL;
   if (project && project[0])
   {
      encoded_project = kb_client_query_escape(project);
      if (!encoded_project)
      {
         free(encoded);
         return 0;
      }
   }
   size_t path_len = strlen(encoded) + (encoded_project ? strlen(encoded_project) : 0) + 112;
   char *path = malloc(path_len);
   if (!path)
   {
      free(encoded_project);
      free(encoded);
      return 0;
   }
   snprintf(path, path_len, "/v1/code/find?identifier=%s&max_results=%d%s%s%s", encoded, max,
            all_projects ? "&scope=all" : "", encoded_project ? "&project=" : "",
            encoded_project ? encoded_project : "");
   free(encoded_project);
   free(encoded);
   char *json = kb_client_v1_get_json(path, kb_client_index_read_timeout_ms(), NULL);
   free(path);
   if (!json)
      return -1;
   cJSON *resp = cJSON_Parse(json);
   free(json);
   if (!resp)
      return -1;
   int count = kb_index_find_parse(resp, out, max);
   cJSON_Delete(resp);
   return count;
}

int kb_client_index_find_project(const char *project, const char *identifier, term_hit_t *out,
                                 int max)
{
   return kb_client_index_find_scoped(project, 0, identifier, out, max);
}

int kb_client_index_find(const char *identifier, term_hit_t *out, int max)
{
   return kb_client_index_find_scoped(NULL, 1, identifier, out, max);
}

int kb_client_index_list(project_info_t *out, int max)
{
   if (!out || max <= 0)
      return 0;
   memset(out, 0, sizeof(*out) * (size_t)max);

   char path[64];
   snprintf(path, sizeof(path), "/v1/code/projects?max_results=%d", max);
   char *json = kb_client_v1_get_json(path, kb_client_index_read_timeout_ms(), NULL);
   if (!json)
      return -1; /* service unavailable — caller can distinguish from empty (0) */
   cJSON *resp = cJSON_Parse(json);
   free(json);
   int count = kb_index_project_list_parse(resp, out, max);
   cJSON_Delete(resp);
   return count;
}

int kb_client_index_blast_radius(const char *project, const char *file_path, blast_radius_t *out)
{
   if (!project || !file_path || !out)
      return -1;
   memset(out, 0, sizeof(*out));
   snprintf(out->file, sizeof(out->file), "%s", file_path);

   char *project_q = kb_client_query_escape(project);
   char *file_q = kb_client_query_escape(file_path);
   if (!project_q || !file_q)
   {
      free(project_q);
      free(file_q);
      return -1;
   }
   char path[MAX_PATH_LEN + 512];
   snprintf(path, sizeof(path), "/v1/code/blast-radius?project=%s&file_path=%s", project_q, file_q);
   free(project_q);
   free(file_q);
   int status = 0;
   char *json = kb_client_v1_get_json(path, kb_client_index_read_timeout_ms(), &status);
   /* Every refusal below used to return a bare -1, and the caller rendered all of
    * them as "blast radius lookup failed". Four distinct causes behind one string
    * is what made a kb serving a perfectly good 200 indistinguishable from a kb
    * that was never reached. Name which boundary refused. */
   if (!json || status < 200 || status >= 300)
   {
      aimee_log(LOG_ERROR, "kb_client", "blast_radius '%s' '%s': fetch failed (http=%d, body=%s)",
                project, file_path, status, json ? "present" : "none");
      free(json);
      return -1;
   }
   size_t json_len = strlen(json);
   cJSON *resp = cJSON_Parse(json);
   if (!resp)
   {
      /* A truncated body parses as garbage. Report the length and the tail so a
       * response cut off by a transport cap is obvious rather than looking like
       * a malformed kb. */
      aimee_log(LOG_ERROR, "kb_client",
                "blast_radius '%s' '%s': unparseable response (%zu bytes, tail: %.40s)", project,
                file_path, json_len, json_len > 40 ? json + json_len - 40 : json);
      free(json);
      return -1;
   }
   free(json);

   cJSON *file = cJSON_GetObjectItemCaseSensitive(resp, "file");
   if (cJSON_IsString(file))
      snprintf(out->file, sizeof(out->file), "%s", file->valuestring);

   cJSON *error = cJSON_GetObjectItemCaseSensitive(resp, "error");
   if (cJSON_IsString(error))
   {
      cJSON_Delete(resp);
      return -1;
   }
   cJSON *project_json = cJSON_GetObjectItemCaseSensitive(resp, "project");
   cJSON *generation = cJSON_GetObjectItemCaseSensitive(resp, "generation");
   cJSON *freshness = cJSON_GetObjectItemCaseSensitive(resp, "freshness");
   cJSON *dependency_edges = cJSON_GetObjectItemCaseSensitive(resp, "dependency_edges");
   cJSON *dependent_edges = cJSON_GetObjectItemCaseSensitive(resp, "dependent_edges");
   /* The contract check lives in kb_client_index_parse.c so it can be asserted
    * against a recorded kb payload without a live kb. It names the first failing
    * term, because one malformed edge anywhere rejects the whole lookup and
    * "the payload was wrong" is not actionable on its own. */
   char why[64] = "";
   if (!kb_client_index_blast_response_valid(resp, why, sizeof(why)))
   {
      aimee_log(LOG_ERROR, "kb_client", "blast_radius '%s' '%s': response rejected on '%s'",
                project, file_path, why);
      cJSON_Delete(resp);
      return -1;
   }
   snprintf(out->project, sizeof(out->project), "%s", project_json->valuestring);
   out->generation = (long long)generation->valuedouble;
   snprintf(out->freshness, sizeof(out->freshness), "%s", freshness->valuestring);
   out->resolved = 1;

   cJSON *edge;
   cJSON_ArrayForEach(edge, dependency_edges)
   {
      if (out->dependency_count >= 64)
         break;
      cJSON *identity = cJSON_GetObjectItemCaseSensitive(edge, "identity");
      int i = out->dependency_count++;
      snprintf(out->dependencies[i], MAX_PATH_LEN, "%s", identity->valuestring);
      cJSON *v = cJSON_GetObjectItemCaseSensitive(edge, "provenance");
      snprintf(out->dependency_meta[i].provenance, sizeof(out->dependency_meta[i].provenance), "%s",
               v->valuestring);
      v = cJSON_GetObjectItemCaseSensitive(edge, "confidence");
      snprintf(out->dependency_meta[i].confidence, sizeof(out->dependency_meta[i].confidence), "%s",
               v->valuestring);
      v = cJSON_GetObjectItemCaseSensitive(edge, "project");
      snprintf(out->dependency_meta[i].project, sizeof(out->dependency_meta[i].project), "%s",
               v->valuestring);
      v = cJSON_GetObjectItemCaseSensitive(edge, "generation");
      out->dependency_meta[i].generation = (long long)v->valuedouble;
      v = cJSON_GetObjectItemCaseSensitive(edge, "freshness");
      snprintf(out->dependency_meta[i].freshness, sizeof(out->dependency_meta[i].freshness), "%s",
               v->valuestring);
   }

   cJSON_ArrayForEach(edge, dependent_edges)
   {
      if (out->dependent_count >= 64)
         break;
      cJSON *edge_path = cJSON_GetObjectItemCaseSensitive(edge, "path");
      int i = out->dependent_count++;
      snprintf(out->dependents[i], MAX_PATH_LEN, "%s", edge_path->valuestring);
      cJSON *v = cJSON_GetObjectItemCaseSensitive(edge, "provenance");
      snprintf(out->dependent_meta[i].provenance, sizeof(out->dependent_meta[i].provenance), "%s",
               v->valuestring);
      v = cJSON_GetObjectItemCaseSensitive(edge, "confidence");
      snprintf(out->dependent_meta[i].confidence, sizeof(out->dependent_meta[i].confidence), "%s",
               v->valuestring);
      v = cJSON_GetObjectItemCaseSensitive(edge, "project");
      snprintf(out->dependent_meta[i].project, sizeof(out->dependent_meta[i].project), "%s",
               v->valuestring);
      v = cJSON_GetObjectItemCaseSensitive(edge, "generation");
      out->dependent_meta[i].generation = (long long)v->valuedouble;
      v = cJSON_GetObjectItemCaseSensitive(edge, "freshness");
      snprintf(out->dependent_meta[i].freshness, sizeof(out->dependent_meta[i].freshness), "%s",
               v->valuestring);
   }
   cJSON_Delete(resp);
   return 0;
}

static const char *kb_index_preview_severity(int dependent_count)
{
   if (dependent_count >= 10)
      return "red";
   if (dependent_count >= 3)
      return "yellow";
   return "green";
}

static int kb_index_preview_severity_rank(const char *severity)
{
   if (severity && strcmp(severity, "red") == 0)
      return 2;
   if (severity && strcmp(severity, "yellow") == 0)
      return 1;
   return 0;
}

static char *kb_client_index_blast_radius_preview_v1(const char *project, char **paths,
                                                     int path_count)
{
   cJSON *root = cJSON_CreateObject();
   if (!root)
      return NULL;
   cJSON *files = cJSON_AddArrayToObject(root, "files");
   cJSON *warnings = cJSON_CreateArray();
   if (!files || !warnings)
   {
      cJSON_Delete(root);
      cJSON_Delete(warnings);
      return NULL;
   }

   int total_dependents = 0;
   int max_severity = 0;
   int red_count = 0;
   char dirs[20][64];
   int dir_count = 0;

   for (int i = 0; i < path_count; i++)
   {
      const char *path = paths[i] ? paths[i] : "";
      blast_radius_t br;
      int rc = kb_client_index_blast_radius(project, path, &br);

      cJSON *file = cJSON_CreateObject();
      cJSON_AddStringToObject(file, "path", path);
      cJSON *deps = cJSON_AddArrayToObject(file, "dependents");
      cJSON *edges = cJSON_AddArrayToObject(file, "dependent_edges");
      int dependent_count = 0;
      if (rc == 0)
      {
         cJSON_AddStringToObject(file, "project", br.project);
         cJSON_AddNumberToObject(file, "generation", (double)br.generation);
         cJSON_AddStringToObject(file, "freshness", br.freshness);
         cJSON_AddBoolToObject(file, "resolved", br.resolved);
         dependent_count = br.dependent_count;
         for (int j = 0; j < br.dependent_count; j++)
         {
            cJSON_AddItemToArray(deps, cJSON_CreateString(br.dependents[j]));
            cJSON *edge = cJSON_CreateObject();
            cJSON_AddStringToObject(edge, "path", br.dependents[j]);
            cJSON_AddStringToObject(edge, "provenance", br.dependent_meta[j].provenance);
            cJSON_AddStringToObject(edge, "confidence", br.dependent_meta[j].confidence);
            cJSON_AddStringToObject(edge, "project", br.dependent_meta[j].project);
            cJSON_AddNumberToObject(edge, "generation", (double)br.dependent_meta[j].generation);
            cJSON_AddStringToObject(edge, "freshness", br.dependent_meta[j].freshness);
            cJSON_AddItemToArray(edges, edge);
         }
      }
      else
      {
         cJSON_AddStringToObject(file, "status", "unavailable");
         cJSON_AddBoolToObject(file, "resolved", 0);
      }
      cJSON_AddNumberToObject(file, "dependent_count", dependent_count);
      const char *severity = kb_index_preview_severity(dependent_count);
      cJSON_AddStringToObject(file, "severity", severity);
      cJSON_AddItemToArray(files, file);

      total_dependents += dependent_count;
      int rank = kb_index_preview_severity_rank(severity);
      if (rank > max_severity)
         max_severity = rank;
      if (rank == 2)
         red_count++;
      if (dependent_count > 10)
      {
         char warn[256];
         snprintf(warn, sizeof(warn), "%s has %d dependents, consider splitting the change", path,
                  dependent_count);
         cJSON_AddItemToArray(warnings, cJSON_CreateString(warn));
      }

      const char *slash = strrchr(path, '/');
      if (slash && dir_count < 20)
      {
         char prefix[64];
         int len = (int)(slash - path);
         if (len > 63)
            len = 63;
         snprintf(prefix, sizeof(prefix), "%.*s", len, path);
         int found = 0;
         for (int d = 0; d < dir_count; d++)
            if (strcmp(dirs[d], prefix) == 0)
               found = 1;
         if (!found)
            snprintf(dirs[dir_count++], sizeof(dirs[0]), "%s", prefix);
      }
   }

   if (red_count > 1)
      cJSON_AddItemToArray(warnings,
                           cJSON_CreateString("Multiple high-impact files in this change set"));
   if (dir_count > 2)
      cJSON_AddItemToArray(
          warnings, cJSON_CreateString("Changes span multiple subsystems, consider separate PRs"));
   cJSON_AddNumberToObject(root, "total_dependents", total_dependents);
   cJSON_AddStringToObject(root, "severity",
                           max_severity == 2   ? "red"
                           : max_severity == 1 ? "yellow"
                                               : "green");
   cJSON_AddItemToObject(root, "warnings", warnings);
   char *json = cJSON_PrintUnformatted(root);
   cJSON_Delete(root);
   return json ? json : strdup("{}");
}

char *kb_client_index_blast_radius_preview_json(const char *project, char **paths, int path_count)
{
   if (!project || !paths || path_count < 1)
      return NULL;

   return kb_client_index_blast_radius_preview_v1(project, paths, path_count);
}

int kb_client_index_structure(const char *project, const char *file_path, definition_t *out,
                              int max)
{
   if (!project || !file_path || !out || max <= 0)
      return 0;
   memset(out, 0, sizeof(*out) * (size_t)max);

   char *project_q = kb_client_query_escape(project);
   char *file_q = kb_client_query_escape(file_path);
   if (!project_q || !file_q)
   {
      free(project_q);
      free(file_q);
      return 0;
   }
   char path[MAX_PATH_LEN + 512];
   snprintf(path, sizeof(path), "/v1/code/structure?project=%s&file_path=%s&max_results=%d",
            project_q, file_q, max);
   free(project_q);
   free(file_q);
   char *json = kb_client_v1_get_json(path, kb_client_index_read_timeout_ms(), NULL);
   if (!json)
      return -1;
   cJSON *resp = cJSON_Parse(json);
   free(json);
   if (!resp)
      return -1;

   int count = 0;
   cJSON *defs = cJSON_GetObjectItemCaseSensitive(resp, "definitions");
   if (cJSON_IsArray(defs))
   {
      cJSON *d;
      cJSON_ArrayForEach(d, defs)
      {
         if (count >= max)
            break;
         cJSON *name = cJSON_GetObjectItemCaseSensitive(d, "name");
         cJSON *kind = cJSON_GetObjectItemCaseSensitive(d, "kind");
         cJSON *line = cJSON_GetObjectItemCaseSensitive(d, "line");
         if (cJSON_IsString(name))
            snprintf(out[count].name, sizeof(out[count].name), "%s", name->valuestring);
         if (cJSON_IsString(kind))
            snprintf(out[count].kind, sizeof(out[count].kind), "%s", kind->valuestring);
         out[count].line = cJSON_IsNumber(line) ? (int)line->valuedouble : 0;
         cJSON *line_end = cJSON_GetObjectItemCaseSensitive(d, "line_end");
         out[count].line_end = cJSON_IsNumber(line_end) ? (int)line_end->valuedouble : 0;
         count++;
      }
   }
   cJSON_Delete(resp);
   return count;
}

int kb_client_index_project_stats(const char *project, int *files_out, int *defs_out)
{
   if (files_out)
      *files_out = 0;
   if (defs_out)
      *defs_out = 0;
   if (!project || !project[0])
      return -1;

   {
      char *project_q = kb_client_query_escape(project);
      if (!project_q)
         return -1;
      char path[512];
      snprintf(path, sizeof(path), "/v1/code/project-stats?project=%s", project_q);
      free(project_q);
      char *json = kb_client_v1_get_json(path, kb_client_index_read_timeout_ms(), NULL);
      if (!json)
         return -1;
      cJSON *resp = cJSON_Parse(json);
      free(json);
      if (!resp)
         return -1;

      cJSON *status = cJSON_GetObjectItemCaseSensitive(resp, "status");
      int rc = -1;
      if (cJSON_IsString(status) && strcmp(status->valuestring, "ok") == 0)
      {
         cJSON *f = cJSON_GetObjectItemCaseSensitive(resp, "files");
         cJSON *d = cJSON_GetObjectItemCaseSensitive(resp, "definitions");
         if (files_out && cJSON_IsNumber(f))
            *files_out = (int)f->valuedouble;
         if (defs_out && cJSON_IsNumber(d))
            *defs_out = (int)d->valuedouble;
         rc = 0;
      }
      cJSON_Delete(resp);
      return rc;
   }
}

int kb_client_index_project_lang(const char *project, char *buf, size_t bufsz)
{
   if (!buf || bufsz < 3)
      return -1;
   buf[0] = '[';
   buf[1] = ']';
   buf[2] = '\0';
   if (!project || !project[0])
      return -1;

   {
      char *project_q = kb_client_query_escape(project);
      if (!project_q)
         return -1;
      char path[512];
      snprintf(path, sizeof(path), "/v1/code/project-stats?project=%s", project_q);
      free(project_q);
      char *json = kb_client_v1_get_json(path, kb_client_index_read_timeout_ms(), NULL);
      if (!json)
         return -1;
      cJSON *resp = cJSON_Parse(json);
      free(json);
      if (!resp)
         return -1;

      cJSON *status = cJSON_GetObjectItemCaseSensitive(resp, "status");
      int rc = -1;
      if (cJSON_IsString(status) && strcmp(status->valuestring, "ok") == 0)
      {
         cJSON *langs = cJSON_GetObjectItemCaseSensitive(resp, "langs");
         if (cJSON_IsArray(langs))
         {
            char *printed = cJSON_PrintUnformatted(langs);
            if (printed)
            {
               snprintf(buf, bufsz, "%s", printed);
               free(printed);
            }
         }
         rc = 0;
      }
      cJSON_Delete(resp);
      return rc;
   }
}

int kb_client_index_code_search_scoped(const char *query, const char *project, int all_projects,
                                       code_search_hit_t *out, int max)
{
   if (!query || !query[0] || !out || max <= 0)
      return 0;
   memset(out, 0, sizeof(*out) * (size_t)max);

   char *query_q = kb_client_query_escape(query);
   char *project_q = (project && project[0]) ? kb_client_query_escape(project) : NULL;
   if (!query_q || ((project && project[0]) && !project_q))
   {
      free(query_q);
      free(project_q);
      return 0;
   }
   size_t path_len = strlen("/v1/code/search?query=&max_results=&scope=all&project=") +
                     strlen(query_q) + (project_q ? strlen(project_q) : 0) + 32;
   char *path = malloc(path_len);
   if (!path)
   {
      free(query_q);
      free(project_q);
      return 0;
   }
   snprintf(path, path_len, "/v1/code/search?query=%s&max_results=%d%s%s%s", query_q, max,
            all_projects ? "&scope=all" : "", project_q ? "&project=" : "",
            project_q ? project_q : "");
   free(query_q);
   free(project_q);

   char *json = kb_client_v1_get_json(path, kb_client_index_read_timeout_ms(), NULL);
   free(path);
   if (!json)
      return -1;
   cJSON *resp = cJSON_Parse(json);
   free(json);
   if (!resp)
      return -1;
   int count = kb_index_code_search_parse(resp, out, max);
   cJSON_Delete(resp);
   return count;
}

int kb_client_index_code_search(const char *query, const char *project, code_search_hit_t *out,
                                int max)
{
   return kb_client_index_code_search_scoped(query, project, !project || !project[0], out, max);
}

int kb_client_index_find_callers_scoped(const char *project, int all_projects, const char *symbol,
                                        caller_hit_t *out, int max)
{
   if (!symbol || !symbol[0] || !out || max <= 0)
      return 0;
   memset(out, 0, sizeof(*out) * (size_t)max);

   char *symbol_q = kb_client_query_escape(symbol);
   char *project_q = (project && project[0]) ? kb_client_query_escape(project) : NULL;
   if (!symbol_q || ((project && project[0]) && !project_q))
   {
      free(symbol_q);
      free(project_q);
      return 0;
   }
   size_t path_len = strlen("/v1/code/callers?symbol=&max_results=&scope=all&project=") +
                     strlen(symbol_q) + (project_q ? strlen(project_q) : 0) + 32;
   char *path = malloc(path_len);
   if (!path)
   {
      free(symbol_q);
      free(project_q);
      return 0;
   }
   snprintf(path, path_len, "/v1/code/callers?symbol=%s&max_results=%d%s%s%s", symbol_q, max,
            all_projects ? "&scope=all" : "", project_q ? "&project=" : "",
            project_q ? project_q : "");
   free(symbol_q);
   free(project_q);

   char *json = kb_client_v1_get_json(path, kb_client_index_read_timeout_ms(), NULL);
   free(path);
   if (!json)
      return -1;
   cJSON *resp = cJSON_Parse(json);
   free(json);
   if (!resp)
      return -1;
   int count = kb_index_find_callers_parse(resp, out, max);
   cJSON_Delete(resp);
   return count;
}

int kb_client_index_find_callers(const char *project, const char *symbol, caller_hit_t *out,
                                 int max)
{
   return kb_client_index_find_callers_scoped(project, !project || !project[0], symbol, out, max);
}

/* S6: cross-repo dependency proxy. The kb response is rich (per-edge evidence +
 * version stamp, or a separate AMBIGUOUS review-queue shape when status=ambiguous)
 * so we forward the raw kb body verbatim rather than flatten it into a struct —
 * same passthrough idiom as kb_client_index_blast_radius_preview_json. Caller frees
 * the returned string; NULL means the kb was unreachable or returned no body. */
char *kb_client_index_cross_repo_deps_json(const char *project, const char *direction,
                                           const char *min_tier, int status_ambiguous, int dry_run)
{
   if (!project || !project[0])
      return NULL;

   char *project_q = kb_client_query_escape(project);
   char *direction_q = (direction && direction[0]) ? kb_client_query_escape(direction) : NULL;
   char *min_tier_q = (min_tier && min_tier[0]) ? kb_client_query_escape(min_tier) : NULL;
   if (!project_q || ((direction && direction[0]) && !direction_q) ||
       ((min_tier && min_tier[0]) && !min_tier_q))
   {
      free(project_q);
      free(direction_q);
      free(min_tier_q);
      return NULL;
   }

   size_t path_len =
       strlen("/v1/code/cross-repo-deps?project=&direction=&min_tier=&status=ambiguous&dry_run=1") +
       strlen(project_q) + (direction_q ? strlen(direction_q) : 0) +
       (min_tier_q ? strlen(min_tier_q) : 0) + 8;
   char *path = malloc(path_len);
   if (!path)
   {
      free(project_q);
      free(direction_q);
      free(min_tier_q);
      return NULL;
   }
   snprintf(path, path_len, "/v1/code/cross-repo-deps?project=%s%s%s%s%s%s%s", project_q,
            direction_q ? "&direction=" : "", direction_q ? direction_q : "",
            min_tier_q ? "&min_tier=" : "", min_tier_q ? min_tier_q : "",
            status_ambiguous ? "&status=ambiguous" : "", dry_run ? "&dry_run=1" : "");
   free(project_q);
   free(direction_q);
   free(min_tier_q);

   char *json = kb_client_v1_get_json(path, kb_client_index_read_timeout_ms(), NULL);
   free(path);
   return json;
}
