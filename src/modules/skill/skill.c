/* skill.c: project-scoped skill context injection for agent system prompts */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "aimee.h"
#include "skill.h"
#include "config.h"
#include "cJSON.h"
#include "dstr.h"
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#ifndef _WIN32
#include <sys/file.h>
#endif
#include <sys/stat.h>
#include <unistd.h>

static int64_t s_skill_self_activations_total = 0;

void skill_metrics(int64_t *self_activations_total)
{
   if (self_activations_total)
      *self_activations_total = s_skill_self_activations_total;
}

static void skill_metrics_record_activation(void)
{
   s_skill_self_activations_total++;
}

/* --- Path resolution --- */

typedef enum
{
   SKILL_SOURCE_NONE = 0,
   SKILL_SOURCE_PROJECT,
   SKILL_SOURCE_USER,
   SKILL_SOURCE_BUNDLED,
} skill_source_t;

static const char *skill_bundled_dir(void)
{
   static char path[SKILL_PATH_MAX];
   const char *override = getenv("AIMEE_BUNDLED_SKILLS_DIR");
   if (override && override[0])
   {
      snprintf(path, sizeof(path), "%s", override);
      return path;
   }
#ifdef _WIN32
   const char *local = getenv("LOCALAPPDATA");
   if (local && local[0])
   {
      snprintf(path, sizeof(path), "%s/aimee/skills", local);
      return path;
   }
   const char *home = getenv("USERPROFILE");
#else
   const char *xdg = getenv("XDG_DATA_HOME");
   if (xdg && xdg[0])
   {
      snprintf(path, sizeof(path), "%s/aimee/skills", xdg);
      return path;
   }
   const char *home = getenv("HOME");
#endif
   if (home && home[0])
      snprintf(path, sizeof(path), "%s/.local/share/aimee/skills", home);
   else
      snprintf(path, sizeof(path), "%s/bundled-skills", config_default_dir());
   return path;
}

static int skill_regular_file(const char *path)
{
   struct stat st;
   return path && stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

static int skill_dir_with_manifest(const char *base, const char *name, char *buf, size_t bufsz)
{
   snprintf(buf, bufsz, "%s/%s/SKILL.md", base, name);
   return skill_regular_file(buf) ? 0 : -1;
}

static int skill_flat_file(const char *base, const char *name, char *buf, size_t bufsz)
{
   snprintf(buf, bufsz, "%s/%s.md", base, name);
   return skill_regular_file(buf) ? 0 : -1;
}

static int skill_try_base(const char *base, const char *name, char *buf, size_t bufsz)
{
   if (!base || !base[0])
      return -1;
   if (skill_dir_with_manifest(base, name, buf, bufsz) == 0)
      return 0;
   return skill_flat_file(base, name, buf, bufsz);
}

static int skill_resolve_path(const char *project_root, const char *name, char *buf, size_t bufsz,
                              skill_source_t *source_out)
{
   if (!name || !name[0] || !buf || bufsz == 0)
      return -1;

   char base[SKILL_PATH_MAX];

   if (project_root && project_root[0])
   {
      snprintf(base, sizeof(base), "%s/.aimee/skills", project_root);
      if (skill_try_base(base, name, buf, bufsz) == 0)
      {
         if (source_out)
            *source_out = SKILL_SOURCE_PROJECT;
         return 0;
      }
   }

   snprintf(base, sizeof(base), "%s/skills", config_default_dir());
   if (skill_try_base(base, name, buf, bufsz) == 0)
   {
      if (source_out)
         *source_out = SKILL_SOURCE_USER;
      return 0;
   }

   if (skill_try_base(skill_bundled_dir(), name, buf, bufsz) == 0)
   {
      if (source_out)
         *source_out = SKILL_SOURCE_BUNDLED;
      return 0;
   }

   buf[0] = '\0';
   if (source_out)
      *source_out = SKILL_SOURCE_NONE;
   return -1;
}

int skill_path(const char *project_root, const char *name, char *buf, size_t bufsz)
{
   return skill_resolve_path(project_root, name, buf, bufsz, NULL);
}

const char *skill_source(const char *project_root, const char *name)
{
   char path[SKILL_PATH_MAX];
   skill_source_t source = SKILL_SOURCE_NONE;
   if (skill_resolve_path(project_root, name, path, sizeof(path), &source) != 0)
      return NULL;
   switch (source)
   {
   case SKILL_SOURCE_PROJECT:
      return "project";
   case SKILL_SOURCE_USER:
      return "user";
   case SKILL_SOURCE_BUNDLED:
      return "bundled";
   default:
      return NULL;
   }
}

/* --- Load --- */

char *skill_load(const char *project_root, const char *name)
{
   if (!name || !name[0])
      return NULL;

   char path[SKILL_PATH_MAX];
   if (skill_path(project_root, name, path, sizeof(path)) != 0)
      return NULL;

   FILE *f = fopen(path, "r");
   if (!f)
      return NULL;

   char *buf = malloc(SKILL_MAX_SIZE + 1);
   if (!buf)
   {
      fclose(f);
      return NULL;
   }

   size_t n = fread(buf, 1, SKILL_MAX_SIZE, f);
   buf[n] = '\0';
   fclose(f);

   if (n == 0)
   {
      free(buf);
      return NULL;
   }

   return buf;
}

/* --- List --- */

int skill_list(const char *project_root, char names_out[][SKILL_NAME_MAX], int max_names)
{
   if (!names_out || max_names <= 0)
      return 0;

   int count = 0;

#define ADD_NAME(name)                                                                             \
   do                                                                                              \
   {                                                                                               \
      int _dup = 0;                                                                                \
      for (int _i = 0; _i < count; _i++)                                                           \
         if (strcmp(names_out[_i], (name)) == 0)                                                   \
         {                                                                                         \
            _dup = 1;                                                                              \
            break;                                                                                 \
         }                                                                                         \
      if (!_dup && count < max_names)                                                              \
         snprintf(names_out[count++], SKILL_NAME_MAX, "%s", (name));                               \
   } while (0)

   const char *scan_dirs[3] = {NULL, NULL, NULL};
   char project_dir[SKILL_PATH_MAX];
   char user_dir[SKILL_PATH_MAX];
   if (project_root && project_root[0])
   {
      snprintf(project_dir, sizeof(project_dir), "%s/.aimee/skills", project_root);
      scan_dirs[0] = project_dir;
   }
   snprintf(user_dir, sizeof(user_dir), "%s/skills", config_default_dir());
   scan_dirs[1] = user_dir;
   scan_dirs[2] = skill_bundled_dir();

   for (int s = 0; s < 3 && count < max_names; s++)
   {
      const char *dir = scan_dirs[s];
      if (!dir || !dir[0])
         continue;
      DIR *d = opendir(dir);
      if (d)
      {
         struct dirent *ent;
         while ((ent = readdir(d)) != NULL && count < max_names)
         {
            size_t nlen = strlen(ent->d_name);
            if (nlen > 3 && strcmp(ent->d_name + nlen - 3, ".md") == 0)
            {
               char skill[SKILL_NAME_MAX];
               size_t rlen = nlen - 3;
               if (rlen < sizeof(skill))
               {
                  memcpy(skill, ent->d_name, rlen);
                  skill[rlen] = '\0';
                  ADD_NAME(skill);
               }
            }
            else if (strcmp(ent->d_name, ".") != 0 && strcmp(ent->d_name, "..") != 0)
            {
               char manifest[SKILL_PATH_MAX];
               snprintf(manifest, sizeof(manifest), "%s/%s/SKILL.md", dir, ent->d_name);
               if (skill_regular_file(manifest) && strlen(ent->d_name) < SKILL_NAME_MAX)
                  ADD_NAME(ent->d_name);
            }
         }
         closedir(d);
      }
   }

#undef ADD_NAME

   return count;
}

/* --- Usage telemetry --- */

static void skill_usage_default(skill_usage_t *out)
{
   if (!out)
      return;
   memset(out, 0, sizeof(*out));
   snprintf(out->state, sizeof(out->state), "active");
   snprintf(out->created_by, sizeof(out->created_by), "user");
}

int skill_name_is_valid(const char *name)
{
   if (!name || !name[0] || strlen(name) >= SKILL_NAME_MAX)
      return 0;
   if (!isalnum((unsigned char)name[0]))
      return 0;
   for (const char *p = name; *p; p++)
   {
      if (!(islower((unsigned char)*p) || isdigit((unsigned char)*p) || *p == '.' || *p == '_' ||
            *p == '-'))
         return 0;
   }
   return 1;
}

static int skill_name_is_session_artifact(const char *name)
{
   if (!name || !name[0])
      return 0;

   if (strncmp(name, "fix-pr-", 7) == 0 || strncmp(name, "pr-", 3) == 0 ||
       strncmp(name, "issue-", 6) == 0 || strncmp(name, "bug-", 4) == 0)
   {
      const char *digits = strrchr(name, '-');
      if (digits && isdigit((unsigned char)digits[1]))
         return 1;
   }
   if (strstr(name, "-today") || strstr(name, "today-") || strstr(name, "-wip") ||
       strstr(name, "wip-"))
      return 1;
   return 0;
}

static void skill_lint_issue(char *report, size_t report_len, int *issues, const char *fmt, ...)
{
   if (issues)
      (*issues)++;
   if (!report || report_len == 0)
      return;
   size_t used = strlen(report);
   if (used >= report_len - 1)
      return;
   va_list ap;
   va_start(ap, fmt);
   int n = vsnprintf(report + used, report_len - used, fmt, ap);
   va_end(ap);
   if (n < 0)
      return;
   used = strlen(report);
   if (used < report_len - 1)
      snprintf(report + used, report_len - used, "\n");
}

static int skill_lint_line_value(const char *header, size_t header_len, const char *key, char *out,
                                 size_t out_len)
{
   if (out && out_len > 0)
      out[0] = '\0';
   if (!header || !key || !out || out_len == 0)
      return 0;
   size_t key_len = strlen(key);
   const char *p = header;
   const char *end = header + header_len;
   while (p < end)
   {
      const char *line_end = memchr(p, '\n', (size_t)(end - p));
      if (!line_end)
         line_end = end;
      size_t line_len = (size_t)(line_end - p);
      if (line_len >= key_len + 1 && strncmp(p, key, key_len) == 0 && p[key_len] == ':')
      {
         const char *v = p + key_len + 1;
         const char *v_end = p + line_len;
         while (v < v_end && isspace((unsigned char)*v))
            v++;
         while (v_end > v && isspace((unsigned char)v_end[-1]))
            v_end--;
         if (v_end > v + 1 &&
             ((*v == '"' && v_end[-1] == '"') || (*v == '\'' && v_end[-1] == '\'')))
         {
            v++;
            v_end--;
         }
         size_t n = (size_t)(v_end - v);
         if (n >= out_len)
            n = out_len - 1;
         memcpy(out, v, n);
         out[n] = '\0';
         return 1;
      }
      p = line_end < end ? line_end + 1 : end;
   }
   return 0;
}

int skill_description(const char *project_root, const char *name, char *out, size_t out_len)
{
   if (out && out_len > 0)
      out[0] = '\0';
   if (!name || !name[0] || !out || out_len == 0)
      return -1;

   char *content = skill_load(project_root, name);
   if (!content)
      return -1;

   int rc = -1;
   if (strncmp(content, "---", 3) == 0 && (content[3] == '\n' || content[3] == '\r'))
   {
      const char *header_start = strchr(content, '\n');
      const char *header_end = header_start ? strstr(header_start + 1, "\n---") : NULL;
      if (header_start && header_end)
      {
         size_t header_len = (size_t)(header_end - header_start - 1);
         if (skill_lint_line_value(header_start + 1, header_len, "description", out, out_len) &&
             out[0])
            rc = 0;
      }
   }
   free(content);
   return rc;
}

static int skill_trigger_value_contains_token(const char *value, size_t value_len,
                                              const char *needle)
{
   if (!value || !needle || !needle[0])
      return 0;
   size_t needle_len = strlen(needle);
   const char *p = value;
   const char *end = value + value_len;
   while (p < end)
   {
      while (p < end && !(isalnum((unsigned char)*p) || *p == '_' || *p == '-' || *p == '.'))
         p++;
      const char *start = p;
      while (p < end && (isalnum((unsigned char)*p) || *p == '_' || *p == '-' || *p == '.'))
         p++;
      if ((size_t)(p - start) == needle_len && strncmp(start, needle, needle_len) == 0)
         return 1;
   }
   return 0;
}

static int skill_trigger_patterns_match(const char *value, size_t value_len, const char *subject)
{
   if (!value || !subject)
      return 0;

   const char *p = value;
   const char *end = value + value_len;
   int saw_quoted = 0;
   while (p < end)
   {
      while (p < end && *p != '"' && *p != '\'')
         p++;
      if (p >= end)
         break;
      char quote = *p++;
      const char *start = p;
      while (p < end && *p != quote)
         p++;
      size_t pat_len = (size_t)(p - start);
      if (pat_len > 0)
      {
         saw_quoted = 1;
         char pat[128];
         if (pat_len >= sizeof(pat))
            pat_len = sizeof(pat) - 1;
         memcpy(pat, start, pat_len);
         pat[pat_len] = '\0';
         if (strstr(subject, pat))
            return 1;
      }
      if (p < end)
         p++;
   }
   if (saw_quoted)
      return 0;

   return skill_trigger_value_contains_token(value, value_len, subject);
}

static int skill_trigger_line_value(const char *line, size_t line_len, const char *key,
                                    const char **value, size_t *value_len)
{
   size_t key_len = strlen(key);
   const char *p = line;
   const char *end = line + line_len;
   while (p < end && isspace((unsigned char)*p))
      p++;
   if ((size_t)(end - p) < key_len + 1 || strncmp(p, key, key_len) != 0 || p[key_len] != ':')
      return 0;
   p += key_len + 1;
   while (p < end && isspace((unsigned char)*p))
      p++;
   while (end > p && isspace((unsigned char)end[-1]))
      end--;
   *value = p;
   *value_len = (size_t)(end - p);
   return 1;
}

int skill_trigger_matches_content(const char *content, const char *tool_name, const char *subject)
{
   if (!content || !tool_name || !tool_name[0])
      return 0;
   if (!subject)
      subject = "";
   if (strncmp(content, "---", 3) != 0 || (content[3] != '\n' && content[3] != '\r'))
      return 0;

   const char *header_start = strchr(content, '\n');
   const char *header_end = header_start ? strstr(header_start + 1, "\n---") : NULL;
   if (!header_start || !header_end)
      return 0;

   int in_triggers = 0;
   int saw_triggers = 0;
   int saw_tool = 0, tool_match = 0;
   int saw_pattern = 0, pattern_match = 0;
   const char *p = header_start + 1;
   while (p < header_end)
   {
      const char *line_end = memchr(p, '\n', (size_t)(header_end - p));
      if (!line_end)
         line_end = header_end;
      size_t line_len = (size_t)(line_end - p);
      const char *value = NULL;
      size_t value_len = 0;

      if (line_len == 0)
      {
         p = line_end < header_end ? line_end + 1 : header_end;
         continue;
      }

      if (!isspace((unsigned char)p[0]))
      {
         if (strncmp(p, "triggers:", 9) == 0)
         {
            in_triggers = 1;
            saw_triggers = 1;
         }
         else if (in_triggers)
            break;
      }
      else if (in_triggers)
      {
         if (skill_trigger_line_value(p, line_len, "tool", &value, &value_len))
         {
            saw_tool = 1;
            if (skill_trigger_value_contains_token(value, value_len, tool_name))
               tool_match = 1;
         }
         else if (skill_trigger_line_value(p, line_len, "arg_pattern", &value, &value_len) ||
                  skill_trigger_line_value(p, line_len, "path_pattern", &value, &value_len))
         {
            saw_pattern = 1;
            if (skill_trigger_patterns_match(value, value_len, subject))
               pattern_match = 1;
         }
      }
      p = line_end < header_end ? line_end + 1 : header_end;
   }

   return saw_triggers && (!saw_tool || tool_match) && (!saw_pattern || pattern_match);
}

int skill_trigger_matches(const char *project_root, const char *name, const char *tool_name,
                          const char *subject)
{
   char *content = skill_load(project_root, name);
   if (!content)
      return 0;
   int rc = skill_trigger_matches_content(content, tool_name, subject);
   free(content);
   return rc;
}

static int skill_lint_has_at_link(const char *body)
{
   for (const char *p = body; p && *p; p++)
      if (*p == '@' && (isalnum((unsigned char)p[1]) || p[1] == '.' || p[1] == '/' || p[1] == '_' ||
                        p[1] == '-'))
         return 1;
   return 0;
}

static int skill_lint_word_count(const char *s)
{
   int words = 0;
   int in_word = 0;
   for (const char *p = s; p && *p; p++)
   {
      if (isspace((unsigned char)*p))
         in_word = 0;
      else if (!in_word)
      {
         in_word = 1;
         words++;
      }
   }
   return words;
}

static int skill_lint_description_is_trigger(const char *description)
{
   return description && (strncmp(description, "Use when ", 9) == 0 ||
                          strncmp(description, "Use before ", 11) == 0);
}

static int skill_lint_has_injection_pattern(const char *body)
{
   static const char *const patterns[] = {
       "ignore previous instructions",
       "ignore all previous",
       "disregard all previous",
       "you are now",
       "your new instructions",
       "forget all instructions",
       "override your",
       "system: ",
       "[system]",
       "<system>",
       NULL,
   };
   if (!body)
      return 0;
   for (int i = 0; patterns[i]; i++)
   {
      const char *pat = patterns[i];
      size_t plen = strlen(pat);
      for (const char *p = body; *p; p++)
      {
         if (strncasecmp(p, pat, plen) == 0)
            return 1;
      }
   }
   return 0;
}

int skill_lint_content(const char *name, const char *content, char *report, size_t report_len)
{
   if (report && report_len > 0)
      report[0] = '\0';
   int issues = 0;
   const char *label = name ? name : "";
   if (!skill_name_is_valid(name))
      skill_lint_issue(report, report_len, &issues, "%s: malformed skill name", label);
   else if (skill_name_is_session_artifact(name))
      skill_lint_issue(report, report_len, &issues,
                       "%s: skill name must be class-level, not a session artifact", label);
   if (!content || !content[0])
   {
      skill_lint_issue(report, report_len, &issues, "%s: empty skill body", label);
      return issues;
   }

   const char *body = content;
   if (strncmp(content, "---", 3) != 0 || (content[3] != '\n' && content[3] != '\r'))
      skill_lint_issue(report, report_len, &issues, "%s: missing frontmatter", label);
   else
   {
      const char *header_start = strchr(content, '\n');
      const char *header_end = header_start ? strstr(header_start + 1, "\n---") : NULL;
      if (!header_start || !header_end)
         skill_lint_issue(report, report_len, &issues, "%s: unterminated frontmatter", label);
      else
      {
         char fm_name[SKILL_NAME_MAX] = "";
         char description[512] = "";
         size_t header_len = (size_t)(header_end - header_start - 1);
         skill_lint_line_value(header_start + 1, header_len, "name", fm_name, sizeof(fm_name));
         skill_lint_line_value(header_start + 1, header_len, "description", description,
                               sizeof(description));
         if (!fm_name[0])
            skill_lint_issue(report, report_len, &issues, "%s: frontmatter missing name", label);
         else if (name && strcmp(name, fm_name) != 0)
            skill_lint_issue(report, report_len, &issues, "%s: frontmatter name is %s", name,
                             fm_name);
         if (!description[0])
            skill_lint_issue(report, report_len, &issues, "%s: frontmatter missing description",
                             label);
         else if (!skill_lint_description_is_trigger(description))
            skill_lint_issue(report, report_len, &issues,
                             "%s: description must start with 'Use when' or 'Use before'", label);
         body = strchr(header_end + 1, '\n');
         body = body ? body + 1 : "";
      }
   }

   if (skill_lint_has_at_link(body))
      skill_lint_issue(report, report_len, &issues, "%s: body must not use @file links", label);
   if (skill_lint_has_injection_pattern(body))
      skill_lint_issue(report, report_len, &issues,
                       "%s: body contains a known prompt-injection pattern", label);
   int words = skill_lint_word_count(body);
   if (words > SKILL_LINT_MAX_WORDS)
      skill_lint_issue(report, report_len, &issues, "%s: body exceeds %d word budget (%d)", label,
                       SKILL_LINT_MAX_WORDS, words);
   return issues;
}

int skill_lint(const char *project_root, const char *name, char *report, size_t report_len)
{
   char *content = skill_load(project_root, name);
   if (!content)
   {
      if (report && report_len > 0)
         snprintf(report, report_len, "%s: skill not found\n", name ? name : "");
      return 1;
   }
   int issues = skill_lint_content(name, content, report, report_len);
   free(content);
   return issues;
}

static void skill_set_err(char *errbuf, size_t errbuf_len, const char *msg);
static char *skill_read_file(const char *path);

/* --- Compliance eval fixtures --- */

static int skill_eval_dir(const char *project_root, const char *name, char *buf, size_t bufsz)
{
   char skill_file[SKILL_PATH_MAX];
   if (!buf || bufsz == 0 ||
       skill_resolve_path(project_root, name, skill_file, sizeof(skill_file), NULL) != 0)
      return -1;

   char *slash = strrchr(skill_file, '/');
   if (!slash)
      return -1;
   *slash = '\0';

   if (strcmp(slash + 1, "SKILL.md") == 0)
      snprintf(buf, bufsz, "%s/eval", skill_file);
   else
      snprintf(buf, bufsz, "%s/%s/eval", skill_file, name);
   return 0;
}

static int skill_eval_check(const char *type, const char *value, const char *response)
{
   if (!type || !type[0] || strcmp(type, "contains") == 0)
      return value && response && strstr(response, value) != NULL;
   return 0;
}

static int skill_eval_json_string(cJSON *obj, const char *key, const char **out)
{
   cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
   if (!cJSON_IsString(v) || !v->valuestring || !v->valuestring[0])
      return -1;
   *out = v->valuestring;
   return 0;
}

static int skill_eval_json_check(cJSON *obj, const char *key, const char **type, const char **value)
{
   cJSON *check = cJSON_GetObjectItemCaseSensitive(obj, key);
   if (!cJSON_IsObject(check))
      return -1;
   cJSON *jt = cJSON_GetObjectItemCaseSensitive(check, "type");
   cJSON *jv = cJSON_GetObjectItemCaseSensitive(check, "value");
   *type = cJSON_IsString(jt) && jt->valuestring ? jt->valuestring : "contains";
   if (!cJSON_IsString(jv) || !jv->valuestring || !jv->valuestring[0])
      return -1;
   *value = jv->valuestring;
   return 0;
}

static void skill_eval_first_failure(skill_eval_result_t *out, const char *scenario,
                                     const char *message)
{
   if (out && !out->first_failure[0])
      snprintf(out->first_failure, sizeof(out->first_failure), "%s: %s",
               scenario && scenario[0] ? scenario : "scenario", message ? message : "failed");
}

static int skill_eval_one_file(const char *path, skill_eval_result_t *out)
{
   char *text = skill_read_file(path);
   if (!text)
      return -1;
   cJSON *root = cJSON_Parse(text);
   free(text);
   if (!cJSON_IsObject(root))
   {
      cJSON_Delete(root);
      return -1;
   }

   const char *scenario = path;
   (void)skill_eval_json_string(root, "name", &scenario);
   const char *baseline = NULL, *treatment = NULL;
   const char *violation_type = NULL, *violation_value = NULL;
   const char *compliance_type = NULL, *compliance_value = NULL;
   int ok =
       skill_eval_json_string(root, "baseline_response", &baseline) == 0 &&
       skill_eval_json_string(root, "treatment_response", &treatment) == 0 &&
       skill_eval_json_check(root, "violation_check", &violation_type, &violation_value) == 0 &&
       skill_eval_json_check(root, "compliance_check", &compliance_type, &compliance_value) == 0;
   if (!ok)
   {
      skill_eval_first_failure(out, scenario, "invalid fixture");
      cJSON_Delete(root);
      return -1;
   }

   int baseline_violation = skill_eval_check(violation_type, violation_value, baseline);
   int baseline_compliance = skill_eval_check(compliance_type, compliance_value, baseline);
   int treatment_compliance = skill_eval_check(compliance_type, compliance_value, treatment);
   out->scenarios++;
   if (baseline_violation)
      out->baseline_violations++;
   else
      skill_eval_first_failure(out, scenario, "baseline did not show target violation");
   if (baseline_compliance)
      out->baseline_compliances++;
   if (treatment_compliance)
      out->treatment_compliances++;
   else
      skill_eval_first_failure(out, scenario, "treatment did not show compliance");

   cJSON_Delete(root);
   return 0;
}

int skill_eval_run(const char *project_root, const char *name, skill_eval_result_t *out,
                   char *errbuf, size_t errbuf_len)
{
   if (out)
      memset(out, 0, sizeof(*out));
   if (!project_root || !project_root[0] || !name || !name[0] || !out)
      return skill_set_err(errbuf, errbuf_len, "project root and skill name are required"), -1;

   char eval_dir[SKILL_PATH_MAX];
   if (skill_eval_dir(project_root, name, eval_dir, sizeof(eval_dir)) != 0)
      return skill_set_err(errbuf, errbuf_len, "skill not found"), -1;

   DIR *d = opendir(eval_dir);
   if (!d)
      return skill_set_err(errbuf, errbuf_len, "skill eval fixtures not found"), -1;

   int errors = 0;
   struct dirent *ent;
   while ((ent = readdir(d)) != NULL && out->scenarios < SKILL_EVAL_MAX_CASES)
   {
      size_t len = strlen(ent->d_name);
      if (len <= 5 || strcmp(ent->d_name + len - 5, ".json") != 0)
         continue;
      char path[SKILL_PATH_MAX];
      snprintf(path, sizeof(path), "%s/%s", eval_dir, ent->d_name);
      if (skill_eval_one_file(path, out) != 0)
         errors++;
   }
   closedir(d);

   if (out->scenarios == 0)
      return skill_set_err(errbuf, errbuf_len, "skill eval fixtures not found"), -1;
   if (errors > 0)
      return skill_set_err(errbuf, errbuf_len, "one or more skill eval fixtures are invalid"), -1;

   double baseline_rate = (double)out->baseline_compliances / (double)out->scenarios;
   double treatment_rate = (double)out->treatment_compliances / (double)out->scenarios;
   out->compliance_delta = treatment_rate - baseline_rate;
   out->passed = out->baseline_violations == out->scenarios &&
                 out->treatment_compliances == out->scenarios && out->compliance_delta > 0.0;
   if (!out->passed && !out->first_failure[0])
      snprintf(out->first_failure, sizeof(out->first_failure),
               "treatment did not improve compliance over baseline");
   return 0;
}

int skill_change_eval_gate_allows(const char *payload_json, double threshold, char *reason,
                                  size_t reason_len)
{
   if (reason && reason_len > 0)
      reason[0] = '\0';
   cJSON *root = cJSON_Parse(payload_json ? payload_json : "");
   if (!cJSON_IsObject(root))
   {
      cJSON_Delete(root);
      return skill_set_err(reason, reason_len, "skill_change payload is not valid JSON"), 0;
   }

   cJSON *eval = cJSON_GetObjectItemCaseSensitive(root, "eval_result");
   if (!cJSON_IsObject(eval))
   {
      cJSON_Delete(root);
      return skill_set_err(reason, reason_len, "skill_change missing eval_result"), 0;
   }

   cJSON *passed_j = cJSON_GetObjectItemCaseSensitive(eval, "passed");
   cJSON *status_j = cJSON_GetObjectItemCaseSensitive(eval, "status");
   int passed = cJSON_IsTrue(passed_j) ||
                (cJSON_IsString(status_j) && strcmp(status_j->valuestring, "pass") == 0);
   cJSON *delta_j = cJSON_GetObjectItemCaseSensitive(eval, "compliance_delta");
   double delta = cJSON_IsNumber(delta_j) ? delta_j->valuedouble : 0.0;
   if (!passed)
   {
      cJSON *msg = cJSON_GetObjectItemCaseSensitive(eval, "message");
      skill_set_err(reason, reason_len,
                    cJSON_IsString(msg) && msg->valuestring && msg->valuestring[0]
                        ? msg->valuestring
                        : "skill eval did not pass");
      cJSON_Delete(root);
      return 0;
   }
   if (delta < threshold)
   {
      if (reason && reason_len > 0)
         snprintf(reason, reason_len, "skill eval delta %.3f is below threshold %.3f", delta,
                  threshold);
      cJSON_Delete(root);
      return 0;
   }

   cJSON_Delete(root);
   return 1;
}

static int skill_usage_path_source(const char *project_root, const char *name, char *usage_path,
                                   size_t usage_path_len, skill_source_t *source_out)
{
   if (!usage_path || usage_path_len == 0)
      return -1;
   usage_path[0] = '\0';

   char skill_file[SKILL_PATH_MAX];
   skill_source_t source = SKILL_SOURCE_NONE;
   if (skill_resolve_path(project_root, name, skill_file, sizeof(skill_file), &source) != 0)
      return -1;
   if (source_out)
      *source_out = source;

   char *slash = strrchr(skill_file, '/');
   if (!slash)
      return -1;
   *slash = '\0';
   snprintf(usage_path, usage_path_len, "%s/.usage.json", skill_file);
   return 0;
}

static int skill_project_path(const char *project_root, const char *name, char *path,
                              size_t path_len)
{
   if (!project_root || !project_root[0] || !skill_name_is_valid(name) || !path || path_len == 0)
      return -1;
   snprintf(path, path_len, "%s/.aimee/skills/%s.md", project_root, name);
   return 0;
}

static int skill_mkdir_p(const char *path)
{
   char tmp[SKILL_PATH_MAX];
   snprintf(tmp, sizeof(tmp), "%s", path ? path : "");
   for (char *p = tmp + 1; *p; p++)
   {
      if (*p == '/')
      {
         *p = '\0';
         if (mkdir(tmp, 0700) != 0)
         {
            if (errno != EEXIST)
               return -1;
            struct stat st;
            if (stat(tmp, &st) != 0 || !S_ISDIR(st.st_mode))
               return -1;
         }
         *p = '/';
      }
   }
   if (mkdir(tmp, 0700) == 0)
      return 0;
   if (errno == EEXIST)
   {
      struct stat st;
      return stat(tmp, &st) == 0 && S_ISDIR(st.st_mode) ? 0 : -1;
   }
   return -1;
}

static int skill_write_text_file(const char *path, const char *content, size_t max_size)
{
   size_t len = strlen(content ? content : "");
   if (len == 0 || len > max_size)
      return -1;
   char tmp[SKILL_PATH_MAX + 64];
   snprintf(tmp, sizeof(tmp), "%s.tmp.%ld", path, (long)getpid());
   FILE *f = fopen(tmp, "w");
   if (!f)
      return -1;
   int rc = fputs(content, f) < 0 ? -1 : 0;
   if (fclose(f) != 0)
      rc = -1;
   if (rc != 0 || rename(tmp, path) != 0)
   {
      unlink(tmp);
      return -1;
   }
   return 0;
}

static char *skill_read_file(const char *path)
{
   FILE *f = fopen(path, "r");
   if (!f)
      return NULL;

   if (fseek(f, 0, SEEK_END) != 0)
   {
      fclose(f);
      return NULL;
   }
   long len = ftell(f);
   if (len < 0 || len > MAX_FILE_SIZE)
   {
      fclose(f);
      return NULL;
   }
   rewind(f);

   char *buf = malloc((size_t)len + 1);
   if (!buf)
   {
      fclose(f);
      return NULL;
   }
   size_t n = fread(buf, 1, (size_t)len, f);
   buf[n] = '\0';
   fclose(f);
   return buf;
}

static void skill_capability_slug(const char *tool_name, char *out, size_t out_len)
{
   if (!out || out_len == 0)
      return;
   out[0] = '\0';
   if (!tool_name || !tool_name[0])
      return;

   size_t pos = 0;
   if (!isalnum((unsigned char)tool_name[0]))
   {
      snprintf(out, out_len, "tool-");
      pos = strlen(out);
   }

   for (const char *p = tool_name; *p && pos + 1 < out_len; p++)
   {
      unsigned char ch = (unsigned char)*p;
      if (islower(ch) || isdigit(ch))
         out[pos++] = (char)ch;
      else if (isupper(ch))
         out[pos++] = (char)tolower(ch);
      else if (ch == '_' || ch == '-' || ch == '.')
         out[pos++] = '-';
      else if (pos > 0 && out[pos - 1] != '-')
         out[pos++] = '-';
   }
   while (pos > 0 && out[pos - 1] == '-')
      pos--;
   out[pos] = '\0';
}

static int skill_capability_tool_is_covered(const char *project_root, const char *tool_name,
                                            const char *slug)
{
   char path[SKILL_PATH_MAX];
   if (skill_path(project_root, tool_name, path, sizeof(path)) == 0 ||
       (slug && slug[0] && skill_path(project_root, slug, path, sizeof(path)) == 0))
      return 1;

   char names[SKILL_MAX_SKILLS][SKILL_NAME_MAX];
   int n = skill_list(project_root, names, SKILL_MAX_SKILLS);
   for (int i = 0; i < n; i++)
   {
      char *content = skill_load(project_root, names[i]);
      if (!content)
         continue;
      int covered = strstr(content, tool_name) != NULL;
      free(content);
      if (covered)
         return 1;
   }
   return 0;
}

static char *skill_capability_stub_content(const char *skill_name, const char *tool_name,
                                           const char *prompt)
{
   cJSON *root = cJSON_CreateObject();
   if (!root)
      return NULL;
   cJSON_AddStringToObject(root, "kind", "skill_change");
   cJSON_AddStringToObject(root, "state", "proposed");
   cJSON_AddStringToObject(root, "target_surface", "skill");
   cJSON_AddStringToObject(root, "skill_name", skill_name);
   cJSON_AddStringToObject(root, "source_tool", tool_name);
   cJSON_AddStringToObject(root, "route_tool", tool_name);
   cJSON_AddStringToObject(root, "review_gate", "skill lint + skill eval before apply");
   cJSON_AddStringToObject(root, "source", "capability-autostub");
   cJSON *eval = cJSON_AddObjectToObject(root, "eval_result");
   if (eval)
   {
      cJSON_AddStringToObject(eval, "status", "missing");
      cJSON_AddBoolToObject(eval, "passed", 0);
      cJSON_AddNumberToObject(eval, "scenarios", 0);
      cJSON_AddNumberToObject(eval, "baseline_violations", 0);
      cJSON_AddNumberToObject(eval, "treatment_compliances", 0);
      cJSON_AddNumberToObject(eval, "compliance_delta", 0.0);
      cJSON_AddStringToObject(eval, "message", "skill eval fixtures have not run");
   }
   if (prompt && prompt[0])
      cJSON_AddStringToObject(root, "tool_prompt", prompt);

   char body[4096];
   snprintf(body, sizeof(body),
            "---\n"
            "name: %s\n"
            "description: Use when %s is the direct Aimee capability for the task.\n"
            "triggers:\n"
            "  tool: [%s]\n"
            "---\n"
            "\n"
            "Route this task to `%s` instead of rebuilding the capability with generic tools.\n"
            "\n"
            "1. Check whether `%s` directly answers the request.\n"
            "2. Prefer the tool result over broad file reads or ad hoc reconstruction.\n"
            "3. If the tool is unavailable or insufficient, fall back to the narrowest generic "
            "workflow and note why.\n",
            skill_name, tool_name, tool_name, tool_name, tool_name);
   cJSON_AddStringToObject(root, "proposed_content", body);

   char *out = cJSON_PrintUnformatted(root);
   cJSON_Delete(root);
   return out;
}

int skill_capability_autostub_from_json(const char *project_root, const char *snapshot_json,
                                        skill_capability_autostub_result_t *out, char *errbuf,
                                        size_t errbuf_len)
{
   if (out)
      memset(out, 0, sizeof(*out));
   if (!project_root || !project_root[0])
      return skill_set_err(errbuf, errbuf_len, "project root is required"), -1;
   if (!snapshot_json || !snapshot_json[0])
      return skill_set_err(errbuf, errbuf_len, "tool registry snapshot is required"), -1;

   cJSON *root = cJSON_Parse(snapshot_json);
   if (!cJSON_IsObject(root))
   {
      cJSON_Delete(root);
      return skill_set_err(errbuf, errbuf_len, "invalid tool registry snapshot"), -1;
   }
   cJSON *status = cJSON_GetObjectItemCaseSensitive(root, "status");
   if (status && cJSON_IsString(status) && strcmp(status->valuestring, "ok") != 0)
   {
      cJSON_Delete(root);
      return skill_set_err(errbuf, errbuf_len, "tool registry snapshot is not ok"), -1;
   }
   cJSON *prompts = cJSON_GetObjectItemCaseSensitive(root, "prompts");
   if (!cJSON_IsArray(prompts))
   {
      cJSON_Delete(root);
      return skill_set_err(errbuf, errbuf_len, "tool registry snapshot missing prompts"), -1;
   }

   char change_dir[SKILL_PATH_MAX];
   snprintf(change_dir, sizeof(change_dir), "%s/.aimee/skills/.changes", project_root);
   if (skill_mkdir_p(change_dir) != 0)
   {
      cJSON_Delete(root);
      return skill_set_err(errbuf, errbuf_len, "failed to create skill change directory"), -1;
   }

   cJSON *item = NULL;
   cJSON_ArrayForEach(item, prompts)
   {
      cJSON *name_j = cJSON_GetObjectItemCaseSensitive(item, "name");
      cJSON *prompt_j = cJSON_GetObjectItemCaseSensitive(item, "prompt");
      if (out)
         out->scanned++;
      if (!cJSON_IsString(name_j) || !name_j->valuestring || !name_j->valuestring[0])
      {
         if (out)
            out->skipped++;
         continue;
      }

      char skill_name[SKILL_NAME_MAX];
      skill_capability_slug(name_j->valuestring, skill_name, sizeof(skill_name));
      if (!skill_name_is_valid(skill_name))
      {
         if (out)
            out->skipped++;
         continue;
      }
      if (skill_capability_tool_is_covered(project_root, name_j->valuestring, skill_name))
      {
         if (out)
            out->existing++;
         continue;
      }

      char change_path[SKILL_PATH_MAX];
      snprintf(change_path, sizeof(change_path), "%s/%s.skill_change.json", change_dir, skill_name);
      if (access(change_path, F_OK) == 0)
      {
         if (out)
            out->skipped++;
         continue;
      }

      char *proposal = skill_capability_stub_content(
          skill_name, name_j->valuestring,
          cJSON_IsString(prompt_j) && prompt_j->valuestring ? prompt_j->valuestring : "");
      if (!proposal || skill_write_text_file(change_path, proposal, SKILL_SUPPORT_MAX_SIZE) != 0)
      {
         free(proposal);
         if (out)
            out->errors++;
         continue;
      }
      free(proposal);
      if (out)
      {
         out->proposed++;
         if (!out->first_proposal[0])
         {
            snprintf(out->first_proposal, sizeof(out->first_proposal), "%s", skill_name);
            snprintf(out->first_change_path, sizeof(out->first_change_path), "%s", change_path);
         }
      }
   }

   cJSON_Delete(root);
   if (out && out->errors > 0)
      return skill_set_err(errbuf, errbuf_len, "one or more skill changes failed"), -1;
   return 0;
}

static cJSON *skill_usage_load_root(const char *path)
{
   char *text = skill_read_file(path);
   if (!text)
      return cJSON_CreateObject();

   cJSON *root = cJSON_Parse(text);
   free(text);
   if (!cJSON_IsObject(root))
   {
      cJSON_Delete(root);
      return cJSON_CreateObject();
   }
   return root;
}

static int skill_usage_write_root(const char *path, cJSON *root)
{
   char *rendered = cJSON_Print(root);
   if (!rendered)
      return -1;

   char tmp[SKILL_PATH_MAX + 64];
   snprintf(tmp, sizeof(tmp), "%s.tmp.%ld", path, (long)getpid());
   FILE *f = fopen(tmp, "w");
   if (!f)
   {
      free(rendered);
      return -1;
   }

   int rc = 0;
   if (fputs(rendered, f) < 0 || fputc('\n', f) == EOF)
      rc = -1;
   if (fclose(f) != 0)
      rc = -1;
   free(rendered);

   if (rc != 0 || rename(tmp, path) != 0)
   {
      unlink(tmp);
      return -1;
   }
   return 0;
}

static int skill_usage_lock(const char *path)
{
#ifdef _WIN32
   (void)path;
   return -2;
#else
   char lock_path[SKILL_PATH_MAX + 16];
   snprintf(lock_path, sizeof(lock_path), "%s.lock", path);
   int fd = open(lock_path, O_CREAT | O_RDWR, 0600);
   if (fd < 0)
      return -1;
   if (flock(fd, LOCK_EX) != 0)
   {
      close(fd);
      return -1;
   }
   return fd;
#endif
}

static void skill_usage_unlock(int fd)
{
#ifndef _WIN32
   if (fd >= 0)
   {
      (void)flock(fd, LOCK_UN);
      close(fd);
   }
#else
   (void)fd;
#endif
}

static int skill_json_int(cJSON *obj, const char *key)
{
   cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
   return cJSON_IsNumber(v) ? (int)v->valuedouble : 0;
}

static void skill_json_copy(cJSON *obj, const char *key, char *out, size_t out_len)
{
   if (!out || out_len == 0)
      return;
   cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
   if (cJSON_IsString(v) && v->valuestring)
      snprintf(out, out_len, "%s", v->valuestring);
}

int skill_usage_get(const char *project_root, const char *name, skill_usage_t *out)
{
   if (!name || !name[0] || !out)
      return -1;
   skill_usage_default(out);

   char path[SKILL_PATH_MAX];
   skill_source_t source = SKILL_SOURCE_NONE;
   if (skill_usage_path_source(project_root, name, path, sizeof(path), &source) != 0)
      return -1;
   if (source == SKILL_SOURCE_BUNDLED)
   {
      out->pinned = 1;
      snprintf(out->created_by, sizeof(out->created_by), "bundled");
   }

   char *text = skill_read_file(path);
   if (!text)
      return 0;

   cJSON *root = cJSON_Parse(text);
   free(text);
   if (!cJSON_IsObject(root))
   {
      cJSON_Delete(root);
      return 0;
   }

   cJSON *obj = cJSON_GetObjectItemCaseSensitive(root, name);
   if (cJSON_IsObject(obj))
   {
      out->use_count = skill_json_int(obj, "use_count");
      out->view_count = skill_json_int(obj, "view_count");
      out->patch_count = skill_json_int(obj, "patch_count");
      cJSON *pinned = cJSON_GetObjectItemCaseSensitive(obj, "pinned");
      out->pinned = cJSON_IsTrue(pinned) ? 1 : 0;
      skill_json_copy(obj, "created_at", out->created_at, sizeof(out->created_at));
      skill_json_copy(obj, "last_used_at", out->last_used_at, sizeof(out->last_used_at));
      skill_json_copy(obj, "last_patched_at", out->last_patched_at, sizeof(out->last_patched_at));
      skill_json_copy(obj, "state", out->state, sizeof(out->state));
      skill_json_copy(obj, "created_by", out->created_by, sizeof(out->created_by));
      if (!out->state[0])
         snprintf(out->state, sizeof(out->state), "active");
      if (!out->created_by[0])
         snprintf(out->created_by, sizeof(out->created_by), "user");
   }

   cJSON_Delete(root);
   return 0;
}

static void skill_json_set_string(cJSON *obj, const char *key, const char *value)
{
   cJSON *existing = cJSON_GetObjectItemCaseSensitive(obj, key);
   if (cJSON_IsString(existing))
   {
      cJSON_SetValuestring(existing, value ? value : "");
      return;
   }
   if (existing)
      cJSON_ReplaceItemInObject(obj, key, cJSON_CreateString(value ? value : ""));
   else
      cJSON_AddStringToObject(obj, key, value ? value : "");
}

static void skill_json_set_number(cJSON *obj, const char *key, int value)
{
   cJSON *existing = cJSON_GetObjectItemCaseSensitive(obj, key);
   if (cJSON_IsNumber(existing))
   {
      cJSON_SetNumberValue(existing, value);
      return;
   }
   if (existing)
      cJSON_ReplaceItemInObject(obj, key, cJSON_CreateNumber(value));
   else
      cJSON_AddNumberToObject(obj, key, value);
}

static int skill_record_mutation(const char *project_root, const char *name, const char *actor,
                                 int bump_patch, const char *state, int set_pinned, int pinned,
                                 const char *absorbed_into)
{
   char path[SKILL_PATH_MAX];
   skill_source_t source = SKILL_SOURCE_NONE;
   if (skill_usage_path_source(project_root, name, path, sizeof(path), &source) != 0)
      return -1;
   if (source == SKILL_SOURCE_BUNDLED)
      return -1;
   int lock_fd = skill_usage_lock(path);
   if (lock_fd == -1)
      return -1;
   cJSON *root = skill_usage_load_root(path);
   if (!root)
   {
      skill_usage_unlock(lock_fd);
      return -1;
   }
   cJSON *obj = cJSON_GetObjectItemCaseSensitive(root, name);
   if (!cJSON_IsObject(obj))
   {
      obj = cJSON_CreateObject();
      cJSON_DeleteItemFromObject(root, name);
      cJSON_AddItemToObject(root, name, obj);
      char ts[32];
      now_utc(ts, sizeof(ts));
      cJSON_AddStringToObject(obj, "created_at", ts);
      cJSON_AddStringToObject(obj, "created_by", actor && actor[0] ? actor : "user");
      cJSON_AddStringToObject(obj, "state", "active");
      cJSON_AddBoolToObject(obj, "pinned", 0);
      cJSON_AddNumberToObject(obj, "use_count", 0);
      cJSON_AddNumberToObject(obj, "view_count", 0);
      cJSON_AddNumberToObject(obj, "patch_count", 0);
   }
   char ts[32];
   now_utc(ts, sizeof(ts));
   if (bump_patch)
      skill_json_set_number(obj, "patch_count", skill_json_int(obj, "patch_count") + 1);
   if (bump_patch || state)
      skill_json_set_string(obj, "last_patched_at", ts);
   if (state)
      skill_json_set_string(obj, "state", state);
   if (actor && actor[0] && !cJSON_GetObjectItemCaseSensitive(obj, "created_by"))
      skill_json_set_string(obj, "created_by", actor);
   if (set_pinned)
   {
      cJSON *existing = cJSON_GetObjectItemCaseSensitive(obj, "pinned");
      if (existing)
         cJSON_ReplaceItemInObject(obj, "pinned", cJSON_CreateBool(pinned ? 1 : 0));
      else
         cJSON_AddBoolToObject(obj, "pinned", pinned ? 1 : 0);
   }
   if (absorbed_into && absorbed_into[0])
      skill_json_set_string(obj, "absorbed_into", absorbed_into);
   int rc = skill_usage_write_root(path, root);
   cJSON_Delete(root);
   skill_usage_unlock(lock_fd);
   return rc;
}

static int skill_record_event(const char *project_root, const char *name, int activation, int view)
{
   if (!name || !name[0])
      return -1;

   char path[SKILL_PATH_MAX];
   skill_source_t source = SKILL_SOURCE_NONE;
   if (skill_usage_path_source(project_root, name, path, sizeof(path), &source) != 0)
      return -1;
   if (source == SKILL_SOURCE_BUNDLED)
      return 0;

   int lock_fd = skill_usage_lock(path);
   if (lock_fd == -1)
      return -1;

   cJSON *root = skill_usage_load_root(path);
   if (!root)
   {
      skill_usage_unlock(lock_fd);
      return -1;
   }

   cJSON *obj = cJSON_GetObjectItemCaseSensitive(root, name);
   if (!cJSON_IsObject(obj))
   {
      obj = cJSON_CreateObject();
      cJSON_DeleteItemFromObject(root, name);
      cJSON_AddItemToObject(root, name, obj);
      char ts[32];
      now_utc(ts, sizeof(ts));
      cJSON_AddStringToObject(obj, "created_at", ts);
      cJSON_AddStringToObject(obj, "created_by", "user");
      cJSON_AddStringToObject(obj, "state", "active");
      cJSON_AddBoolToObject(obj, "pinned", 0);
      cJSON_AddNumberToObject(obj, "use_count", 0);
      cJSON_AddNumberToObject(obj, "view_count", 0);
      cJSON_AddNumberToObject(obj, "patch_count", 0);
   }

   if (activation)
   {
      skill_json_set_number(obj, "use_count", skill_json_int(obj, "use_count") + 1);
      char ts[32];
      now_utc(ts, sizeof(ts));
      skill_json_set_string(obj, "last_used_at", ts);
      skill_json_set_string(obj, "state", "active");
   }
   if (view)
      skill_json_set_number(obj, "view_count", skill_json_int(obj, "view_count") + 1);

   int rc = skill_usage_write_root(path, root);
   cJSON_Delete(root);
   skill_usage_unlock(lock_fd);
   return rc;
}

int skill_record_activation(const char *project_root, const char *name)
{
   return skill_record_event(project_root, name, 1, 0);
}

int skill_record_view(const char *project_root, const char *name)
{
   return skill_record_event(project_root, name, 0, 1);
}

int skill_set_pinned(const char *project_root, const char *name, int pinned)
{
   return skill_record_mutation(project_root, name, NULL, 0, NULL, 1, pinned, NULL);
}

static void skill_set_err(char *errbuf, size_t errbuf_len, const char *msg)
{
   if (errbuf && errbuf_len > 0)
      snprintf(errbuf, errbuf_len, "%s", msg ? msg : "skill mutation failed");
}

/* Scan skill body for prompt-injection patterns before accepting it.
 * Returns 0 if clean, -1 and fills errbuf if a pattern is found. */
static int skill_body_poison_check(const char *body, char *errbuf, size_t errbuf_len)
{
   if (!body || !body[0])
      return 0;

   /* Case-insensitive search helper for a substring. */
#define BODY_ICASE(needle) (strcasestr(body, needle) != NULL)

   if (BODY_ICASE("ignore previous instructions") || BODY_ICASE("ignore all instructions"))
      return skill_set_err(errbuf, errbuf_len,
                           "skill body rejected: prompt injection pattern detected "
                           "(ignore instructions)"),
             -1;

   /* "disregard" followed by "instruction" within 60 chars */
   const char *dis = strcasestr(body, "disregard");
   if (dis)
   {
      const char *instr = strcasestr(dis, "instruction");
      if (instr && (instr - dis) < 60)
         return skill_set_err(errbuf, errbuf_len,
                              "skill body rejected: prompt injection pattern detected "
                              "(disregard instructions)"),
                -1;
   }

   /* Line-level checks */
   char line[256];
   const char *p = body;
   while (*p)
   {
      /* Copy one line */
      int i = 0;
      while (*p && *p != '\n' && i < (int)sizeof(line) - 1)
         line[i++] = *p++;
      if (*p == '\n')
         p++;
      line[i] = '\0';

      /* Skip leading whitespace */
      const char *lp = line;
      while (*lp == ' ' || *lp == '\t')
         lp++;

      if (strncasecmp(lp, "SYSTEM:", 7) == 0 || strncmp(lp, "<|system|>", 10) == 0 ||
          strncmp(lp, "<system>", 8) == 0)
         return skill_set_err(errbuf, errbuf_len,
                              "skill body rejected: prompt injection pattern detected "
                              "(SYSTEM block)"),
                -1;
   }
#undef BODY_ICASE
   return 0;
}

int skill_manage_create(const char *project_root, const char *name, const char *content,
                        const char *created_by, char *errbuf, size_t errbuf_len)
{
   char path[SKILL_PATH_MAX], dir[SKILL_PATH_MAX];
   if (skill_project_path(project_root, name, path, sizeof(path)) != 0)
      return skill_set_err(errbuf, errbuf_len, "invalid skill name or project root"), -1;
   if (skill_body_poison_check(content, errbuf, errbuf_len) != 0)
      return -1;
   char lint_report[512];
   if (skill_lint_content(name, content, lint_report, sizeof(lint_report)) != 0)
      return skill_set_err(errbuf, errbuf_len, lint_report[0] ? lint_report : "skill lint failed"),
             -1;
   if (access(path, F_OK) == 0)
      return skill_set_err(errbuf, errbuf_len, "skill already exists"), -1;
   snprintf(dir, sizeof(dir), "%s/.aimee/skills", project_root);
   if (skill_mkdir_p(dir) != 0 || skill_write_text_file(path, content, SKILL_MAX_SIZE) != 0)
      return skill_set_err(errbuf, errbuf_len, "failed to write skill"), -1;
   (void)skill_record_mutation(project_root, name, created_by, 0, "active", 0, 0, NULL);
   return 0;
}

int skill_import_content(const char *project_root, const char *content, const char *created_by,
                         char *name_out, size_t name_out_len, char *errbuf, size_t errbuf_len)
{
   if (name_out && name_out_len > 0)
      name_out[0] = '\0';
   if (!content || !content[0])
      return skill_set_err(errbuf, errbuf_len, "skill content is required"), -1;
   if (strncmp(content, "---", 3) != 0 || (content[3] != '\n' && content[3] != '\r'))
      return skill_set_err(errbuf, errbuf_len, "skill import requires frontmatter"), -1;

   const char *header_start = strchr(content, '\n');
   const char *header_end = header_start ? strstr(header_start + 1, "\n---") : NULL;
   if (!header_start || !header_end)
      return skill_set_err(errbuf, errbuf_len, "skill import has unterminated frontmatter"), -1;

   char name[SKILL_NAME_MAX] = "";
   size_t header_len = (size_t)(header_end - header_start - 1);
   if (!skill_lint_line_value(header_start + 1, header_len, "name", name, sizeof(name)) || !name[0])
      return skill_set_err(errbuf, errbuf_len, "skill import frontmatter missing name"), -1;

   int rc = skill_manage_create(project_root, name, content, created_by, errbuf, errbuf_len);
   if (rc == 0 && name_out && name_out_len > 0)
      snprintf(name_out, name_out_len, "%s", name);
   return rc;
}

int skill_manage_edit(const char *project_root, const char *name, const char *content,
                      const char *updated_by, char *errbuf, size_t errbuf_len)
{
   char path[SKILL_PATH_MAX];
   if (skill_project_path(project_root, name, path, sizeof(path)) != 0 || access(path, F_OK) != 0)
      return skill_set_err(errbuf, errbuf_len, "skill not found"), -1;
   char lint_report[512];
   if (skill_lint_content(name, content, lint_report, sizeof(lint_report)) != 0)
      return skill_set_err(errbuf, errbuf_len, lint_report[0] ? lint_report : "skill lint failed"),
             -1;
   if (skill_write_text_file(path, content, SKILL_MAX_SIZE) != 0)
      return skill_set_err(errbuf, errbuf_len, "failed to write skill"), -1;
   return skill_record_mutation(project_root, name, updated_by, 1, "active", 0, 0, NULL);
}

int skill_manage_patch(const char *project_root, const char *name, const char *old_string,
                       const char *new_string, int replace_all, const char *updated_by,
                       char *errbuf, size_t errbuf_len)
{
   char *content = skill_load(project_root, name);
   if (!content)
      return skill_set_err(errbuf, errbuf_len, "skill not found"), -1;
   if (!old_string || !old_string[0] || !new_string)
   {
      free(content);
      return skill_set_err(errbuf, errbuf_len, "old_string and new_string are required"), -1;
   }
   int hits = 0;
   dstr_t out;
   dstr_init(&out);
   const char *p = content;
   size_t old_len = strlen(old_string);
   while (1)
   {
      const char *hit = strstr(p, old_string);
      if (!hit || (!replace_all && hits > 0))
      {
         dstr_append_str(&out, p);
         break;
      }
      dstr_append(&out, p, (size_t)(hit - p));
      dstr_append_str(&out, new_string);
      p = hit + old_len;
      hits++;
   }
   char *patched = dstr_steal(&out);
   free(content);
   if (hits == 0 || !patched)
   {
      free(patched);
      return skill_set_err(errbuf, errbuf_len, "old_string not found"), -1;
   }
   int rc = skill_manage_edit(project_root, name, patched, updated_by, errbuf, errbuf_len);
   free(patched);
   return rc;
}

int skill_manage_archive(const char *project_root, const char *name, const char *absorbed_into,
                         char *errbuf, size_t errbuf_len)
{
   skill_usage_t usage;
   if (skill_usage_get(project_root, name, &usage) != 0)
      return skill_set_err(errbuf, errbuf_len, "skill not found"), -1;
   if (usage.pinned)
      return skill_set_err(errbuf, errbuf_len, "pinned skills cannot be archived"), -1;
   char path[SKILL_PATH_MAX], archive_dir[SKILL_PATH_MAX], archive_path[SKILL_PATH_MAX];
   if (skill_project_path(project_root, name, path, sizeof(path)) != 0 || access(path, F_OK) != 0)
      return skill_set_err(errbuf, errbuf_len, "skill not found"), -1;
   snprintf(archive_dir, sizeof(archive_dir), "%s/.aimee/skills/.archive", project_root);
   snprintf(archive_path, sizeof(archive_path), "%s/%s.md", archive_dir, name);
   if (skill_mkdir_p(archive_dir) != 0 || access(archive_path, F_OK) == 0 ||
       skill_record_mutation(project_root, name, NULL, 1, "archived", 0, 0, absorbed_into) != 0 ||
       rename(path, archive_path) != 0)
      return skill_set_err(errbuf, errbuf_len, "failed to archive skill"), -1;
   return 0;
}

static int skill_timestamp_age_days(const char *ts, time_t now)
{
   if (!ts || !ts[0])
      return -1;
   struct tm tmv;
   memset(&tmv, 0, sizeof(tmv));
   int year, mon, day, hour, min, sec;
   if (sscanf(ts, "%4d-%2d-%2dT%2d:%2d:%2dZ", &year, &mon, &day, &hour, &min, &sec) != 6)
      return -1;
   tmv.tm_year = year - 1900;
   tmv.tm_mon = mon - 1;
   tmv.tm_mday = day;
   tmv.tm_hour = hour;
   tmv.tm_min = min;
   tmv.tm_sec = sec;
   time_t then = timegm(&tmv);
   if (then == (time_t)-1 || then > now)
      return -1;
   return (int)((now - then) / 86400);
}

static int skill_project_list(const char *project_root, char names[][SKILL_NAME_MAX], int max_names)
{
   if (!project_root || !project_root[0] || !names || max_names <= 0)
      return 0;
   char dir[SKILL_PATH_MAX];
   snprintf(dir, sizeof(dir), "%s/.aimee/skills", project_root);
   DIR *d = opendir(dir);
   if (!d)
      return 0;
   int count = 0;
   struct dirent *ent;
   while ((ent = readdir(d)) != NULL && count < max_names)
   {
      size_t nlen = strlen(ent->d_name);
      if (nlen > 3 && strcmp(ent->d_name + nlen - 3, ".md") == 0)
      {
         size_t rlen = nlen - 3;
         if (rlen < SKILL_NAME_MAX)
         {
            memcpy(names[count], ent->d_name, rlen);
            names[count][rlen] = '\0';
            count++;
         }
      }
   }
   closedir(d);
   return count;
}

int skill_lifecycle_apply(const char *project_root, int stale_after_days, int archive_after_days,
                          skill_lifecycle_result_t *out, char *errbuf, size_t errbuf_len)
{
   if (out)
      memset(out, 0, sizeof(*out));
   if (!project_root || !project_root[0])
      return skill_set_err(errbuf, errbuf_len, "project root is required"), -1;

   char names[SKILL_MAX_SKILLS][SKILL_NAME_MAX];
   int n = skill_project_list(project_root, names, SKILL_MAX_SKILLS);
   time_t now = time(NULL);
   for (int i = 0; i < n; i++)
   {
      if (out)
         out->considered++;
      skill_usage_t usage;
      if (skill_usage_get(project_root, names[i], &usage) != 0)
      {
         if (out)
            out->errors++;
         continue;
      }
      if (usage.pinned)
      {
         if (out)
            out->skipped_pinned++;
         continue;
      }
      const char *activity = usage.last_used_at[0] ? usage.last_used_at : usage.created_at;
      int age_days = skill_timestamp_age_days(activity, now);
      if (age_days < 0)
         continue;
      if (archive_after_days > 0 && age_days >= archive_after_days)
      {
         char err[256] = "";
         if (skill_manage_archive(project_root, names[i], NULL, err, sizeof(err)) == 0)
         {
            if (out)
               out->archived++;
         }
         else if (out)
            out->errors++;
      }
      else if (stale_after_days > 0 && age_days >= stale_after_days &&
               strcmp(usage.state, "stale") != 0)
      {
         if (skill_record_mutation(project_root, names[i], NULL, 0, "stale", 0, 0, NULL) == 0)
         {
            if (out)
               out->stale_marked++;
         }
         else if (out)
            out->errors++;
      }
   }

   if (out && out->errors > 0)
      return skill_set_err(errbuf, errbuf_len, "one or more skill lifecycle updates failed"), -1;
   return 0;
}

static int skill_support_path_ok(const char *file_path)
{
   static const char *dirs[] = {"references/", "templates/", "scripts/", "assets/", NULL};
   if (!file_path || !file_path[0] || file_path[0] == '/' || strstr(file_path, ".."))
      return 0;
   for (int i = 0; dirs[i]; i++)
      if (strncmp(file_path, dirs[i], strlen(dirs[i])) == 0 && strlen(file_path) > strlen(dirs[i]))
         return 1;
   return 0;
}

int skill_manage_write_file(const char *project_root, const char *name, const char *file_path,
                            const char *content, const char *updated_by, char *errbuf,
                            size_t errbuf_len)
{
   char skill_file[SKILL_PATH_MAX];
   if (skill_project_path(project_root, name, skill_file, sizeof(skill_file)) != 0 ||
       access(skill_file, F_OK) != 0)
      return skill_set_err(errbuf, errbuf_len, "skill not found"), -1;
   if (!skill_support_path_ok(file_path))
      return skill_set_err(errbuf, errbuf_len, "invalid support file path"), -1;
   char path[SKILL_PATH_MAX], dir[SKILL_PATH_MAX];
   snprintf(path, sizeof(path), "%s/.aimee/skills/%s", project_root, file_path);
   snprintf(dir, sizeof(dir), "%s", path);
   char *slash = strrchr(dir, '/');
   if (slash)
      *slash = '\0';
   if (skill_mkdir_p(dir) != 0 || skill_write_text_file(path, content, SKILL_SUPPORT_MAX_SIZE) != 0)
      return skill_set_err(errbuf, errbuf_len, "failed to write support file"), -1;
   return skill_record_mutation(project_root, name, updated_by, 1, "active", 0, 0, NULL);
}

char *skill_support_file_load(const char *project_root, const char *name, const char *file_path,
                              char *errbuf, size_t errbuf_len)
{
   char skill_file[SKILL_PATH_MAX];
   if (skill_resolve_path(project_root, name, skill_file, sizeof(skill_file), NULL) != 0)
   {
      skill_set_err(errbuf, errbuf_len, "skill not found");
      return NULL;
   }
   if (!skill_support_path_ok(file_path))
   {
      skill_set_err(errbuf, errbuf_len, "invalid support file path");
      return NULL;
   }
   char path[SKILL_PATH_MAX];
   char *slash = strrchr(skill_file, '/');
   if (!slash)
   {
      skill_set_err(errbuf, errbuf_len, "skill not found");
      return NULL;
   }
   *slash = '\0';
   snprintf(path, sizeof(path), "%s/%s", skill_file, file_path);
   FILE *f = fopen(path, "r");
   if (!f)
   {
      skill_set_err(errbuf, errbuf_len, "support file not found");
      return NULL;
   }
   char *buf = malloc(SKILL_SUPPORT_MAX_SIZE + 1);
   if (!buf)
   {
      fclose(f);
      skill_set_err(errbuf, errbuf_len, "out of memory");
      return NULL;
   }
   size_t n = fread(buf, 1, SKILL_SUPPORT_MAX_SIZE, f);
   if (ferror(f))
   {
      free(buf);
      fclose(f);
      skill_set_err(errbuf, errbuf_len, "failed to read support file");
      return NULL;
   }
   buf[n] = '\0';
   if (n == SKILL_SUPPORT_MAX_SIZE)
   {
      int extra = fgetc(f);
      if (extra != EOF)
      {
         free(buf);
         fclose(f);
         skill_set_err(errbuf, errbuf_len, "support file is too large");
         return NULL;
      }
   }
   fclose(f);
   return buf;
}

/* --- Inject --- */

char *skill_inject(const char *project_root, const char *base_prompt, const char *skill_name)
{
   char *skill_content = skill_load(project_root, skill_name);

   if (!skill_content)
   {
      /* Skill not found: return copy of base prompt (or NULL if no base either) */
      return base_prompt ? safe_strdup(base_prompt) : NULL;
   }

   dstr_t out;
   dstr_init(&out);

   if (base_prompt && base_prompt[0])
   {
      dstr_append_str(&out, base_prompt);
      /* Ensure blank line separator */
      size_t blen = strlen(base_prompt);
      if (blen > 0 && base_prompt[blen - 1] != '\n')
         dstr_append_char(&out, '\n');
      dstr_append_char(&out, '\n');
   }

   dstr_appendf(&out, "### ACTIVE SKILL: %s\n", skill_name);
   dstr_append_str(&out, skill_content);
   free(skill_content);
   (void)skill_record_activation(project_root, skill_name);
   skill_metrics_record_activation();

   char *result = dstr_steal(&out);
   if (!result)
      result = safe_strdup(base_prompt ? base_prompt : "");
   return result;
}
