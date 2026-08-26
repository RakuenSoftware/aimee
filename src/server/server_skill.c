#include "server_skill.h"
#include "aimee.h"
#include <aimee/skills/skill.h>
#include "cJSON.h"
#include "json_fluent.h" /* jo_ok */
#include "kb_client.h"
#include "modules/workspace/workspace_turn.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static const char *skill_req_str(cJSON *req, const char *name)
{
   cJSON *item = cJSON_GetObjectItemCaseSensitive(req, name);
   return cJSON_IsString(item) ? item->valuestring : NULL;
}

/* A request may identify a project only through the operator's registered
 * workspace set.  The no-cwd fallback is the server's own trusted startup cwd;
 * it is not caller-controlled and preserves local CLI behaviour. */
static const char *skill_req_cwd(cJSON *req, char *fallback, size_t fallback_len)
{
   const char *cwd = skill_req_str(req, "cwd");
   if (cwd && cwd[0])
   {
      if (workspace_turn_workspace_authorized(cwd, fallback, fallback_len))
         return fallback;
      if (fallback && fallback_len)
         fallback[0] = '\0';
      return NULL;
   }
   if (fallback && fallback_len > 0 && getcwd(fallback, fallback_len))
      return fallback;
   if (fallback && fallback_len > 0)
      fallback[0] = '\0';
   return fallback ? fallback : "";
}

static int skill_send_error(server_conn_t *conn, const char *message)
{
   cJSON *resp = cJSON_CreateObject();
   cJSON_AddStringToObject(resp, "status", "error");
   cJSON_AddStringToObject(resp, "message", message ? message : "skill command failed");
   return server_send_ok(conn, resp);
}

static int skill_send_ok(server_conn_t *conn, const char *action, const char *name)
{
   cJSON *resp = jo_ok();
   cJSON_AddStringToObject(resp, "action", action ? action : "");
   if (name && name[0])
      cJSON_AddStringToObject(resp, "name", name);
   return server_send_ok(conn, resp);
}

int handle_skill_list(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   char cwd[MAX_PATH_LEN];
   const char *root = skill_req_cwd(req, cwd, sizeof(cwd));
   if (!root)
      return skill_send_error(conn, "cwd is not inside a registered workspace");
   char names[SKILL_MAX_SKILLS][SKILL_NAME_MAX];
   int n = skill_list(root, names, SKILL_MAX_SKILLS);
   cJSON *resp = jo_ok();
   cJSON *arr = cJSON_AddArrayToObject(resp, "skills");
   for (int i = 0; i < n; i++)
   {
      const char *source = skill_source(root, names[i]);
      if (!source)
         source = "user";
      cJSON *item = cJSON_CreateObject();
      cJSON_AddStringToObject(item, "name", names[i]);
      cJSON_AddStringToObject(item, "source", source);
      skill_usage_t usage;
      skill_usage_get(root, names[i], &usage);
      cJSON_AddNumberToObject(item, "use_count", usage.use_count);
      cJSON_AddNumberToObject(item, "view_count", usage.view_count);
      cJSON_AddNumberToObject(item, "patch_count", usage.patch_count);
      cJSON_AddStringToObject(item, "state", usage.state);
      cJSON_AddBoolToObject(item, "pinned", usage.pinned);
      cJSON_AddStringToObject(item, "created_by", usage.created_by);
      cJSON_AddItemToArray(arr, item);
   }
   return server_send_ok(conn, resp);
}

int handle_skill_show(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   const char *name = skill_req_str(req, "name");
   if (!skill_name_is_valid(name))
      return skill_send_error(conn, "skill.show requires a valid skill name");
   char cwd[MAX_PATH_LEN], err[256] = "";
   const char *root = skill_req_cwd(req, cwd, sizeof(cwd));
   if (!root)
      return skill_send_error(conn, "cwd is not inside a registered workspace");
   const char *file_path = skill_req_str(req, "file_path");
   char *content = file_path && file_path[0]
                       ? skill_support_file_load(root, name, file_path, err, sizeof(err))
                       : skill_load(root, name);
   if (!content)
      return skill_send_error(conn, err[0] ? err : "skill not found");
   (void)skill_record_view(root, name);
   cJSON *resp = jo_ok();
   cJSON_AddStringToObject(resp, "name", name);
   if (file_path && file_path[0])
      cJSON_AddStringToObject(resp, "file_path", file_path);
   cJSON_AddStringToObject(resp, "content", content);
   free(content);
   return server_send_ok(conn, resp);
}

int handle_skill_lint(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   char cwd[MAX_PATH_LEN];
   const char *root = skill_req_cwd(req, cwd, sizeof(cwd));
   if (!root)
      return skill_send_error(conn, "cwd is not inside a registered workspace");
   int issues = 0;
   int checked = 0;
   char combined[4096] = "";
   cJSON *jall = cJSON_GetObjectItemCaseSensitive(req, "all");
   if (cJSON_IsTrue(jall))
   {
      char names[SKILL_MAX_SKILLS][SKILL_NAME_MAX];
      int n = skill_list(root, names, SKILL_MAX_SKILLS);
      for (int i = 0; i < n; i++)
      {
         char report[1024];
         int rc = skill_lint(root, names[i], report, sizeof(report));
         checked++;
         if (rc > 0)
         {
            issues += rc;
            strncat(combined, report, sizeof(combined) - strlen(combined) - 1);
         }
      }
   }
   else
   {
      const char *name = skill_req_str(req, "name");
      if (!name || !name[0])
         return skill_send_error(conn, "skill.lint requires name or --all");
      issues = skill_lint(root, name, combined, sizeof(combined));
      checked = 1;
   }

   cJSON *resp = cJSON_CreateObject();
   cJSON_AddStringToObject(resp, "status", issues == 0 ? "ok" : "error");
   if (issues > 0)
      cJSON_AddStringToObject(resp, "message", combined[0] ? combined : "skill lint failed");
   cJSON_AddNumberToObject(resp, "checked", checked);
   cJSON_AddNumberToObject(resp, "issues", issues);
   if (combined[0])
      cJSON_AddStringToObject(resp, "report", combined);
   return server_send_ok(conn, resp);
}

int handle_skill_eval(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   const char *name = skill_req_str(req, "name");
   if (!name || !name[0])
      return skill_send_error(conn, "skill.eval requires name");
   char cwd[MAX_PATH_LEN], err[256] = "";
   const char *root = skill_req_cwd(req, cwd, sizeof(cwd));
   if (!root)
      return skill_send_error(conn, "cwd is not inside a registered workspace");
   skill_eval_result_t result;
   int rc = skill_eval_run(root, name, &result, err, sizeof(err));

   cJSON *resp = cJSON_CreateObject();
   cJSON_AddStringToObject(resp, "status", rc == 0 && result.passed ? "ok" : "error");
   cJSON_AddStringToObject(resp, "name", name);
   cJSON_AddBoolToObject(resp, "passed", rc == 0 && result.passed ? 1 : 0);
   cJSON_AddNumberToObject(resp, "scenarios", result.scenarios);
   cJSON_AddNumberToObject(resp, "baseline_violations", result.baseline_violations);
   cJSON_AddNumberToObject(resp, "baseline_compliances", result.baseline_compliances);
   cJSON_AddNumberToObject(resp, "treatment_compliances", result.treatment_compliances);
   cJSON_AddNumberToObject(resp, "compliance_delta", result.compliance_delta);
   if (rc != 0)
      cJSON_AddStringToObject(resp, "message", err[0] ? err : "skill eval failed");
   else if (result.first_failure[0])
      cJSON_AddStringToObject(resp, "message", result.first_failure);
   return server_send_ok(conn, resp);
}

int handle_skill_create(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   const char *name = skill_req_str(req, "name");
   const char *content = skill_req_str(req, "content");
   if (!name || !content)
      return skill_send_error(conn, "skill.create requires name and content");
   char cwd[MAX_PATH_LEN], err[256] = "";
   if (skill_lint_content(name, content, err, sizeof(err)) != 0)
      return skill_send_error(conn, err[0] ? err : "skill lint failed");
   const char *root = skill_req_cwd(req, cwd, sizeof(cwd));
   if (!root)
      return skill_send_error(conn, "cwd is not inside a registered workspace");
   int rc = skill_manage_create(root, name, content, "user", err, sizeof(err));
   return rc == 0 ? skill_send_ok(conn, "create", name)
                  : skill_send_error(conn, err[0] ? err : "skill create failed");
}

int handle_skill_edit(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   const char *name = skill_req_str(req, "name");
   const char *content = skill_req_str(req, "content");
   if (!name || !content)
      return skill_send_error(conn, "skill.edit requires name and content");
   char cwd[MAX_PATH_LEN], err[256] = "";
   if (skill_lint_content(name, content, err, sizeof(err)) != 0)
      return skill_send_error(conn, err[0] ? err : "skill lint failed");
   const char *root = skill_req_cwd(req, cwd, sizeof(cwd));
   if (!root)
      return skill_send_error(conn, "cwd is not inside a registered workspace");
   int rc = skill_manage_edit(root, name, content, "user", err, sizeof(err));
   return rc == 0 ? skill_send_ok(conn, "edit", name)
                  : skill_send_error(conn, err[0] ? err : "skill edit failed");
}

int handle_skill_patch(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   const char *name = skill_req_str(req, "name");
   const char *old_string = skill_req_str(req, "old_string");
   const char *new_string = skill_req_str(req, "new_string");
   if (!name || !old_string || !new_string)
      return skill_send_error(conn, "skill.patch requires name, old_string, and new_string");
   cJSON *jall = cJSON_GetObjectItemCaseSensitive(req, "replace_all");
   char cwd[MAX_PATH_LEN], err[256] = "";
   const char *root = skill_req_cwd(req, cwd, sizeof(cwd));
   if (!root)
      return skill_send_error(conn, "cwd is not inside a registered workspace");
   int rc = skill_manage_patch(root, name, old_string, new_string, cJSON_IsTrue(jall), "user", err,
                               sizeof(err));
   return rc == 0 ? skill_send_ok(conn, "patch", name)
                  : skill_send_error(conn, err[0] ? err : "skill patch failed");
}

int handle_skill_archive(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   const char *name = skill_req_str(req, "name");
   if (!name)
      return skill_send_error(conn, "skill.archive requires name");
   char cwd[MAX_PATH_LEN], err[256] = "";
   const char *root = skill_req_cwd(req, cwd, sizeof(cwd));
   if (!root)
      return skill_send_error(conn, "cwd is not inside a registered workspace");
   int rc = skill_manage_archive(root, name, skill_req_str(req, "absorbed_into"), err, sizeof(err));
   return rc == 0 ? skill_send_ok(conn, "archive", name)
                  : skill_send_error(conn, err[0] ? err : "skill archive failed");
}

int handle_skill_pin(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   const char *name = skill_req_str(req, "name");
   if (!name)
      return skill_send_error(conn, "skill.pin requires name");
   cJSON *jpinned = cJSON_GetObjectItemCaseSensitive(req, "pinned");
   char cwd[MAX_PATH_LEN];
   const char *root = skill_req_cwd(req, cwd, sizeof(cwd));
   if (!root)
      return skill_send_error(conn, "cwd is not inside a registered workspace");
   int rc = skill_set_pinned(root, name, cJSON_IsTrue(jpinned));
   return rc == 0 ? skill_send_ok(conn, cJSON_IsTrue(jpinned) ? "pin" : "unpin", name)
                  : skill_send_error(conn, "skill not found");
}

int handle_skill_lifecycle(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   char cwd[MAX_PATH_LEN], err[256] = "";
   const char *root = skill_req_cwd(req, cwd, sizeof(cwd));
   if (!root)
      return skill_send_error(conn, "cwd is not inside a registered workspace");
   cJSON *jstale = cJSON_GetObjectItemCaseSensitive(req, "stale_after_days");
   cJSON *jarchive = cJSON_GetObjectItemCaseSensitive(req, "archive_after_days");
   int stale_days = cJSON_IsNumber(jstale) ? jstale->valueint : config_skills_stale_after_days();
   int archive_days =
       cJSON_IsNumber(jarchive) ? jarchive->valueint : config_skills_archive_after_days();
   if (stale_days <= 0 || archive_days <= 0)
      return skill_send_error(conn, "skill lifecycle thresholds must be positive integers");
   skill_lifecycle_result_t result;
   int rc = skill_lifecycle_apply(root, stale_days, archive_days, &result, err, sizeof(err));
   cJSON *resp = cJSON_CreateObject();
   cJSON_AddStringToObject(resp, "status", rc == 0 ? "ok" : "error");
   if (err[0])
      cJSON_AddStringToObject(resp, "message", err);
   cJSON_AddNumberToObject(resp, "considered", result.considered);
   cJSON_AddNumberToObject(resp, "stale_marked", result.stale_marked);
   cJSON_AddNumberToObject(resp, "archived", result.archived);
   cJSON_AddNumberToObject(resp, "skipped_pinned", result.skipped_pinned);
   cJSON_AddNumberToObject(resp, "errors", result.errors);
   return server_send_ok(conn, resp);
}

int handle_skill_autostub(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   char cwd[MAX_PATH_LEN], err[256] = "";
   const char *root = skill_req_cwd(req, cwd, sizeof(cwd));
   if (!root)
      return skill_send_error(conn, "cwd is not inside a registered workspace");
   cJSON *jforce = cJSON_GetObjectItemCaseSensitive(req, "force");
   if (!config_skills_capability_autostub() && !cJSON_IsTrue(jforce))
   {
      cJSON *resp = cJSON_CreateObject();
      cJSON_AddStringToObject(resp, "status", "disabled");
      cJSON_AddStringToObject(resp, "message", "skills.capability.autostub is false");
      return server_send_ok(conn, resp);
   }

   if (skill_req_str(req, "snapshot_path"))
      return skill_send_error(conn, "snapshot_path is not accepted by the server API");
   char *snapshot = kb_client_tool_registry_snapshot_json();
   if (!snapshot)
      return skill_send_error(conn, "failed to read tool registry snapshot");

   skill_capability_autostub_result_t result;
   int rc = skill_capability_autostub_from_json(root, snapshot, &result, err, sizeof(err));
   free(snapshot);

   cJSON *resp = cJSON_CreateObject();
   cJSON_AddStringToObject(resp, "status", rc == 0 ? "ok" : "error");
   if (err[0])
      cJSON_AddStringToObject(resp, "message", err);
   cJSON_AddNumberToObject(resp, "scanned", result.scanned);
   cJSON_AddNumberToObject(resp, "existing", result.existing);
   cJSON_AddNumberToObject(resp, "proposed", result.proposed);
   cJSON_AddNumberToObject(resp, "skipped", result.skipped);
   cJSON_AddNumberToObject(resp, "errors", result.errors);
   if (result.first_proposal[0])
      cJSON_AddStringToObject(resp, "first_proposal", result.first_proposal);
   if (result.first_change_path[0])
      cJSON_AddStringToObject(resp, "first_change_path", result.first_change_path);
   return server_send_ok(conn, resp);
}
