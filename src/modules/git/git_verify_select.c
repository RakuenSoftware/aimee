#include "git_verify_select.h"

#include "dstr.h"
#include "platform_process.h"
#include "util.h"

#include <ctype.h>
#include <fnmatch.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct
{
   char hash[64];
   int failed;
   char step_results[256];
} verify_baseline_t;

static void trim(char *s)
{
   if (!s)
      return;
   while (isspace((unsigned char)*s))
      memmove(s, s + 1, strlen(s));
   char *e = s + strlen(s);
   while (e > s && isspace((unsigned char)e[-1]))
      *--e = '\0';
}

int verify_path_match(const char *pattern, const char *path)
{
   if (!pattern || !pattern[0] || !path || !path[0])
      return 0;
   if (strcmp(pattern, "**") == 0 || strcmp(pattern, "*") == 0)
      return 1;
   size_t len = strlen(pattern);
   if (len > 3 && strcmp(pattern + len - 3, "/**") == 0)
      return strncmp(path, pattern, len - 3) == 0 && (path[len - 3] == '/' || path[len - 3] == 0);
   if (strncmp(pattern, "**/*", 4) == 0)
   {
      const char *suffix = pattern + 4;
      size_t slen = strlen(suffix), plen = strlen(path);
      return plen >= slen && strcmp(path + plen - slen, suffix) == 0;
   }
   if (fnmatch(pattern, path, 0) == 0)
      return 1;
   if (!strchr(pattern, '/'))
   {
      const char *base = strrchr(path, '/');
      base = base ? base + 1 : path;
      return fnmatch(pattern, base, 0) == 0;
   }
   return 0;
}

int verify_path_list_matches(const char *patterns, const char *path)
{
   if (!patterns || !patterns[0] || !path || !path[0])
      return 0;
   char tmp[512];
   snprintf(tmp, sizeof(tmp), "%s", patterns);
   for (char *p = tmp; p && *p;)
   {
      char *comma = strchr(p, ',');
      if (comma)
         *comma = '\0';
      trim(p);
      if (verify_path_match(p, path))
         return 1;
      p = comma ? comma + 1 : NULL;
   }
   return 0;
}

static int step_result_lookup(const char *step_results, const char *name, int *out_rc)
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

static int read_passing_baseline(const char *project_root, verify_baseline_t *out)
{
   char path[MAX_PATH_LEN];
   verify_state_path(project_root, path, sizeof(path));
   FILE *f = fopen(path, "r");
   if (!f)
      return 0;
   char line[512];
   while (fgets(line, sizeof(line), f))
   {
      char h[64] = "";
      int failed = 0, total = 0;
      long long ts = 0;
      if (sscanf(line, "%lld %63s failed=%d/total=%d", &ts, h, &failed, &total) >= 4 && failed == 0)
      {
         memset(out, 0, sizeof(*out));
         snprintf(out->hash, sizeof(out->hash), "%s", h);
         out->failed = failed;
         const char *sp = strstr(line, " steps=");
         if (sp)
         {
            snprintf(out->step_results, sizeof(out->step_results), "%s", sp + 7);
            trim(out->step_results);
         }
         fclose(f);
         return out->step_results[0] != '\0';
      }
   }
   fclose(f);
   return 0;
}

static void append_cmd_lines(dstr_t *out, const char *project_root, const char *cmd)
{
   char full[MAX_PATH_LEN + 256];
   if (project_root && project_root[0])
   {
      char *esc_root = shell_escape(project_root);
      snprintf(full, sizeof(full), "git -C '%s' %s", esc_root ? esc_root : "", cmd);
      free(esc_root);
   }
   else
      snprintf(full, sizeof(full), "git %s", cmd);
   int rc = 0;
   char *res = run_cmd(full, &rc);
   if (rc == 0 && res && res[0])
      dstr_append_str(out, res);
   free(res);
}

static char *changed_paths(const char *project_root, const char *baseline)
{
   dstr_t out;
   dstr_init(&out);
   char cmd[160];
   char *esc_baseline = shell_escape(baseline);
   snprintf(cmd, sizeof(cmd), "diff --name-only '%s' HEAD 2>/dev/null",
            esc_baseline ? esc_baseline : "");
   free(esc_baseline);
   append_cmd_lines(&out, project_root, cmd);
   append_cmd_lines(&out, project_root, "diff --name-only HEAD 2>/dev/null");
   append_cmd_lines(&out, project_root, "ls-files --others --exclude-standard 2>/dev/null");
   return dstr_steal(&out);
}

static int changed_any_match(const char *changed, const char *patterns)
{
   if (!changed || !changed[0] || !patterns || !patterns[0])
      return 0;
   char *copy = safe_strdup(changed);
   int matched = 0;
   for (char *line = strtok(copy, "\n"); line; line = strtok(NULL, "\n"))
   {
      if (verify_path_list_matches(patterns, line))
      {
         matched = 1;
         break;
      }
   }
   free(copy);
   return matched;
}

static void matched_paths(char *out, size_t out_len, const char *changed, const char *patterns)
{
   out[0] = '\0';
   if (!changed || !changed[0] || !patterns || !patterns[0])
      return;
   char *copy = safe_strdup(changed);
   size_t pos = 0;
   for (char *line = strtok(copy, "\n"); line; line = strtok(NULL, "\n"))
   {
      if (!verify_path_list_matches(patterns, line))
         continue;
      int w = snprintf(out + pos, out_len - pos, "%s%s", pos ? " " : "", line);
      if (w < 0 || (size_t)w >= out_len - pos)
         break;
      pos += (size_t)w;
   }
   free(copy);
}

static void write_changed_file(char *out, size_t out_len, const char *changed)
{
   out[0] = '\0';
   char tmpl[] = "/tmp/aimee-verify-changed-XXXXXX";
   int fd = mkstemp(tmpl);
   if (fd < 0)
      return;
   FILE *f = fdopen(fd, "w");
   if (!f)
   {
      close(fd);
      return;
   }
   if (changed)
      fputs(changed, f);
   fclose(f);
   snprintf(out, out_len, "%s", tmpl);
}

void verify_incremental_apply(const char *project_root, verify_config_t *cfg,
                              verify_thread_ctx_t *contexts, int *step_state, int *remaining)
{
   if (!cfg || !contexts)
      return;
   for (int i = 0; i < cfg->count; i++)
      contexts[i].changed_all = cfg->incremental ? 0 : 1;
   if (!cfg->incremental)
      return;

   verify_baseline_t base;
   if (!read_passing_baseline(project_root, &base))
   {
      for (int i = 0; i < cfg->count; i++)
         contexts[i].changed_all = 1;
      return;
   }

   char *changed = changed_paths(project_root, base.hash);
   char changed_file[MAX_PATH_LEN] = "";
   write_changed_file(changed_file, sizeof(changed_file), changed);
   int run_all = changed_any_match(changed, cfg->always_run_globs);

   for (int i = 0; i < cfg->count; i++)
   {
      snprintf(contexts[i].baseline_ref, sizeof(contexts[i].baseline_ref), "%s", base.hash);
      snprintf(contexts[i].changed_files_path, sizeof(contexts[i].changed_files_path), "%s",
               changed_file);
      matched_paths(contexts[i].changed_matched, sizeof(contexts[i].changed_matched), changed,
                    cfg->steps[i].paths);
      int saved_rc = -1;
      int has_saved = step_result_lookup(base.step_results, cfg->steps[i].name, &saved_rc);
      int step_changed = changed_any_match(changed, cfg->steps[i].paths);
      if (run_all || !cfg->steps[i].paths[0] || !has_saved || saved_rc != 0 || step_changed)
         continue;
      contexts[i].rc = 0;
      contexts[i].skipped = 1;
      snprintf(contexts[i].skip_reason, sizeof(contexts[i].skip_reason), "no changes since %.12s",
               base.hash);
      if (step_state && remaining && step_state[i] == 0)
      {
         step_state[i] = 2;
         (*remaining)--;
      }
   }
   free(changed);
}
