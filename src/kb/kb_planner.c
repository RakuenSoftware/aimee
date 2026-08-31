/* kb_planner.c: Deliberate planning — MCTS search and SMT constraint validation.
 * See docs/proposals/accepted/deliberate-planning-search-and-constraint-execution.md
 */

#include "kb_planner.h"
#include "modules/db2/c/artifacts.h"
#include "modules/db2/c/db2_internal.h"
#include "modules/db2/c/db_postgres.h"
#include "headers/platform_process.h"

#include <cJSON.h>
#include <stdlib.h>
#include <string.h>

static int run_sidecar(const char *cmd, const char *request_json, char **out_json, size_t *out_len)
{
   if (!cmd || !cmd[0] || !request_json)
      return -1;
   if (!out_json || !out_len)
      return -1;

   *out_json = NULL;
   *out_len = 0;

   char *out = NULL;
   size_t len = 0;
   platform_exec_pipe(cmd, request_json, strlen(request_json), &out, &len);
   if (!out)
      return -1;

   *out_json = out;
   *out_len = len;
   return 0;
}

int kb_planner_search(const char *request_json, char **out_json, size_t *out_len)
{
   const char *cmd = config_planner_search_command();
   if (!cmd || !cmd[0])
      return -1;
   return run_sidecar(cmd, request_json, out_json, out_len);
}

int kb_planner_validate(const char *request_json, char **out_json, size_t *out_len)
{
   const char *cmd = config_constraint_solver_command();
   if (!cmd || !cmd[0])
      return -1;
   return run_sidecar(cmd, request_json, out_json, out_len);
}

int kb_planner_artifact_write(const char *kind, const char *scope_id, const char *payload,
                              char *id_out, size_t id_out_len)
{
   if (!kind || !scope_id || !payload)
      return -1;

   /* Only the two planner kinds are accepted here. The charter table accepts any
    * string, but the planner module owns this surface and must not silently
    * accept typos that would later confuse retrieval. */
   if (strcmp(kind, "plan_candidate") != 0 && strcmp(kind, "plan_template") != 0)
      return -1;

   /* Validate payload parses as JSON. */
   cJSON *parsed = cJSON_Parse(payload);
   if (!parsed)
      return -1;
   cJSON_Delete(parsed);

   char id[64];
   db2_artifact_gen_id(id, sizeof(id));

   int rc = db2_artifact_write(id, kind, "proposed", "plan", scope_id, "", 0.5, payload);
   if (rc != 0)
      return -1;

   if (id_out && id_out_len > 0)
   {
      strncpy(id_out, id, id_out_len - 1);
      id_out[id_out_len - 1] = '\0';
   }
   return 0;
}
