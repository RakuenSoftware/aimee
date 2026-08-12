/* delegate_launch.c -- turn a delegate plan into a running coordinator job.
 *
 * What is decided here and what is not:
 *
 *   - The DECISIONS about a plan -- which packets are work, whether a packet
 *     may launch, what its delegate is briefed with, how a named path that does
 *     not exist is repaired -- belong to the delegates module (stage 19).
 *   - The I/O belongs here: does this file exist, which tracked files share its
 *     basename, and every database write. The module returns rows; this file
 *     writes them.
 *
 * The split is not tidiness. A plan that fails validation must leave NOTHING
 * behind, so the whole decision is taken before the first row is created, and
 * a refusal returns before any of it. */

#include <aimee/delegates/delegate_launch.h>
#include "aimee.h"
#include "agent_tasks.h"
#include "db1.h"
#include <aimee/delegates/delegate_launch_args.h>
#include <aimee/delegates/delegate_role.h>
#include <aimee/delegates/module_api.h>
#include "log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifndef MAX_PATH_LEN
#define MAX_PATH_LEN 4096
#endif

/* Room for a plan and for what it becomes. A plan larger than this is refused
 * rather than truncated: half a launch is worse than none. */
#define LAUNCH_WIRE_CAP (512u * 1024u)

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

static const char *path_basename(const char *path)
{
   const char *slash = strrchr(path, '/');
   return slash ? slash + 1 : path;
}

/* --- the repository's tracked files -------------------------------------- */

typedef struct
{
   char **items;
   int count;
} tracked_files_t;

static void tracked_files_free(tracked_files_t *t)
{
   for (int i = 0; i < t->count; i++)
      free(t->items[i]);
   free(t->items);
   t->items = NULL;
   t->count = 0;
}

/* Read `git ls-files` ONCE per launch.
 *
 * The C this replaces re-ran it for every missing path. The list does not
 * change between packets, so reading it once is both cheaper and more obviously
 * consistent: every repair in one launch now sees the same repository. */
static int tracked_files_load(const char *cwd, tracked_files_t *out)
{
   out->items = NULL;
   out->count = 0;

   char ls_cmd[MAX_PATH_LEN + 64];
   if (cwd && cwd[0])
   {
      char quoted[MAX_PATH_LEN * 2];
      if (shell_quote(quoted, sizeof(quoted), cwd) != 0)
         return -1;
      snprintf(ls_cmd, sizeof(ls_cmd), "git -C %s ls-files --full-name", quoted);
   }
   else
      snprintf(ls_cmd, sizeof(ls_cmd), "git ls-files --full-name");

   FILE *fp = popen(ls_cmd, "r");
   if (!fp)
      return -1;

   int cap = 0;
   char line[MAX_PATH_LEN];
   while (fgets(line, sizeof(line), fp))
   {
      size_t len = strlen(line);
      while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
         line[--len] = '\0';
      if (!len)
         continue;
      if (out->count == cap)
      {
         int next = cap ? cap * 2 : 256;
         char **grown = realloc(out->items, (size_t)next * sizeof(*grown));
         if (!grown)
         {
            pclose(fp);
            tracked_files_free(out);
            return -1;
         }
         out->items = grown;
         cap = next;
      }
      out->items[out->count] = strdup(line);
      if (!out->items[out->count])
      {
         pclose(fp);
         tracked_files_free(out);
         return -1;
      }
      out->count++;
   }
   pclose(fp);
   return 0;
}

/* --- encoding the plan ---------------------------------------------------- */

static const char *json_str(cJSON *obj, const char *key)
{
   cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
   return (cJSON_IsString(item) && item->valuestring) ? item->valuestring : "";
}

/* Write one owned file: its path, whether it exists, and -- only when it does
 * not -- the tracked files sharing its basename. Candidates rather than the
 * whole file list because the whole list does not fit in one event. */
static void encode_owned_file(aimee_delegates_wire_t *w, const char *path, const char *cwd,
                              const tracked_files_t *tracked)
{
   aimee_delegates_wire_str(w, path);

   int exists = packet_path_exists(cwd, path);
   aimee_delegates_wire_u32(w, exists ? 1u : 0u);

   if (exists)
   {
      aimee_delegates_wire_u32(w, 0);
      return;
   }

   const char *base = path_basename(path);
   uint32_t matches = 0;
   for (int i = 0; i < tracked->count; i++)
      if (strcmp(path_basename(tracked->items[i]), base) == 0)
         matches++;

   aimee_delegates_wire_u32(w, matches);
   for (int i = 0; i < tracked->count; i++)
      if (strcmp(path_basename(tracked->items[i]), base) == 0)
         aimee_delegates_wire_str(w, tracked->items[i]);
}

static void encode_packet(aimee_delegates_wire_t *w, cJSON *packet, const char *cwd,
                          const tracked_files_t *tracked)
{
   aimee_delegates_wire_str(w, json_str(packet, "id"));
   aimee_delegates_wire_str(w, json_str(packet, "title"));
   aimee_delegates_wire_str(w, json_str(packet, "objective"));

   /* Role aliases resolve before the module sees them: "reviewer" and "review"
    * must not launch as two different things. The canonicalizer is itself a
    * module seam, so this is one module's answer feeding another's question. */
   const char *role = json_str(packet, "role");
   if (role[0])
      role = delegate_role_canonicalize(role);
   aimee_delegates_wire_str(w, role);
   aimee_delegates_wire_str(w, json_str(packet, "handoff_schema"));

   cJSON *owned = cJSON_GetObjectItemCaseSensitive(packet, "owned_files");
   uint32_t file_count = 0;
   cJSON *item;
   if (cJSON_IsArray(owned))
      cJSON_ArrayForEach(item, owned) if (cJSON_IsString(item) && item->valuestring[0])
          file_count++;

   aimee_delegates_wire_u32(w, file_count);
   if (cJSON_IsArray(owned))
      cJSON_ArrayForEach(item, owned)
      {
         if (cJSON_IsString(item) && item->valuestring[0])
            encode_owned_file(w, item->valuestring, cwd, tracked);
      }
}

static int encode_request(cJSON *plan, int max_concurrent, const char *cwd,
                          const tracked_files_t *tracked, uint8_t *buf, size_t cap, size_t *out_len)
{
   aimee_delegates_wire_t w;
   aimee_delegates_launchplan_request_begin(&w, buf, cap, max_concurrent, json_str(plan, "schema"),
                                            json_str(plan, "title"));

   cJSON *missing = cJSON_GetObjectItemCaseSensitive(plan, "missing_owned_files");
   uint32_t missing_count = 0;
   cJSON *item;
   if (cJSON_IsArray(missing))
      cJSON_ArrayForEach(item, missing) if (cJSON_IsString(item)) missing_count++;
   aimee_delegates_wire_u32(&w, missing_count);
   if (cJSON_IsArray(missing))
      cJSON_ArrayForEach(item, missing) if (cJSON_IsString(item))
          aimee_delegates_wire_str(&w, item->valuestring);

   cJSON *packets = cJSON_GetObjectItemCaseSensitive(plan, "packets");
   uint32_t packet_count = 0;
   if (cJSON_IsArray(packets))
      cJSON_ArrayForEach(item, packets) if (cJSON_IsObject(item)) packet_count++;
   aimee_delegates_wire_u32(&w, packet_count);
   if (cJSON_IsArray(packets))
      cJSON_ArrayForEach(item, packets) if (cJSON_IsObject(item))
          encode_packet(&w, item, cwd, tracked);

   if (w.overflow)
      return -1;
   *out_len = w.len;
   return 0;
}

/* --- the launch ----------------------------------------------------------- */

int delegate_launch_coord_job(cJSON *plan, int max_concurrent, const char *cwd,
                              delegate_launch_result_t *out, char *errbuf, size_t errbuf_len)
{
   if (out)
      memset(out, 0, sizeof(*out));

   if (!cJSON_IsObject(plan))
   {
      launch_set_err(errbuf, errbuf_len, "missing delegate plan");
      return -1;
   }
   /* The packets array must exist before anything else: without it there is no
    * plan to read, and the module would only be able to say "no packets". */
   if (!cJSON_IsArray(cJSON_GetObjectItemCaseSensitive(plan, "packets")))
   {
      launch_set_err(errbuf, errbuf_len, "delegate plan missing packets");
      return -1;
   }

   tracked_files_t tracked;
   if (tracked_files_load(cwd, &tracked) != 0)
   {
      launch_set_err(errbuf, errbuf_len, "delegate launch: cannot list repository files");
      return -1;
   }

   uint8_t *request = malloc(LAUNCH_WIRE_CAP);
   uint8_t *response = malloc(LAUNCH_WIRE_CAP);
   if (!request || !response)
   {
      free(request);
      free(response);
      tracked_files_free(&tracked);
      launch_set_err(errbuf, errbuf_len, "out of memory building delegate launch");
      return -1;
   }

   size_t request_len = 0;
   int rc =
       encode_request(plan, max_concurrent, cwd, &tracked, request, LAUNCH_WIRE_CAP, &request_len);
   tracked_files_free(&tracked);
   if (rc != 0)
   {
      free(request);
      free(response);
      launch_set_err(errbuf, errbuf_len, "delegate plan is too large to launch");
      return -1;
   }

   size_t response_len = 0;
   rc = delegate_launch_plan_call(request, request_len, response, LAUNCH_WIRE_CAP, &response_len);
   free(request);
   if (rc != 0)
   {
      free(response);
      launch_set_err(errbuf, errbuf_len, "delegate launch could not be planned");
      return -1;
   }

   aimee_delegates_rd_t r;
   char module_err[512] = "";
   int effective_par = max_concurrent;
   if (aimee_delegates_launchplan_response_begin(&r, response, response_len, module_err,
                                                 sizeof(module_err), &effective_par) != 0)
   {
      free(response);
      launch_set_err(errbuf, errbuf_len, "delegate launch returned an unreadable plan");
      return -1;
   }
   if (module_err[0])
   {
      free(response);
      launch_set_err(errbuf, errbuf_len, module_err);
      return -1;
   }

   /* From here the plan is accepted and the writes begin. Anything that fails
    * below cancels what it already created: a half-built job would sit in the
    * queue looking runnable. */
   cJSON *steps = cJSON_CreateArray();
   if (!steps)
   {
      free(response);
      launch_set_err(errbuf, errbuf_len, "out of memory building delegate launch steps");
      return -1;
   }

   uint32_t step_count = aimee_delegates_rd_u32(&r);
   for (uint32_t i = 0; i < step_count && !r.bad; i++)
   {
      char action[512], precondition[512], success[1024], rollback[256];
      aimee_delegates_rd_str(&r, action, sizeof(action));
      aimee_delegates_rd_str(&r, precondition, sizeof(precondition));
      aimee_delegates_rd_str(&r, success, sizeof(success));
      aimee_delegates_rd_str(&r, rollback, sizeof(rollback));

      cJSON *step = cJSON_CreateObject();
      if (!step)
      {
         r.bad = 1;
         break;
      }
      cJSON_AddStringToObject(step, "action", action);
      cJSON_AddStringToObject(step, "precondition", precondition);
      cJSON_AddStringToObject(step, "success_predicate", success);
      cJSON_AddStringToObject(step, "rollback", rollback);
      cJSON_AddItemToArray(steps, step);
   }
   if (r.bad)
   {
      cJSON_Delete(steps);
      free(response);
      launch_set_err(errbuf, errbuf_len, "delegate launch returned an unreadable plan");
      return -1;
   }

   const char *title = json_str(plan, "title");
   int plan_id = db1_execution_plan_create("delegate-plan",
                                           title[0] ? title : "delegate work packet plan", steps);
   cJSON_Delete(steps);
   if (plan_id <= 0)
   {
      free(response);
      launch_set_err(errbuf, errbuf_len, "failed to create execution plan");
      return -1;
   }

   plan_t stored;
   if (db1_execution_plan_get(plan_id, &stored) != 0 || stored.step_count < (int)step_count)
   {
      db1_execution_plan_cancel_by_id(plan_id, "delegate launch could not read created steps");
      free(response);
      launch_set_err(errbuf, errbuf_len, "failed to read created execution plan");
      return -1;
   }

   int job_id = db1_coord_job_create(plan_id, effective_par);
   if (job_id <= 0)
   {
      db1_execution_plan_cancel_by_id(plan_id, "delegate launch could not create coord job");
      free(response);
      launch_set_err(errbuf, errbuf_len, "failed to create coord job");
      return -1;
   }

   uint32_t task_count = aimee_delegates_rd_u32(&r);
   int added = 0;
   for (uint32_t i = 0; i < task_count && !r.bad; i++)
   {
      cJSON *files = cJSON_CreateArray();
      uint32_t file_count = aimee_delegates_rd_u32(&r);
      for (uint32_t f = 0; f < file_count && !r.bad; f++)
      {
         char path[MAX_PATH_LEN];
         aimee_delegates_rd_str(&r, path, sizeof(path));
         if (files)
            cJSON_AddItemToArray(files, cJSON_CreateString(path));
      }

      char role[64];
      aimee_delegates_rd_str(&r, role, sizeof(role));

      uint32_t prompt_len = aimee_delegates_rd_u32(&r);
      char *prompt = malloc((size_t)prompt_len + 1);
      if (prompt && !r.bad && r.at + prompt_len <= r.len)
      {
         memcpy(prompt, r.buf + r.at, prompt_len);
         prompt[prompt_len] = '\0';
         r.at += prompt_len;
      }
      else
         r.bad = 1;

      char *files_json = files ? cJSON_PrintUnformatted(files) : NULL;
      cJSON_Delete(files);

      if (!r.bad && files_json && prompt &&
          db1_coord_job_add_task(job_id, i < (uint32_t)stored.step_count ? stored.steps[i].id : 0,
                                 files_json, role, prompt, cwd, "engineer") > 0)
         added++;
      free(files_json);
      free(prompt);
   }

   /* Repairs and warnings the module made on our behalf, reported here because
    * this is the process an operator is watching. */
   uint32_t repair_count = aimee_delegates_rd_u32(&r);
   for (uint32_t i = 0; i < repair_count && !r.bad; i++)
   {
      char from[MAX_PATH_LEN], to[MAX_PATH_LEN];
      aimee_delegates_rd_str(&r, from, sizeof(from));
      aimee_delegates_rd_str(&r, to, sizeof(to));
      LOG_INFO("delegate", "packet path repair: '%s' -> '%s'", from, to);
   }
   uint32_t warning_count = aimee_delegates_rd_u32(&r);
   for (uint32_t i = 0; i < warning_count && !r.bad; i++)
   {
      char warning[1024];
      aimee_delegates_rd_str(&r, warning, sizeof(warning));
      LOG_WARN("delegate", "%s", warning);
   }
   free(response);

   if (r.bad || added != (int)task_count)
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
      out->max_concurrent = effective_par;
   }
   return 0;
}
