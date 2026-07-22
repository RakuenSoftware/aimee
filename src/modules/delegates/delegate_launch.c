#include "delegate_launch.h"
#include "aimee.h"
#include "agent_tasks.h"
#include "db1.h"
#include "delegate_role.h"
#include "log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void launch_set_err(char *errbuf, size_t errbuf_len, const char *msg)
{
   if (errbuf && errbuf_len > 0)
      snprintf(errbuf, errbuf_len, "%s", msg ? msg : "delegate launch failed");
}

static int shell_quote(char *out, size_t out_len, const char *raw)
{
   if (!out || out_len < 3 || !raw)
      return -1;
   size_t pos = 0;
   out[pos++] = '\'';
   for (const char *p = raw; *p; p++)
   {
      if (*p == '\'')
      {
         if (pos + 4 >= out_len)
            return -1;
         memcpy(out + pos, "'\\''", 4);
         pos += 4;
      }
      else
      {
         if (pos + 1 >= out_len)
            return -1;
         out[pos++] = *p;
      }
   }
   if (pos + 1 >= out_len)
      return -1;
   out[pos++] = '\'';
   out[pos] = '\0';
   return 0;
}

static int packet_path_exists(const char *cwd, const char *path)
{
   if (!path || !path[0])
      return 0;
   if (path[0] == '/' || !cwd || !cwd[0])
      return access(path, F_OK) == 0;

   char full[MAX_PATH_LEN];
   if (snprintf(full, sizeof(full), "%s/%s", cwd, path) >= (int)sizeof(full))
      return 0;
   return access(full, F_OK) == 0;
}

/* Canonicalize the "role" field in a packet JSON object in-place.
 * Records an INFO log when an alias is resolved. */
static void packet_canonicalize_role(cJSON *packet)
{
   cJSON *role = cJSON_GetObjectItemCaseSensitive(packet, "role");
   if (!cJSON_IsString(role) || !role->valuestring || !role->valuestring[0])
      return;
   const char *canonical = delegate_role_canonicalize(role->valuestring);
   if (canonical != role->valuestring)
   {
      LOG_INFO("delegate", "packet role alias '%s' -> '%s'", role->valuestring, canonical);
      cJSON_SetValuestring(role, canonical);
   }
}

/* Normalize a single path by looking up its basename in `git ls-files`.
 * Returns 0 if the path exists or was repaired.  Returns -1 if missing and
 * unresolvable (zero or multiple matches); errbuf is populated on -1. */
static int normalize_one_path(cJSON *item, const char *cwd, char *errbuf, size_t errbuf_len)
{
   const char *path = item->valuestring;

   /* Fast path: file exists as-is. */
   if (packet_path_exists(cwd, path))
      return 0;

   /* Extract the basename of the missing path. */
   const char *slash = strrchr(path, '/');
   const char *base = slash ? slash + 1 : path;
   if (!base[0])
   {
      snprintf(errbuf, errbuf_len, "packet owned_files: invalid path '%s'", path);
      return -1;
   }

   /* Scan git ls-files for files with the same basename. */
   char candidates[8][MAX_PATH_LEN];
   int ncandidates = 0;

   char ls_cmd[MAX_PATH_LEN + 64];
   if (cwd && cwd[0])
   {
      char quoted[MAX_PATH_LEN * 2];
      if (shell_quote(quoted, sizeof(quoted), cwd) != 0)
      {
         snprintf(errbuf, errbuf_len, "packet path repair: cannot quote cwd for '%s'", path);
         return -1;
      }
      snprintf(ls_cmd, sizeof(ls_cmd), "git -C %s ls-files --full-name", quoted);
   }
   else
      snprintf(ls_cmd, sizeof(ls_cmd), "git ls-files --full-name");

   FILE *fp = popen(ls_cmd, "r");
   if (!fp)
   {
      snprintf(errbuf, errbuf_len, "packet path repair: cannot run git ls-files for '%s'", path);
      return -1;
   }

   char line[MAX_PATH_LEN];
   while (ncandidates < 8 && fgets(line, sizeof(line), fp))
   {
      size_t len = strlen(line);
      while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
         line[--len] = '\0';
      if (!len)
         continue;

      const char *lslash = strrchr(line, '/');
      const char *lbase = lslash ? lslash + 1 : line;
      if (strcmp(lbase, base) == 0)
         snprintf(candidates[ncandidates++], MAX_PATH_LEN, "%s", line);
   }
   pclose(fp);

   if (ncandidates == 1)
   {
      LOG_INFO("delegate", "packet path repair: '%s' -> '%s'", path, candidates[0]);
      cJSON_SetValuestring(item, candidates[0]);
      return 0;
   }

   if (ncandidates == 0)
   {
      /* No match found — warn and continue.  The delegate will surface its
       * own error if it cannot locate the file at runtime.  We do not fail
       * here because the planner may legitimately reference files that do not
       * exist yet (new-file creation tasks). */
      LOG_WARN("delegate",
               "packet owned_files: '%s' not found in repository (basename '%s' "
               "has no match in git ls-files)",
               path, base);
      return 0;
   }

   /* Multiple matches — cannot pick safely; fail before dispatch. */
   char clist[1024] = "";
   size_t cpos = 0;
   for (int i = 0; i < ncandidates && cpos < sizeof(clist) - 2; i++)
   {
      int n = snprintf(clist + cpos, sizeof(clist) - cpos, "%s%s", i ? ", " : "", candidates[i]);
      if (n > 0)
         cpos += (size_t)n < sizeof(clist) - cpos ? (size_t)n : sizeof(clist) - cpos - 1;
   }
   snprintf(errbuf, errbuf_len,
            "packet owned_files: '%s' not found; ambiguous basename '%s' matches: %s — "
            "repair the path before launching",
            path, base, clist);
   return -1;
}

/* Validate and repair owned_files paths in a packet before launch.
 * Returns 0 if all paths exist or were repaired, -1 on unresolvable missing path. */
static int packet_normalize_paths(cJSON *packet, const char *cwd, char *errbuf, size_t errbuf_len)
{
   cJSON *owned = cJSON_GetObjectItemCaseSensitive(packet, "owned_files");
   if (!cJSON_IsArray(owned))
      return 0;

   cJSON *item;
   cJSON_ArrayForEach(item, owned)
   {
      if (!cJSON_IsString(item) || !item->valuestring || !item->valuestring[0])
         continue;
      if (normalize_one_path(item, cwd, errbuf, errbuf_len) != 0)
         return -1;
   }
   return 0;
}

static int packet_is_review(cJSON *packet)
{
   cJSON *role = cJSON_GetObjectItemCaseSensitive(packet, "role");
   return cJSON_IsString(role) && strcmp(role->valuestring, "review") == 0;
}

static int packet_owned_file_count(cJSON *packet)
{
   cJSON *owned = cJSON_GetObjectItemCaseSensitive(packet, "owned_files");
   if (!cJSON_IsArray(owned))
      return 0;

   int count = 0;
   cJSON *item;
   cJSON_ArrayForEach(item, owned)
   {
      if (cJSON_IsString(item) && item->valuestring[0])
         count++;
   }
   return count;
}

static int packet_handoff_schema_ok(cJSON *packet)
{
   cJSON *schema = cJSON_GetObjectItemCaseSensitive(packet, "handoff_schema");
   return cJSON_IsString(schema) && strcmp(schema->valuestring, "delegate_result_v1") == 0;
}

static int append_packet_step(cJSON *steps, cJSON *packet, char *errbuf, size_t errbuf_len)
{
   if (!cJSON_IsObject(packet))
   {
      launch_set_err(errbuf, errbuf_len, "delegate plan contains an invalid packet");
      return -1;
   }
   if (packet_is_review(packet))
      return 0;
   if (packet_owned_file_count(packet) == 0)
   {
      launch_set_err(errbuf, errbuf_len, "delegate plan packet missing owned_files");
      return -1;
   }
   if (!packet_handoff_schema_ok(packet))
   {
      launch_set_err(errbuf, errbuf_len,
                     "delegate plan packet missing handoff_schema delegate_result_v1");
      return -1;
   }

   cJSON *step = cJSON_CreateObject();
   if (!step)
   {
      launch_set_err(errbuf, errbuf_len, "out of memory building delegate launch steps");
      return -1;
   }

   cJSON *title = cJSON_GetObjectItemCaseSensitive(packet, "title");
   cJSON *objective = cJSON_GetObjectItemCaseSensitive(packet, "objective");
   cJSON *id = cJSON_GetObjectItemCaseSensitive(packet, "id");
   cJSON_AddStringToObject(step, "action",
                           cJSON_IsString(title) && title->valuestring[0]
                               ? title->valuestring
                               : (cJSON_IsString(id) ? id->valuestring : "delegate packet"));
   cJSON_AddStringToObject(step, "precondition",
                           cJSON_IsString(id) && id->valuestring[0] ? id->valuestring
                                                                    : "delegate packet");
   cJSON_AddStringToObject(step, "success_predicate",
                           cJSON_IsString(objective) && objective->valuestring[0]
                               ? objective->valuestring
                               : "delegate packet completed");
   cJSON_AddStringToObject(step, "rollback", "");
   cJSON_AddItemToArray(steps, step);
   return 1;
}

static char *packet_build_prompt(cJSON *packet)
{
   cJSON *title = cJSON_GetObjectItemCaseSensitive(packet, "title");
   cJSON *objective = cJSON_GetObjectItemCaseSensitive(packet, "objective");
   cJSON *owned = cJSON_GetObjectItemCaseSensitive(packet, "owned_files");

   const char *t =
       (cJSON_IsString(title) && title->valuestring[0]) ? title->valuestring : "delegate packet";
   const char *obj =
       (cJSON_IsString(objective) && objective->valuestring[0]) ? objective->valuestring : t;

   char files_list[1024] = "";
   if (cJSON_IsArray(owned))
   {
      size_t pos = 0;
      cJSON *item;
      cJSON_ArrayForEach(item, owned)
      {
         if (!cJSON_IsString(item) || !item->valuestring[0])
            continue;
         int n = snprintf(files_list + pos, sizeof(files_list) - pos - 1, "%s%s", pos ? ", " : "",
                          item->valuestring);
         if (n > 0 && (size_t)n < sizeof(files_list) - pos - 1)
            pos += (size_t)n;
      }
   }

   size_t cap = strlen(t) + strlen(obj) + strlen(files_list) + 256;
   char *out = malloc(cap);
   if (!out)
      return NULL;
   snprintf(out, cap,
            "%s\n\n"
            "Objective: %s\n\n"
            "Owned files (modify only these): %s\n\n"
            "When done, respond with a structured handoff JSON (delegate_result_v1 schema).",
            t, obj, files_list[0] ? files_list : "(none)");
   return out;
}

static char *packet_owned_files_json(cJSON *packet)
{
   cJSON *owned = cJSON_GetObjectItemCaseSensitive(packet, "owned_files");
   cJSON *files = cJSON_CreateArray();
   if (!files)
      return NULL;

   if (cJSON_IsArray(owned))
   {
      cJSON *item;
      cJSON_ArrayForEach(item, owned)
      {
         if (cJSON_IsString(item) && item->valuestring[0])
            cJSON_AddItemToArray(files, cJSON_CreateString(item->valuestring));
      }
   }
   char *json = cJSON_PrintUnformatted(files);
   cJSON_Delete(files);
   return json;
}

int delegate_launch_coord_job(cJSON *plan, int max_concurrent, const char *cwd,
                              delegate_launch_result_t *out, char *errbuf, size_t errbuf_len)
{
   if (out)
      memset(out, 0, sizeof(*out));
   if (max_concurrent <= 0)
      max_concurrent = DB1_COORD_DEFAULT_PAR;

   if (!cJSON_IsObject(plan))
   {
      launch_set_err(errbuf, errbuf_len, "missing delegate plan");
      return -1;
   }

   cJSON *schema = cJSON_GetObjectItemCaseSensitive(plan, "schema");
   if (!cJSON_IsString(schema) || strcmp(schema->valuestring, "delegate_plan_v1") != 0)
   {
      launch_set_err(errbuf, errbuf_len, "invalid delegate plan schema");
      return -1;
   }

   cJSON *packets = cJSON_GetObjectItemCaseSensitive(plan, "packets");
   if (!cJSON_IsArray(packets))
   {
      launch_set_err(errbuf, errbuf_len, "delegate plan missing packets");
      return -1;
   }

   cJSON *missing_owned = cJSON_GetObjectItemCaseSensitive(plan, "missing_owned_files");
   if (cJSON_IsArray(missing_owned) && cJSON_GetArraySize(missing_owned) > 0)
   {
      cJSON *first = cJSON_GetArrayItem(missing_owned, 0);
      if (errbuf && errbuf_len > 0)
         snprintf(errbuf, errbuf_len,
                  "delegate plan has missing owned_files%s%s; review or mark new files before "
                  "launching",
                  cJSON_IsString(first) ? ": " : "",
                  cJSON_IsString(first) ? first->valuestring : "");
      return -1;
   }

   cJSON *steps = cJSON_CreateArray();
   if (!steps)
   {
      launch_set_err(errbuf, errbuf_len, "out of memory building delegate launch steps");
      return -1;
   }

   /* Canonicalize role aliases and normalize owned_files paths in all packets
    * before step validation so path repairs are visible to append_packet_step. */
   {
      cJSON *p;
      cJSON_ArrayForEach(p, packets)
      {
         packet_canonicalize_role(p);
         if (!packet_is_review(p))
         {
            if (packet_normalize_paths(p, cwd, errbuf, errbuf_len) != 0)
            {
               cJSON_Delete(steps);
               return -1;
            }
         }
      }
   }

   int task_count = 0;
   cJSON *packet;
   cJSON_ArrayForEach(packet, packets)
   {
      int rc = append_packet_step(steps, packet, errbuf, errbuf_len);
      if (rc < 0)
      {
         cJSON_Delete(steps);
         return -1;
      }
      task_count += rc;
      if (task_count > AGENT_MAX_PLAN_STEPS)
      {
         cJSON_Delete(steps);
         launch_set_err(errbuf, errbuf_len, "delegate plan has too many implementation packets");
         return -1;
      }
   }

   if (task_count <= 0)
   {
      cJSON_Delete(steps);
      launch_set_err(errbuf, errbuf_len, "delegate plan has no implementation packets");
      return -1;
   }

   cJSON *title = cJSON_GetObjectItemCaseSensitive(plan, "title");
   const char *task = cJSON_IsString(title) && title->valuestring[0] ? title->valuestring
                                                                     : "delegate work packet plan";
   int plan_id = db1_execution_plan_create("delegate-plan", task, steps);
   cJSON_Delete(steps);
   if (plan_id <= 0)
   {
      launch_set_err(errbuf, errbuf_len, "failed to create execution plan");
      return -1;
   }

   plan_t stored;
   if (db1_execution_plan_get(plan_id, &stored) != 0 || stored.step_count < task_count)
   {
      db1_execution_plan_cancel_by_id(plan_id, "delegate launch could not read created steps");
      launch_set_err(errbuf, errbuf_len, "failed to read created execution plan");
      return -1;
   }

   int job_id = db1_coord_job_create(plan_id, max_concurrent);
   if (job_id <= 0)
   {
      db1_execution_plan_cancel_by_id(plan_id, "delegate launch could not create coord job");
      launch_set_err(errbuf, errbuf_len, "failed to create coord job");
      return -1;
   }

   int added = 0;
   int step_idx = 0;
   cJSON_ArrayForEach(packet, packets)
   {
      if (packet_is_review(packet))
         continue;
      if (packet_owned_file_count(packet) == 0)
         continue;

      char *files_json = packet_owned_files_json(packet);
      if (!files_json)
         break;

      cJSON *prole = cJSON_GetObjectItemCaseSensitive(packet, "role");
      const char *role =
          (cJSON_IsString(prole) && prole->valuestring[0]) ? prole->valuestring : "execute";
      char *prompt = packet_build_prompt(packet);

      int step_id = step_idx < stored.step_count ? stored.steps[step_idx].id : 0;
      if (db1_coord_job_add_task(job_id, step_id, files_json, role, prompt, cwd, "engineer") > 0)
         added++;
      free(files_json);
      free(prompt);
      step_idx++;
   }

   if (added != task_count)
   {
      db1_coord_job_cancel(job_id);
      db1_execution_plan_cancel_by_id(plan_id, "delegate launch could not enqueue all packets");
      launch_set_err(errbuf, errbuf_len, "failed to enqueue all delegate packets");
      return -1;
   }

   if (out)
   {
      out->plan_id = plan_id;
      out->job_id = job_id;
      out->tasks = added;
      out->max_concurrent = max_concurrent;
   }
   return 0;
}
