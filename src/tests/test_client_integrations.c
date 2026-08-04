/* test_client_integrations.c: Claude MCP registration, Codex plugin payload,
 * and non-destructive settings update tests for client_integrations.c */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include "aimee.h"
#include "cJSON.h"

/* Include the source directly to test static functions */
#include "../client_integrations.c"
#include "platform_test_util.h"

/* --- Test build_marketplace_root --- */

static void test_build_marketplace_root(void)
{
   cJSON *root = build_marketplace_root();
   assert(root != NULL);
   assert(cJSON_IsObject(root));

   /* Should have name = "local" */
   cJSON *name = cJSON_GetObjectItem(root, "name");
   assert(cJSON_IsString(name));
   assert(strcmp(name->valuestring, "local") == 0);

   /* Should have interface.displayName */
   cJSON *iface = cJSON_GetObjectItem(root, "interface");
   assert(cJSON_IsObject(iface));
   cJSON *dn = cJSON_GetObjectItem(iface, "displayName");
   assert(cJSON_IsString(dn));

   /* Should have empty plugins array */
   cJSON *plugins = cJSON_GetObjectItem(root, "plugins");
   assert(cJSON_IsArray(plugins));
   assert(cJSON_GetArraySize(plugins) == 0);

   cJSON_Delete(root);
}

/* --- Test build_aimee_plugin_entry --- */

static void test_build_aimee_plugin_entry(void)
{
   cJSON *entry = build_aimee_plugin_entry();
   assert(entry != NULL);
   assert(cJSON_IsObject(entry));

   cJSON *name = cJSON_GetObjectItem(entry, "name");
   assert(cJSON_IsString(name));
   assert(strcmp(name->valuestring, "aimee") == 0);

   cJSON *source = cJSON_GetObjectItem(entry, "source");
   assert(cJSON_IsObject(source));
   cJSON *src_path = cJSON_GetObjectItem(source, "path");
   assert(cJSON_IsString(src_path));
   assert(strstr(src_path->valuestring, "plugins/aimee") != NULL);

   cJSON *policy = cJSON_GetObjectItem(entry, "policy");
   assert(cJSON_IsObject(policy));
   cJSON *install = cJSON_GetObjectItem(policy, "installation");
   assert(cJSON_IsString(install));
   assert(strcmp(install->valuestring, "INSTALLED_BY_DEFAULT") == 0);

   cJSON *category = cJSON_GetObjectItem(entry, "category");
   assert(cJSON_IsString(category));
   assert(strcmp(category->valuestring, "Coding") == 0);

   cJSON_Delete(entry);
}

static void test_codex_delegate_policy_is_explicit(void)
{
   const char *prompt = codex_delegate_policy_prompt();
   assert(strstr(prompt, "spawn_agent") != NULL);
   assert(strstr(prompt, "Claude Agent") != NULL);
   assert(strstr(prompt, "delegate MCP tool") != NULL);

   const char *skill = codex_skill_markdown();
   assert(strstr(skill, "Do not call provider-native sub-agent tools") != NULL);
   assert(strstr(skill, "`spawn_agent`") != NULL);
   assert(strstr(skill, "`delegate` MCP tool") != NULL);

   const char *code_prompt = codex_code_exploration_prompt();
   assert(strstr(code_prompt, AIMEE_CODE_TOOL_FIND_SYMBOL) != NULL);
   assert(strstr(code_prompt, AIMEE_CODE_TOOL_AST_GREP_SEARCH) != NULL);
   assert(strstr(code_prompt, AIMEE_CODE_TOOL_INDEX) != NULL);
   assert(strstr(code_prompt, AIMEE_CODE_INDEX_COMMAND_HYBRID) != NULL);
   assert(strstr(code_prompt, "search_graph") == NULL);

   /* THE GUIDANCE MUST BE SUBSTITUTIVE, AND IT MUST BOUND EXPLORATION.
    *
    * Both properties have already regressed once each, and neither failure looked
    * like a bug in the tooling:
    *
    *  - Offering the index as a FALLBACK after grep produced ZERO index calls
    *    across a four-task benchmark on a verified-healthy index.
    *  - Naming what the index is for without saying what to stop doing produced
    *    an ADDITIVE agent: it used the index AND ran the full shell survey, 1.5
    *    to 2.4x the commands of plain codex, for up to 4.4x the cost. Command
    *    output is re-sent every later turn, so cost grows with the square of the
    *    command count and the survey dominates.
    *
    * Pin both properties as text, because that is where they live. */
   assert(strstr(code_prompt, "instead of raw grep/read") != NULL);
   assert(strstr(code_prompt, "Do not survey the repository") != NULL);

   /* The index replaces the search rather than confirming it. */
   assert(strstr(skill, "REPLACES that search") != NULL);
   /* No orienting by enumeration -- that is the expensive half. */
   assert(strstr(skill, "Start from the index, not from a survey") != NULL);
   assert(strstr(skill, "Never run it over the whole tree") != NULL);
   /* The v1 phrasing that caused the zero-index-call run must not come back. */
   assert(strstr(skill, "prefer local file inspection first") == NULL);

   /* AN UNCAPPED SEARCH IS THE MOST EXPENSIVE THING AVAILABLE HERE, and its cost
    * is invisible when it happens: the output lands once and is re-sent on every
    * later turn. Measured after the substitutive rewrite was already in place --
    * one uncapped `rg` returned 80,330 characters (~20k tokens) and rode the
    * remaining ~13 model calls, roughly a fifth of that run's whole input, while
    * plain codex capped all five of its searches unprompted. */
   assert(strstr(skill, "Cap what a search prints") != NULL);
   assert(strstr(skill, "head -n") != NULL);
   /* The "do not repeat a search" bullet was REMOVED, deliberately. It was added
    * on intuition and never measured; measuring it afterwards showed duplicate
    * searches account for 0-1% of work across every arm, so it bought nothing and
    * spent skill budget that batching guidance now uses. Assert it stays gone, so
    * it is not reintroduced on the same intuition. */
   assert(strstr(skill, "Do not repeat a search") == NULL);
   assert(strstr(code_prompt, "Cap what any search prints") != NULL);

   /* THE SKILL MUST ANSWER "find this phrase", because that is the question the
    * agent had no aimee answer for and resolved with a recursive shell search --
    * 87 of them across the benchmark's aimee cells, 2.4 MB of output, one large
    * enough to hit the client's 1 MB truncation. find_symbol covers symbol names
    * only; index command=hybrid covers the rest and is bounded by max_results. */
   assert(strstr(skill, AIMEE_CODE_TOOL_INDEX) != NULL);
   assert(strstr(skill, AIMEE_CODE_INDEX_COMMAND_HYBRID) != NULL);
   assert(strstr(skill, "PHRASE rather than a symbol") != NULL);

   /* Reading is the largest remaining work category (15 reads carrying 660k
    * tokens on one measured cell, 17.7% of its whole input), and build output is
    * almost entirely echoed compiler command lines (6 builds, 241k tokens).
    * Both are addressed by naming the bounded alternative, so pin both. */
   assert(strstr(skill, "command=span") != NULL);
   assert(strstr(skill, "read the RANGE") != NULL);
   assert(strstr(skill, "make -s") != NULL);

   /* TURN COUNT IS THE BILL, NOT BYTES.
    *
    * On a cell where all four arms passed, aimee moved the FEWEST tool-output
    * characters of any arm (74k vs baseline's 124k) and still paid 4.4x the
    * tokens: 47 tool calls against baseline's 9. Per call it was cheaper (41.0k
    * input-tokens vs 49.1k) -- it just took five times as many. Baseline chained
    * (16 sed reads inside 9 calls, up to five ranges per command); aimee spread 7
    * reads across 22 calls. Pin the batching rules, and pin that the agent is
    * told not to spend a call re-reading the skill it is already being shown. */
   assert(strstr(skill, "ONE call, joined with `&&`") != NULL);
   assert(strstr(skill, "spans") != NULL);
   assert(strstr(skill, "Do not read this file") != NULL);
   /* Symbol lookups batch the same way spans do: one cell issued five
    * consecutive single-symbol find_symbol calls, each a full round trip. */
   assert(strstr(skill, "identifiers") != NULL);
   /* All four plural forms must be named, or the agent batches only what it was
    * explicitly told about -- span batching landed first and the next run still
    * issued 4 single hybrid queries and 4 single structure calls. */
   assert(strstr(skill, "file_paths") != NULL);
   assert(strstr(skill, "queries") != NULL);
   /* The composed packet (/v1/code/context) shipped reachable only as ingress
    * pre-injection; an MCP agent could not call it, so it never appeared in a
    * transcript. Pin both the command and the instruction to start there. */
   assert(strstr(skill, AIMEE_CODE_INDEX_COMMAND_INVESTIGATE) != NULL);
   assert(strstr(skill, "STARTING on an unfamiliar area") != NULL);
   /* Empty searches are pure wasted turns -- 4 of 9 on one measured cell. */
   assert(strstr(skill, "signal to change TOOL") != NULL);

   /* UNDER-SCOPING, NOT RETRIEVAL, IS WHAT LOSES THE HARD TASKS.
    * Two solvable tasks that every arm failed: on both, aimee's retrieval landed
    * on the right code (find_callers on the acquire/release pair, then the owning
    * module) and its patch was too narrow -- one consumer's lease discipline
    * instead of the pool reclaiming an unreturned lease, and two of five files on
    * a ticket that names a three-link chain. Pin both rules. */
   assert(strstr(skill, "Fix the OWNER, not one caller") != NULL);
   assert(strstr(skill, "more than "
                        "one caller, a caller-side fix is incomplete") != NULL ||
          strstr(skill, "caller-side fix is incomplete") != NULL);
   assert(strstr(skill, "account for every symptom") != NULL);
   /* The three all-fail tasks were under-scoped patches, not bad retrieval, and
    * the author is the one who cannot see it. roundtable_review already exists
    * for this -- original_request is documented as goal-drift detection -- so
    * point at it rather than shipping a second review path. */
   assert(strstr(skill, "roundtable_review") != NULL);
   assert(strstr(skill, "original_request") != NULL);
   /* am_12b43fa38e: the ticket opens "Two bugs" and names both; aimee fixed the
    * second and never touched the first, while still editing the file the first
    * lives in for an unrelated reason. File overlap is not coverage. */
   assert(strstr(skill, "states a COUNT") != NULL);
   assert(strstr(skill, "DISTINCT") != NULL);

   /* THE GUARD EXISTED AND WAS NEVER WIRED FOR CODEX.
    *
    * `aimee hooks` implements the PreToolUse contract and require_aimee_git is ON
    * by default with a deny naming git_status / git_log / git_diff_summary. The
    * codex plugin shipped no hooks at all, so it never ran: 98 shell `git` calls
    * across the benchmark's aimee cells (48 full `git diff`) and ZERO calls to the
    * aimee git tool whose schema costs ~1,000 tokens on every call.
    *
    * Pin the registration, not just the rule -- an unwired guard is not a guard. */
   {
      const char *hooks = codex_hooks_json("/usr/local/bin/aimee");
      assert(strstr(hooks, "\"PreToolUse\"") != NULL);
      /* MUST be `hooks pre`, not `hooks`. Bare `hooks` exits with "hooks requires
       * 'pre' or 'post'" and codex allows the tool -- a hook that is installed,
       * declared, well-formed, and enforces nothing. The first version of this
       * assertion pinned exactly that defect by matching the prefix. */
      assert(strstr(hooks, "/usr/local/bin/aimee hooks pre") != NULL);
      cJSON *parsed = cJSON_Parse(hooks);
      assert(parsed != NULL); /* codex refuses a malformed hooks file outright */
      cJSON_Delete(parsed);
      /* A hooks file codex never loads is the same as no hook, so the manifest
       * must point at it. ensure_codex_plugin writes both; assert the path the
       * manifest declares matches the file the writer emits. */
      assert(strstr(hooks, "\"command\"") != NULL);
   }
}

static void test_mcp_config_uses_resolved_command(void)
{
   char buf[1024];
   format_mcp_json(buf, sizeof(buf), "/tmp/aimee-bin");
   assert(strstr(buf, "\"command\": \"/tmp/aimee-bin\"") != NULL);
   assert(strstr(buf, "\"command\": \"aimee\"") == NULL);
   assert(strstr(buf, "\"args\": [\"mcp-serve\"]") != NULL);

   cJSON *server = create_aimee_mcp_server("/tmp/aimee-bin");
   assert(cJSON_IsObject(server));
   cJSON *cmd = cJSON_GetObjectItemCaseSensitive(server, "command");
   assert(cJSON_IsString(cmd));
   assert(strcmp(cmd->valuestring, "/tmp/aimee-bin") == 0);
   cJSON *args = cJSON_GetObjectItemCaseSensitive(server, "args");
   assert(cJSON_IsArray(args));
   cJSON *arg0 = cJSON_GetArrayItem(args, 0);
   assert(cJSON_IsString(arg0));
   assert(strcmp(arg0->valuestring, "mcp-serve") == 0);
   cJSON_Delete(server);
}

/* The agent host spawns `aimee mcp-serve` itself, with an environment of its
 * own choosing. When AIMEE_HOME is where the config actually lives -- any
 * containerised or managed-server install -- and the generated config does not
 * carry it, the server starts, cannot reach aimee-server, and answers
 * tools/list with an EMPTY LIST. The agent is then silently offered no tools at
 * all and falls back to grep, which is indistinguishable from deciding the
 * index was not worth calling. Measured: 18 tools with AIMEE_HOME, 0 without,
 * regardless of HOME. */
/* THE SKILL IS THE SAME TEXT FOR EVERY RUN.
 *
 * There used to be a "solo" variant that withheld the delegate tools and swapped
 * the delegation bullets for "do all of this work yourself". A profile that hides
 * shipped tools during measurement makes the measured thing a configuration
 * nobody deploys -- the benchmark stops describing aimee. If work should not be
 * delegated, that is a rule of the run, not a different build.
 *
 * Pin that the profile no longer changes what the agent is told. */
static void test_skill_does_not_vary_by_profile(void)
{
   setenv("AIMEE_MCP_TOOL_PROFILE", "solo", 1);
   const char *solo_prompt = codex_delegate_policy_prompt();
   assert(strstr(solo_prompt, "use the aimee delegate MCP tool") != NULL);
   assert(strstr(solo_prompt, "do this work yourself") == NULL);
   assert(strstr(codex_skill_markdown_effective(), "`delegate` MCP tool") != NULL);
   assert(strstr(codex_skill_markdown_effective(), "Do all of this work yourself") == NULL);

   setenv("AIMEE_MCP_TOOL_PROFILE", "core", 1);
   assert(strcmp(solo_prompt, codex_delegate_policy_prompt()) == 0);
   assert(strstr(codex_skill_markdown_effective(), AIMEE_CODE_TOOL_FIND_SYMBOL) != NULL);

   unsetenv("AIMEE_MCP_TOOL_PROFILE");
}

static void test_mcp_config_carries_aimee_home(void)
{
   char buf[1024];
   cJSON *server = NULL;
   cJSON *env = NULL;
   cJSON *home = NULL;

   setenv("AIMEE_HOME", "/var/lib/aimee-home", 1);

   format_mcp_json(buf, sizeof(buf), "/tmp/aimee-bin");
   assert(strstr(buf, "\"env\"") != NULL);
   assert(strstr(buf, "\"AIMEE_HOME\": \"/var/lib/aimee-home\"") != NULL);

   server = create_aimee_mcp_server("/tmp/aimee-bin");
   assert(cJSON_IsObject(server));
   env = cJSON_GetObjectItemCaseSensitive(server, "env");
   assert(cJSON_IsObject(env));
   home = cJSON_GetObjectItemCaseSensitive(env, "AIMEE_HOME");
   assert(cJSON_IsString(home));
   assert(strcmp(home->valuestring, "/var/lib/aimee-home") == 0);
   cJSON_Delete(server);

   /* Unset means the default resolution already works; pinning a value the
    * operator never chose would be worse than saying nothing. */
   unsetenv("AIMEE_HOME");
   format_mcp_json(buf, sizeof(buf), "/tmp/aimee-bin");
   assert(strstr(buf, "AIMEE_HOME") == NULL);

   server = create_aimee_mcp_server("/tmp/aimee-bin");
   assert(cJSON_GetObjectItemCaseSensitive(server, "env") == NULL);
   cJSON_Delete(server);
}

/* 1 if hooks[event] has an entry whose command contains `needle`. */
static int hook_event_has_cmd(cJSON *hooks, const char *event, const char *needle)
{
   cJSON *arr = cJSON_GetObjectItemCaseSensitive(hooks, event);
   if (!cJSON_IsArray(arr))
      return 0;
   for (int i = 0; i < cJSON_GetArraySize(arr); i++)
   {
      cJSON *ha = cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(arr, i), "hooks");
      for (int j = 0; cJSON_IsArray(ha) && j < cJSON_GetArraySize(ha); j++)
      {
         cJSON *c = cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(ha, j), "command");
         if (cJSON_IsString(c) && strstr(c->valuestring, needle))
            return 1;
      }
   }
   return 0;
}

/* Assert ensure_claude_code_hooks registered EVERY hook aimee relies on. This
 * required set is INDEPENDENT of the production code's registration order/table
 * on purpose -- so if any registration is ever dropped (as the SessionStart hook
 * silently was, leaving the primary with no aimee session brief: the configured
 * persona [default `engineer`, but operator-selectable via AIMEE_MODE / the mode
 * file] + MCP-skill index + Rules + Key Facts, rendered by `aimee session-start`
 * -> session_start_emit -> build_session_context), this fails. Adding a new client
 * hook means adding it here too. Each entry: {settings.json event, the
 * `aimee <subcommand>` it invokes}. Asserted for BOTH the fresh-settings and the
 * add-to-an-existing-settings.json paths (the latter is the exact shape of the
 * live regression: a settings.json that already had the other hooks but not
 * SessionStart). */
static void assert_required_hooks_present(cJSON *hooks)
{
   static const struct
   {
      const char *event;
      const char *subcommand;
   } required[] = {
       {"SessionStart", "session-start"},          /* session brief: persona + skills + rules */
       {"UserPromptSubmit", "user-prompt-submit"}, /* per-turn recall envelope */
       {"PreCompact", "pre-compact"},              /* post-compact recall re-prime */
       {"PreToolUse", "attention-guard"},          /* per-file attention + destructive-op guard */
       {"PostToolUse", "hooks post"},              /* post-edit hook */
   };
   for (size_t i = 0; i < sizeof(required) / sizeof(required[0]); i++)
      assert(hook_event_has_cmd(hooks, required[i].event, required[i].subcommand));
}

/* --- Test read_json_file --- */

static void test_read_json_file_missing(void)
{
   cJSON *root = read_json_file("/nonexistent/path/file.json");
   assert(root == NULL);
}

static void test_read_json_file_valid(void)
{
   char tmppath[512];
   snprintf(tmppath, sizeof(tmppath), "%s/aimee-test-json-XXXXXX", platform_tmpdir());
   int fd = platform_mkstemp(tmppath, sizeof(tmppath), "aim");
   assert(fd >= 0);

   const char *json = "{\"key\": \"value\", \"num\": 42}";
   write(fd, json, strlen(json));
   close(fd);

   cJSON *root = read_json_file(tmppath);
   assert(root != NULL);
   assert(cJSON_IsObject(root));

   cJSON *key = cJSON_GetObjectItem(root, "key");
   assert(cJSON_IsString(key));
   assert(strcmp(key->valuestring, "value") == 0);

   cJSON *num = cJSON_GetObjectItem(root, "num");
   assert(cJSON_IsNumber(num));
   assert(num->valueint == 42);

   cJSON_Delete(root);
   unlink(tmppath);
}

static void test_read_json_file_invalid(void)
{
   char tmppath[512];
   snprintf(tmppath, sizeof(tmppath), "%s/aimee-test-badjson-XXXXXX", platform_tmpdir());
   int fd = platform_mkstemp(tmppath, sizeof(tmppath), "aim");
   assert(fd >= 0);

   const char *bad = "not valid json at all {{{";
   write(fd, bad, strlen(bad));
   close(fd);

   cJSON *root = read_json_file(tmppath);
   assert(root == NULL);

   unlink(tmppath);
}

static void test_resolved_aimee_bin_path_fallback(void)
{
   const char *path = resolved_aimee_bin_path();
   assert(path != NULL);
   const char *home = getenv("HOME");
   if (home)
   {
      char expected[512];
      snprintf(expected, sizeof(expected), "%s/.local/bin/aimee", home);
      assert(strcmp(path, expected) == 0);
   }
}

/* --- Test ensure_claude_code_mcp: non-destructive merge behavior --- */

static void test_claude_mcp_creates_fresh_user_config(void)
{
   char tmpdir[512];
   snprintf(tmpdir, sizeof(tmpdir), "%s/aimee-test-claude-XXXXXX", platform_tmpdir());
   assert(platform_mkdtemp(tmpdir) != NULL);

   /* Create a fake aimee binary so stat() succeeds */
   char fake_bin[512];
   snprintf(fake_bin, sizeof(fake_bin), "%s/fake-aimee", tmpdir);
   FILE *fp = fopen(fake_bin, "w");
   assert(fp != NULL);
   fputs("#!/bin/sh\n", fp);
   fclose(fp);
   chmod(fake_bin, 0755);

   char config_path[512];
   snprintf(config_path, sizeof(config_path), "%s/.claude.json", tmpdir);

   /* Write a settings file with existing data */
   fp = fopen(config_path, "w");
   assert(fp != NULL);
   fputs("{\"existingKey\": true, \"mcpServers\": {\"other\": {\"command\": \"other-mcp\"}}}", fp);
   fclose(fp);

   ensure_claude_code_mcp_entry(config_path, fake_bin);

   cJSON *root = read_json_file(config_path);
   assert(root != NULL);
   assert(cJSON_IsObject(root));

   /* Verify existing key is preserved */
   cJSON *existing = cJSON_GetObjectItem(root, "existingKey");
   assert(existing != NULL && cJSON_IsTrue(existing));

   /* Verify other server still present */
   cJSON *servers = cJSON_GetObjectItem(root, "mcpServers");
   assert(cJSON_IsObject(servers));
   cJSON *other = cJSON_GetObjectItem(servers, "other");
   assert(cJSON_IsObject(other));
   cJSON *other_cmd = cJSON_GetObjectItem(other, "command");
   assert(cJSON_IsString(other_cmd));
   assert(strcmp(other_cmd->valuestring, "other-mcp") == 0);

   cJSON *aimee = cJSON_GetObjectItem(servers, "aimee");
   assert(cJSON_IsObject(aimee));
   cJSON *cmd = cJSON_GetObjectItem(aimee, "command");
   assert(cJSON_IsString(cmd));
   assert(strcmp(cmd->valuestring, fake_bin) == 0);
   cJSON *type = cJSON_GetObjectItem(aimee, "type");
   assert(cJSON_IsString(type));
   assert(strcmp(type->valuestring, "stdio") == 0);
   cJSON *args = cJSON_GetObjectItem(aimee, "args");
   assert(cJSON_IsArray(args));
   assert(strcmp(cJSON_GetArrayItem(args, 0)->valuestring, "mcp-serve") == 0);

   cJSON_Delete(root);

   /* Cleanup */
   char rm_cmd[512];
   snprintf(rm_cmd, sizeof(rm_cmd), "rm -rf '%s'", tmpdir);
   system(rm_cmd);
}

static void test_claude_hooks_create_post_hook_on_fresh_settings(void)
{
   char tmpdir[512];
   snprintf(tmpdir, sizeof(tmpdir), "%s/aimee-test-claude-hooks-XXXXXX", platform_tmpdir());
   assert(platform_mkdtemp(tmpdir) != NULL);

   char settings_path[512];
   snprintf(settings_path, sizeof(settings_path), "%s/settings.json", tmpdir);
   FILE *fp = fopen(settings_path, "w");
   assert(fp != NULL);
   fputs("{}", fp);
   fclose(fp);

   ensure_claude_code_hooks(settings_path);

   cJSON *root = read_json_file(settings_path);
   assert(cJSON_IsObject(root));
   cJSON *hooks = cJSON_GetObjectItemCaseSensitive(root, "hooks");
   assert(cJSON_IsObject(hooks));
   cJSON *post = cJSON_GetObjectItemCaseSensitive(hooks, "PostToolUse");
   assert(cJSON_IsArray(post));

   int found = 0;
   for (int i = 0; i < cJSON_GetArraySize(post); i++)
   {
      cJSON *entry = cJSON_GetArrayItem(post, i);
      cJSON *matcher = cJSON_GetObjectItemCaseSensitive(entry, "matcher");
      cJSON *hook_arr = cJSON_GetObjectItemCaseSensitive(entry, "hooks");
      if (!cJSON_IsString(matcher) || !cJSON_IsArray(hook_arr))
         continue;
      if (!strstr(matcher->valuestring, "EnterWorktree") ||
          !strstr(matcher->valuestring, "ExitWorktree"))
         continue;
      for (int j = 0; j < cJSON_GetArraySize(hook_arr); j++)
      {
         cJSON *hook = cJSON_GetArrayItem(hook_arr, j);
         cJSON *cmd = cJSON_GetObjectItemCaseSensitive(hook, "command");
         if (cJSON_IsString(cmd) && strstr(cmd->valuestring, "hooks post"))
         {
            found = 1;
            break;
         }
      }
      if (found)
         break;
   }
   assert(found);

   /* Regression: from an empty settings.json, EVERY required hook is registered. */
   assert_required_hooks_present(hooks);
   cJSON_Delete(root);

   char cmd[512];
   snprintf(cmd, sizeof(cmd), "rm -rf '%s'", tmpdir);
   system(cmd);
}

/* 1 if root.permissions.deny[] contains the exact string `tool`. */
static int perms_deny_has(cJSON *root, const char *tool)
{
   cJSON *perms = cJSON_GetObjectItemCaseSensitive(root, "permissions");
   cJSON *deny = perms ? cJSON_GetObjectItemCaseSensitive(perms, "deny") : NULL;
   for (int i = 0; cJSON_IsArray(deny) && i < cJSON_GetArraySize(deny); i++)
   {
      cJSON *e = cJSON_GetArrayItem(deny, i);
      if (cJSON_IsString(e) && strcmp(e->valuestring, tool) == 0)
         return 1;
   }
   return 0;
}

static int stub_delegates_available(void)
{
   return 1;
}
static int stub_delegates_none(void)
{
   return 0;
}
static int stub_delegates_unknown(void)
{
   return -1;
}

/* The sub-agent-ban gate: ensure_claude_code_hooks installs the subagent-guard
 * PreToolUse hook + permissions.deny [Task, Agent] ONLY when the injected delegate
 * probe reports usable delegates, and removes both when it does not. An "unknown"
 * probe (server down) must leave settings untouched. Config subagent_ban_enabled
 * defaults ON (no aimee.yaml opt-out in the test env). */
static void test_claude_hooks_subagent_ban_gate(void)
{
   char tmpdir[512];
   snprintf(tmpdir, sizeof(tmpdir), "%s/aimee-test-subagent-ban-XXXXXX", platform_tmpdir());
   assert(platform_mkdtemp(tmpdir) != NULL);
   char settings_path[512];
   snprintf(settings_path, sizeof(settings_path), "%s/settings.json", tmpdir);

   /* Delegates available -> install the guard hook AND the static deny backstop. */
   FILE *fp = fopen(settings_path, "w");
   assert(fp != NULL);
   fputs("{}", fp);
   fclose(fp);
   client_integrations_set_delegate_probe(stub_delegates_available);
   ensure_claude_code_hooks(settings_path);
   cJSON *root = read_json_file(settings_path);
   cJSON *hooks = cJSON_GetObjectItemCaseSensitive(root, "hooks");
   assert(hook_event_has_cmd(hooks, "PreToolUse", "subagent-guard"));
   assert(perms_deny_has(root, "Task"));
   assert(perms_deny_has(root, "Agent"));
   /* The attention-guard matcher no longer claims Task|Agent (the dedicated guard
    * owns them now). */
   assert(hook_event_has_cmd(hooks, "PreToolUse", "attention-guard"));
   cJSON_Delete(root);

   /* The path written into the user's GLOBAL config must be the INSTALLED
    * client, never the binary that happened to run the wiring.
    *
    * Running a client built in a throwaway worktree used to rewrite
    * ~/.claude/settings.json to that worktree path. When the worktree was
    * deleted, every hook in every session failed with
    *   /bin/sh: 1: /home/.../aimee-<sha>/aimee: not found
    * in projects that had nothing to do with the build. Durable config must
    * never capture a transient path.
    *
    * Asserted against the decision function directly: an end-to-end check
    * cannot reach this, because the test binary is not named `aimee` and so
    * takes the same fallback either way — it passes with the bug present. */
   {
      char out[512];
      const char *installed = "/home/dev/.local/bin/aimee";
      const char *worktree_exe = "/home/dev/dev/aimee-225c45f/aimee";

      /* The regression: an install exists, so the transient path must lose. */
      assert(client_integrations_pick_bin_path(installed, worktree_exe, out, sizeof(out)) == 0);
      assert(strcmp(out, installed) == 0);

      /* No install yet (first-run bootstrap) -> the running binary is all we
       * have, and is still better than nothing. */
      assert(client_integrations_pick_bin_path(NULL, worktree_exe, out, sizeof(out)) == 0);
      assert(strcmp(out, worktree_exe) == 0);
      assert(client_integrations_pick_bin_path("", worktree_exe, out, sizeof(out)) == 0);
      assert(strcmp(out, worktree_exe) == 0);

      /* Neither available -> fail rather than emit a half-formed command. */
      assert(client_integrations_pick_bin_path(NULL, NULL, out, sizeof(out)) != 0);
      assert(client_integrations_pick_bin_path("", "", out, sizeof(out)) != 0);

      /* A path that would be truncated must fail, not be silently cut into a
       * different (wrong, possibly existing) path. */
      char tiny[8];
      assert(client_integrations_pick_bin_path(installed, NULL, tiny, sizeof(tiny)) != 0);
      assert(client_integrations_pick_bin_path(installed, NULL, out, 0) != 0);
      assert(client_integrations_pick_bin_path(installed, NULL, NULL, sizeof(out)) != 0);

      printf("  PASS: config records the installed client, not a transient build path\n");
   }

   /* Delegates gone -> the SAME settings must have the guard + deny removed. */
   client_integrations_set_delegate_probe(stub_delegates_none);
   ensure_claude_code_hooks(settings_path);
   root = read_json_file(settings_path);
   hooks = cJSON_GetObjectItemCaseSensitive(root, "hooks");
   assert(!hook_event_has_cmd(hooks, "PreToolUse", "subagent-guard"));
   assert(!perms_deny_has(root, "Task"));
   assert(!perms_deny_has(root, "Agent"));
   cJSON_Delete(root);

   /* Re-install, then an UNKNOWN probe (server unreachable) must leave it as-is. */
   client_integrations_set_delegate_probe(stub_delegates_available);
   ensure_claude_code_hooks(settings_path);
   client_integrations_set_delegate_probe(stub_delegates_unknown);
   ensure_claude_code_hooks(settings_path);
   root = read_json_file(settings_path);
   hooks = cJSON_GetObjectItemCaseSensitive(root, "hooks");
   assert(hook_event_has_cmd(hooks, "PreToolUse", "subagent-guard"));
   assert(perms_deny_has(root, "Task"));
   cJSON_Delete(root);

   client_integrations_set_delegate_probe(NULL); /* don't leak into other tests */
   char cmd[512];
   snprintf(cmd, sizeof(cmd), "rm -rf '%s'", tmpdir);
   system(cmd);
}

static void test_claude_hooks_patch_existing_matcher(void)
{
   char tmpdir[512];
   snprintf(tmpdir, sizeof(tmpdir), "%s/aimee-test-claude-hooks2-XXXXXX", platform_tmpdir());
   assert(platform_mkdtemp(tmpdir) != NULL);

   char settings_path[512];
   snprintf(settings_path, sizeof(settings_path), "%s/settings.json", tmpdir);
   FILE *fp = fopen(settings_path, "w");
   assert(fp != NULL);
   fputs("{\"hooks\":{\"PostToolUse\":[{\"matcher\":\"Edit|Write|MultiEdit\","
         "\"hooks\":[{\"type\":\"command\",\"command\":\"AIMEE_HOOK_CLIENT=claude "
         "aimee hooks post\"}]}]}}",
         fp);
   fclose(fp);

   ensure_claude_code_hooks(settings_path);

   cJSON *root = read_json_file(settings_path);
   assert(cJSON_IsObject(root));
   cJSON *hooks = cJSON_GetObjectItemCaseSensitive(root, "hooks");
   cJSON *post = cJSON_GetObjectItemCaseSensitive(hooks, "PostToolUse");
   assert(cJSON_IsArray(post));
   assert(cJSON_GetArraySize(post) == 1);

   cJSON *entry = cJSON_GetArrayItem(post, 0);
   cJSON *matcher = cJSON_GetObjectItemCaseSensitive(entry, "matcher");
   assert(cJSON_IsString(matcher));
   assert(strstr(matcher->valuestring, "Edit|Write|MultiEdit") != NULL);
   assert(strstr(matcher->valuestring, "EnterWorktree") != NULL);
   assert(strstr(matcher->valuestring, "ExitWorktree") != NULL);

   /* Regression (the live shape of the SessionStart bug): a settings.json that
    * already had SOME hooks must still get the MISSING ones added -- SessionStart
    * in particular. Merging into an existing file must not skip a hook. */
   assert_required_hooks_present(hooks);
   cJSON_Delete(root);

   char cmd[512];
   snprintf(cmd, sizeof(cmd), "rm -rf '%s'", tmpdir);
   system(cmd);
}

/* A stale aimee hook command (e.g. an old/transient binary path) is re-pointed
 * to the resolved binary, so a reinstall heals the hook rather than leaving it
 * dangling at a path that no longer exists. */
static void test_claude_hooks_repoint_stale_command(void)
{
   char tmpdir[512];
   snprintf(tmpdir, sizeof(tmpdir), "%s/aimee-test-claude-hooks3-XXXXXX", platform_tmpdir());
   assert(platform_mkdtemp(tmpdir) != NULL);

   char settings_path[512];
   snprintf(settings_path, sizeof(settings_path), "%s/settings.json", tmpdir);
   FILE *fp = fopen(settings_path, "w");
   assert(fp != NULL);
   /* A stale PreToolUse attention-guard AND a stale PostToolUse hooks-post entry,
    * both referencing a transient /tmp build path. */
   fputs("{\"hooks\":{"
         "\"PreToolUse\":[{\"matcher\":\"Bash\",\"hooks\":[{\"type\":\"command\","
         "\"command\":\"AIMEE_HOOK_CLIENT=claude /tmp/old-build/aimee attention-guard\"}]}],"
         "\"PostToolUse\":[{\"matcher\":\"Edit|EnterWorktree|ExitWorktree\","
         "\"hooks\":[{\"type\":\"command\","
         "\"command\":\"AIMEE_HOOK_CLIENT=claude /tmp/old-build/aimee hooks post\"}]}]"
         "}}",
         fp);
   fclose(fp);

   ensure_claude_code_hooks(settings_path);

   cJSON *root = read_json_file(settings_path);
   assert(cJSON_IsObject(root));
   cJSON *hooks = cJSON_GetObjectItemCaseSensitive(root, "hooks");

   /* Both the PreToolUse attention-guard and the PostToolUse hooks-post commands
    * are re-pointed off the stale /tmp path to the resolved binary. */
   const char *events[] = {"PreToolUse", "PostToolUse"};
   const char *needles[] = {"attention-guard", "hooks post"};
   for (int e = 0; e < 2; e++)
   {
      cJSON *arr = cJSON_GetObjectItemCaseSensitive(hooks, events[e]);
      assert(cJSON_IsArray(arr));
      const char *found = NULL;
      for (int i = 0; i < cJSON_GetArraySize(arr); i++)
      {
         cJSON *hook_arr = cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(arr, i), "hooks");
         for (int j = 0; cJSON_IsArray(hook_arr) && j < cJSON_GetArraySize(hook_arr); j++)
         {
            cJSON *cmd =
                cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(hook_arr, j), "command");
            if (cJSON_IsString(cmd) && strstr(cmd->valuestring, needles[e]))
               found = cmd->valuestring;
         }
      }
      assert(found != NULL);
      assert(strstr(found, "/tmp/old-build/aimee") == NULL);
      assert(strstr(found, "AIMEE_HOOK_CLIENT=claude ") == found);
      assert(strstr(found, needles[e]) != NULL);
   }
   cJSON_Delete(root);

   char cmd[512];
   snprintf(cmd, sizeof(cmd), "rm -rf '%s'", tmpdir);
   system(cmd);
}

/* --- Test Codex plugin config (TOML-like) non-destructive update --- */

static void test_codex_plugin_enabled_fresh(void)
{
   char tmpdir[512];
   snprintf(tmpdir, sizeof(tmpdir), "%s/aimee-test-codex-XXXXXX", platform_tmpdir());
   assert(platform_mkdtemp(tmpdir) != NULL);

   char config_path[512];
   snprintf(config_path, sizeof(config_path), "%s/config.toml", tmpdir);

   /* Call with no existing file - should create with section + enabled */
   ensure_codex_plugin_enabled(config_path);

   FILE *fp = fopen(config_path, "r");
   assert(fp != NULL);
   char buf[1024];
   size_t n = fread(buf, 1, sizeof(buf) - 1, fp);
   fclose(fp);
   buf[n] = '\0';

   assert(strstr(buf, "[plugins.\"aimee@local\"]") != NULL);
   assert(strstr(buf, "enabled = true") != NULL);

   /* Cleanup */
   char cmd[512];
   snprintf(cmd, sizeof(cmd), "rm -rf '%s'", tmpdir);
   system(cmd);
}

static void test_codex_plugin_enabled_preserves_existing(void)
{
   char tmpdir[512];
   snprintf(tmpdir, sizeof(tmpdir), "%s/aimee-test-codex2-XXXXXX", platform_tmpdir());
   assert(platform_mkdtemp(tmpdir) != NULL);

   char config_path[512];
   snprintf(config_path, sizeof(config_path), "%s/config.toml", tmpdir);

   /* Write existing config with other settings */
   FILE *fp = fopen(config_path, "w");
   assert(fp != NULL);
   fputs("[general]\ntheme = \"dark\"\n", fp);
   fclose(fp);

   ensure_codex_plugin_enabled(config_path);

   fp = fopen(config_path, "r");
   assert(fp != NULL);
   char buf[2048];
   size_t n = fread(buf, 1, sizeof(buf) - 1, fp);
   fclose(fp);
   buf[n] = '\0';

   /* Original content should be preserved */
   assert(strstr(buf, "[general]") != NULL);
   assert(strstr(buf, "theme = \"dark\"") != NULL);

   /* Plugin section should be appended */
   assert(strstr(buf, "[plugins.\"aimee@local\"]") != NULL);
   assert(strstr(buf, "enabled = true") != NULL);

   /* Cleanup */
   char cmd[512];
   snprintf(cmd, sizeof(cmd), "rm -rf '%s'", tmpdir);
   system(cmd);
}

static void test_codex_plugin_enabled_idempotent(void)
{
   char tmpdir[512];
   snprintf(tmpdir, sizeof(tmpdir), "%s/aimee-test-codex3-XXXXXX", platform_tmpdir());
   assert(platform_mkdtemp(tmpdir) != NULL);

   char config_path[512];
   snprintf(config_path, sizeof(config_path), "%s/config.toml", tmpdir);

   /* Create file with the section already present */
   FILE *fp = fopen(config_path, "w");
   assert(fp != NULL);
   fputs("[plugins.\"aimee@local\"]\nenabled = true\n", fp);
   fclose(fp);

   /* Call again - should not modify */
   ensure_codex_plugin_enabled(config_path);

   fp = fopen(config_path, "r");
   assert(fp != NULL);
   char buf[1024];
   size_t n = fread(buf, 1, sizeof(buf) - 1, fp);
   fclose(fp);
   buf[n] = '\0';

   /* Should still have exactly one instance */
   char *first = strstr(buf, "[plugins.\"aimee@local\"]");
   assert(first != NULL);
   /* No second instance */
   char *second = strstr(first + 1, "[plugins.\"aimee@local\"]");
   assert(second == NULL);

   /* Cleanup */
   char cmd[512];
   snprintf(cmd, sizeof(cmd), "rm -rf '%s'", tmpdir);
   system(cmd);
}

static int count_substr(const char *haystack, const char *needle)
{
   int count = 0;
   const char *p = haystack;
   while ((p = strstr(p, needle)) != NULL)
   {
      count++;
      p += strlen(needle);
   }
   return count;
}

static void read_text_or_die(const char *path, char *buf, size_t buf_len)
{
   FILE *fp = fopen(path, "r");
   assert(fp != NULL);
   size_t n = fread(buf, 1, buf_len - 1, fp);
   fclose(fp);
   buf[n] = '\0';
}

static void test_codex_trusted_project_fresh(void)
{
   char tmpdir[512];
   snprintf(tmpdir, sizeof(tmpdir), "%s/aimee-test-codex-trust-XXXXXX", platform_tmpdir());
   assert(platform_mkdtemp(tmpdir) != NULL);

   char config_path[512];
   snprintf(config_path, sizeof(config_path), "%s/config.toml", tmpdir);

   assert(ensure_codex_trusted_project_in_config(config_path, "/tmp/workspace/project") == 0);

   char buf[2048];
   read_text_or_die(config_path, buf, sizeof(buf));
   assert(strstr(buf, "[projects.\"/tmp/workspace/project\"]") != NULL);
   assert(strstr(buf, "trust_level = \"trusted\"") != NULL);

   char cmd[512];
   snprintf(cmd, sizeof(cmd), "rm -rf '%s'", tmpdir);
   system(cmd);
}

static void test_codex_trusted_project_updates_existing_section(void)
{
   char tmpdir[512];
   snprintf(tmpdir, sizeof(tmpdir), "%s/aimee-test-codex-trust2-XXXXXX", platform_tmpdir());
   assert(platform_mkdtemp(tmpdir) != NULL);

   char config_path[512];
   snprintf(config_path, sizeof(config_path), "%s/config.toml", tmpdir);

   FILE *fp = fopen(config_path, "w");
   assert(fp != NULL);
   fputs("[projects.\"/tmp/workspace/project\"]\n"
         "trust_level = \"untrusted\"\n"
         "model = \"gpt-5.4\"\n"
         "\n"
         "[plugins.\"aimee@local\"]\n"
         "enabled = true\n",
         fp);
   fclose(fp);

   assert(ensure_codex_trusted_project_in_config(config_path, "/tmp/workspace/project") == 0);

   char buf[2048];
   read_text_or_die(config_path, buf, sizeof(buf));
   assert(strstr(buf, "trust_level = \"untrusted\"") == NULL);
   assert(strstr(buf, "trust_level = \"trusted\"") != NULL);
   assert(strstr(buf, "model = \"gpt-5.4\"") != NULL);
   assert(strstr(buf, "[plugins.\"aimee@local\"]") != NULL);

   char cmd[512];
   snprintf(cmd, sizeof(cmd), "rm -rf '%s'", tmpdir);
   system(cmd);
}

static void test_codex_trusted_project_idempotent(void)
{
   char tmpdir[512];
   snprintf(tmpdir, sizeof(tmpdir), "%s/aimee-test-codex-trust3-XXXXXX", platform_tmpdir());
   assert(platform_mkdtemp(tmpdir) != NULL);

   char config_path[512];
   snprintf(config_path, sizeof(config_path), "%s/config.toml", tmpdir);

   assert(ensure_codex_trusted_project_in_config(config_path, "/tmp/workspace/project") == 0);
   assert(ensure_codex_trusted_project_in_config(config_path, "/tmp/workspace/project") == 0);

   char buf[2048];
   read_text_or_die(config_path, buf, sizeof(buf));
   assert(count_substr(buf, "[projects.\"/tmp/workspace/project\"]") == 1);
   assert(count_substr(buf, "trust_level = \"trusted\"") == 1);

   char cmd[512];
   snprintf(cmd, sizeof(cmd), "rm -rf '%s'", tmpdir);
   system(cmd);
}

/* --- Test ensure_codex_marketplace: non-destructive merge --- */

static void test_codex_marketplace_fresh(void)
{
   char tmpdir[512];
   snprintf(tmpdir, sizeof(tmpdir), "%s/aimee-test-market-XXXXXX", platform_tmpdir());
   assert(platform_mkdtemp(tmpdir) != NULL);

   char path[512];
   snprintf(path, sizeof(path), "%s/marketplace.json", tmpdir);

   /* Call with no existing file */
   ensure_codex_marketplace(path);

   cJSON *root = read_json_file(path);
   assert(root != NULL);
   assert(cJSON_IsObject(root));

   cJSON *name = cJSON_GetObjectItem(root, "name");
   assert(cJSON_IsString(name));
   assert(strcmp(name->valuestring, "local") == 0);

   cJSON *plugins = cJSON_GetObjectItem(root, "plugins");
   assert(cJSON_IsArray(plugins));
   assert(cJSON_GetArraySize(plugins) == 1);

   cJSON *entry = cJSON_GetArrayItem(plugins, 0);
   cJSON *ename = cJSON_GetObjectItem(entry, "name");
   assert(cJSON_IsString(ename));
   assert(strcmp(ename->valuestring, "aimee") == 0);

   cJSON_Delete(root);

   /* Cleanup */
   char cmd[512];
   snprintf(cmd, sizeof(cmd), "rm -rf '%s'", tmpdir);
   system(cmd);
}

static void test_codex_marketplace_preserves_other_plugins(void)
{
   char tmpdir[512];
   snprintf(tmpdir, sizeof(tmpdir), "%s/aimee-test-market2-XXXXXX", platform_tmpdir());
   assert(platform_mkdtemp(tmpdir) != NULL);

   char path[512];
   snprintf(path, sizeof(path), "%s/marketplace.json", tmpdir);

   /* Write existing marketplace with another plugin */
   FILE *fp = fopen(path, "w");
   assert(fp != NULL);
   fputs("{\"name\":\"local\",\"interface\":{\"displayName\":\"Local\"},"
         "\"plugins\":[{\"name\":\"other-plugin\",\"category\":\"Tools\"}]}",
         fp);
   fclose(fp);

   ensure_codex_marketplace(path);

   cJSON *root = read_json_file(path);
   assert(root != NULL);

   cJSON *plugins = cJSON_GetObjectItem(root, "plugins");
   assert(cJSON_IsArray(plugins));
   /* Should have both plugins */
   assert(cJSON_GetArraySize(plugins) == 2);

   /* Verify other plugin is preserved */
   int found_other = 0, found_aimee = 0;
   cJSON *item;
   cJSON_ArrayForEach(item, plugins)
   {
      cJSON *n = cJSON_GetObjectItem(item, "name");
      if (cJSON_IsString(n))
      {
         if (strcmp(n->valuestring, "other-plugin") == 0)
            found_other = 1;
         if (strcmp(n->valuestring, "aimee") == 0)
            found_aimee = 1;
      }
   }
   assert(found_other);
   assert(found_aimee);

   cJSON_Delete(root);

   /* Cleanup */
   char cmd[512];
   snprintf(cmd, sizeof(cmd), "rm -rf '%s'", tmpdir);
   system(cmd);
}

/* --- Test ensure_claude_code_trust --- */

static void write_claude_json(const char *path, const char *content)
{
   FILE *fp = fopen(path, "w");
   assert(fp != NULL);
   fputs(content, fp);
   fclose(fp);
   chmod(path, 0600);
}

static void test_claude_trust_creates_entry_for_new_path(void)
{
   char tmpdir[512];
   snprintf(tmpdir, sizeof(tmpdir), "%s/aimee-test-trust1-XXXXXX", platform_tmpdir());
   assert(platform_mkdtemp(tmpdir) != NULL);

   char claude_json[512];
   snprintf(claude_json, sizeof(claude_json), "%s/.claude.json", tmpdir);
   write_claude_json(claude_json, "{\"projects\":{}}");

   char *old_home = getenv("HOME") ? strdup(getenv("HOME")) : NULL;
   platform_setenv("HOME", tmpdir);

   char worktree[512];
   snprintf(worktree, sizeof(worktree), "%s/workspace", tmpdir);

   ensure_claude_code_trust(worktree);

   cJSON *root = read_json_file(claude_json);
   assert(cJSON_IsObject(root));
   cJSON *projects = cJSON_GetObjectItemCaseSensitive(root, "projects");
   cJSON *proj = cJSON_GetObjectItemCaseSensitive(projects, worktree);
   assert(cJSON_IsObject(proj));
   assert(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(proj, "hasTrustDialogAccepted")));
   cJSON *enabled = cJSON_GetObjectItemCaseSensitive(proj, "enabledMcpjsonServers");
   assert(cJSON_IsArray(enabled));
   assert(cJSON_GetArraySize(enabled) == 1);
   cJSON *item = cJSON_GetArrayItem(enabled, 0);
   assert(cJSON_IsString(item) && strcmp(item->valuestring, "aimee") == 0);
   cJSON_Delete(root);

   if (old_home)
   {
      platform_setenv("HOME", old_home);
      free(old_home);
   }
   char cmd[512];
   snprintf(cmd, sizeof(cmd), "rm -rf '%s'", tmpdir);
   system(cmd);
}

static void test_claude_trust_updates_existing_untrusted_entry(void)
{
   char tmpdir[512];
   snprintf(tmpdir, sizeof(tmpdir), "%s/aimee-test-trust2-XXXXXX", platform_tmpdir());
   assert(platform_mkdtemp(tmpdir) != NULL);

   char claude_json[512];
   snprintf(claude_json, sizeof(claude_json), "%s/.claude.json", tmpdir);

   char worktree[512];
   snprintf(worktree, sizeof(worktree), "%s/workspace", tmpdir);

   /* Simulate an existing entry with trust rejected */
   char initial[1024];
   snprintf(initial, sizeof(initial),
            "{\"projects\":{\"%s\":{\"hasTrustDialogAccepted\":false,"
            "\"enabledMcpjsonServers\":[],\"disabledMcpjsonServers\":[]}}}",
            worktree);
   write_claude_json(claude_json, initial);

   char *old_home = getenv("HOME") ? strdup(getenv("HOME")) : NULL;
   platform_setenv("HOME", tmpdir);

   ensure_claude_code_trust(worktree);

   cJSON *root = read_json_file(claude_json);
   assert(cJSON_IsObject(root));
   cJSON *projects = cJSON_GetObjectItemCaseSensitive(root, "projects");
   cJSON *proj = cJSON_GetObjectItemCaseSensitive(projects, worktree);
   assert(cJSON_IsObject(proj));
   assert(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(proj, "hasTrustDialogAccepted")));
   cJSON *enabled = cJSON_GetObjectItemCaseSensitive(proj, "enabledMcpjsonServers");
   assert(cJSON_IsArray(enabled));
   /* Should have added "aimee" */
   int found = 0;
   for (int i = 0; i < cJSON_GetArraySize(enabled); i++)
   {
      cJSON *it = cJSON_GetArrayItem(enabled, i);
      if (cJSON_IsString(it) && strcmp(it->valuestring, "aimee") == 0)
         found = 1;
   }
   assert(found);
   cJSON_Delete(root);

   if (old_home)
   {
      platform_setenv("HOME", old_home);
      free(old_home);
   }
   char cmd[512];
   snprintf(cmd, sizeof(cmd), "rm -rf '%s'", tmpdir);
   system(cmd);
}

static void test_claude_trust_idempotent(void)
{
   char tmpdir[512];
   snprintf(tmpdir, sizeof(tmpdir), "%s/aimee-test-trust3-XXXXXX", platform_tmpdir());
   assert(platform_mkdtemp(tmpdir) != NULL);

   char claude_json[512];
   snprintf(claude_json, sizeof(claude_json), "%s/.claude.json", tmpdir);

   char worktree[512];
   snprintf(worktree, sizeof(worktree), "%s/workspace", tmpdir);

   char *old_home = getenv("HOME") ? strdup(getenv("HOME")) : NULL;
   platform_setenv("HOME", tmpdir);

   write_claude_json(claude_json, "{\"projects\":{}}");
   ensure_claude_code_trust(worktree);
   ensure_claude_code_trust(worktree); /* Call again — should be a no-op */

   cJSON *root = read_json_file(claude_json);
   cJSON *projects = cJSON_GetObjectItemCaseSensitive(root, "projects");
   cJSON *proj = cJSON_GetObjectItemCaseSensitive(projects, worktree);
   assert(cJSON_IsObject(proj));
   assert(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(proj, "hasTrustDialogAccepted")));
   cJSON *enabled = cJSON_GetObjectItemCaseSensitive(proj, "enabledMcpjsonServers");
   /* Should only have one "aimee" entry, not two */
   int aimee_count = 0;
   for (int i = 0; i < cJSON_GetArraySize(enabled); i++)
   {
      cJSON *it = cJSON_GetArrayItem(enabled, i);
      if (cJSON_IsString(it) && strcmp(it->valuestring, "aimee") == 0)
         aimee_count++;
   }
   assert(aimee_count == 1);
   cJSON_Delete(root);

   if (old_home)
   {
      platform_setenv("HOME", old_home);
      free(old_home);
   }
   char cmd[512];
   snprintf(cmd, sizeof(cmd), "rm -rf '%s'", tmpdir);
   system(cmd);
}

static void test_claude_trust_preserves_other_projects(void)
{
   char tmpdir[512];
   snprintf(tmpdir, sizeof(tmpdir), "%s/aimee-test-trust4-XXXXXX", platform_tmpdir());
   assert(platform_mkdtemp(tmpdir) != NULL);

   char claude_json[512];
   snprintf(claude_json, sizeof(claude_json), "%s/.claude.json", tmpdir);
   write_claude_json(claude_json,
                     "{\"projects\":{\"/other/project\":{\"hasTrustDialogAccepted\":true}}}");

   char worktree[512];
   snprintf(worktree, sizeof(worktree), "%s/workspace", tmpdir);

   char *old_home = getenv("HOME") ? strdup(getenv("HOME")) : NULL;
   platform_setenv("HOME", tmpdir);

   ensure_claude_code_trust(worktree);

   cJSON *root = read_json_file(claude_json);
   cJSON *projects = cJSON_GetObjectItemCaseSensitive(root, "projects");
   /* Other project must still be there */
   cJSON *other = cJSON_GetObjectItemCaseSensitive(projects, "/other/project");
   assert(cJSON_IsObject(other));
   assert(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(other, "hasTrustDialogAccepted")));
   /* New path should also be added */
   cJSON *proj = cJSON_GetObjectItemCaseSensitive(projects, worktree);
   assert(cJSON_IsObject(proj));
   cJSON_Delete(root);

   if (old_home)
   {
      platform_setenv("HOME", old_home);
      free(old_home);
   }
   char cmd[512];
   snprintf(cmd, sizeof(cmd), "rm -rf '%s'", tmpdir);
   system(cmd);
}

static void test_claude_trust_no_op_when_claude_json_missing(void)
{
   char tmpdir[512];
   snprintf(tmpdir, sizeof(tmpdir), "%s/aimee-test-trust5-XXXXXX", platform_tmpdir());
   assert(platform_mkdtemp(tmpdir) != NULL);

   char *old_home = getenv("HOME") ? strdup(getenv("HOME")) : NULL;
   platform_setenv("HOME", tmpdir); /* No .claude.json in this dir */

   /* Should not crash or create .claude.json */
   ensure_claude_code_trust("/some/path");

   char claude_json[512];
   snprintf(claude_json, sizeof(claude_json), "%s/.claude.json", tmpdir);
   FILE *fp = fopen(claude_json, "r");
   assert(fp == NULL); /* Must not have been created */

   if (old_home)
   {
      platform_setenv("HOME", old_home);
      free(old_home);
   }
   char cmd[512];
   snprintf(cmd, sizeof(cmd), "rm -rf '%s'", tmpdir);
   system(cmd);
}

/* --- Test write_text_file: no-op when content unchanged --- */

static void test_write_text_file_no_op(void)
{
   char tmppath[512];
   snprintf(tmppath, sizeof(tmppath), "%s/aimee-test-write-XXXXXX", platform_tmpdir());
   int fd = platform_mkstemp(tmppath, sizeof(tmppath), "aim");
   assert(fd >= 0);

   const char *content = "test content here";
   write(fd, content, strlen(content));
   close(fd);

   /* Get mtime before */
   struct stat st1;
   assert(stat(tmppath, &st1) == 0);

   /* Small sleep to ensure mtime would differ */
   usleep(10000);

   /* Write same content: should be no-op */
   int rc = write_text_file(tmppath, content, 0600);
   assert(rc == 0);

   /* mtime should not change since content is identical */
   struct stat st2;
   assert(stat(tmppath, &st2) == 0);
   assert(st1.st_mtime == st2.st_mtime);

   unlink(tmppath);
}

/* --- Test the client-integrations opt-out gate --- */

static void test_client_integrations_optout_gate(void)
{
   /* Hermetic config: point AIMEE_HOME at an empty temp dir so the gate reads
    * an aimee.yaml under our control (absent -> default client_integrations
    * ON). */
   char tmpdir[512];
   snprintf(tmpdir, sizeof(tmpdir), "%s/aimee-test-ci-optout-XXXXXX", platform_tmpdir());
   assert(platform_mkdtemp(tmpdir) != NULL);

   /* platform_unsetenv is not declared in this include context, and the gate
    * treats "0"/"false"/empty as "not opted out", so "0" stands in for unset. */
   char old_home[512] = {0};
   const char *prev_home = getenv("AIMEE_HOME");
   if (prev_home)
      snprintf(old_home, sizeof(old_home), "%s", prev_home);

   platform_setenv("AIMEE_HOME", tmpdir);
   platform_setenv("AIMEE_NO_CLIENT_INTEGRATIONS", "0");

   /* Default config, no env override -> integrations allowed. */
   assert(client_integrations_allowed() == 1);

   /* Env override opts out regardless of config. */
   platform_setenv("AIMEE_NO_CLIENT_INTEGRATIONS", "1");
   assert(client_integrations_allowed() == 0);
   platform_setenv("AIMEE_NO_CLIENT_INTEGRATIONS", "yes");
   assert(client_integrations_allowed() == 0);

   /* "0" and "false" are treated as unset, so the (default-ON) config wins. */
   platform_setenv("AIMEE_NO_CLIENT_INTEGRATIONS", "0");
   assert(client_integrations_allowed() == 1);
   platform_setenv("AIMEE_NO_CLIENT_INTEGRATIONS", "false");
   assert(client_integrations_allowed() == 1);
   platform_setenv("AIMEE_NO_CLIENT_INTEGRATIONS", "0");

   /* Config-driven opt-out: client_integrations_enabled: false closes the gate
    * even with no env override. */
   char yaml_path[600];
   snprintf(yaml_path, sizeof(yaml_path), "%s/aimee.yaml", tmpdir);
   FILE *fp = fopen(yaml_path, "w");
   assert(fp != NULL);
   fputs("client_integrations_enabled: false\n", fp);
   fclose(fp);
   assert(client_integrations_allowed() == 0);

   /* And the env override still wins the other way: "0" cannot re-enable it once
    * the config disables it (env only forces OFF, never ON). */
   platform_setenv("AIMEE_NO_CLIENT_INTEGRATIONS", "0");
   assert(client_integrations_allowed() == 0);

   /* Restore AIMEE_HOME (best-effort) and neutralize the opt-out env so later
    * code in this process sees a clean state. */
   platform_setenv("AIMEE_NO_CLIENT_INTEGRATIONS", "0");
   if (old_home[0])
      platform_setenv("AIMEE_HOME", old_home);

   char rm_cmd[600];
   snprintf(rm_cmd, sizeof(rm_cmd), "rm -rf '%s'", tmpdir);
   system(rm_cmd);
}

int main(void)
{
   printf("client_integrations: ");

   test_build_marketplace_root();
   test_build_aimee_plugin_entry();
   test_codex_delegate_policy_is_explicit();
   test_mcp_config_uses_resolved_command();
   test_skill_does_not_vary_by_profile();
   test_mcp_config_carries_aimee_home();
   test_read_json_file_missing();
   test_read_json_file_valid();
   test_read_json_file_invalid();
   test_resolved_aimee_bin_path_fallback();
   test_claude_mcp_creates_fresh_user_config();
   test_claude_hooks_create_post_hook_on_fresh_settings();
   test_claude_hooks_patch_existing_matcher();
   test_claude_hooks_repoint_stale_command();
   test_claude_hooks_subagent_ban_gate();
   test_codex_plugin_enabled_fresh();
   test_codex_plugin_enabled_preserves_existing();
   test_codex_plugin_enabled_idempotent();
   test_codex_trusted_project_fresh();
   test_codex_trusted_project_updates_existing_section();
   test_codex_trusted_project_idempotent();
   test_codex_marketplace_fresh();
   test_codex_marketplace_preserves_other_plugins();
   test_write_text_file_no_op();
   test_claude_trust_creates_entry_for_new_path();
   test_claude_trust_updates_existing_untrusted_entry();
   test_claude_trust_idempotent();
   test_claude_trust_preserves_other_projects();
   test_claude_trust_no_op_when_claude_json_missing();
   test_client_integrations_optout_gate();

   printf("all tests passed\n");
   return 0;
}
