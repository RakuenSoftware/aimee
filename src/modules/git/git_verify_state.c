/* git_verify_state.c: verify-result state persistence — commit/tree-hash change
 * detection and the per-tree verify-state file (read/write/lookup). A real
 * translation unit (was git_verify_state.inc, which git_verify.c textually
 * included only to stay under the line-check ceiling). Cross-TU symbols —
 * VERIFY_STATE_MAX, verify_state_entry_t, and the helpers git_verify.c calls —
 * are declared in headers/git_verify_internal.h. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <ctype.h>
#include <unistd.h>
#include <sys/stat.h>
#include <errno.h>
#include "aimee.h"
#include "cJSON.h"
#include "git_verify.h"
#include "git_verify_internal.h"
#include "headers/module_json_call.h"
#include "util.h"
#include "log.h"
#include "platform_path.h"
#include <aimee/git/module_api.h>

/* The ledger below -- the hashes verify keys itself on and the .last-verify
 * file -- now lives in the git MODULE (server-go/modules/git, verify_state.go)
 * and is reached as bus stage AIMEE_GIT_STAGE_VERIFY_RUN. This is the first of
 * git's I/O paths to move, chosen because verify touches no credential: the
 * forge and credential paths cannot follow until the vault has a bus surface
 * (docs/proposals/pending/vault-bus-only-access.md).
 *
 * Every function keeps its signature and its failure convention, so callers do
 * not know the work moved. An unreachable module therefore reports "nothing is
 * verified" rather than an error, which is the same thing a missing ledger has
 * always reported and is the safe direction: it forces a verify run, it never
 * lets one be skipped. */
#define GIT_VERIFY_STATE_MAX_BODY   (256u * 1024u)
#define GIT_VERIFY_STATE_TIMEOUT_MS 10000

/* One round trip to the verify-state stage. Takes ownership of `request` the
 * way aimee_module_json_call does; returns the reply to cJSON_Delete, or NULL.
 * A reply whose "ok" is false is discarded here: it means the module could not
 * answer, which every caller treats the same as no answer at all. */
static cJSON *verify_state_call(cJSON *request)
{
   if (!request)
      return NULL;
   cJSON *reply =
       aimee_module_json_call(AIMEE_GIT_EVENT_VERIFY_RUN, AIMEE_GIT_STAGE_VERIFY_RUN, request,
                              GIT_VERIFY_STATE_MAX_BODY, GIT_VERIFY_STATE_TIMEOUT_MS, NULL);
   if (!reply)
      return NULL;
   if (!cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(reply, "ok")))
   {
      cJSON_Delete(reply);
      return NULL;
   }
   return reply;
}

/* Build the common {op, project_root} request. project_root may be NULL, which
 * the module reads as "use the working directory", exactly as the shell form
 * without -C did. */
static cJSON *verify_state_request(const char *op, const char *project_root)
{
   cJSON *request = cJSON_CreateObject();
   if (!request)
      return NULL;
   cJSON_AddStringToObject(request, "op", op);
   cJSON_AddStringToObject(request, "project_root", project_root ? project_root : "");
   return request;
}

/* Ask the module for a hash and hand back an owned copy, or NULL. */
static char *verify_state_hash(const char *op, const char *project_root)
{
   cJSON *reply = verify_state_call(verify_state_request(op, project_root));
   if (!reply)
      return NULL;
   const cJSON *hash = cJSON_GetObjectItemCaseSensitive(reply, "hash");
   char *result = (cJSON_IsString(hash) && hash->valuestring[0]) ? strdup(hash->valuestring) : NULL;
   cJSON_Delete(reply);
   return result;
}

/* --- Commit-hash-based change detection --- */

char *verify_compute_file_hash(const char *project_root)
{
   /* Return the tree hash (HEAD^{tree}) rather than the commit hash.
    * Tree hashes are stable across squash-merges and rebases that don't change
    * content, so a verified worktree HEAD matches the squash-merge commit that
    * GitHub creates from the same tree. */
   return verify_state_hash("tree-hash", project_root);
}

/* Return the HEAD commit hash (for display only — not used as the verify key).
 * Caller must free the returned string. Returns NULL on failure. */
char *verify_compute_commit_hash(const char *project_root)
{
   return verify_state_hash("commit-hash", project_root);
}

int verify_worktree_has_changes(const char *project_root)
{
   cJSON *reply = verify_state_call(verify_state_request("worktree-dirty", project_root));
   if (!reply)
      return 0; /* Unchanged from the shell form: a git that could not answer
                 * reported a clean tree. Preserved deliberately -- this port
                 * moves the work, not the policy. */
   int dirty = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(reply, "dirty"));
   cJSON_Delete(reply);
   return dirty;
}

/* --- State file management --- */

void verify_state_path(const char *project_root, char *buf, size_t len)
{
   /* Always write to the main checkout, not a worktree-specific path.
    * This lets the pre-push hook (which runs from the main checkout) find
    * verification state that was recorded in any worktree of the same repo. */
   char main_root[MAX_PATH_LEN];
   const char *base = project_root;
   if (project_root && project_root[0] &&
       resolve_main_repo_root(project_root, main_root, sizeof(main_root)) == 0 && main_root[0])
      base = main_root;

   if (base && base[0])
      snprintf(buf, len, "%s/.aimee/.last-verify", base);
   else
      snprintf(buf, len, ".aimee/.last-verify");
}

/* State file format — one entry per line (multi-branch rolling window):
 *   <unix_timestamp> <commit_hash> failed=N/total=M
 *
 * Up to VERIFY_STATE_MAX entries are kept (oldest pruned on write).
 * Legacy single-entry format (timestamp on line 1, hash on line 2, result
 * on line 3) is parsed on read and silently upgraded on next write.
 */

/* Parse the state file into entries[].  Returns the number of entries read
 * (0 if the file doesn't exist or is empty/corrupt). */
int read_verify_entries(const char *project_root, verify_state_entry_t *entries, int cap)
{
   if (!entries || cap <= 0)
      return 0;

   cJSON *reply = verify_state_call(verify_state_request("state-read", project_root));
   if (!reply)
      return 0;

   const cJSON *list = cJSON_GetObjectItemCaseSensitive(reply, "entries");
   int n = 0;
   const cJSON *item = NULL;
   cJSON_ArrayForEach(item, list)
   {
      if (n >= cap)
         break;
      const cJSON *hash = cJSON_GetObjectItemCaseSensitive(item, "hash");
      if (!cJSON_IsString(hash) || !hash->valuestring[0])
         continue;
      const cJSON *ts = cJSON_GetObjectItemCaseSensitive(item, "timestamp");
      const cJSON *failed = cJSON_GetObjectItemCaseSensitive(item, "failed");
      const cJSON *total = cJSON_GetObjectItemCaseSensitive(item, "total");
      const cJSON *steps = cJSON_GetObjectItemCaseSensitive(item, "step_results");

      entries[n].ts = (time_t)(cJSON_IsNumber(ts) ? ts->valuedouble : 0);
      snprintf(entries[n].hash, sizeof(entries[n].hash), "%s", hash->valuestring);
      entries[n].failed = cJSON_IsNumber(failed) ? failed->valueint : 0;
      entries[n].total = cJSON_IsNumber(total) ? total->valueint : 0;
      snprintf(entries[n].step_results, sizeof(entries[n].step_results), "%s",
               cJSON_IsString(steps) ? steps->valuestring : "");
      n++;
   }
   cJSON_Delete(reply);
   return n;
}

/* Find the entry in entries[] whose hash matches target_hash (first 40 chars).
 * Returns the index, or -1 if not found. */
int find_verify_entry(const verify_state_entry_t *entries, int n, const char *target_hash)
{
   for (int i = 0; i < n; i++)
      if (strncmp(entries[i].hash, target_hash, 40) == 0)
         return i;
   return -1;
}

/* Look up a step's recorded exit code in a "name:rc,name:rc,..." string.
 * Returns 1 and sets *out_rc on success, 0 if name not found. */
int verify_state_step_result_lookup(const char *step_results, const char *name, int *out_rc)
{
   if (!step_results || !step_results[0] || !name || !out_rc)
      return 0;
   char key[MAX_STEP_NAME + 2];
   snprintf(key, sizeof(key), "%s:", name);
   size_t klen = strlen(key);
   const char *p = step_results;
   while (p && *p)
   {
      if (strncmp(p, key, klen) == 0)
      {
         *out_rc = atoi(p + klen);
         return 1;
      }
      p = strchr(p, ',');
      if (p)
         p++;
   }
   return 0;
}

int write_verify_state(const char *project_root, time_t timestamp, const char *hash,
                       int failed_steps, int total_steps, const char *step_results)
{
   if (!hash || !hash[0])
      return -1;

   cJSON *request = verify_state_request("state-write", project_root);
   if (!request)
      return -1;
   cJSON_AddNumberToObject(request, "timestamp", (double)timestamp);
   cJSON_AddStringToObject(request, "hash", hash);
   cJSON_AddNumberToObject(request, "failed", failed_steps);
   cJSON_AddNumberToObject(request, "total", total_steps);
   cJSON_AddStringToObject(request, "step_results", step_results ? step_results : "");

   cJSON *reply = verify_state_call(request);
   if (!reply)
      return -1;
   cJSON_Delete(reply);
   return 0;
}

/* --- Parallel step execution --- */

void format_step_results(const verify_thread_ctx_t *ctxs, int n, char *buf, size_t len)
{
   size_t pos = 0;
   for (int i = 0; i < n && pos + 4 < len; i++)
   {
      if (i > 0)
         buf[pos++] = ',';
      int w = snprintf(buf + pos, len - pos, "%s:%d", ctxs[i].step->name, ctxs[i].rc);
      if (w < 0 || (size_t)w >= len - pos)
         break;
      pos += (size_t)w;
   }
   buf[pos] = '\0';
}
