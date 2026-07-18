/* delegate_checkout.c: checkout metadata helpers for delegate results */
#include "aimee.h"
#include "cmd_agent_delegate_impl.h"
#include "util.h"
#include "cJSON.h"

static const char *DELEGATE_HEAD_UNAVAILABLE = "unavailable";

int delegate_git_head(const char *path, char *buf, size_t len)
{
   if (!buf || len == 0)
      return -1;
   buf[0] = '\0';
   if (!path || !path[0])
      return -1;

   char *esc_path = shell_escape(path);
   if (!esc_path)
      return -1;

   size_t cmd_len = strlen(esc_path) + 48;
   char *cmd = malloc(cmd_len);
   if (!cmd)
   {
      free(esc_path);
      return -1;
   }
   snprintf(cmd, cmd_len, "git -C '%s' rev-parse HEAD 2>/dev/null", esc_path);
   free(esc_path);

   int rc;
   char *out = run_cmd(cmd, &rc);
   free(cmd);
   if (rc != 0 || !out || !out[0])
   {
      free(out);
      return -1;
   }

   out[strcspn(out, "\r\n")] = '\0';
   if (!out[0])
   {
      free(out);
      return -1;
   }
   snprintf(buf, len, "%s", out);
   free(out);
   return 0;
}

int delegate_head_changed(const char *path, const char *old_head)
{
   if (!path || !path[0] || !old_head || !old_head[0])
      return 0;
   char current_head[64];
   if (delegate_git_head(path, current_head, sizeof(current_head)) != 0)
      return 0;
   return strcmp(current_head, old_head) != 0;
}

int delegate_git_worktree_fingerprint(const char *path, char *buf, size_t len)
{
   if (!buf || len == 0)
      return -1;
   buf[0] = '\0';
   if (!path || !path[0])
      return -1;

   char *esc_path = shell_escape(path);
   if (!esc_path)
      return -1;

   size_t cmd_len = strlen(esc_path) + 96;
   char *cmd = malloc(cmd_len);
   if (!cmd)
   {
      free(esc_path);
      return -1;
   }
   snprintf(cmd, cmd_len, "git -C '%s' status --porcelain=v1 --untracked-files=all 2>/dev/null",
            esc_path);
   free(esc_path);

   int rc;
   char *out = run_cmd(cmd, &rc);
   free(cmd);
   if (rc != 0 || !out)
   {
      free(out);
      return -1;
   }

   unsigned long long hash = 1469598103934665603ULL;
   int lines = 0;
   int saw_non_newline = 0;
   for (const unsigned char *p = (const unsigned char *)out; *p; p++)
   {
      hash ^= (unsigned long long)*p;
      hash *= 1099511628211ULL;
      if (*p == '\n')
      {
         if (saw_non_newline)
            lines++;
         saw_non_newline = 0;
      }
      else
         saw_non_newline = 1;
   }
   if (saw_non_newline)
      lines++;

   snprintf(buf, len, "%s:%d:%016llx", out[0] ? "dirty" : "clean", lines, hash);
   free(out);
   return 0;
}

static void delegate_prefix_response_warning(cJSON *resp, const char *field, const char *warning)
{
   cJSON *item = cJSON_GetObjectItemCaseSensitive(resp, field);
   if (!cJSON_IsString(item) || !item->valuestring)
      return;

   size_t wl = strlen(warning);
   size_t rl = strlen(item->valuestring);
   char *combined = malloc(wl + rl + 3);
   if (!combined)
      return;
   memcpy(combined, warning, wl);
   memcpy(combined + wl, "\n\n", 2);
   memcpy(combined + wl + 2, item->valuestring, rl + 1);
   cJSON_SetValuestring(item, combined);
   free(combined);
}

void delegate_checkout_add_result_ex(cJSON *resp, const char *launch_worktree_path,
                                     const char *launch_head, const char *parent_worktree_path,
                                     const char *parent_worktree_head,
                                     const char *parent_worktree_fingerprint)
{
   if (!resp)
      return;

   if (!launch_worktree_path || !launch_worktree_path[0])
      goto parent;

   cJSON_AddStringToObject(resp, "launch_worktree_path", launch_worktree_path);
   {
      if (launch_head && launch_head[0])
         cJSON_AddStringToObject(resp, "launch_head", launch_head);

      char finish_head[64];
      if (delegate_git_head(launch_worktree_path, finish_head, sizeof(finish_head)) != 0)
      {
         cJSON_AddStringToObject(resp, "finish_head", DELEGATE_HEAD_UNAVAILABLE);
         if (launch_head && launch_head[0])
         {
            cJSON_AddBoolToObject(resp, "checkout_drift", 1);
            const char *warning =
                "[CHECKOUT WARNING: delegate checkout head could not be read at result write; "
                "re-read referenced files before trusting this result.]";
            cJSON_AddStringToObject(resp, "checkout_warning", warning);
            delegate_prefix_response_warning(resp, "response", warning);
            delegate_prefix_response_warning(resp, "message", warning);
         }
         goto parent;
      }
      cJSON_AddStringToObject(resp, "finish_head", finish_head);
      if (launch_head && launch_head[0])
      {
         int drift = strcmp(launch_head, finish_head) != 0;
         cJSON_AddBoolToObject(resp, "checkout_drift", drift);
         if (drift)
         {
            const char *warning =
                "[CHECKOUT WARNING: delegate checkout changed between launch and result write; "
                "re-read referenced files before trusting this result.]";
            cJSON_AddStringToObject(resp, "checkout_warning", warning);
            delegate_prefix_response_warning(resp, "response", warning);
            delegate_prefix_response_warning(resp, "message", warning);
         }
      }
   }

parent:
   if (!parent_worktree_path || !parent_worktree_path[0])
      return;
   cJSON_AddStringToObject(resp, "parent_worktree_path", parent_worktree_path);
   if (parent_worktree_head && parent_worktree_head[0])
      cJSON_AddStringToObject(resp, "parent_worktree_head", parent_worktree_head);
   if (parent_worktree_fingerprint && parent_worktree_fingerprint[0])
      cJSON_AddStringToObject(resp, "parent_worktree_fingerprint", parent_worktree_fingerprint);

   int parent_drift = 0;
   const char *parent_warning = NULL;

   char parent_current_head[64];
   if (parent_worktree_head && parent_worktree_head[0])
   {
      if (delegate_git_head(parent_worktree_path, parent_current_head,
                            sizeof(parent_current_head)) == 0)
      {
         cJSON_AddStringToObject(resp, "parent_worktree_finish_head", parent_current_head);
         parent_drift = strcmp(parent_worktree_head, parent_current_head) != 0;
         cJSON_AddBoolToObject(resp, "parent_worktree_drift", parent_drift);
      }
      else
      {
         cJSON_AddStringToObject(resp, "parent_worktree_finish_head", DELEGATE_HEAD_UNAVAILABLE);
         cJSON_AddBoolToObject(resp, "parent_worktree_drift", 1);
         parent_drift = 1;
         parent_warning =
             "[PARENT CHECKOUT WARNING: parent worktree head could not be read at result write; "
             "re-read current source before trusting this result.]";
      }
   }

   char parent_current_fp[64];
   if (parent_worktree_fingerprint && parent_worktree_fingerprint[0] &&
       delegate_git_worktree_fingerprint(parent_worktree_path, parent_current_fp,
                                         sizeof(parent_current_fp)) == 0)
   {
      int status_drift = strcmp(parent_worktree_fingerprint, parent_current_fp) != 0;
      cJSON_AddStringToObject(resp, "parent_worktree_finish_fingerprint", parent_current_fp);
      cJSON_AddBoolToObject(resp, "parent_worktree_status_drift", status_drift);
      if (status_drift)
         parent_drift = 1;
   }

   if (!parent_drift)
      return;

   if (!parent_warning)
      parent_warning =
          "[PARENT CHECKOUT WARNING: parent worktree changed since delegate launch; re-read "
          "current source before trusting this result.]";
   cJSON_AddStringToObject(resp, "parent_worktree_warning", parent_warning);
   delegate_prefix_response_warning(resp, "response", parent_warning);
   delegate_prefix_response_warning(resp, "message", parent_warning);
}

void write_result_json_with_checkout_ex(const char *path, const agent_result_t *result,
                                        const char *launch_worktree_path, const char *launch_head,
                                        const char *parent_worktree_path,
                                        const char *parent_worktree_head,
                                        const char *parent_worktree_fingerprint)
{
   cJSON *obj = agent_result_to_json(result);
   if (!obj)
      return;
   delegate_checkout_add_result_ex(obj, launch_worktree_path, launch_head, parent_worktree_path,
                                   parent_worktree_head, parent_worktree_fingerprint);
   char *json = cJSON_Print(obj);
   cJSON_Delete(obj);
   if (!json)
      return;
   FILE *f = fopen(path, "w");
   if (f)
   {
      fputs(json, f);
      fputc('\n', f);
      fclose(f);
   }
   free(json);
}

void write_result_json_with_checkout(const char *path, const agent_result_t *result,
                                     const char *launch_worktree_path, const char *launch_head)
{
   write_result_json_with_checkout_ex(path, result, launch_worktree_path, launch_head, NULL, NULL,
                                      NULL);
}

void write_result_json(const char *path, const agent_result_t *result)
{
   write_result_json_with_checkout(path, result, NULL, NULL);
}
