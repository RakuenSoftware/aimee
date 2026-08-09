#include "client_constants.h"
#include "client_integrations.h"
#include "agent_code_capabilities.h"
#include "aimee_home.h"
#include "yaml.h"
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

/* Injected by the CLI (client_integrations_set_delegate_probe): reports whether
 * usable aimee delegates exist (1), none do (0), or the answer is unknown (-1,
 * server unreachable). CORE must not make a /v1 call directly — it links into
 * both the DB-free client and the server — so the CLI supplies the real probe. */
static int (*g_delegate_probe)(void) = NULL;

void client_integrations_set_delegate_probe(int (*probe)(void))
{
   g_delegate_probe = probe;
}

/* Defined further down; forward-declared for ensure_subagent_ban (which sits
 * beside ensure_claude_code_hooks, above the definition). */
static int client_config_bool(const char *key, int default_val);

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

/* Path to write into the user's GLOBAL client config (Claude Code hooks, MCP
 * server command). Those files outlive whatever binary happens to be running,
 * so the path must outlive it too.
 *
 * This used to return the running executable's own path whenever that binary was
 * named `aimee`. Build a client in a throwaway worktree, run it once, and it
 * rewrote ~/.claude/settings.json to point every hook at that worktree — then the
 * worktree was deleted and EVERY hook in EVERY session began failing with
 * "/bin/sh: 1: /home/.../aimee-<sha>/aimee: not found", in projects that had
 * nothing to do with the build. A transient path must never be persisted into
 * durable config.
 *
 * Split from the getenv/exe-path plumbing so the decision itself is testable:
 * |installed| is the installed client (NULL/empty when absent) and |exe| is the
 * running binary. Prefer the install; fall back to the running binary only when
 * there is nothing installed to point at — the first-run bootstrap the exe path
 * was there to serve. Returns 0 and fills |out| on success. */
int client_integrations_pick_bin_path(const char *installed, const char *exe, char *out, size_t cap)
{
   if (!out || cap == 0)
      return -1;
   if (installed && installed[0])
      return (size_t)snprintf(out, cap, "%s", installed) < cap ? 0 : -1;
   if (exe && exe[0])
      return (size_t)snprintf(out, cap, "%s", exe) < cap ? 0 : -1;
   return -1;
}

/* The installed client, or NULL when there is none. */
static const char *aimee_installed_bin_path(char *buf, size_t cap)
{
   const char *home = getenv("HOME");
   if (!home || !home[0])
      return NULL;
   if ((size_t)snprintf(buf, cap, "%s/.local/bin/aimee", home) >= cap)
      return NULL;
   return access(buf, X_OK) == 0 ? buf : NULL;
}

static const char *resolved_aimee_bin_path(void)
{
   static char path[MAX_PATH_LEN];
   if (path[0])
      return path;

   char installed_buf[MAX_PATH_LEN];
   const char *installed = aimee_installed_bin_path(installed_buf, sizeof(installed_buf));

   char exe[MAX_PATH_LEN] = "";
   if (platform_get_exe_path(exe, sizeof(exe)) == 0)
   {
      char *base = strrchr(exe, '/');
      base = base ? base + 1 : exe;
      if (strcmp(base, "aimee-server") == 0)
         snprintf(base, sizeof(exe) - (size_t)(base - exe), "aimee");
      else if (strcmp(base, "aimee-server.exe") == 0)
         snprintf(base, sizeof(exe) - (size_t)(base - exe), "aimee.exe");
      else if (strcmp(base, "aimee") != 0 && strcmp(base, "aimee.exe") != 0 &&
               strcmp(base, "aimee-client") != 0 && strcmp(base, "aimee-client.exe") != 0)
         exe[0] = '\0'; /* not a client binary — not a usable fallback */
   }

   if (client_integrations_pick_bin_path(installed, exe, path, sizeof(path)) == 0)
      return path;

   const char *home = getenv("HOME");
   path[0] = '\0';
   if (home)
      snprintf(path, sizeof(path), "%s/.local/bin/aimee", home);
   return path;
}

/* The agent host spawns `aimee mcp-serve` itself, with an environment of its own
 * choosing, and the generated config is the only place we can state what the
 * server needs. Where AIMEE_HOME is what locates the config -- every
 * containerised or managed-server install -- leaving it out means the server
 * starts, cannot reach aimee-server, and answers tools/list with an EMPTY list.
 * The agent is offered no tools at all and falls back to grep, which looks
 * exactly like deciding the index was not worth calling. Measured on a
 * container install: 18 tools with AIMEE_HOME present, 0 without it, whatever
 * HOME is set to.
 *
 * Only when the operator set it explicitly: with AIMEE_HOME unset the default
 * resolution already works, and pinning a value they never chose would freeze
 * this machine's layout into a config that may be copied elsewhere. */
static const char *explicit_aimee_home(void)
{
   const char *home = getenv("AIMEE_HOME");
   return (home && *home) ? home : NULL;
}

static void format_mcp_json(char *buf, size_t cap, const char *aimee_bin)
{
   const char *aimee_home = explicit_aimee_home();
   char env_block[MAX_PATH_LEN + 64];

   if (!buf || cap == 0)
      return;

   env_block[0] = '\0';
   /* This writer emits raw JSON, so refuse a value it cannot represent rather
    * than produce a config that silently fails to parse. */
   if (aimee_home && !strpbrk(aimee_home, "\"\\"))
      snprintf(env_block, sizeof(env_block), ",\n      \"env\": { \"AIMEE_HOME\": \"%s\" }",
               aimee_home);

   snprintf(buf, cap,
            "{\n"
            "  \"mcpServers\": {\n"
            "    \"aimee\": {\n"
            "      \"command\": \"%s\",\n"
            "      \"args\": [\"mcp-serve\"]%s\n"
            "    }\n"
            "  }\n"
            "}\n",
            aimee_bin ? aimee_bin : "aimee", env_block);
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
   const char *aimee_home = explicit_aimee_home();
   if (aimee_home)
   {
      cJSON *env = cJSON_CreateObject();
      if (env)
      {
         cJSON_AddStringToObject(env, "AIMEE_HOME", aimee_home);
         cJSON_AddItemToObject(aimee_server, "env", env);
      }
   }
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
          /* TWO REVISIONS, TWO DIFFERENT FAILURES — BOTH CAUSED BY THIS TEXT.
           *
           * v1 opened with "prefer local file inspection first for nearby code"
           * and offered find_symbol only "when the local search surface is
           * missing indexed context" — the index as a fallback after grep.
           * Agents followed it exactly: over a four-task benchmark with a
           * verified-healthy index, every cell read this file and then made ZERO
           * index calls, resolving everything with three to four recursive greps.
           *
           * v2 (leading with the questions the index answers) fixed the zero-call
           * problem but was ADDITIVE: it said what to use the index for without
           * saying what to stop doing. Agents then did both. Measured over eight
           * tasks against the same corpus, the aimee arm ran 1.5-2.4x the commands
           * of the plain-codex arm (22 vs 9 on one task) and cost up to 4.4x as
           * much, while making 2 index calls. The extra spend was not the index:
           * every command's output stays in the conversation and is re-sent on
           * every later turn, so cost grows with the SQUARE of the command count.
           * One unfiltered `rg --files` early in a run carried ~24k tokens for the
           * rest of it.
           *
           * So this list has to be substitutive and it has to bound exploration.
           * The index replacing a search is the entire point; an index used
           * alongside the search it was meant to replace is pure overhead. */
          "**Start from the index, not from a survey of the repository.** Do not "
          "orient yourself by listing files, walking directories, or reading whole "
          "files to see what is there. Every command's output stays in context for "
          "the rest of this session, so a repo-wide listing costs more than the "
          "answer it was meant to find.\n"
          "\n"
          "- Locating a definition, or finding every caller of something: use "
          "`" AIMEE_CODE_TOOL_FIND_SYMBOL
          "`. It answers from the index in one call, and it is exact where a "
          "recursive text search is a guess that also matches comments, strings "
          "and unrelated names. It REPLACES that search — do not also grep for "
          "the same symbol to confirm it. Looking up several names? Pass them "
          "together as `identifiers` — one call, one section per name.\n"
          "- Before changing anything shared, or editing more than one file: use "
          "`" AIMEE_CODE_TOOL_PREVIEW_BLAST_RADIUS
          "` to see what depends on it. A grep for the symbol will not tell you "
          "what breaks.\n"
          /* The composed packet existed as /v1/code/context and was wired ONLY as
           * automatic pre-injection for aimee's own ingress and for delegates --
           * an agent over MCP could not reach it at all. That is why the measured
           * opening move is four hybrid queries followed by four structure calls
           * and then reads: the one-call answer was there and was never offered. */
          "- STARTING on an unfamiliar area, or you do not yet know which files "
          "matter: use `" AIMEE_CODE_TOOL_INDEX
          "` with command=" AIMEE_CODE_INDEX_COMMAND_INVESTIGATE
          " and a plain-words question. It returns ranked evidence with the code "
          "already attached and an explicit answerable/no_answer verdict, capped "
          "so it cannot flood the context. One call where searching, mapping and "
          "reading would be three. If it says no_answer, stop probing the index "
          "and go read the files it named.\n"
          "- Looking for a PHRASE rather than a symbol -- an error string, a config "
          "key, a concept like \"config cache\" or \"pool lease\": use `" AIMEE_CODE_TOOL_INDEX
          "` with command=" AIMEE_CODE_INDEX_COMMAND_HYBRID
          " and a query. It fuses lexical and semantic retrieval over the index and "
          "returns a bounded, ranked result set (max_results, default 20), where a "
          "recursive search returns every line that matched in whatever order the "
          "filesystem walk found them.\n"
          /* THE MEASURED FAILURE MODE IS UNDER-SCOPING, NOT BAD RETRIEVAL.
           * On two benchmark tasks that every arm failed, aimee's retrieval was
           * correct and its patch was too narrow. On a leaked-connection task it
           * ran find_callers on the acquire and release functions, read the
           * owning module -- then patched ONE consumer to hold the lease for less
           * time, where the reference made the pool reclaim a lease that is never
           * returned. On a wedged-fan-out task whose ticket names a three-link
           * chain, it changed two of the five files the reference changes. Both
           * fixes were reasoned and both left the defect reachable. */
          "- Fix the OWNER, not one caller. If something is acquired and never "
          "released, or handed out and never reclaimed, the durable fix belongs "
          "with whatever hands it out — a fix in one caller leaves every other "
          "caller able to reproduce it. Run `" AIMEE_CODE_TOOL_INDEX
          "` command=find_callers on the function at fault: if it has more than "
          "one caller, a caller-side fix is incomplete by construction.\n"
          /* Third instance of the same failure, and the sharpest: the ticket
           * opens "Two bugs that made ..." and names both. aimee fixed the
           * second (a request shape) and never touched the first -- an
           * over-broad provider/endpoint/model test that had to be narrowed to
           * one model. It edited the right FILE for an unrelated reason, so file
           * overlap with the reference looked like coverage and was not. */
          "- If the ticket states a COUNT — \"two bugs\", \"both paths\", "
          "\"three call sites\" — your patch must address that many DISTINCT "
          "defects. Name them to yourself before you start and check them off at "
          "the end. Editing a file the second defect happens to live in is not "
          "fixing it.\n"
          "- Before you finish, re-read the ticket and account for every symptom "
          "it names. A ticket that describes a CHAIN (\"X did A, which collided "
          "with B, so C never ran\") is describing several places that must "
          "agree; fixing the first link usually leaves the rest wedged. Check "
          "your changed symbols with `" AIMEE_CODE_TOOL_PREVIEW_BLAST_RADIUS
          "` and ask what else participates.\n"
          /* A completeness check was nearly built as a new tool before noticing
           * roundtable_review already IS one: every seat is an ordinary delegate,
           * `original_request` exists precisely to detect goal drift, `brief`
           * takes focus/invariants/questions, and `workdir` gives the reviewer
           * the checkout. A one-seat preset is a single reviewer with a persona.
           * Reuse it rather than shipping a parallel path that would have thrown
           * away chairman synthesis, evidence requirements and cost caps. */
          "- Before you report the work done, get a second pair of eyes on it: "
          "`roundtable_review` with `diff` set to your change and "
          "`original_request` set to the FULL ticket text. It runs reviewers as "
          "delegates and uses the original request to catch goal drift — the "
          "case where the change is reasonable but is not the change that was "
          "asked for. The call returns the verdict itself; it blocks until the "
          "review finishes, which can take several minutes. Self-review misses "
          "omissions because you already believe you are finished.\n"
          "- Use `search_memory` for stored project facts or prior decisions.\n"
          "- Do not read this file, or anything else under the plugin cache. You "
          "are already reading it; spending a call to fetch it again tells you "
          "nothing new.\n"
          /* Reading is the largest remaining category. Measured on one cell:
           * 15 reads carrying 660k tokens, 17.7% of that run's whole input --
           * more than search, build and git. `sed -n '1,280p'` is a guess at a
           * range; span is the range the index already knows. */
          "- Reading a file the index has already pointed you at: read the RANGE, "
          "never the whole file. `" AIMEE_CODE_TOOL_INDEX
          "` with command=span and file_path + line_start/line_end returns exactly "
          "that slice; use it when you know the lines, and a plain read only when "
          "you do not.\n"
          /* Build output is almost entirely echoed compiler command lines: each
           * one repeats the full -I/-D flag set, hundreds of characters, per
           * translation unit. Measured on one cell: 6 builds carrying 241k
           * tokens for a handful of real diagnostics. */
          "- Building or running tests: silence the command echo (`make -s`, or "
          "redirect stdout and keep stderr) and build only the target you need. "
          "A default `make` echoes every compiler invocation with its whole flag "
          "set, which is thousands of characters per file and tells you nothing "
          "the diagnostics do not.\n"
          "- Recursive grep is for text that is not a code symbol, or for when the "
          "index has no answer. Scope it to a path when you use it. Never run it "
          "over the whole tree to find out what the tree contains.\n"
          /* An unbounded recursive search is the single most expensive thing an
           * agent can do here, and it is invisible at the time: the output lands
           * once but is re-sent on every later turn. Measured on one cell after
           * the guidance above was already in place -- a single
           * `rg -n "<alternation>" src --glob '*.[ch]'` with no cap returned
           * 80,330 characters (~20k tokens) and rode the remaining ~13 model
           * calls, about a fifth of that run's entire input. Plain codex capped
           * every one of its five searches with `| head -n 100` unprompted and
           * spent 15k characters on search in total against aimee's 93k. */
          "- Cap what a search prints. Pipe it through `head` (say `| head -n 60`) "
          "and prefer a narrow pattern over a wide alternation: an uncapped "
          "recursive search can return tens of thousands of characters, and every "
          "one of them is re-sent on every later turn of this session.\n"
          /* The dominant remaining cost, and it is not bytes. Measured on one
           * cell where all four arms passed: aimee moved the FEWEST tool-output
           * characters of any arm (74k against baseline's 124k) and still paid
           * 4.4x the tokens, because it took 47 tool calls against baseline's 9.
           * Per call it was cheaper (41.0k input-tokens vs 49.1k); it simply took
           * five times as many. Plain codex chained aggressively -- 16 `sed`
           * reads inside 9 calls, up to five ranges joined with `&&` in a single
           * command -- where aimee spread 7 reads across 22 calls. A round trip
           * re-sends the whole conversation prefix, so an extra call costs far
           * more than an extra command on a line that was already being sent. */
          "- Put independent commands in ONE call, joined with `&&`. Every extra "
          "round trip re-sends this entire conversation, so four reads chained "
          "into one command cost a fraction of four separate calls. Batch your "
          "reads, and fold `git status` / `git diff --check` / a cleanup into the "
          "command you were already running rather than spending a turn on each.\n"
          /* Every retrieval verb the agent uses in bulk now takes a plural form.
           * Measured on one cell AFTER span batching landed: 4 separate hybrid
           * queries and 4 separate structure calls over 4 different files, all
           * independent, all a full round trip each. */
          "- The same applies to every index lookup: `" AIMEE_CODE_TOOL_INDEX
          "` takes `spans` ([{file_path, line_start, line_end}, ...]) for reads, "
          "`file_paths` for command=structure, and `queries` for "
          "command=" AIMEE_CODE_INDEX_COMMAND_HYBRID ". `" AIMEE_CODE_TOOL_FIND_SYMBOL
          "` takes `identifiers`. If you are about to ask the same question about "
          "several files, ranges, symbols or topics, ask it ONCE with the plural "
          "form -- independent lookups do not need separate turns.\n"
          /* Measured on one cell: 4 of 9 shell searches returned 0-35 characters.
           * Each is a full round trip bought for nothing, and the pattern is
           * always the same -- a guessed regex misses, so the agent guesses a
           * narrower or wider one instead of switching to the index. */
          "- A search that comes back empty is a signal to change TOOL, not to "
          "retry with another pattern. Two empty greps in a row means the thing "
          "you are looking for is not literal text: use `" AIMEE_CODE_TOOL_INDEX
          "` with command=" AIMEE_CODE_INDEX_COMMAND_HYBRID " or `" AIMEE_CODE_TOOL_FIND_SYMBOL
          "` instead.\n"
          "- Do not call provider-native sub-agent tools such as `spawn_agent`; use "
          "the aimee `delegate` MCP tool for every delegated or parallel sub-task.\n"
          "- Use `delegate` only for bounded sub-tasks that materially advance the "
          "current work.\n";
}

/* The skill is the same text for every run. There is no benchmark variant: a
 * profile that hides shipped tools during measurement makes the measured thing a
 * configuration nobody deploys. */
static const char *codex_skill_markdown_effective(void)
{
   return codex_skill_markdown();
}

/* The codex PreToolUse registration. aimee already HAS the guard -- `aimee hooks`
 * implements the full wire contract (permissionDecision / permissionDecisionReason,
 * and updatedInput where the client supports it), and require_aimee_git is ON by
 * default with a deny that names git_status / git_log / git_diff_summary and the
 * rest. It simply never ran under codex, because this plugin shipped no hooks at
 * all: only .mcp.json and the skill.
 *
 * Measured consequence across the benchmark's aimee cells: 98 shell `git`
 * invocations (48 of them full `git diff`) and ZERO calls to the aimee git tool --
 * whose schema we pay ~1,000 tokens for on every single call. The rule was written,
 * defaulted on, and left unwired.
 *
 * Codex does not honour updatedInput on PreToolUse, so the guard's codex path
 * denies with an instruction to retry through the tool. That costs one turn and
 * redirects the remaining ones. */
static const char *codex_hooks_json(const char *aimee_bin)
{
   static char buf[1024];
   snprintf(buf, sizeof(buf),
            "{\n"
            "  \"hooks\": {\n"
            "    \"PreToolUse\": [\n"
            "      {\n"
            "        \"hooks\": [\n"
            "          {\n"
            "            \"type\": \"command\",\n"
            /* `hooks` alone exits with "hooks requires 'pre' or 'post'" and codex then
             * allows the tool. The subcommand is the whole difference between a
             * registered hook and an enforcing one. */
            "            \"command\": \"%s hooks pre\",\n"
            "            \"timeout\": 10\n"
            "          }\n"
            "        ]\n"
            "      }\n"
            "    ]\n"
            "  }\n"
            "}\n",
            aimee_bin && aimee_bin[0] ? aimee_bin : "aimee");
   return buf;
}

static const char *codex_code_exploration_prompt(void)
{
   /* "instead of" is load-bearing: the index is a REPLACEMENT for the search, not
    * an addition to it. The second sentence bounds exploration, because an agent
    * that uses the index and then surveys the tree anyway pays for both — and the
    * survey is the expensive half, since every command's output is re-sent on every
    * later turn. See the measurement in codex_skill_markdown(). */
   return "Explore the codebase through aimee's tools (" AIMEE_CODE_TOOL_FIND_SYMBOL
          ", " AIMEE_CODE_TOOL_AST_GREP_SEARCH ", " AIMEE_CODE_TOOL_INDEX
          " command=" AIMEE_CODE_INDEX_COMMAND_HYBRID
          ") instead of raw grep/read. Do not survey the repository first: no "
          "repo-wide file listings, no directory walks, no reading whole files to "
          "orient. Ask the index, then read only what it points at. Cap what any "
          "search prints (`| head -n 60`): its output is re-sent on every later turn.";
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
   char hooks_json[MAX_PATH_LEN];
   char marketplace_hooks_json[MAX_PATH_LEN];
   char installed_hooks_json[MAX_PATH_LEN];
   snprintf(hooks_json, sizeof(hooks_json), "%s/plugins/aimee/hooks/codex-hooks.json", home);
   snprintf(marketplace_hooks_json, sizeof(marketplace_hooks_json),
            "%s/.agents/plugins/plugins/aimee/hooks/codex-hooks.json", home);
   snprintf(installed_hooks_json, sizeof(installed_hooks_json),
            "%s/.codex/plugins/cache/local/aimee/hooks/codex-hooks.json", home);
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
            "  \"hooks\": \"./hooks/codex-hooks.json\",\n"
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
            "      \"%s\",\n"
            "      \"Preview the blast radius before editing multiple files\",\n"
            "      \"%s\",\n"
            "      \"Delegate bounded work through aimee delegate, not provider sub-agents\"\n"
            "    ],\n"
            "    \"brandColor\": \"#1F6FEB\",\n"
            "    \"screenshots\": []\n"
            "  }\n"
            "}\n",
            AIMEE_VERSION, codex_code_exploration_prompt(), codex_delegate_policy_prompt());

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
            "      \"%s\",\n"
            "      \"Preview the blast radius before editing multiple files\",\n"
            "      \"%s\",\n"
            "      \"Delegate bounded work through aimee delegate, not provider sub-agents\"\n"
            "    ],\n"
            "    \"brandColor\": \"#1F6FEB\",\n"
            "    \"screenshots\": []\n"
            "  }\n"
            "}\n",
            AIMEE_VERSION, codex_code_exploration_prompt(), codex_delegate_policy_prompt());

   char mcp_buf[MAX_PATH_LEN + 256];
   format_mcp_json(mcp_buf, sizeof(mcp_buf), aimee_bin);

   const char *skill_buf = codex_skill_markdown_effective();

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
   const char *hooks_buf = codex_hooks_json(aimee_bin);
   write_text_file(hooks_json, hooks_buf, 0644);
   write_text_file(marketplace_hooks_json, hooks_buf, 0644);
   write_text_file(installed_hooks_json, hooks_buf, 0644);
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

/* Merge Aimee into Claude Code's USER MCP registry (~/.claude.json).  Current
 * Claude releases do not read mcpServers from ~/.claude/settings.json; that
 * file is for settings/hooks.  Keeping the JSON merge separate from binary
 * discovery makes the on-disk contract directly testable. */
static void ensure_claude_code_mcp_entry(const char *config_path, const char *aimee_bin)
{
   if (!config_path || !config_path[0] || !aimee_bin || !aimee_bin[0])
      return;

   cJSON *root = read_json_file(config_path);
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
         cJSON *type = cJSON_GetObjectItemCaseSensitive(aimee, "type");
         if (cJSON_IsString(type) && strcmp(type->valuestring, "stdio") == 0 &&
             cJSON_IsString(cmd) && strcmp(cmd->valuestring, aimee_bin) == 0 &&
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
   cJSON_AddStringToObject(aimee_server, "type", "stdio");
   cJSON_AddItemToObject(servers, "aimee", aimee_server);

   char *json = cJSON_Print(root);
   if (json)
   {
      write_text_file(config_path, json, 0600);
      free(json);
   }
   cJSON_Delete(root);
}

static void ensure_claude_code_mcp(const char *config_path)
{
   const char *aimee_bin = resolved_aimee_bin_path();
   struct stat st;
   if (stat(aimee_bin, &st) != 0)
      return;
   ensure_claude_code_mcp_entry(config_path, aimee_bin);
}

/* Remove only Aimee's obsolete settings.json registration after migrating it
 * to ~/.claude.json.  Preserve unrelated keys and any other legacy entries. */
static void remove_legacy_claude_settings_mcp(const char *settings_path)
{
   cJSON *root = read_json_file(settings_path);
   if (!cJSON_IsObject(root))
   {
      cJSON_Delete(root);
      return;
   }
   cJSON *servers = cJSON_GetObjectItemCaseSensitive(root, "mcpServers");
   if (!cJSON_IsObject(servers) || !cJSON_GetObjectItemCaseSensitive(servers, "aimee"))
   {
      cJSON_Delete(root);
      return;
   }
   cJSON_DeleteItemFromObjectCaseSensitive(servers, "aimee");
   if (cJSON_GetArraySize(servers) == 0)
      cJSON_DeleteItemFromObjectCaseSensitive(root, "mcpServers");
   char *json = cJSON_Print(root);
   if (json)
   {
      write_text_file(settings_path, json, 0600);
      free(json);
   }
   cJSON_Delete(root);
}

/* Ensure `hooks.<event>` contains an entry running
 * `AIMEE_HOOK_CLIENT=claude <aimee> <subcommand>`, optionally scoped to
 * `matcher` (NULL = fire on every event of this type). Idempotent (keyed on the
 * subcommand substring); sets *dirty when it adds the array or the entry. Used
 * for the context-pre-injection hooks (UserPromptSubmit, PreCompact — no
 * matcher) and the attention guard (PreToolUse — matcher-scoped). */
static void ensure_aimee_event_hook(cJSON *hooks, const char *event, const char *subcommand,
                                    const char *matcher, int *dirty)
{
   const char *aimee_bin = resolved_aimee_bin_path();
   char cmd[512];
   snprintf(cmd, sizeof(cmd), "AIMEE_HOOK_CLIENT=claude %s %s", aimee_bin ? aimee_bin : "aimee",
            subcommand);

   cJSON *arr = cJSON_GetObjectItemCaseSensitive(hooks, event);
   if (!cJSON_IsArray(arr))
   {
      if (arr)
         cJSON_DeleteItemFromObjectCaseSensitive(hooks, event);
      arr = cJSON_AddArrayToObject(hooks, event);
      *dirty = 1;
   }
   for (int i = 0; i < cJSON_GetArraySize(arr); i++)
   {
      cJSON *hook_arr = cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(arr, i), "hooks");
      if (!cJSON_IsArray(hook_arr))
         continue;
      for (int j = 0; j < cJSON_GetArraySize(hook_arr); j++)
      {
         cJSON *cmdj = cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(hook_arr, j), "command");
         if (cJSON_IsString(cmdj) && strstr(cmdj->valuestring, subcommand))
         {
            /* Found this aimee hook. Re-point it if its command references a
             * different (e.g. stale or transient) binary path, so a reinstall
             * to a new location heals the hook instead of leaving it dangling. */
            if (strcmp(cmdj->valuestring, cmd) != 0)
            {
               cJSON_SetValuestring(cmdj, cmd);
               *dirty = 1;
            }
            return;
         }
      }
   }
   cJSON *entry = cJSON_CreateObject();
   cJSON *hook_arr = cJSON_CreateArray();
   cJSON *hook = cJSON_CreateObject();
   if (entry && hook_arr && hook)
   {
      if (matcher && matcher[0])
         cJSON_AddStringToObject(entry, "matcher", matcher);
      cJSON_AddStringToObject(hook, "type", "command");
      cJSON_AddStringToObject(hook, "command", cmd);
      cJSON_AddItemToArray(hook_arr, hook);
      cJSON_AddItemToObject(entry, "hooks", hook_arr);
      cJSON_AddItemToArray(arr, entry);
      *dirty = 1;
   }
   else
   {
      cJSON_Delete(entry);
      cJSON_Delete(hook_arr);
      cJSON_Delete(hook);
   }
}

/* Remove any PreToolUse (or other event) hook entry whose command runs the given
 * aimee subcommand — the inverse of ensure_aimee_event_hook, for un-installing a
 * hook when its gate no longer holds. */
static void remove_aimee_event_hook(cJSON *hooks, const char *event, const char *subcommand,
                                    int *dirty)
{
   cJSON *arr = cJSON_GetObjectItemCaseSensitive(hooks, event);
   if (!cJSON_IsArray(arr))
      return;
   for (int i = cJSON_GetArraySize(arr) - 1; i >= 0; i--)
   {
      cJSON *hlist = cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(arr, i), "hooks");
      if (!cJSON_IsArray(hlist))
         continue;
      int match = 0;
      for (int j = 0; j < cJSON_GetArraySize(hlist); j++)
      {
         cJSON *cmd = cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(hlist, j), "command");
         if (cJSON_IsString(cmd) && strstr(cmd->valuestring, subcommand))
         {
            match = 1;
            break;
         }
      }
      if (match)
      {
         cJSON_DeleteItemFromArray(arr, i);
         *dirty = 1;
      }
   }
}

/* Ensure root.permissions.deny[] contains `tool` (creating permissions/deny as
 * needed). Idempotent; sets *dirty on any change. */
static void ensure_permissions_deny_tool(cJSON *root, const char *tool, int *dirty)
{
   cJSON *perms = cJSON_GetObjectItemCaseSensitive(root, "permissions");
   if (!cJSON_IsObject(perms))
   {
      if (perms)
         cJSON_DeleteItemFromObjectCaseSensitive(root, "permissions");
      perms = cJSON_AddObjectToObject(root, "permissions");
      *dirty = 1;
   }
   cJSON *deny = cJSON_GetObjectItemCaseSensitive(perms, "deny");
   if (!cJSON_IsArray(deny))
   {
      if (deny)
         cJSON_DeleteItemFromObjectCaseSensitive(perms, "deny");
      deny = cJSON_AddArrayToObject(perms, "deny");
      *dirty = 1;
   }
   for (int i = 0; i < cJSON_GetArraySize(deny); i++)
   {
      cJSON *e = cJSON_GetArrayItem(deny, i);
      if (cJSON_IsString(e) && strcmp(e->valuestring, tool) == 0)
         return; /* already denied */
   }
   cJSON_AddItemToArray(deny, cJSON_CreateString(tool));
   *dirty = 1;
}

/* Remove `tool` from root.permissions.deny[] if present, leaving other entries
 * (and other permissions) intact. */
static void remove_permissions_deny_tool(cJSON *root, const char *tool, int *dirty)
{
   cJSON *perms = cJSON_GetObjectItemCaseSensitive(root, "permissions");
   if (!cJSON_IsObject(perms))
      return;
   cJSON *deny = cJSON_GetObjectItemCaseSensitive(perms, "deny");
   if (!cJSON_IsArray(deny))
      return;
   for (int i = cJSON_GetArraySize(deny) - 1; i >= 0; i--)
   {
      cJSON *e = cJSON_GetArrayItem(deny, i);
      if (cJSON_IsString(e) && strcmp(e->valuestring, tool) == 0)
      {
         cJSON_DeleteItemFromArray(deny, i);
         *dirty = 1;
      }
   }
}

/* Materialize (or tear down) the sub-agent ban in the Claude Code settings.
 * The gate is evaluated ONCE here at client setup: config `subagent_ban_enabled`
 * (default on) AND the injected delegate probe reporting usable delegates. When
 * the gate holds we install a dedicated `subagent-guard` PreToolUse hook (carries
 * the actionable "use aimee delegate" message) PLUS a static permissions.deny
 * [Task, Agent] backstop that blocks the spawn even if the hook fails to run.
 * When the gate does not hold (config opt-out or no delegates) we remove both, so
 * a config/delegate change un-installs on the next setup / session-start. A probe
 * result of "unknown" (server unreachable) leaves settings untouched — we neither
 * install nor tear down on a transient outage. */
static void ensure_subagent_ban(cJSON *root, cJSON *hooks, int *dirty)
{
   /* Config opt-out is checked FIRST, from local aimee.yaml — no server call. So
    * `subagent_ban_enabled: false` reliably tears the ban down even when the
    * server is unreachable, and we never probe when the operator has opted out. */
   if (!client_config_bool("subagent_ban_enabled", 1))
   {
      remove_aimee_event_hook(hooks, "PreToolUse", "subagent-guard", dirty);
      remove_permissions_deny_tool(root, "Task", dirty);
      remove_permissions_deny_tool(root, "Agent", dirty);
      return;
   }

   int probe = g_delegate_probe ? g_delegate_probe() : -1; /* 1 avail, 0 none, -1 unknown */
   if (probe < 0)
      return; /* delegate availability unknown (server down / no probe): leave as-is */
   if (probe == 1)
   {
      /* Matcher covers every tool client_tool_is_subagent recognizes, incl.
       * RemoteTrigger. permissions.deny lists Task+Agent (the Claude-native
       * spawns); the hook backstops the rest with the actionable message. */
      ensure_aimee_event_hook(hooks, "PreToolUse", "subagent-guard",
                              "Agent|Task|Subagent|spawn_agent|RemoteTrigger", dirty);
      ensure_permissions_deny_tool(root, "Task", dirty);
      ensure_permissions_deny_tool(root, "Agent", dirty);
   }
   else /* probe == 0: no usable delegate to redirect to -> don't ban */
   {
      remove_aimee_event_hook(hooks, "PreToolUse", "subagent-guard", dirty);
      remove_permissions_deny_tool(root, "Task", dirty);
      remove_permissions_deny_tool(root, "Agent", dirty);
   }
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
         {
            found_aimee = 1;
            /* Re-point a stale/transient binary path to the resolved one, so a
             * reinstall heals this hook (mirrors ensure_aimee_event_hook). */
            const char *bin = resolved_aimee_bin_path();
            char want[512];
            snprintf(want, sizeof(want), "AIMEE_HOOK_CLIENT=claude %s hooks post",
                     bin ? bin : "aimee");
            if (strcmp(cmd->valuestring, want) != 0)
            {
               cJSON_SetValuestring(cmd, want);
               dirty = 1;
            }
         }
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

   /* Session-level injection: the SessionStart hook is what delivers aimee's
    * session brief (`aimee session-start` -> session_start_emit -> the persona
    * principles/brief, the MCP-skill index, Rules, and Key Facts via
    * build_session_context) as additionalContext. Without it the primary agent
    * starts with NO aimee persona/skills/rules context -- the per-turn
    * UserPromptSubmit + PreCompact hooks only re-prime memory RECALL, not the
    * brief, so this is the seam that establishes identity. No matcher: it fires
    * on startup/resume/clear AND compact, so the brief is re-injected after a
    * compaction (which PreCompact's recall-only re-prime does not restore).
    * session_start_emit gates its heavy startup work (db check, worktree
    * checkout) on is_startup, so the non-startup sources stay cheap. */
   ensure_aimee_event_hook(hooks, "SessionStart", "session-start", NULL, &dirty);
   /* Context pre-injection hooks: the P1 per-turn UserPromptSubmit envelope and
    * the P3 PreCompact re-prime. Both fire with no matcher and soft-fail, so
    * they never block a turn. */
   ensure_aimee_event_hook(hooks, "UserPromptSubmit", "user-prompt-submit", NULL, &dirty);
   ensure_aimee_event_hook(hooks, "PreCompact", "pre-compact", NULL, &dirty);
   /* P3 attention guard: PreToolUse hook scoped to read/edit/destructive tools;
    * accrues per-file attention and blocks hard-destructive ops on files the
    * session has actively touched. It does NOT gate sub-agent tools — that is the
    * dedicated `subagent-guard` hook installed by ensure_subagent_ban below (this
    * matcher deliberately no longer lists Task|Agent). */
   ensure_aimee_event_hook(hooks, "PreToolUse", "attention-guard",
                           "Read|Edit|Write|MultiEdit|NotebookEdit|Bash|Grep|Glob", &dirty);

   /* Sub-agent ban (delegate-only): gated at setup on subagent_ban_enabled AND a
    * one-shot delegate probe; installs/removes the subagent-guard hook + the
    * permissions.deny [Task, Agent] backstop accordingly. */
   ensure_subagent_ban(root, hooks, &dirty);

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
                   "Use the aimee MCP tool `" AIMEE_CODE_TOOL_PREVIEW_BLAST_RADIUS
                   "` for: $ARGUMENTS\n"
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
   char user_config_path[MAX_PATH_LEN];
   snprintf(user_config_path, sizeof(user_config_path), "%s/.claude.json", home);
   ensure_claude_code_mcp(user_config_path);
   remove_legacy_claude_settings_mcp(settings_path);
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

/* Read a single top-level boolean from aimee.yaml, returning default_val when
 * the file is absent/unparseable or the key is missing. client_integrations.c
 * links into the thin, DB-free CLI client, which excludes config.c (and thus
 * config_load), so we read the key directly with the lightweight yaml + cJSON
 * primitives the thin client does link, from the same path config_load uses
 * (aimee_home()/aimee.yaml). */
static int client_config_bool(const char *key, int default_val)
{
   const char *home = aimee_home();
   if (!home || !home[0])
      return default_val;

   char path[MAX_PATH_LEN];
   snprintf(path, sizeof(path), "%s/aimee.yaml", home);

   FILE *fp = fopen(path, "r");
   if (!fp)
      return default_val;
   fseek(fp, 0, SEEK_END);
   long sz = ftell(fp);
   fseek(fp, 0, SEEK_SET);
   if (sz < 0 || sz >= (long)(1 << 20))
   {
      fclose(fp);
      return default_val;
   }
   char *buf = malloc((size_t)sz + 1);
   if (!buf)
   {
      fclose(fp);
      return default_val;
   }
   size_t n = fread(buf, 1, (size_t)sz, fp);
   fclose(fp);
   buf[n] = '\0';

   cJSON *root = yaml_parse(buf);
   free(buf);

   int val = default_val;
   if (cJSON_IsObject(root))
   {
      cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
      if (cJSON_IsBool(item))
         val = cJSON_IsTrue(item);
   }
   if (root)
      cJSON_Delete(root);
   return val;
}

/* Whether aimee is allowed to auto-register itself into external AI-tool user
 * configs. The env var AIMEE_NO_CLIENT_INTEGRATIONS overrides the persisted
 * config: any non-empty value other than "0"/"false" forces the integrations
 * off for this run (useful for CI or a one-off install). Otherwise the
 * default-ON client_integrations_enabled config key decides. */
static int client_integrations_allowed(void)
{
   const char *env = getenv("AIMEE_NO_CLIENT_INTEGRATIONS");
   if (env && env[0] && strcmp(env, "0") != 0 && strcmp(env, "false") != 0)
      return 0;

   return client_config_bool("client_integrations_enabled", 1);
}

void ensure_client_integrations(void)
{
   if (!client_integrations_allowed())
      return;

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
