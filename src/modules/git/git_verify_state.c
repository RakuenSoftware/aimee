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
#include "git_verify.h"
#include "git_verify_internal.h"
#include "util.h"
#include "log.h"
#include "platform_path.h"

/* --- Commit-hash-based change detection --- */

char *verify_compute_file_hash(const char *project_root)
{
   /* Return the tree hash (HEAD^{tree}) rather than the commit hash.
    * Tree hashes are stable across squash-merges and rebases that don't change
    * content, so a verified worktree HEAD matches the squash-merge commit that
    * GitHub creates from the same tree. */
   char cmd[MAX_PATH_LEN + 64];
   if (project_root && project_root[0])
      snprintf(cmd, sizeof(cmd), "git -C '%s' rev-parse HEAD^{tree} 2>/dev/null", project_root);
   else
      snprintf(cmd, sizeof(cmd), "git rev-parse HEAD^{tree} 2>/dev/null");

   int rc;
   char *out = run_cmd(cmd, &rc);
   if (rc != 0 || !out || !out[0])
   {
      free(out);
      return NULL;
   }

   /* Strip trailing whitespace */
   for (char *p = out + strlen(out) - 1; p >= out && (*p == '\n' || *p == '\r' || *p == ' '); p--)
      *p = '\0';

   char *result = strdup(out);
   free(out);
   return result;
}

/* Return the HEAD commit hash (for display only — not used as the verify key).
 * Caller must free the returned string. Returns NULL on failure. */
char *verify_compute_commit_hash(const char *project_root)
{
   char cmd[MAX_PATH_LEN + 64];
   if (project_root && project_root[0])
      snprintf(cmd, sizeof(cmd), "git -C '%s' rev-parse HEAD 2>/dev/null", project_root);
   else
      snprintf(cmd, sizeof(cmd), "git rev-parse HEAD 2>/dev/null");

   int rc;
   char *out = run_cmd(cmd, &rc);
   if (rc != 0 || !out || !out[0])
   {
      free(out);
      return NULL;
   }
   for (char *p = out + strlen(out) - 1; p >= out && (*p == '\n' || *p == '\r' || *p == ' '); p--)
      *p = '\0';
   char *result = strdup(out);
   free(out);
   return result;
}

int verify_worktree_has_changes(const char *project_root)
{
   char cmd[MAX_PATH_LEN + 64];
   int rc;
   if (project_root && project_root[0])
      snprintf(cmd, sizeof(cmd), "git -C '%s' status --porcelain 2>/dev/null", project_root);
   else
      snprintf(cmd, sizeof(cmd), "git status --porcelain 2>/dev/null");
   char *status = run_cmd(cmd, &rc);
   int has_changes = (rc == 0 && status && status[0]);
   free(status);
   return has_changes;
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
   char path[MAX_PATH_LEN];
   verify_state_path(project_root, path, sizeof(path));

   FILE *f = fopen(path, "r");
   if (!f)
      return 0;

   char line[512];
   int n = 0;

   /* Peek at the first line to detect legacy format (pure integer = old ts line). */
   if (!fgets(line, sizeof(line), f))
   {
      fclose(f);
      return 0;
   }

   /* Strip trailing whitespace */
   char *ep = line + strlen(line) - 1;
   while (ep >= line && (*ep == '\n' || *ep == '\r' || *ep == ' '))
      *ep-- = '\0';

   /* Legacy format: first line is a bare integer (no spaces). */
   if (!strchr(line, ' '))
   {
      if (n < cap)
      {
         time_t ts = (time_t)strtoll(line, NULL, 10);
         char hline[64] = {0};
         char rline[32] = {0};
         if (fgets(hline, sizeof(hline), f))
         {
            char *p = hline + strlen(hline) - 1;
            while (p >= hline && (*p == '\n' || *p == '\r' || *p == ' '))
               *p-- = '\0';
            (void)fgets(rline, sizeof(rline), f);
            p = rline + strlen(rline) - 1;
            while (p >= rline && (*p == '\n' || *p == '\r' || *p == ' '))
               *p-- = '\0';

            if (hline[0])
            {
               entries[n].ts = ts;
               snprintf(entries[n].hash, sizeof(entries[n].hash), "%s", hline);
               entries[n].failed = 0;
               entries[n].total = 0;
               entries[n].step_results[0] = '\0';
               if (rline[0])
                  sscanf(rline, "failed=%d/total=%d", &entries[n].failed, &entries[n].total);
               n++;
            }
         }
      }
      fclose(f);
      return n;
   }

   /* New format: parse the first line we already read, then the rest. */
   for (;;)
   {
      if (line[0] && n < cap)
      {
         long long ts_ll = 0;
         char h[64] = {0};
         int fv = 0, tv = 0;
         if (sscanf(line, "%lld %63s failed=%d/total=%d", &ts_ll, h, &fv, &tv) >= 2 && h[0])
         {
            entries[n].ts = (time_t)ts_ll;
            snprintf(entries[n].hash, sizeof(entries[n].hash), "%s", h);
            entries[n].failed = fv;
            entries[n].total = tv;
            entries[n].step_results[0] = '\0';
            const char *sp = strstr(line, " steps=");
            if (sp)
               snprintf(entries[n].step_results, sizeof(entries[n].step_results), "%s", sp + 7);
            n++;
         }
      }
      if (!fgets(line, sizeof(line), f))
         break;
      ep = line + strlen(line) - 1;
      while (ep >= line && (*ep == '\n' || *ep == '\r' || *ep == ' '))
         *ep-- = '\0';
   }
   fclose(f);
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
   char path[MAX_PATH_LEN], tmp_path[MAX_PATH_LEN];
   verify_state_path(project_root, path, sizeof(path));
   snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", path);

   char parent[MAX_PATH_LEN];
   snprintf(parent, sizeof(parent), "%s", path);
   char *slash = strrchr(parent, '/');
   if (slash)
   {
      *slash = '\0';
      if (parent[0] && platform_mkdir_p(parent, 0755) != 0 && errno != EEXIST)
         return -1;
   }

   verify_state_entry_t old[VERIFY_STATE_MAX];
   int nold = read_verify_entries(project_root, old, VERIFY_STATE_MAX);

   FILE *f = fopen(tmp_path, "w");
   if (!f)
      return -1;

   if (step_results && step_results[0])
      fprintf(f, "%lld %s failed=%d/total=%d steps=%s\n", (long long)timestamp, hash, failed_steps,
              total_steps, step_results);
   else
      fprintf(f, "%lld %s failed=%d/total=%d\n", (long long)timestamp, hash, failed_steps,
              total_steps);

   int kept = 1;
   for (int i = 0; i < nold && kept < VERIFY_STATE_MAX; i++)
   {
      if (strncmp(old[i].hash, hash, 40) == 0)
         continue;
      if (old[i].step_results[0])
         fprintf(f, "%lld %s failed=%d/total=%d steps=%s\n", (long long)old[i].ts, old[i].hash,
                 old[i].failed, old[i].total, old[i].step_results);
      else
         fprintf(f, "%lld %s failed=%d/total=%d\n", (long long)old[i].ts, old[i].hash,
                 old[i].failed, old[i].total);
      kept++;
   }
   fclose(f);

   if (rename(tmp_path, path) != 0)
   {
      remove(tmp_path);
      return -1;
   }
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
