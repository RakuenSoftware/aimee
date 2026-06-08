#include "client_constants.h"
#include "client_integrations.h"
#include "platform_path.h"
#include "platform_process.h"
#include "cJSON.h"
#include "dstr.h"
#include <ctype.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void ensure_parent_dir(const char *path, mode_t mode)
{
   char dir[MAX_PATH_LEN];
   snprintf(dir, sizeof(dir), "%s", path);
   char *last_slash = strrchr(dir, '/');
   if (!last_slash)
      return;
   *last_slash = '\0';
   for (char *p = dir + 1; *p; p++)
   {
      if (*p == '/')
      {
         *p = '\0';
         platform_mkdir_p(dir, mode);
         *p = '/';
      }
   }
   platform_mkdir_p(dir, mode);
}

static int write_text_file(const char *path, const char *content, mode_t mode)
{
   dstr_t existing;
   dstr_init(&existing);
   if (dstr_read_file(&existing, path) == 0 && dstr_equals_cstr(&existing, content))
   {
      dstr_free(&existing);
      return 0;
   }
   dstr_free(&existing);

   ensure_parent_dir(path, 0700);

   /* Atomic write: write to a temp file then rename into place so readers
    * never see a truncated/empty file (avoids race with Claude Code reading
    * settings.json while we rewrite it). */
   char tmp[MAX_PATH_LEN];
   snprintf(tmp, sizeof(tmp), "%s.tmp.%d", path, (int)getpid());
   FILE *fp = fopen(tmp, "w");
   if (!fp)
      return -1;
   fputs(content, fp);
   if (fclose(fp) != 0)
   {
      unlink(tmp);
      return -1;
   }
   chmod(tmp, mode);
   if (rename(tmp, path) != 0)
   {
      unlink(tmp);
      return -1;
   }
   return 0;
}

static const char *resolved_aimee_bin_path(void)
{
   static char path[MAX_PATH_LEN];
   if (path[0])
      return path;

   if (platform_get_exe_path(path, sizeof(path)) == 0)
   {
      char *base = strrchr(path, '/');
      base = base ? base + 1 : path;
      if (strcmp(base, "aimee") == 0 || strcmp(base, "aimee.exe") == 0 ||
          strcmp(base, "aimee-client") == 0 || strcmp(base, "aimee-client.exe") == 0)
         return path;
      if (strcmp(base, "aimee-server") == 0)
      {
         snprintf(base, sizeof(path) - (size_t)(base - path), "aimee");
         return path;
      }
      if (strcmp(base, "aimee-server.exe") == 0)
      {
         snprintf(base, sizeof(path) - (size_t)(base - path), "aimee.exe");
         return path;
      }
   }

   path[0] = '\0';
   const char *home = getenv("HOME");
   if (home)
      snprintf(path, sizeof(path), "%s/.local/bin/aimee", home);
   return path;
}

static void format_mcp_json(char *buf, size_t cap, const char *aimee_bin)
{
   if (!buf || cap == 0)
      return;
   snprintf(buf, cap,
            "{\n"
            "  \"mcpServers\": {\n"
            "    \"aimee\": {\n"
            "      \"command\": \"%s\",\n"
            "      \"args\": [\"mcp-serve\"]\n"
            "    }\n"
            "  }\n"
            "}\n",
            aimee_bin ? aimee_bin : "aimee");
}

static cJSON *create_aimee_mcp_server(const char *aimee_bin)
{
   cJSON *aimee_server = cJSON_CreateObject();
   if (!aimee_server)
      return NULL;
   cJSON_AddStringToObject(aimee_server, "command", aimee_bin ? aimee_bin : "aimee");
   cJSON *a = cJSON_CreateArray();
   cJSON_AddItemToArray(a, cJSON_CreateString("mcp-serve"));
   cJSON_AddItemToObject(aimee_server, "args", a);
   return aimee_server;
}

static cJSON *build_marketplace_root(void)
{
   cJSON *root = cJSON_CreateObject();
   cJSON_AddStringToObject(root, "name", "local");
   cJSON *iface = cJSON_AddObjectToObject(root, "interface");
   cJSON_AddStringToObject(iface, "displayName", "Local Plugins");
   cJSON_AddItemToObject(root, "plugins", cJSON_CreateArray());
   return root;
}

static cJSON *build_aimee_plugin_entry(void)
{
   cJSON *entry = cJSON_CreateObject();
   cJSON_AddStringToObject(entry, "name", "aimee");
   cJSON *source = cJSON_AddObjectToObject(entry, "source");
   cJSON_AddStringToObject(source, "source", "local");
   cJSON_AddStringToObject(source, "path", "./plugins/aimee");
   cJSON *policy = cJSON_AddObjectToObject(entry, "policy");
   cJSON_AddStringToObject(policy, "installation", "INSTALLED_BY_DEFAULT");
   cJSON_AddStringToObject(policy, "authentication", "ON_USE");
   cJSON_AddStringToObject(entry, "category", "Coding");
   return entry;
}

static const char *codex_delegate_policy_prompt(void)
{
   return "Do not spawn provider-native sub-agents such as Codex spawn_agent or "
          "Claude Agent; use the aimee delegate MCP tool for every delegated or "
          "parallel sub-task";
}

static const char *codex_skill_markdown(void)
{
   return "---\n"
          "name: aimee\n"
          "description: Use aimee for repo memory, indexed symbol lookup, "
          "blast-radius preview, and delegated work.\n"
          "---\n"
          "\n"
          "# aimee\n"
          "\n"
          "Use this plugin when Codex needs repository memory or aimee-specific helpers.\n"
          "\n"
          "- Prefer local file inspection first for nearby code.\n"
          "- Use `search_memory` for stored project facts or prior decisions.\n"
          "- Use `find_symbol` when the local search surface is missing indexed context.\n"
          "- Use `preview_blast_radius` before broad multi-file edits.\n"
          "- Do not call provider-native sub-agent tools such as `spawn_agent`; use "
          "the aimee `delegate` MCP tool for every delegated or parallel sub-task.\n"
          "- Use `delegate` only for bounded sub-tasks that materially advance the "
          "current work.\n";
}

static void ensure_codex_marketplace(const char *path)
{
   cJSON *root = NULL;
   FILE *fp = fopen(path, "r");
   if (fp)
   {
      fseek(fp, 0, SEEK_END);
      long sz = ftell(fp);
      fseek(fp, 0, SEEK_SET);
      if (sz >= 0 && sz < (long)(1 << 20))
      {
         char *buf = malloc((size_t)sz + 1);
         if (buf)
         {
            if (fread(buf, 1, (size_t)sz, fp) == (size_t)sz)
            {
               buf[sz] = '\0';
               root = cJSON_Parse(buf);
            }
            free(buf);
         }
      }
      fclose(fp);
   }

   if (!cJSON_IsObject(root))
   {
      if (root)
         cJSON_Delete(root);
      root = build_marketplace_root();
   }

   cJSON *plugins = cJSON_GetObjectItemCaseSensitive(root, "plugins");
   if (!cJSON_IsArray(plugins))
   {
      if (plugins)
         cJSON_DeleteItemFromObjectCaseSensitive(root, "plugins");
      plugins = cJSON_CreateArray();
      cJSON_AddItemToObject(root, "plugins", plugins);
   }

   int replaced = 0;
   cJSON *item = NULL;
   cJSON_ArrayForEach(item, plugins)
   {
      cJSON *name = cJSON_GetObjectItemCaseSensitive(item, "name");
      if (cJSON_IsString(name) && strcmp(name->valuestring, "aimee") == 0)
      {
         cJSON *entry = build_aimee_plugin_entry();
         cJSON_ReplaceItemViaPointer(plugins, item, entry);
         replaced = 1;
         break;
      }
   }
   if (!replaced)
      cJSON_AddItemToArray(plugins, build_aimee_plugin_entry());

   char *json = cJSON_Print(root);
   if (json)
   {
      write_text_file(path, json, 0600);
      free(json);
   }
   cJSON_Delete(root);
}

static void ensure_codex_plugin_enabled(const char *path)
{
   const char *section = "[plugins.\"aimee@local\"]";
   const char *enabled_true = "enabled = true";

   FILE *fp = fopen(path, "r");
   if (!fp)
   {
      char buf[256];
      snprintf(buf, sizeof(buf), "%s\n%s\n", section, enabled_true);
      write_text_file(path, buf, 0600);
      return;
   }

   fseek(fp, 0, SEEK_END);
   long sz = ftell(fp);
   fseek(fp, 0, SEEK_SET);
   if (sz < 0 || sz >= (long)(1 << 20))
   {
      fclose(fp);
      return;
   }

   char *buf = malloc((size_t)sz + 1);
   if (!buf)
   {
      fclose(fp);
      return;
   }
   size_t n = fread(buf, 1, (size_t)sz, fp);
   fclose(fp);
   buf[n] = '\0';

   char *section_pos = strstr(buf, section);
   if (!section_pos)
   {
      size_t len = strlen(buf);
      int needs_nl = (len > 0 && buf[len - 1] != '\n');
      size_t extra = (needs_nl ? 1u : 0u) + (len > 0 ? 1u : 0u) + strlen(section) + 1u +
                     strlen(enabled_true) + 1u;
      char *out = malloc(len + extra + 1u);
      if (out)
      {
         size_t pos = 0;
         memcpy(out + pos, buf, len);
         pos += len;
         if (needs_nl)
            out[pos++] = '\n';
         if (len > 0)
            out[pos++] = '\n';
         memcpy(out + pos, section, strlen(section));
         pos += strlen(section);
         out[pos++] = '\n';
         memcpy(out + pos, enabled_true, strlen(enabled_true));
         pos += strlen(enabled_true);
         out[pos++] = '\n';
         out[pos] = '\0';
         write_text_file(path, out, 0600);
         free(out);
      }
      free(buf);
      return;
   }

   char *next_section = strstr(section_pos + strlen(section), "\n[");
   size_t section_len = next_section ? (size_t)(next_section - section_pos) : strlen(section_pos);
   char *enabled_pos = strstr(section_pos, enabled_true);
   if (enabled_pos && (size_t)(enabled_pos - section_pos) < section_len)
   {
      free(buf);
      return;
   }

   char *enabled_false = strstr(section_pos, "enabled = false");
   if (enabled_false && (size_t)(enabled_false - section_pos) < section_len)
   {
      size_t prefix_len = (size_t)(enabled_false - buf);
      size_t suffix_off = prefix_len + strlen("enabled = false");
      size_t suffix_len = strlen(buf + suffix_off);
      char *out = malloc(prefix_len + strlen(enabled_true) + suffix_len + 1u);
      if (out)
      {
         memcpy(out, buf, prefix_len);
         memcpy(out + prefix_len, enabled_true, strlen(enabled_true));
         memcpy(out + prefix_len + strlen(enabled_true), buf + suffix_off, suffix_len + 1u);
         write_text_file(path, out, 0600);
         free(out);
      }
      free(buf);
      return;
   }

   char *line_end = strchr(section_pos, '\n');
   if (!line_end)
   {
      free(buf);
      return;
   }

   size_t insert_off = (size_t)(line_end - buf) + 1u;
   size_t len = strlen(buf);
   size_t insert_len = strlen(enabled_true) + 1u;
   char *out = malloc(len + insert_len + 1u);
   if (out)
   {
      memcpy(out, buf, insert_off);
      memcpy(out + insert_off, enabled_true, strlen(enabled_true));
      out[insert_off + strlen(enabled_true)] = '\n';
      memcpy(out + insert_off + insert_len, buf + insert_off, len - insert_off + 1u);
      write_text_file(path, out, 0600);
      free(out);
   }
   free(buf);
}

static int codex_project_header(const char *project_root, char *out, size_t out_len)
{
   char escaped[MAX_PATH_LEN * 2];
   size_t pos = 0;
   for (const char *p = project_root; p && *p; p++)
   {
      if (*p == '"' || *p == '\\')
      {
         if (pos + 2 >= sizeof(escaped))
            return -1;
         escaped[pos++] = '\\';
         escaped[pos++] = *p;
      }
      else
      {
         if (pos + 1 >= sizeof(escaped))
            return -1;
         escaped[pos++] = *p;
      }
   }
   escaped[pos] = '\0';

   int n = snprintf(out, out_len, "[projects.\"%s\"]", escaped);
   return (n >= 0 && (size_t)n < out_len) ? 0 : -1;
}

static int codex_find_project_section(const char *buf, const char *header, const char **section_out,
                                      const char **section_end_out)
{
   size_t header_len = strlen(header);
   const char *p = buf;
   while ((p = strstr(p, header)) != NULL)
   {
      int at_line_start = (p == buf || p[-1] == '\n');
      char after = p[header_len];
      int header_ends = (after == '\0' || after == '\n' || after == '\r');
      if (at_line_start && header_ends)
      {
         const char *next = strstr(p + header_len, "\n[");
         *section_out = p;
         *section_end_out = next ? next : buf + strlen(buf);
         return 1;
      }
      p += header_len;
   }
   return 0;
}

static const char *codex_find_trust_line(const char *section, const char *section_end,
                                         const char **line_after_out)
{
   const char *p = section;
   while (p < section_end)
   {
      const char *line_end = memchr(p, '\n', (size_t)(section_end - p));
      if (!line_end)
         line_end = section_end;

      const char *s = p;
      while (s < line_end && isspace((unsigned char)*s))
         s++;
      const char key[] = "trust_level";
      size_t key_len = sizeof(key) - 1;
      if ((size_t)(line_end - s) >= key_len && strncmp(s, key, key_len) == 0)
      {
         const char *q = s + key_len;
         while (q < line_end && isspace((unsigned char)*q))
            q++;
         if (q < line_end && *q == '=')
         {
            if (line_after_out)
               *line_after_out = line_end < section_end ? line_end + 1 : line_end;
            return p;
         }
      }

      p = line_end < section_end ? line_end + 1 : section_end;
   }
   return NULL;
}

static int codex_line_is_trusted(const char *line, const char *line_after)
{
   size_t len = (size_t)(line_after - line);
   const char *trusted = strstr(line, "trust_level = \"trusted\"");
   return len >= strlen("trust_level = \"trusted\"") && trusted && trusted < line_after;
}

static int ensure_codex_trusted_project_in_config(const char *config_path, const char *project_root)
{
   if (!config_path || !config_path[0] || !project_root || !project_root[0])
      return -1;

   char header[MAX_PATH_LEN * 2 + 32];
   if (codex_project_header(project_root, header, sizeof(header)) != 0)
      return -1;

   dstr_t input;
   dstr_init(&input);
   if (dstr_read_file(&input, config_path) != 0)
      dstr_reset(&input);
   const char *buf = dstr_cstr(&input);

   const char *section = NULL;
   const char *section_end = NULL;
   if (!codex_find_project_section(buf, header, &section, &section_end))
   {
      dstr_t out;
      dstr_init(&out);
      size_t len = strlen(buf);
      if (len > 0)
      {
         dstr_append(&out, buf, len);
         if (buf[len - 1] != '\n')
            dstr_append_char(&out, '\n');
         dstr_append_char(&out, '\n');
      }
      dstr_appendf(&out, "%s\ntrust_level = \"trusted\"\n", header);
      int rc = write_text_file(config_path, dstr_cstr(&out), 0600);
      dstr_free(&out);
      dstr_free(&input);
      return rc;
   }

   const char *trust_after = NULL;
   const char *trust_line = codex_find_trust_line(section, section_end, &trust_after);
   if (trust_line && codex_line_is_trusted(trust_line, trust_after))
   {
      dstr_free(&input);
      return 0;
   }

   dstr_t out;
   dstr_init(&out);
   if (trust_line)
   {
      dstr_append(&out, buf, (size_t)(trust_line - buf));
      dstr_append_str(&out, "trust_level = \"trusted\"\n");
      dstr_append_str(&out, trust_after);
   }
   else
   {
      const char *header_end = strchr(section, '\n');
      const char *insert_at = header_end && header_end < section_end ? header_end + 1 : section_end;
      dstr_append(&out, buf, (size_t)(insert_at - buf));
      if (insert_at == section_end && (insert_at == buf || insert_at[-1] != '\n'))
         dstr_append_char(&out, '\n');
      dstr_append_str(&out, "trust_level = \"trusted\"\n");
      dstr_append_str(&out, insert_at);
   }

   int rc = write_text_file(config_path, dstr_cstr(&out), 0600);
   dstr_free(&out);
   dstr_free(&input);
   return rc;
}

void ensure_codex_project_trusted(const char *codex_home, const char *project_root)
{
   if (!codex_home || !codex_home[0] || !project_root || !project_root[0])
      return;

   char root[MAX_PATH_LEN];
#ifdef _WIN32
   if (_fullpath(root, project_root, sizeof(root)) == NULL)
      snprintf(root, sizeof(root), "%s", project_root);
#else
   if (realpath(project_root, root) == NULL)
      snprintf(root, sizeof(root), "%s", project_root);
#endif

   char config_path[MAX_PATH_LEN];
   snprintf(config_path, sizeof(config_path), "%s/config.toml", codex_home);
   (void)ensure_codex_trusted_project_in_config(config_path, root);
}

static char *codex_shell_quote(const char *raw)
{
   if (!raw)
      return NULL;
   size_t len = 2; /* surrounding single quotes */
   for (const char *p = raw; *p; p++)
      len += (*p == '\'') ? 4u : 1u;
   char *out = malloc(len + 1);
   if (!out)
      return NULL;
   size_t pos = 0;
   out[pos++] = '\'';
   for (const char *p = raw; *p; p++)
   {
      if (*p == '\'')
      {
         out[pos++] = '\'';
         out[pos++] = '\\';
         out[pos++] = '\'';
         out[pos++] = '\'';
      }
      else
         out[pos++] = *p;
   }
   out[pos++] = '\'';
   out[pos] = '\0';
   return out;
}

void ensure_codex_current_project_trusted(const char *codex_home)
{
   if (!codex_home || !codex_home[0])
      return;

   char cwd[MAX_PATH_LEN];
   if (!getcwd(cwd, sizeof(cwd)))
      return;

   ensure_codex_project_trusted(codex_home, cwd);

   char *esc = codex_shell_quote(cwd);
   if (!esc)
      return;
   char cmd[MAX_PATH_LEN + 128];
   snprintf(cmd, sizeof(cmd), "git -C %s rev-parse --show-toplevel 2>/dev/null", esc);
   free(esc);

   char *git_root = NULL;
   size_t git_root_len = 0;
   int rc = platform_exec_capture(cmd, &git_root, &git_root_len, 5000);
   if (rc == 0 && git_root && git_root[0])
   {
      size_t len = strlen(git_root);
      while (len > 0 && (git_root[len - 1] == '\n' || git_root[len - 1] == '\r'))
         git_root[--len] = '\0';
      if (git_root[0])
         ensure_codex_project_trusted(codex_home, git_root);
   }
   free(git_root);
}

static void ensure_codex_plugin_files(const char *home)
{
   const char *aimee_bin = resolved_aimee_bin_path();
   struct stat st;
   if (stat(aimee_bin, &st) != 0)
      return;

   char plugin_json[MAX_PATH_LEN];
   char marketplace_plugin_json[MAX_PATH_LEN];
   char installed_plugin_json[MAX_PATH_LEN];
   char mcp_json[MAX_PATH_LEN];
   char marketplace_mcp_json[MAX_PATH_LEN];
   char installed_mcp_json[MAX_PATH_LEN];
   char compat_plugin_json[MAX_PATH_LEN];
   char marketplace_compat_plugin_json[MAX_PATH_LEN];
   char installed_compat_plugin_json[MAX_PATH_LEN];
   char skill_md[MAX_PATH_LEN];
   char marketplace_skill_md[MAX_PATH_LEN];
   char installed_skill_md[MAX_PATH_LEN];
   char compat_mcp_json[MAX_PATH_LEN];
   char marketplace_compat_mcp_json[MAX_PATH_LEN];
   char installed_compat_mcp_json[MAX_PATH_LEN];
   char marketplace[MAX_PATH_LEN];
   char config_toml[MAX_PATH_LEN];
   snprintf(plugin_json, sizeof(plugin_json), "%s/plugins/aimee/.codex-plugin/plugin.json", home);
   snprintf(marketplace_plugin_json, sizeof(marketplace_plugin_json),
            "%s/.agents/plugins/plugins/aimee/.codex-plugin/plugin.json", home);
   snprintf(installed_plugin_json, sizeof(installed_plugin_json),
            "%s/.codex/plugins/cache/local/aimee/.codex-plugin/plugin.json", home);
   snprintf(mcp_json, sizeof(mcp_json), "%s/plugins/aimee/.mcp.json", home);
   snprintf(marketplace_mcp_json, sizeof(marketplace_mcp_json),
            "%s/.agents/plugins/plugins/aimee/.mcp.json", home);
   snprintf(installed_mcp_json, sizeof(installed_mcp_json),
            "%s/.codex/plugins/cache/local/aimee/.mcp.json", home);
   snprintf(compat_plugin_json, sizeof(compat_plugin_json),
            "%s/plugins/aimee/skills/.codex-plugin/plugin.json", home);
   snprintf(marketplace_compat_plugin_json, sizeof(marketplace_compat_plugin_json),
            "%s/.agents/plugins/plugins/aimee/skills/.codex-plugin/plugin.json", home);
   snprintf(installed_compat_plugin_json, sizeof(installed_compat_plugin_json),
            "%s/.codex/plugins/cache/local/aimee/skills/.codex-plugin/plugin.json", home);
   snprintf(skill_md, sizeof(skill_md), "%s/plugins/aimee/skills/aimee/SKILL.md", home);
   snprintf(marketplace_skill_md, sizeof(marketplace_skill_md),
            "%s/.agents/plugins/plugins/aimee/skills/aimee/SKILL.md", home);
   snprintf(installed_skill_md, sizeof(installed_skill_md),
            "%s/.codex/plugins/cache/local/aimee/skills/aimee/SKILL.md", home);
   snprintf(compat_mcp_json, sizeof(compat_mcp_json), "%s/plugins/aimee/skills/.mcp.json", home);
   snprintf(marketplace_compat_mcp_json, sizeof(marketplace_compat_mcp_json),
            "%s/.agents/plugins/plugins/aimee/skills/.mcp.json", home);
   snprintf(installed_compat_mcp_json, sizeof(installed_compat_mcp_json),
            "%s/.codex/plugins/cache/local/aimee/skills/.mcp.json", home);
   snprintf(marketplace, sizeof(marketplace), "%s/.agents/plugins/marketplace.json", home);
   snprintf(config_toml, sizeof(config_toml), "%s/.codex/config.toml", home);

   char plugin_buf[4096];
   snprintf(plugin_buf, sizeof(plugin_buf),
            "{\n"
            "  \"name\": \"aimee\",\n"
            "  \"version\": \"%s\",\n"
            "  \"description\": "
            "\"Persistent memory, code search, blast-radius preview, and delegation "
            "for local coding sessions.\",\n"
            "  \"author\": {\n"
            "    \"name\": \"aimee\",\n"
            "    \"email\": \"support@example.invalid\",\n"
            "    \"url\": \"https://github.com/RakuenSoftware/aimee\"\n"
            "  },\n"
            "  \"homepage\": \"https://github.com/RakuenSoftware/aimee\",\n"
            "  \"repository\": \"https://github.com/RakuenSoftware/aimee\",\n"
            "  \"license\": \"MIT\",\n"
            "  \"keywords\": [\"memory\", \"mcp\", \"coding\", \"search\", \"delegation\"],\n"
            "  \"skills\": \"./skills/\",\n"
            "  \"mcpServers\": \"./.mcp.json\",\n"
            "  \"interface\": {\n"
            "    \"displayName\": \"aimee\",\n"
            "    \"shortDescription\": \"Memory, search, and delegation for Codex\",\n"
            "    \"longDescription\": "
            "\"Expose aimee's MCP server to Codex so sessions can search memory, "
            "inspect indexed code, preview blast radius, and delegate sub-tasks "
            "through the same local backend.\",\n"
            "    \"developerName\": \"aimee\",\n"
            "    \"category\": \"Coding\",\n"
            "    \"capabilities\": [\"Interactive\", \"Write\"],\n"
            "    \"websiteURL\": \"https://github.com/RakuenSoftware/aimee\",\n"
            "    \"privacyPolicyURL\": \"https://github.com/RakuenSoftware/aimee\",\n"
            "    \"termsOfServiceURL\": \"https://github.com/RakuenSoftware/aimee\",\n"
            "    \"defaultPrompt\": [\n"
            "      \"Search aimee memory before answering repo-specific questions\",\n"
            "      \"Explore the codebase through aimee's tools (find_symbol, "
            "ast_grep_search, search_graph) instead of raw grep/read\",\n"
            "      \"Preview the blast radius before editing multiple files\",\n"
            "      \"%s\",\n"
            "      \"Delegate bounded work through aimee delegate, not provider sub-agents\"\n"
            "    ],\n"
            "    \"brandColor\": \"#1F6FEB\",\n"
            "    \"screenshots\": []\n"
            "  }\n"
            "}\n",
            AIMEE_VERSION, codex_delegate_policy_prompt());

   char compat_plugin_buf[4096];
   snprintf(compat_plugin_buf, sizeof(compat_plugin_buf),
            "{\n"
            "  \"name\": \"aimee\",\n"
            "  \"version\": \"%s\",\n"
            "  \"description\": "
            "\"Persistent memory, code search, blast-radius preview, and delegation "
            "for local coding sessions.\",\n"
            "  \"author\": {\n"
            "    \"name\": \"aimee\",\n"
            "    \"email\": \"support@example.invalid\",\n"
            "    \"url\": \"https://github.com/RakuenSoftware/aimee\"\n"
            "  },\n"
            "  \"homepage\": \"https://github.com/RakuenSoftware/aimee\",\n"
            "  \"repository\": \"https://github.com/RakuenSoftware/aimee\",\n"
            "  \"license\": \"MIT\",\n"
            "  \"keywords\": [\"memory\", \"mcp\", \"coding\", \"search\", \"delegation\"],\n"
            "  \"skills\": \"./aimee/\",\n"
            "  \"mcpServers\": \"./.mcp.json\",\n"
            "  \"interface\": {\n"
            "    \"displayName\": \"aimee\",\n"
            "    \"shortDescription\": \"Memory, search, and delegation for Codex\",\n"
            "    \"longDescription\": "
            "\"Expose aimee's MCP server to Codex so sessions can search memory, "
            "inspect indexed code, preview blast radius, and delegate sub-tasks "
            "through the same local backend.\",\n"
            "    \"developerName\": \"aimee\",\n"
            "    \"category\": \"Coding\",\n"
            "    \"capabilities\": [\"Interactive\", \"Write\"],\n"
            "    \"websiteURL\": \"https://github.com/RakuenSoftware/aimee\",\n"
            "    \"privacyPolicyURL\": \"https://github.com/RakuenSoftware/aimee\",\n"
            "    \"termsOfServiceURL\": \"https://github.com/RakuenSoftware/aimee\",\n"
            "    \"defaultPrompt\": [\n"
            "      \"Search aimee memory before answering repo-specific questions\",\n"
            "      \"Explore the codebase through aimee's tools (find_symbol, "
            "ast_grep_search, search_graph) instead of raw grep/read\",\n"
            "      \"Preview the blast radius before editing multiple files\",\n"
            "      \"%s\",\n"
            "      \"Delegate bounded work through aimee delegate, not provider sub-agents\"\n"
            "    ],\n"
            "    \"brandColor\": \"#1F6FEB\",\n"
            "    \"screenshots\": []\n"
            "  }\n"
            "}\n",
            AIMEE_VERSION, codex_delegate_policy_prompt());

   char mcp_buf[MAX_PATH_LEN + 256];
   format_mcp_json(mcp_buf, sizeof(mcp_buf), aimee_bin);

   const char *skill_buf = codex_skill_markdown();

   write_text_file(plugin_json, plugin_buf, 0644);
   write_text_file(marketplace_plugin_json, plugin_buf, 0644);
   write_text_file(installed_plugin_json, plugin_buf, 0644);
   write_text_file(mcp_json, mcp_buf, 0644);
   write_text_file(marketplace_mcp_json, mcp_buf, 0644);
   write_text_file(installed_mcp_json, mcp_buf, 0644);
   write_text_file(compat_plugin_json, compat_plugin_buf, 0644);
   write_text_file(marketplace_compat_plugin_json, compat_plugin_buf, 0644);
   write_text_file(installed_compat_plugin_json, compat_plugin_buf, 0644);
   write_text_file(compat_mcp_json, mcp_buf, 0644);
   write_text_file(marketplace_compat_mcp_json, mcp_buf, 0644);
   write_text_file(installed_compat_mcp_json, mcp_buf, 0644);
   write_text_file(skill_md, skill_buf, 0644);
   write_text_file(marketplace_skill_md, skill_buf, 0644);
   write_text_file(installed_skill_md, skill_buf, 0644);
   ensure_codex_marketplace(marketplace);
   ensure_codex_plugin_enabled(config_toml);
}

/* --- Claude Code integration ---
 * Registers aimee MCP server in ~/.claude/settings.json and installs
 * custom slash commands to ~/.claude/commands/. */

static cJSON *read_json_file(const char *path)
{
   FILE *fp = fopen(path, "r");
   if (!fp)
      return NULL;
   fseek(fp, 0, SEEK_END);
   long sz = ftell(fp);
   fseek(fp, 0, SEEK_SET);
   if (sz < 0 || sz >= (long)(1 << 20))
   {
      fclose(fp);
      return NULL;
   }
   char *buf = malloc((size_t)sz + 1);
   if (!buf)
   {
      fclose(fp);
      return NULL;
   }
   size_t n = fread(buf, 1, (size_t)sz, fp);
   fclose(fp);
   buf[n] = '\0';
   cJSON *root = cJSON_Parse(buf);
   free(buf);
   return root;
}

static void ensure_claude_code_mcp(const char *settings_path)
{
   const char *aimee_bin = resolved_aimee_bin_path();
   struct stat st;
   if (stat(aimee_bin, &st) != 0)
      return;

   cJSON *root = read_json_file(settings_path);
   if (!cJSON_IsObject(root))
   {
      if (root)
         cJSON_Delete(root);
      root = cJSON_CreateObject();
   }

   /* Check if mcpServers.aimee already exists with the correct command */
   cJSON *servers = cJSON_GetObjectItemCaseSensitive(root, "mcpServers");
   if (cJSON_IsObject(servers))
   {
      cJSON *aimee = cJSON_GetObjectItemCaseSensitive(servers, "aimee");
      if (cJSON_IsObject(aimee))
      {
         cJSON *cmd = cJSON_GetObjectItemCaseSensitive(aimee, "command");
         cJSON *cmd_args = cJSON_GetObjectItemCaseSensitive(aimee, "args");
         if (cJSON_IsString(cmd) && strcmp(cmd->valuestring, aimee_bin) == 0 &&
             cJSON_IsArray(cmd_args) && cJSON_GetArraySize(cmd_args) == 1)
         {
            cJSON *arg0 = cJSON_GetArrayItem(cmd_args, 0);
            if (cJSON_IsString(arg0) && strcmp(arg0->valuestring, "mcp-serve") == 0)
            {
               cJSON_Delete(root);
               return; /* Already configured correctly */
            }
         }
      }
   }

   /* Ensure mcpServers object exists */
   if (!cJSON_IsObject(servers))
   {
      if (servers)
         cJSON_DeleteItemFromObjectCaseSensitive(root, "mcpServers");
      servers = cJSON_AddObjectToObject(root, "mcpServers");
   }

   /* Create or replace aimee entry */
   cJSON *existing = cJSON_GetObjectItemCaseSensitive(servers, "aimee");
   if (existing)
      cJSON_DeleteItemFromObjectCaseSensitive(servers, "aimee");

   cJSON *aimee_server = create_aimee_mcp_server(aimee_bin);
   cJSON_AddItemToObject(servers, "aimee", aimee_server);

   char *json = cJSON_Print(root);
   if (json)
   {
      write_text_file(settings_path, json, 0600);
      free(json);
   }
   cJSON_Delete(root);
}

/* Ensure PostToolUse hooks include EnterWorktree|ExitWorktree so that
 * aimee's CWD tracking file gets updated when the session enters/exits
 * a worktree. Without this, MCP git tools won't follow worktree changes. */
static void ensure_claude_code_hooks(const char *settings_path)
{
   cJSON *root = read_json_file(settings_path);
   if (!cJSON_IsObject(root))
   {
      cJSON_Delete(root);
      root = cJSON_CreateObject();
      if (!root)
         return;
   }

   int dirty = 0;
   cJSON *hooks = cJSON_GetObjectItemCaseSensitive(root, "hooks");
   if (!cJSON_IsObject(hooks))
   {
      if (hooks)
         cJSON_DeleteItemFromObjectCaseSensitive(root, "hooks");
      hooks = cJSON_AddObjectToObject(root, "hooks");
      dirty = 1;
   }

   cJSON *post = cJSON_GetObjectItemCaseSensitive(hooks, "PostToolUse");
   if (!cJSON_IsArray(post))
   {
      if (post)
         cJSON_DeleteItemFromObjectCaseSensitive(hooks, "PostToolUse");
      post = cJSON_AddArrayToObject(hooks, "PostToolUse");
      dirty = 1;
   }

   /* Find the aimee hooks post entry and check its matcher */
   int found_post_entry = 0;
   int n = cJSON_GetArraySize(post);
   for (int i = 0; i < n; i++)
   {
      cJSON *entry = cJSON_GetArrayItem(post, i);
      if (!cJSON_IsObject(entry))
         continue;
      cJSON *matcher = cJSON_GetObjectItemCaseSensitive(entry, "matcher");
      if (!cJSON_IsString(matcher))
         continue;
      /* Check if this is an aimee hooks entry */
      cJSON *hook_arr = cJSON_GetObjectItemCaseSensitive(entry, "hooks");
      if (!cJSON_IsArray(hook_arr))
         continue;
      int found_aimee = 0;
      for (int j = 0; j < cJSON_GetArraySize(hook_arr); j++)
      {
         cJSON *h = cJSON_GetArrayItem(hook_arr, j);
         cJSON *cmd = cJSON_GetObjectItemCaseSensitive(h, "command");
         if (cJSON_IsString(cmd) && (strstr(cmd->valuestring, "aimee hooks post") ||
                                     strstr(cmd->valuestring, "aimee-client hooks post")))
            found_aimee = 1;
      }
      if (!found_aimee)
         continue;
      found_post_entry = 1;

      /* This is the aimee PostToolUse entry — ensure Enter/ExitWorktree are in matcher. */
      int has_enter = strstr(matcher->valuestring, "EnterWorktree") != NULL;
      int has_exit = strstr(matcher->valuestring, "ExitWorktree") != NULL;
      if (!has_enter || !has_exit)
      {
         char new_matcher[512];
         snprintf(new_matcher, sizeof(new_matcher), "%s%s%s", matcher->valuestring,
                  has_enter ? "" : "|EnterWorktree", has_exit ? "" : "|ExitWorktree");
         cJSON_SetValuestring(matcher, new_matcher);
         dirty = 1;
      }
   }

   if (!found_post_entry)
   {
      const char *aimee_bin = resolved_aimee_bin_path();
      char post_cmd[512];
      snprintf(post_cmd, sizeof(post_cmd), "AIMEE_HOOK_CLIENT=claude %s hooks post",
               aimee_bin ? aimee_bin : "aimee");

      cJSON *entry = cJSON_CreateObject();
      cJSON *hook_arr = cJSON_CreateArray();
      cJSON *hook = cJSON_CreateObject();
      if (entry && hook_arr && hook)
      {
         cJSON_AddStringToObject(entry, "matcher",
                                 "Edit|Write|MultiEdit|EnterWorktree|ExitWorktree");
         cJSON_AddStringToObject(hook, "type", "command");
         cJSON_AddStringToObject(hook, "command", post_cmd);
         cJSON_AddItemToArray(hook_arr, hook);
         cJSON_AddItemToObject(entry, "hooks", hook_arr);
         cJSON_AddItemToArray(post, entry);
         dirty = 1;
      }
      else
      {
         cJSON_Delete(entry);
         cJSON_Delete(hook_arr);
         cJSON_Delete(hook);
      }
   }

   /* P1 context pre-injection: a UserPromptSubmit hook that injects a per-turn
    * <aimee-context> envelope (recall seeded by the prompt + an explore-with
    * pointer at aimee's tools) so Claude Code reasons over already-loaded
    * context instead of re-exploring. The hook fires on every prompt (no
    * matcher) and soft-fails, so it never blocks a turn. */
   cJSON *ups = cJSON_GetObjectItemCaseSensitive(hooks, "UserPromptSubmit");
   if (!cJSON_IsArray(ups))
   {
      if (ups)
         cJSON_DeleteItemFromObjectCaseSensitive(hooks, "UserPromptSubmit");
      ups = cJSON_AddArrayToObject(hooks, "UserPromptSubmit");
      dirty = 1;
   }
   int found_ups = 0;
   for (int i = 0; i < cJSON_GetArraySize(ups); i++)
   {
      cJSON *hook_arr = cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(ups, i), "hooks");
      if (!cJSON_IsArray(hook_arr))
         continue;
      for (int j = 0; j < cJSON_GetArraySize(hook_arr); j++)
      {
         cJSON *cmd = cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(hook_arr, j), "command");
         if (cJSON_IsString(cmd) && strstr(cmd->valuestring, "user-prompt-submit"))
            found_ups = 1;
      }
   }
   if (!found_ups)
   {
      const char *aimee_bin = resolved_aimee_bin_path();
      char ups_cmd[512];
      snprintf(ups_cmd, sizeof(ups_cmd), "AIMEE_HOOK_CLIENT=claude %s user-prompt-submit",
               aimee_bin ? aimee_bin : "aimee");
      cJSON *entry = cJSON_CreateObject();
      cJSON *hook_arr = cJSON_CreateArray();
      cJSON *hook = cJSON_CreateObject();
      if (entry && hook_arr && hook)
      {
         cJSON_AddStringToObject(hook, "type", "command");
         cJSON_AddStringToObject(hook, "command", ups_cmd);
         cJSON_AddItemToArray(hook_arr, hook);
         cJSON_AddItemToObject(entry, "hooks", hook_arr);
         cJSON_AddItemToArray(ups, entry);
         dirty = 1;
      }
      else
      {
         cJSON_Delete(entry);
         cJSON_Delete(hook_arr);
         cJSON_Delete(hook);
      }
   }

   if (dirty)
   {
      char *json = cJSON_Print(root);
      if (json)
      {
         write_text_file(settings_path, json, 0600);
         free(json);
      }
   }
   cJSON_Delete(root);
}

static void ensure_claude_code_commands(const char *home)
{
   char path[MAX_PATH_LEN];

   snprintf(path, sizeof(path), "%s/.claude/commands/aimee-search.md", home);
   write_text_file(path,
                   "Search aimee memory for project facts, prior decisions, and stored context.\n"
                   "\n"
                   "Use the aimee MCP tool `search_memory` with the query: $ARGUMENTS\n"
                   "\n"
                   "If no query is provided, use `list_facts` to show all stored facts.\n",
                   0644);

   snprintf(path, sizeof(path), "%s/.claude/commands/aimee-delegate.md", home);
   write_text_file(path,
                   "Delegate a bounded sub-task to an aimee delegate agent.\n"
                   "\n"
                   "Use the aimee MCP tool `delegate` with the task: $ARGUMENTS\n"
                   "\n"
                   "Do not use provider-native sub-agent tools such as Claude Agent.\n"
                   "\n"
                   "The delegate will execute the task using the cheapest suitable model\n"
                   "and return the result. Only delegate bounded, well-defined tasks.\n",
                   0644);

   snprintf(path, sizeof(path), "%s/.claude/commands/aimee-blast-radius.md", home);
   write_text_file(path,
                   "Preview the blast radius of a multi-file edit before making changes.\n"
                   "\n"
                   "Use the aimee MCP tool `preview_blast_radius` for: $ARGUMENTS\n"
                   "\n"
                   "This shows which files and symbols would be affected by the change,\n"
                   "helping you understand the impact before editing.\n",
                   0644);
}

/* Normalize an aimee server URL into an Anthropic base URL: Claude Code appends
 * "/v1/messages", so we want the origin without a trailing "/" or "/v1".
 * Writes into out (caller-sized). */
static void normalize_anthropic_base(const char *server_url, char *out, size_t out_len)
{
   size_t n;
   snprintf(out, out_len, "%s", server_url ? server_url : "");
   n = strlen(out);
   while (n > 0 && out[n - 1] == '/')
      out[--n] = '\0';
   if (n >= 3 && strcmp(out + n - 3, "/v1") == 0)
      out[n - 3] = '\0';
}

/* Enable or disable routing Claude Code through aimee's Anthropic Messages
 * ingress by writing (enable) or removing (disable) ANTHROPIC_BASE_URL and
 * ANTHROPIC_AUTH_TOKEN under the "env" key of ~/.claude/settings.json.
 *
 * Enabling reroutes ALL of the operator's Claude Code traffic — including any
 * live session — off Anthropic to aimee's primary model, so it is only ever
 * invoked explicitly via `aimee claude-proxy enable`. server_url is the
 * aimee-server origin (required on enable); token is the server bearer (may be
 * NULL/empty → a local placeholder is written, since Claude Code always sends
 * an auth value). Returns 0 on success, -1 on error. */
int claude_code_proxy_configure(const char *server_url, const char *token, int enable)
{
   const char *home = getenv("HOME");
   char settings_path[MAX_PATH_LEN];
   cJSON *root, *env;
   char *json;
   int rc = -1;

   if (!home || !home[0])
      return -1;
   if (enable && (!server_url || !server_url[0]))
      return -1;

   snprintf(settings_path, sizeof(settings_path), "%s/.claude/settings.json", home);
   root = read_json_file(settings_path);
   if (!cJSON_IsObject(root))
   {
      if (root)
         cJSON_Delete(root);
      if (!enable)
         return 0; /* disabling a never-enabled proxy is a no-op success */
      root = cJSON_CreateObject();
      if (!root)
         return -1;
   }

   env = cJSON_GetObjectItemCaseSensitive(root, "env");
   if (!cJSON_IsObject(env))
   {
      if (env)
         cJSON_DeleteItemFromObjectCaseSensitive(root, "env");
      if (!enable)
      {
         cJSON_Delete(root); /* nothing to remove */
         return 0;
      }
      env = cJSON_AddObjectToObject(root, "env");
   }

   cJSON_DeleteItemFromObjectCaseSensitive(env, "ANTHROPIC_BASE_URL");
   cJSON_DeleteItemFromObjectCaseSensitive(env, "ANTHROPIC_AUTH_TOKEN");
   if (enable)
   {
      char base[MAX_PATH_LEN];
      normalize_anthropic_base(server_url, base, sizeof(base));
      cJSON_AddStringToObject(env, "ANTHROPIC_BASE_URL", base);
      cJSON_AddStringToObject(env, "ANTHROPIC_AUTH_TOKEN",
                              (token && token[0]) ? token : "aimee-local");
   }

   json = cJSON_Print(root);
   if (json)
   {
      write_text_file(settings_path, json, 0600);
      free(json);
      rc = 0;
   }
   cJSON_Delete(root);
   return rc;
}

/* Ensure the "env" key in settings.json has required environment variables.
 * Currently sets CLAUDE_BASH_MAINTAIN_PROJECT_WORKING_DIR=0 so that cd
 * commands persist across Bash calls, enabling worktree workflows. */
static void ensure_claude_code_env(const char *settings_path)
{
   cJSON *root = read_json_file(settings_path);
   if (!cJSON_IsObject(root))
   {
      cJSON_Delete(root);
      return;
   }

   int dirty = 0;
   cJSON *env = cJSON_GetObjectItemCaseSensitive(root, "env");
   if (!cJSON_IsObject(env))
   {
      if (env)
         cJSON_DeleteItemFromObjectCaseSensitive(root, "env");
      env = cJSON_AddObjectToObject(root, "env");
      dirty = 1;
   }

   cJSON *cwd_var =
       cJSON_GetObjectItemCaseSensitive(env, "CLAUDE_BASH_MAINTAIN_PROJECT_WORKING_DIR");
   if (!cJSON_IsString(cwd_var) || strcmp(cwd_var->valuestring, "0") != 0)
   {
      if (cwd_var)
         cJSON_DeleteItemFromObjectCaseSensitive(env, "CLAUDE_BASH_MAINTAIN_PROJECT_WORKING_DIR");
      cJSON_AddStringToObject(env, "CLAUDE_BASH_MAINTAIN_PROJECT_WORKING_DIR", "0");
      dirty = 1;
   }

   if (dirty)
   {
      char *json = cJSON_Print(root);
      if (json)
      {
         write_text_file(settings_path, json, 0600);
         free(json);
      }
   }
   cJSON_Delete(root);
}

/* Ensure ~/.claude.json has hasTrustDialogAccepted=true and
 * enabledMcpjsonServers=["aimee"] for the given directory path.
 *
 * Claude Code shows a trust dialog when a session opens a directory containing
 * a .mcp.json for the first time. Delegate worktrees are spawned non-
 * interactively by aimee-server, so no user is present to accept the dialog —
 * Claude Code silently rejects the project-scoped MCP server and the session
 * starts without aimee tools. Pre-accepting trust here fixes that. */
void ensure_claude_code_trust(const char *dir)
{
   if (!dir || !dir[0])
      return;
   const char *home = getenv("HOME");
   if (!home)
      return;

   char claude_json[MAX_PATH_LEN];
   snprintf(claude_json, sizeof(claude_json), "%s/.claude.json", home);

   cJSON *root = read_json_file(claude_json);
   if (!cJSON_IsObject(root))
   {
      cJSON_Delete(root);
      return; /* Don't create .claude.json from scratch */
   }

   cJSON *projects = cJSON_GetObjectItemCaseSensitive(root, "projects");
   if (!cJSON_IsObject(projects))
   {
      cJSON_Delete(root);
      return;
   }

   int dirty = 0;
   cJSON *proj = cJSON_GetObjectItemCaseSensitive(projects, dir);
   if (!cJSON_IsObject(proj))
   {
      if (proj)
         cJSON_DeleteItemFromObjectCaseSensitive(projects, dir);
      proj = cJSON_AddObjectToObject(projects, dir);
      dirty = 1;
   }

   /* Ensure hasTrustDialogAccepted is true */
   cJSON *trust = cJSON_GetObjectItemCaseSensitive(proj, "hasTrustDialogAccepted");
   if (!cJSON_IsTrue(trust))
   {
      if (trust)
         cJSON_DeleteItemFromObjectCaseSensitive(proj, "hasTrustDialogAccepted");
      cJSON_AddTrueToObject(proj, "hasTrustDialogAccepted");
      dirty = 1;
   }

   /* Ensure enabledMcpjsonServers array exists and contains "aimee" */
   cJSON *enabled = cJSON_GetObjectItemCaseSensitive(proj, "enabledMcpjsonServers");
   if (!cJSON_IsArray(enabled))
   {
      if (enabled)
         cJSON_DeleteItemFromObjectCaseSensitive(proj, "enabledMcpjsonServers");
      enabled = cJSON_AddArrayToObject(proj, "enabledMcpjsonServers");
      dirty = 1;
   }
   int found_aimee = 0;
   for (int i = 0; i < cJSON_GetArraySize(enabled); i++)
   {
      cJSON *item = cJSON_GetArrayItem(enabled, i);
      if (cJSON_IsString(item) && strcmp(item->valuestring, "aimee") == 0)
      {
         found_aimee = 1;
         break;
      }
   }
   if (!found_aimee)
   {
      cJSON_AddItemToArray(enabled, cJSON_CreateString("aimee"));
      dirty = 1;
   }

   /* Ensure disabledMcpjsonServers array exists (Claude Code expects it) */
   cJSON *disabled = cJSON_GetObjectItemCaseSensitive(proj, "disabledMcpjsonServers");
   if (!cJSON_IsArray(disabled))
   {
      if (disabled)
         cJSON_DeleteItemFromObjectCaseSensitive(proj, "disabledMcpjsonServers");
      cJSON_AddArrayToObject(proj, "disabledMcpjsonServers");
      dirty = 1;
   }

   if (dirty)
   {
      char *json = cJSON_Print(root);
      if (json)
      {
         write_text_file(claude_json, json, 0600);
         free(json);
      }
   }
   cJSON_Delete(root);
}

static void ensure_claude_code_integration(const char *home)
{
   const char *aimee_bin = resolved_aimee_bin_path();
   struct stat st;
   if (stat(aimee_bin, &st) != 0)
      return;

   char settings_path[MAX_PATH_LEN];
   snprintf(settings_path, sizeof(settings_path), "%s/.claude/settings.json", home);
   ensure_claude_code_mcp(settings_path);
   ensure_claude_code_hooks(settings_path);
   ensure_claude_code_env(settings_path);
   ensure_claude_code_commands(home);
}

static void ensure_gemini_integration(const char *home)
{
   const char *aimee_bin = resolved_aimee_bin_path();
   struct stat st;
   if (stat(aimee_bin, &st) != 0)
      return;

   char path[MAX_PATH_LEN];
   snprintf(path, sizeof(path), "%s/.gemini/settings.json", home);

   cJSON *root = read_json_file(path);
   if (!cJSON_IsObject(root))
   {
      if (root)
         cJSON_Delete(root);
      root = cJSON_CreateObject();
   }

   cJSON *servers = cJSON_GetObjectItemCaseSensitive(root, "mcpServers");
   if (!cJSON_IsObject(servers))
   {
      if (servers)
         cJSON_DeleteItemFromObjectCaseSensitive(root, "mcpServers");
      servers = cJSON_AddObjectToObject(root, "mcpServers");
   }

   cJSON *aimee = cJSON_GetObjectItemCaseSensitive(servers, "aimee");
   if (aimee)
      cJSON_DeleteItemFromObjectCaseSensitive(servers, "aimee");

   cJSON *aimee_server = create_aimee_mcp_server(aimee_bin);
   cJSON_AddItemToObject(servers, "aimee", aimee_server);

   char *json = cJSON_Print(root);
   if (json)
   {
      write_text_file(path, json, 0600);
      free(json);
   }
   cJSON_Delete(root);
}

static void ensure_copilot_integration(const char *home)
{
   const char *aimee_bin = resolved_aimee_bin_path();
   struct stat st;
   if (stat(aimee_bin, &st) != 0)
      return;

   char path[MAX_PATH_LEN];
   snprintf(path, sizeof(path), "%s/.copilot/mcp-config.json", home);

   cJSON *root = read_json_file(path);
   if (!cJSON_IsObject(root))
   {
      if (root)
         cJSON_Delete(root);
      root = cJSON_CreateObject();
   }

   cJSON *servers = cJSON_GetObjectItemCaseSensitive(root, "mcpServers");
   if (!cJSON_IsObject(servers))
   {
      if (servers)
         cJSON_DeleteItemFromObjectCaseSensitive(root, "mcpServers");
      servers = cJSON_AddObjectToObject(root, "mcpServers");
   }

   cJSON *aimee = cJSON_GetObjectItemCaseSensitive(servers, "aimee");
   if (aimee)
      cJSON_DeleteItemFromObjectCaseSensitive(servers, "aimee");

   cJSON *aimee_server = create_aimee_mcp_server(aimee_bin);
   cJSON_AddItemToObject(servers, "aimee", aimee_server);

   char *json = cJSON_Print(root);
   if (json)
   {
      write_text_file(path, json, 0600);
      free(json);
   }
   cJSON_Delete(root);
}

void ensure_client_integrations(void)
{
   const char *home = platform_home_dir();
   if (!home || !home[0])
      return;

   struct stat st;

   char codex_dir[MAX_PATH_LEN];
   snprintf(codex_dir, sizeof(codex_dir), "%s/.codex", home);
   if (stat(codex_dir, &st) == 0 && S_ISDIR(st.st_mode))
      ensure_codex_plugin_files(home);

   char claude_dir[MAX_PATH_LEN];
   snprintf(claude_dir, sizeof(claude_dir), "%s/.claude", home);
   if (stat(claude_dir, &st) == 0 && S_ISDIR(st.st_mode))
      ensure_claude_code_integration(home);

   char gemini_dir[MAX_PATH_LEN];
   snprintf(gemini_dir, sizeof(gemini_dir), "%s/.gemini", home);
   if (stat(gemini_dir, &st) == 0 && S_ISDIR(st.st_mode))
      ensure_gemini_integration(home);

   char copilot_dir[MAX_PATH_LEN];
   snprintf(copilot_dir, sizeof(copilot_dir), "%s/.copilot", home);
   if (stat(copilot_dir, &st) == 0 && S_ISDIR(st.st_mode))
      ensure_copilot_integration(home);
}
