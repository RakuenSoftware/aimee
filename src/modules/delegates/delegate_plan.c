#include "delegate_plan.h"
#include "util.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#ifndef MAX_PATH_LEN
#define MAX_PATH_LEN 4096
#endif

typedef struct
{
   char items[64][1024];
   int count;
} str_list_t;

static const char *DELEGATE_RESULT_CONTRACT =
    "Return only valid JSON matching delegate_result_v1. Do not include markdown. "
    "If blocked, use status=blocked and explain the blocker in supervisor_actions. "
    "If you touched files outside owned_files, list them in outside_ownership_touches. "
    "For status=done, include at least one tests entry with status=passed.";

static const char *DELEGATE_REVIEW_CONTRACT =
    "Return only valid JSON matching delegate_review_v1. Do not include markdown. "
    "Use status=pass, pass_with_notes, block, or needs_supervisor. "
    "Put blocking issues in findings with severity, category, file, line when known, "
    "owner_packet when attributable, verification, description, and suggestion. "
    "Only include findings verified against inspected current-code evidence. "
    "List uncovered proposal requirements in missing_requirements.";

static void set_err(char *errbuf, size_t errbuf_len, const char *msg)
{
   if (errbuf && errbuf_len > 0)
      snprintf(errbuf, errbuf_len, "%s", msg ? msg : "unknown error");
}

static char *read_file_all(const char *path, char *errbuf, size_t errbuf_len)
{
   FILE *f = fopen(path, "rb");
   if (!f)
   {
      set_err(errbuf, errbuf_len, "cannot open proposal file");
      return NULL;
   }
   if (fseek(f, 0, SEEK_END) != 0)
   {
      fclose(f);
      set_err(errbuf, errbuf_len, "cannot seek proposal file");
      return NULL;
   }
   long sz = ftell(f);
   if (sz < 0 || sz > 2 * 1024 * 1024)
   {
      fclose(f);
      set_err(errbuf, errbuf_len, "proposal file is empty or too large");
      return NULL;
   }
   rewind(f);

   char *buf = malloc((size_t)sz + 1);
   if (!buf)
   {
      fclose(f);
      set_err(errbuf, errbuf_len, "out of memory reading proposal");
      return NULL;
   }
   size_t n = fread(buf, 1, (size_t)sz, f);
   buf[n] = '\0';
   fclose(f);
   return buf;
}

static void strip_code_ticks(char *s)
{
   size_t len = strlen(s);
   if (len >= 2 && s[0] == '`' && s[len - 1] == '`')
   {
      memmove(s, s + 1, len - 2);
      s[len - 2] = '\0';
   }
}

static int path_like(const char *s)
{
   if (!s || !s[0])
      return 0;
   if (strcmp(s, "File") == 0 || strcmp(s, "Path") == 0)
      return 0;
   if (s[0] == '/' || strstr(s, "://") != NULL)
      return 0;
   if (strpbrk(s, "<>*?[]{}") != NULL)
      return 0;
   for (const char *p = s; *p; p++)
   {
      if (isspace((unsigned char)*p))
         return 0;
   }
   return strchr(s, '/') != NULL || strncmp(s, ".aimee", 6) == 0 || strncmp(s, ".github", 7) == 0;
}

static void str_list_add(str_list_t *list, const char *item)
{
   if (!list || !item || !item[0] || list->count >= 64)
      return;
   snprintf(list->items[list->count++], sizeof(list->items[0]), "%s", item);
}

static int str_list_contains(const str_list_t *list, const char *item)
{
   if (!list || !item)
      return 0;
   for (int i = 0; i < list->count; i++)
      if (strcmp(list->items[i], item) == 0)
         return 1;
   return 0;
}

static int path_prefix_src_db(const char *path, char tier)
{
   return path && path[0] == 's' && path[1] == 'r' && path[2] == 'c' && path[3] == '/' &&
          path[4] == 'd' && path[5] == 'b' && path[6] == tier && path[7] == '/';
}

static int tail_is_legacy_schema(const char *tail, char tier, int sqlite_variant)
{
   if (!tail || tail[0] != 'd' || tail[1] != 'b' || tail[2] != tier || tail[3] != '_')
      return 0;
   tail += 4;
   const char *schema = "schema";
   for (int i = 0; schema[i]; i++)
   {
      if (tail[i] != schema[i])
         return 0;
   }
   tail += 6;
   if (sqlite_variant)
   {
      if (tail[0] != '_' || tail[1] != 's' || tail[2] != 'q' || tail[3] != 'l' || tail[4] != 'i' ||
          tail[5] != 't' || tail[6] != 'e')
         return 0;
      tail += 7;
   }
   return tail[0] == '.' && tail[1] == 's' && tail[2] == 'q' && tail[3] == 'l' && tail[4] == '\0';
}

static void build_schema_path(char *buf, size_t buf_len, char tier, int sqlite_variant)
{
   if (!buf || buf_len == 0)
      return;
   size_t n = 0;
#define APPEND_CH(c)                                                                               \
   do                                                                                              \
   {                                                                                               \
      if (n + 1 < buf_len)                                                                         \
         buf[n++] = (char)(c);                                                                     \
   } while (0)
   APPEND_CH('s');
   APPEND_CH('r');
   APPEND_CH('c');
   APPEND_CH('/');
   APPEND_CH('d');
   APPEND_CH('b');
   APPEND_CH(tier);
   APPEND_CH('/');
   APPEND_CH('s');
   APPEND_CH('c');
   APPEND_CH('h');
   APPEND_CH('e');
   APPEND_CH('m');
   APPEND_CH('a');
   if (sqlite_variant)
   {
      APPEND_CH('_');
      APPEND_CH('s');
      APPEND_CH('q');
      APPEND_CH('l');
      APPEND_CH('i');
      APPEND_CH('t');
      APPEND_CH('e');
   }
   APPEND_CH('.');
   APPEND_CH('s');
   APPEND_CH('q');
   APPEND_CH('l');
   buf[n] = '\0';
#undef APPEND_CH
}

static const char *canonical_owned_path(const char *path, char *buf, size_t buf_len)
{
   if (!path || !buf || buf_len == 0)
      return path;
   if (path_prefix_src_db(path, '1') && tail_is_legacy_schema(path + 8, '1', 0))
   {
      build_schema_path(buf, buf_len, '1', 0);
      return buf;
   }
   if (path_prefix_src_db(path, '2') && tail_is_legacy_schema(path + 8, '2', 0))
   {
      build_schema_path(buf, buf_len, '2', 0);
      return buf;
   }
   if (path_prefix_src_db(path, '2') && tail_is_legacy_schema(path + 8, '2', 1))
   {
      build_schema_path(buf, buf_len, '2', 1);
      return buf;
   }
   return path;
}

static void str_list_add_unique(str_list_t *list, const char *item)
{
   char canon_buf[1024];
   const char *canon = canonical_owned_path(item, canon_buf, sizeof(canon_buf));
   if (!str_list_contains(list, canon))
      str_list_add(list, canon);
}

static void str_list_add_path(str_list_t *list, const char *item)
{
   char canon_buf[1024];
   str_list_add(list, canonical_owned_path(item, canon_buf, sizeof(canon_buf)));
}

static void extract_paths_from_backticks(const char *line, str_list_t *paths);
static void extract_paths_from_backticks_dupes_ok(const char *line, str_list_t *paths);
static int plan_path_is_readable_file(const char *path);

static void extract_title(const char *text, char *title, size_t title_len)
{
   snprintf(title, title_len, "Delegate work plan");
   if (!text)
      return;

   char *copy = strdup(text);
   if (!copy)
      return;
   char *save = NULL;
   for (char *line = strtok_r(copy, "\n", &save); line; line = strtok_r(NULL, "\n", &save))
   {
      char *t = util_trim(line);
      if (t[0] == '#')
      {
         while (*t == '#')
            t++;
         t = util_trim(t);
         if (strncmp(t, "Proposal:", 9) == 0)
            t = util_trim(t + 9);
         if (t[0])
            snprintf(title, title_len, "%s", t);
         break;
      }
   }
   free(copy);
}

static int heading_is(const char *line, const char *name)
{
   if (!line || line[0] != '#')
      return 0;
   while (*line == '#')
      line++;
   line = util_trim((char *)line);
   return strcasecmp(line, name) == 0;
}

static int acceptance_bullet_text(char *line, char **out)
{
   char *t = util_trim(line);
   if (strncmp(t, "- [ ]", 5) == 0 || strncmp(t, "- [x]", 5) == 0 || strncmp(t, "- [X]", 5) == 0)
      t = util_trim(t + 5);
   else if (strncmp(t, "- ", 2) == 0)
      t = util_trim(t + 2);
   else
      return 0;
   *out = t;
   return t[0] != '\0';
}

static void append_acceptance_continuation(char *buf, size_t buf_len, const char *text)
{
   if (!buf || buf_len == 0 || !text || !text[0])
      return;
   size_t len = strlen(buf);
   if (len + 2 >= buf_len)
      return;
   snprintf(buf + len, buf_len - len, "%s%s", len > 0 ? " " : "", text);
}

static void extract_acceptance(const char *text, str_list_t *criteria)
{
   char *copy = strdup(text ? text : "");
   if (!copy)
      return;

   int in_section = 0;
   char current[1024] = "";
   char *save = NULL;
   for (char *line = strtok_r(copy, "\n", &save); line; line = strtok_r(NULL, "\n", &save))
   {
      int indented = line[0] && isspace((unsigned char)line[0]);
      char *t = util_trim(line);
      if (t[0] == '#')
      {
         if (in_section && current[0])
         {
            str_list_add(criteria, current);
            current[0] = '\0';
         }
         in_section = heading_is(t, "Acceptance Criteria");
         continue;
      }
      if (!in_section)
         continue;

      char *bullet = NULL;
      if (acceptance_bullet_text(t, &bullet))
      {
         if (current[0])
            str_list_add(criteria, current);
         snprintf(current, sizeof(current), "%s", bullet);
      }
      else if (current[0] && indented && t[0])
      {
         append_acceptance_continuation(current, sizeof(current), t);
      }
   }
   if (in_section && current[0])
      str_list_add(criteria, current);
   free(copy);

   if (criteria->count == 0)
      str_list_add(criteria, "Implementation satisfies the source proposal.");
}

static int first_table_cell_paths(char *line, str_list_t *paths)
{
   char *t = util_trim(line);
   if (t[0] != '|')
      return 0;
   t++;
   char *bar = strchr(t, '|');
   if (!bar)
      return 0;
   *bar = '\0';
   t = util_trim(t);
   if (t[0] == '-' || t[0] == '\0')
      return 0;

   int before = paths ? paths->count : 0;
   extract_paths_from_backticks_dupes_ok(t, paths);
   if (paths && paths->count > before)
      return 1;

   char cell[512];
   snprintf(cell, sizeof(cell), "%s", t);
   strip_code_ticks(cell);
   if (!path_like(cell))
      return 0;
   str_list_add_unique(paths, cell);
   return 1;
}

static void extract_paths_from_backticks_dupes_ok(const char *line, str_list_t *paths)
{
   const char *p = line;
   while ((p = strchr(p, '`')) != NULL)
   {
      const char *q = strchr(++p, '`');
      if (!q)
         break;
      size_t len = (size_t)(q - p);
      if (len > 0 && len < 512)
      {
         char tmp[512];
         memcpy(tmp, p, len);
         tmp[len] = '\0';
         if (path_like(tmp))
            str_list_add_path(paths, tmp);
      }
      p = q + 1;
   }
}

static void extract_paths_from_backticks(const char *line, str_list_t *paths)
{
   const char *p = line;
   while ((p = strchr(p, '`')) != NULL)
   {
      const char *q = strchr(++p, '`');
      if (!q)
         break;
      size_t len = (size_t)(q - p);
      if (len > 0 && len < 512)
      {
         char tmp[512];
         memcpy(tmp, p, len);
         tmp[len] = '\0';
         if (path_like(tmp))
            str_list_add_unique(paths, tmp);
      }
      p = q + 1;
   }
}

static void extract_owned_paths(const char *text, str_list_t *paths)
{
   char *copy = strdup(text ? text : "");
   if (!copy)
      return;

   str_list_t table_paths = {0};
   str_list_t fallback_paths = {0};
   int in_changes = 0;
   int saw_changes_table_row = 0;
   char *save = NULL;
   for (char *line = strtok_r(copy, "\n", &save); line; line = strtok_r(NULL, "\n", &save))
   {
      char original[1024];
      snprintf(original, sizeof(original), "%s", line);
      char *t = util_trim(line);

      if (t[0] == '#')
      {
         in_changes = heading_is(t, "Changes");
         continue;
      }

      if (in_changes)
      {
         char probe[1024];
         snprintf(probe, sizeof(probe), "%s", t);
         char *cell = util_trim(probe);
         if (cell[0] == '|')
         {
            char *p = util_trim(cell + 1);
            if (p[0] && p[0] != '-')
               saw_changes_table_row = 1;
         }
         if (first_table_cell_paths(t, &table_paths))
         {
            continue;
         }
      }
      extract_paths_from_backticks(original, &fallback_paths);
   }
   free(copy);

   const str_list_t *selected = saw_changes_table_row ? &table_paths : &fallback_paths;
   for (int i = 0; i < selected->count; i++)
      str_list_add(paths, selected->items[i]);
}

static int proposal_declares_path_new(const char *text, const char *path)
{
   if (!text || !path || !path[0])
      return 0;

   char *copy = strdup(text);
   if (!copy)
      return 0;

   int found = 0;
   char *save = NULL;
   for (char *line = strtok_r(copy, "\n", &save); line; line = strtok_r(NULL, "\n", &save))
   {
      if (!strstr(line, path))
         continue;
      const char *marker = strstr(line, "(new)");
      if (marker)
      {
         found = 1;
         break;
      }
   }
   free(copy);
   return found;
}

static void classify_missing_owned_paths(const char *proposal_text, const str_list_t *owned,
                                         str_list_t *missing, str_list_t *new_files)
{
   if (!owned)
      return;
   for (int i = 0; i < owned->count; i++)
   {
      const char *path = owned->items[i];
      if (plan_path_is_readable_file(path))
         continue;
      if (proposal_declares_path_new(proposal_text, path))
         str_list_add_unique(new_files, path);
      else
         str_list_add_unique(missing, path);
   }
}

static void build_launchable_owned_paths(const str_list_t *owned, const str_list_t *missing,
                                         str_list_t *launchable)
{
   if (!owned || !launchable)
      return;
   for (int i = 0; i < owned->count; i++)
   {
      if (str_list_contains(missing, owned->items[i]))
         continue;
      str_list_add(launchable, owned->items[i]);
   }
}

static void sanitize_id(const char *in, char *out, size_t out_len)
{
   size_t n = 0;
   for (const char *p = in; *p && n + 1 < out_len; p++)
   {
      char c = (char)tolower((unsigned char)*p);
      if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))
         out[n++] = c;
      else if (n > 0 && out[n - 1] != '-')
         out[n++] = '-';
   }
   while (n > 0 && out[n - 1] == '-')
      n--;
   out[n] = '\0';
   if (out[0] == '\0')
      snprintf(out, out_len, "packet");
}

static void json_add_str_list(cJSON *obj, const char *name, const str_list_t *list)
{
   cJSON *arr = cJSON_AddArrayToObject(obj, name);
   for (int i = 0; list && i < list->count; i++)
      cJSON_AddItemToArray(arr, cJSON_CreateString(list->items[i]));
}

static int plan_path_is_readable_file(const char *path)
{
   struct stat st;
   if (!path || !path[0])
      return 0;
   if (stat(path, &st) == 0 && S_ISREG(st.st_mode))
      return 1;
   if (strncmp(path, "src/", 4) == 0 && stat(path + 4, &st) == 0 && S_ISREG(st.st_mode))
      return 1;
   return 0;
}

static void json_add_read_context_path(cJSON *read, const char *path)
{
   if (plan_path_is_readable_file(path))
      cJSON_AddItemToArray(read, cJSON_CreateString(path));
}

/* Derive a source file's unit-test stem: the basename minus its extension.
 * `src/server/server_http.c` -> "server_http"; a header maps to the same stem
 * so a `.h` change verifies against the matching test. Returns 0 on success. */
static int plan_test_stem(const char *path, char *stem, size_t stem_len)
{
   if (!path || !stem || stem_len == 0)
      return -1;
   const char *base = strrchr(path, '/');
   base = base ? base + 1 : path;
   if (!base[0])
      return -1;
   snprintf(stem, stem_len, "%s", base);
   char *dot = strrchr(stem, '.');
   if (dot)
      *dot = '\0';
   return stem[0] ? 0 : -1;
}

/* Map an owned source path to its unit-test source and binary, following the
 * repo convention `src/tests/test_<stem>.c` -> `unit-test-<stem-dashed>` (the
 * binary swaps `_` for `-`). Returns 1 when the test source exists, else 0. */
static int plan_test_artifacts(const char *path, char *test_src, size_t test_src_len, char *bin,
                               size_t bin_len)
{
   char stem[256];
   if (plan_test_stem(path, stem, sizeof(stem)) != 0)
      return 0;
   snprintf(test_src, test_src_len, "src/tests/test_%s.c", stem);
   if (!plan_path_is_readable_file(test_src))
      return 0;
   char dashed[256];
   snprintf(dashed, sizeof(dashed), "%s", stem);
   for (char *p = dashed; *p; p++)
      if (*p == '_')
         *p = '-';
   snprintf(bin, bin_len, "unit-test-%s", dashed);
   return 1;
}

/* Add the build + run command pair for a unit-test binary, skipping duplicates
 * so several owned files sharing one test only emit it once. */
static void verify_add_test_bin(cJSON *arr, const char *bin)
{
   char build_cmd[320];
   snprintf(build_cmd, sizeof(build_cmd), "make -C src build/obj/tests/%s", bin);
   cJSON *it;
   cJSON_ArrayForEach(it, arr)
   {
      if (cJSON_IsString(it) && strcmp(it->valuestring, build_cmd) == 0)
         return;
   }
   char run_cmd[320];
   snprintf(run_cmd, sizeof(run_cmd), "./src/build/obj/tests/%s", bin);
   cJSON_AddItemToArray(arr, cJSON_CreateString(build_cmd));
   cJSON_AddItemToArray(arr, cJSON_CreateString(run_cmd));
}

/* Verify commands derived from the packet's owned files: always lint, then the
 * matching unit-test target for each owned source that has one. A file with no
 * test contributes nothing (lint-only), rather than the prior hardcoded
 * delegate-subsystem tests that were unrelated to the file under change. */
static void add_verify_for_files(cJSON *packet, const str_list_t *files)
{
   cJSON *arr = cJSON_AddArrayToObject(packet, "verify_commands");
   cJSON_AddItemToArray(arr, cJSON_CreateString("make -C src lint"));
   for (int i = 0; files && i < files->count; i++)
   {
      char test_src[1280], bin[288];
      if (plan_test_artifacts(files->items[i], test_src, sizeof(test_src), bin, sizeof(bin)))
         verify_add_test_bin(arr, bin);
   }
}

static void add_packet(cJSON *packets, const char *source_path, const char *path,
                       const str_list_t *criteria, int index)
{
   char sid[160];
   sanitize_id(path, sid, sizeof(sid));

   cJSON *packet = cJSON_CreateObject();
   char id[192];
   snprintf(id, sizeof(id), "packet-%02d-%s", index + 1, sid);
   cJSON_AddStringToObject(packet, "id", id);
   cJSON_AddStringToObject(packet, "role", "code");

   char title[640];
   snprintf(title, sizeof(title), "Implement changes for %s", path);
   cJSON_AddStringToObject(packet, "title", title);
   cJSON_AddStringToObject(packet, "objective",
                           "Implement the proposal slice for the owned file while preserving "
                           "existing behavior outside that scope.");

   cJSON *owned = cJSON_AddArrayToObject(packet, "owned_files");
   cJSON_AddItemToArray(owned, cJSON_CreateString(path));

   cJSON *read = cJSON_AddArrayToObject(packet, "read_context");
   cJSON_AddItemToArray(read, cJSON_CreateString(source_path));
   json_add_read_context_path(read, path);
   /* Point the delegate at the test that actually covers its owned file, if one
    * exists, so it reads the right fixtures rather than unrelated tests. */
   char test_src[1280], bin[288];
   if (plan_test_artifacts(path, test_src, sizeof(test_src), bin, sizeof(bin)))
      cJSON_AddItemToArray(read, cJSON_CreateString(test_src));

   cJSON_AddArrayToObject(packet, "symbols");
   json_add_str_list(packet, "acceptance_criteria", criteria);
   str_list_t owned_one = {0};
   snprintf(owned_one.items[0], sizeof(owned_one.items[0]), "%s", path);
   owned_one.count = 1;
   add_verify_for_files(packet, &owned_one);

   cJSON *non_goals = cJSON_AddArrayToObject(packet, "non_goals");
   cJSON_AddItemToArray(non_goals,
                        cJSON_CreateString("do not edit files outside owned_files without "
                                           "listing the ownership drift"));
   cJSON_AddItemToArray(non_goals, cJSON_CreateString("do not move proposal files"));

   cJSON_AddStringToObject(packet, "handoff_schema", "delegate_result_v1");
   cJSON_AddStringToObject(packet, "final_output_contract", DELEGATE_RESULT_CONTRACT);
   cJSON_AddItemToArray(packets, packet);
}

static void add_reviewer_packet(cJSON *packets, const char *source_path, const str_list_t *owned,
                                const str_list_t *criteria)
{
   cJSON *packet = cJSON_CreateObject();
   cJSON_AddStringToObject(packet, "id", "packet-reviewer");
   cJSON_AddStringToObject(packet, "role", "review");
   if (owned->count == 0)
   {
      cJSON_AddStringToObject(packet, "title",
                              "Review proposal for manual or operational execution");
      cJSON_AddStringToObject(packet, "objective",
                              "Review the proposal for concrete next steps because no concrete "
                              "owned files were detected.");
   }
   else
   {
      cJSON_AddStringToObject(packet, "title", "Review generated implementation against proposal");
      cJSON_AddStringToObject(packet, "objective",
                              "Review the completed implementation for proposal coverage, "
                              "missing tests, and file ownership drift.");
   }
   cJSON_AddArrayToObject(packet, "owned_files");
   json_add_str_list(packet, "expected_files", owned);

   cJSON *read = cJSON_AddArrayToObject(packet, "read_context");
   cJSON_AddItemToArray(read, cJSON_CreateString(source_path));
   for (int i = 0; i < owned->count; i++)
   {
      json_add_read_context_path(read, owned->items[i]);
      char test_src[1280], bin[288];
      if (plan_test_artifacts(owned->items[i], test_src, sizeof(test_src), bin, sizeof(bin)))
         cJSON_AddItemToArray(read, cJSON_CreateString(test_src));
   }

   cJSON_AddArrayToObject(packet, "symbols");
   json_add_str_list(packet, "acceptance_criteria", criteria);
   add_verify_for_files(packet, owned);
   cJSON *non_goals = cJSON_AddArrayToObject(packet, "non_goals");
   cJSON_AddItemToArray(non_goals, cJSON_CreateString("do not edit files"));
   cJSON_AddStringToObject(packet, "handoff_schema", "delegate_review_v1");
   cJSON_AddStringToObject(packet, "final_output_contract", DELEGATE_REVIEW_CONTRACT);
   cJSON_AddItemToArray(packets, packet);
}

static int add_conflicts(cJSON *plan, const str_list_t *paths)
{
   cJSON *conflicts = cJSON_AddArrayToObject(plan, "conflicts");
   int conflict_count = 0;
   for (int i = 0; i < paths->count; i++)
   {
      int seen_before = 0;
      for (int j = 0; j < i; j++)
      {
         if (strcmp(paths->items[i], paths->items[j]) == 0)
         {
            seen_before = 1;
            break;
         }
      }
      if (seen_before)
         continue;

      int count = 0;
      for (int j = 0; j < paths->count; j++)
         if (strcmp(paths->items[i], paths->items[j]) == 0)
            count++;
      if (count <= 1)
         continue;

      cJSON *conflict = cJSON_CreateObject();
      cJSON_AddStringToObject(conflict, "file", paths->items[i]);
      cJSON_AddNumberToObject(conflict, "packet_count", count);
      cJSON_AddItemToArray(conflicts, conflict);
      conflict_count++;
   }
   return conflict_count;
}

cJSON *delegate_plan_build_from_text(const char *source_path, const char *proposal_text,
                                     char *errbuf, size_t errbuf_len)
{
   if (!proposal_text || !proposal_text[0])
   {
      set_err(errbuf, errbuf_len, "proposal text is empty");
      return NULL;
   }

   str_list_t criteria = {0};
   str_list_t owned = {0};
   str_list_t missing_owned = {0};
   str_list_t new_owned = {0};
   str_list_t launchable_owned = {0};
   char title[256];
   extract_title(proposal_text, title, sizeof(title));
   extract_acceptance(proposal_text, &criteria);
   extract_owned_paths(proposal_text, &owned);
   classify_missing_owned_paths(proposal_text, &owned, &missing_owned, &new_owned);
   build_launchable_owned_paths(&owned, &missing_owned, &launchable_owned);

   cJSON *plan = cJSON_CreateObject();
   cJSON_AddStringToObject(plan, "schema", "delegate_plan_v1");
   cJSON_AddStringToObject(plan, "source", source_path ? source_path : "");
   cJSON_AddStringToObject(plan, "title", title);

   cJSON *packets = cJSON_AddArrayToObject(plan, "packets");
   for (int i = 0; i < launchable_owned.count; i++)
      add_packet(packets, source_path ? source_path : "", launchable_owned.items[i], &criteria, i);

   int reviewer_included = launchable_owned.count == 0 || launchable_owned.count >= 2 ||
                           missing_owned.count > 0 || strlen(proposal_text) > 2000;
   if (reviewer_included)
      add_reviewer_packet(packets, source_path ? source_path : "", &owned, &criteria);

   int conflicts = add_conflicts(plan, &launchable_owned);
   json_add_str_list(plan, "missing_owned_files", &missing_owned);
   json_add_str_list(plan, "new_owned_files", &new_owned);
   cJSON_AddNumberToObject(plan, "packet_count", cJSON_GetArraySize(packets));
   cJSON_AddNumberToObject(plan, "implementation_packet_count", launchable_owned.count);
   cJSON_AddNumberToObject(plan, "parallel_safe_count", launchable_owned.count - conflicts);
   cJSON_AddNumberToObject(plan, "needs_sequencing_count", conflicts);
   cJSON_AddBoolToObject(plan, "reviewer_packet_included", reviewer_included);
   cJSON_AddBoolToObject(plan, "manual_only", launchable_owned.count == 0);
   cJSON_AddBoolToObject(plan, "requires_supervisor_review", missing_owned.count > 0);
   if (launchable_owned.count == 0)
      cJSON_AddStringToObject(plan, "no_implementation_reason",
                              "No concrete owned files were detected; template and glob path "
                              "specifications and unmarked missing files are ignored by the "
                              "planner.");

   return plan;
}

cJSON *delegate_plan_build_from_file(const char *proposal_path, char *errbuf, size_t errbuf_len)
{
   if (!proposal_path || !proposal_path[0])
   {
      set_err(errbuf, errbuf_len, "proposal path is required");
      return NULL;
   }

   char *text = read_file_all(proposal_path, errbuf, errbuf_len);
   if (!text)
      return NULL;
   cJSON *plan = delegate_plan_build_from_text(proposal_path, text, errbuf, errbuf_len);
   free(text);
   return plan;
}
