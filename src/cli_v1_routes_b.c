/* ===================================================================
 * /v1 thin-client routing: route CLI subcommands through the server's native /v1 HTTP endpoints.
 * Unported commands fail in cli_main before reaching the server.
 * =================================================================== */

#include "cli_argspec.h"
#include "cli_v1_routes_internal.h"
#include "platform_path.h"
#include "cli_client.h"
#define V1_PROTOCOL_VERSION 1
#include "util.h"
#include <aimee/workspace/client_diff.h> /* workspace.mirror-sync ships the client diff */
#include "aimee_client.h" /* aimee_client_request: transport-agnostic /v1 client (Windows path) */
#include "code_collect.h" /* code_collect_files + code_collect_discover_repos (thin-client push) */
#if !defined(_WIN32) && !defined(_WIN64)
#include "aimee_home.h"
#include <dirent.h>
#include <stdio.h> /* fprintf: the grant marshaller explains its own refusals */
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#endif /* !_WIN32 (preamble guard) */

static cJSON *marshal_aux_test(int argc, char **argv)
{
   cli_args_t opts;
   cli_args_parse(argc, argv, NULL, &opts);

   cJSON *req = marshal_no_args("aux.test");

   if (opts.pos_count > 0)
      cJSON_AddStringToObject(req, "task", opts.positional[0]);
   if (opts.pos_count > 1)
      cJSON_AddStringToObject(req, "prompt", opts.positional[1]);
   if (opts.pos_count > 2)
      cJSON_AddNumberToObject(req, "max_tokens", atoi(opts.positional[2]));
   return req;
}

static cJSON *marshal_agent_episodes(int argc, char **argv)
{
   cli_args_t opts;
   cli_args_parse(argc, argv, NULL, &opts);

   cJSON *req = marshal_no_args("model.episodes");

   const char *agent = opts.pos_count > 0 ? opts.positional[0] : cli_args_get(&opts, "agent");
   if (agent && agent[0])
      cJSON_AddStringToObject(req, "agent", agent);

   return req;
}

static char *cli_read_file_limited(const char *path, size_t limit, char *err, size_t err_len)
{
   FILE *f = fopen(path, "rb");
   if (!f)
   {
      snprintf(err, err_len, "cannot open %s", path ? path : "(null)");
      return NULL;
   }
   if (fseek(f, 0, SEEK_END) != 0)
   {
      fclose(f);
      snprintf(err, err_len, "cannot seek %s", path);
      return NULL;
   }
   long sz = ftell(f);
   if (sz <= 0 || (size_t)sz > limit)
   {
      fclose(f);
      snprintf(err, err_len, "plan file is empty or too large");
      return NULL;
   }
   rewind(f);
   char *buf = malloc((size_t)sz + 1);
   if (!buf)
   {
      fclose(f);
      snprintf(err, err_len, "out of memory reading plan");
      return NULL;
   }
   size_t n = fread(buf, 1, (size_t)sz, f);
   if (n != (size_t)sz && ferror(f))
   {
      fclose(f);
      free(buf);
      snprintf(err, err_len, "failed reading plan file");
      return NULL;
   }
   fclose(f);
   buf[n] = '\0';
   return buf;
}

static cJSON *marshal_skill_request(const char *method, int argc, char **argv)
{
   cJSON *req = cJSON_CreateObject();
   cJSON_AddStringToObject(req, "method", method);
   cJSON_AddNumberToObject(req, "protocol_version", V1_PROTOCOL_VERSION);
   char cwd[4096];
   if (getcwd(cwd, sizeof(cwd)))
      cJSON_AddStringToObject(req, "cwd", cwd);

   if (strcmp(method, "skill.list") == 0)
      return req;

   if (strcmp(method, "skill.lint") == 0)
   {
      if (argc >= 1 && strcmp(argv[0], "--all") == 0)
         cJSON_AddBoolToObject(req, "all", 1);
      else if (argc >= 1)
         cJSON_AddStringToObject(req, "name", argv[0]);
      return req;
   }

   if (strcmp(method, "skill.eval") == 0)
   {
      if (argc >= 1)
         cJSON_AddStringToObject(req, "name", argv[0]);
      return req;
   }

   if (strcmp(method, "skill.lifecycle") == 0)
   {
      for (int i = 0; i < argc; i++)
      {
         if (strcmp(argv[i], "--stale-days") == 0 && i + 1 < argc)
            cJSON_AddNumberToObject(req, "stale_after_days", atoi(argv[++i]));
         else if (strcmp(argv[i], "--archive-days") == 0 && i + 1 < argc)
            cJSON_AddNumberToObject(req, "archive_after_days", atoi(argv[++i]));
      }
      return req;
   }

   if (strcmp(method, "skill.autostub") == 0)
   {
      for (int i = 0; i < argc; i++)
      {
         if (strcmp(argv[i], "--force") == 0)
            cJSON_AddBoolToObject(req, "force", 1);
         else if (strcmp(argv[i], "--snapshot") == 0 && i + 1 < argc)
            cJSON_AddStringToObject(req, "snapshot_path", argv[++i]);
      }
      return req;
   }

   if (argc < 1)
      return req;
   cJSON_AddStringToObject(req, "name", argv[0]);

   if (strcmp(method, "skill.show") == 0)
   {
      if (argc == 3 && strcmp(argv[1], "--file") == 0)
         cJSON_AddStringToObject(req, "file_path", argv[2]);
      return req;
   }

   if (strcmp(method, "skill.create") == 0 || strcmp(method, "skill.edit") == 0)
   {
      if (argc >= 2)
      {
         char err[256] = "";
         char *content = cli_read_file_limited(argv[1], 100 * 1024, err, sizeof(err));
         if (content)
         {
            cJSON_AddStringToObject(req, "content", content);
            free(content);
         }
      }
      return req;
   }

   if (strcmp(method, "skill.patch") == 0)
   {
      if (argc >= 3)
      {
         cJSON_AddStringToObject(req, "old_string", argv[1]);
         cJSON_AddStringToObject(req, "new_string", argv[2]);
      }
      if (argc > 3 && strcmp(argv[3], "--all") == 0)
         cJSON_AddBoolToObject(req, "replace_all", 1);
      return req;
   }

   if (strcmp(method, "skill.archive") == 0)
   {
      if (argc > 2 && strcmp(argv[1], "--absorbed-into") == 0)
         cJSON_AddStringToObject(req, "absorbed_into", argv[2]);
      return req;
   }

   if (strcmp(method, "skill.pin") == 0 || strcmp(method, "skill.unpin") == 0)
      cJSON_AddBoolToObject(req, "pinned", strcmp(method, "skill.pin") == 0);

   return req;
}

static cJSON *marshal_delegate_launch(int argc, char **argv)
{
   static const char *bool_flags[] = {"json", NULL};
   cli_args_t opts;
   cli_args_parse(argc, argv, bool_flags, &opts);

   const char *path = opts.pos_count > 0 ? opts.positional[0] : NULL;
   if (!path || !path[0])
   {
      fprintf(stderr, "aimee: usage: aimee delegate launch <plan.json> [--parallel N]\n");
      return NULL;
   }

   char err[256] = "";
   char *text = cli_read_file_limited(path, 2 * 1024 * 1024, err, sizeof(err));
   if (!text)
   {
      fprintf(stderr, "aimee: delegate launch failed: %s\n", err[0] ? err : "cannot read plan");
      return NULL;
   }
   cJSON *plan = cJSON_Parse(text);
   free(text);
   if (!cJSON_IsObject(plan))
   {
      cJSON_Delete(plan);
      fprintf(stderr, "aimee: delegate launch failed: invalid plan JSON\n");
      return NULL;
   }

   cJSON *req = marshal_no_args("delegate.launch");
   cJSON_AddStringToObject(req, "plan_path", path);
   cJSON_AddItemToObject(req, "plan", plan);

   int parallel = cli_args_get_int(&opts, "parallel", 3);
   if (parallel > 0)
      cJSON_AddNumberToObject(req, "parallel", parallel);

   /* Anchor the coord launch worktree. handle_delegate_launch reads "cwd" as the
    * git-root anchor (resolution order: caller-supplied cwd first), under which it
    * creates the .aimee/worktrees/<id> the delegates write into. --workdir lets a
    * caller target a repo the SERVER can see (e.g. a benchmark checkout on the
    * remote host) instead of the client's own cwd; default to getcwd() so ordinary
    * local use is unchanged. */
   const char *workdir = cli_args_get(&opts, "workdir");
   char cwd_buf[4096];
   if (workdir && workdir[0])
      cJSON_AddStringToObject(req, "cwd", workdir);
   else if (getcwd(cwd_buf, sizeof(cwd_buf)))
      cJSON_AddStringToObject(req, "cwd", cwd_buf);
   return req;
}

static cJSON *marshal_trigger_list(int argc, char **argv)
{
   cli_args_t opts;
   cli_args_parse(argc, argv, NULL, &opts);

   cJSON *req = marshal_no_args("trigger.list");
   const char *status = cli_args_get(&opts, "status");
   if (status && status[0])
      cJSON_AddStringToObject(req, "status", status);
   return req;
}

static cJSON *marshal_trigger_id(const char *method, int argc, char **argv)
{
   cli_args_t opts;
   cli_args_parse(argc, argv, NULL, &opts);

   const char *id = opts.pos_count > 0 ? opts.positional[0] : cli_args_get(&opts, "id");
   if (!id || !id[0])
   {
      fprintf(stderr, "aimee: usage: aimee trigger %s <id>\n",
              strcmp(method, "trigger.status") == 0 ? "status" : "cancel");
      return NULL;
   }

   cJSON *req = cJSON_CreateObject();
   cJSON_AddStringToObject(req, "method", method);
   cJSON_AddNumberToObject(req, "protocol_version", V1_PROTOCOL_VERSION);
   cJSON_AddStringToObject(req, "id", id);
   return req;
}

static cJSON *marshal_trigger_fire(int argc, char **argv)
{
   cli_args_t opts;
   cli_args_parse(argc, argv, NULL, &opts);

   const char *source = cli_args_get(&opts, "source");
   const char *task = cli_args_get(&opts, "task");
   const char *proposal = cli_args_get(&opts, "proposal");

   /* Proposals one-at-a-time fire: `--source proposals --proposal <name> --workspace <ws>`
    * files exactly that pending proposal through the WFE pipeline (no --task needed). */
   int is_proposal_fire = source && strcmp(source, "proposals") == 0 && proposal && proposal[0];

   if (!source || !source[0] || (!is_proposal_fire && (!task || !task[0])))
   {
      fprintf(stderr,
              "aimee: usage:\n"
              "  aimee trigger fire --source <source> --task <task> [--event <e>] "
              "[--workspace <ws>] [--token <t>]\n"
              "  aimee trigger fire --source proposals --proposal <name> --workspace <ws> "
              "[--pipeline <wf>] [--ref <ref>] [--mode <m>] [--event <dir>] [--token <t>]\n");
      return NULL;
   }

   cJSON *req = marshal_no_args("trigger.fire");
   cJSON_AddStringToObject(req, "source", source);
   if (task && task[0])
      cJSON_AddStringToObject(req, "task", task);

   const char *v;
   if ((v = cli_args_get(&opts, "proposal")) && v[0])
      cJSON_AddStringToObject(req, "proposal", v);
   if ((v = cli_args_get(&opts, "pipeline")) && v[0])
      cJSON_AddStringToObject(req, "pipeline", v);
   if ((v = cli_args_get(&opts, "ref")) && v[0])
      cJSON_AddStringToObject(req, "ref", v);
   if ((v = cli_args_get(&opts, "mode")) && v[0])
      cJSON_AddStringToObject(req, "mode", v);
   if ((v = cli_args_get(&opts, "event")) && v[0])
      cJSON_AddStringToObject(req, "event", v);
   if ((v = cli_args_get(&opts, "workspace")) && v[0])
      cJSON_AddStringToObject(req, "workspace", v);
   if ((v = cli_args_get(&opts, "token")) && v[0])
      cJSON_AddStringToObject(req, "auth_token", v);
   if ((v = cli_args_get(&opts, "metadata")) && v[0])
      cJSON_AddStringToObject(req, "metadata", v);
   return req;
}

static cJSON *marshal_cron_id(const char *method, int argc, char **argv)
{
   static const char *bools[] = {"all", NULL};
   cli_args_t opts;
   cli_args_parse(argc, argv, bools, &opts);

   const char *id = opts.pos_count > 0 ? opts.positional[0] : cli_args_get(&opts, "id");
   int all = (strcmp(method, "cron.enable") == 0 || strcmp(method, "cron.disable") == 0) &&
             cli_args_get(&opts, "all") != NULL;
   if (!id || !id[0])
   {
      if (all)
      {
         cJSON *req = cJSON_CreateObject();
         cJSON_AddStringToObject(req, "method", method);
         cJSON_AddNumberToObject(req, "protocol_version", V1_PROTOCOL_VERSION);
         cJSON_AddBoolToObject(req, "all", 1);
         return req;
      }
      const char *sub = "show";
      if (strcmp(method, "cron.history") == 0)
         sub = "history";
      else if (strcmp(method, "cron.run") == 0)
         sub = "run";
      else if (strcmp(method, "cron.enable") == 0)
         sub = "enable";
      else if (strcmp(method, "cron.disable") == 0)
         sub = "disable";
      fprintf(stderr, "aimee: usage: aimee cron %s <id>\n", sub);
      return NULL;
   }

   cJSON *req = cJSON_CreateObject();
   cJSON_AddStringToObject(req, "method", method);
   cJSON_AddNumberToObject(req, "protocol_version", V1_PROTOCOL_VERSION);
   cJSON_AddStringToObject(req, "job_id", id);
   if (strcmp(method, "cron.history") == 0)
      cJSON_AddNumberToObject(req, "limit", cli_args_get_int(&opts, "limit", 20));
   return req;
}

static cJSON *marshal_cron_add(int argc, char **argv)
{
   static const char *bools[] = {"only-if-changed", "first-run-silent", "pre-wake-gate", "disabled",
                                 NULL};
   cli_args_t opts;
   cli_args_parse(argc, argv, bools, &opts);

   const char *id = opts.pos_count > 0 ? opts.positional[0] : cli_args_get(&opts, "id");
   const char *schedule = cli_args_get(&opts, "schedule");
   if (!id || !id[0] || !schedule || !schedule[0])
   {
      fprintf(stderr, "aimee: usage: aimee cron add <id> --schedule S [--mode llm|script|hybrid] "
                      "[--script CMD] [--prompt TEXT]\n");
      return NULL;
   }

   cJSON *req = marshal_no_args("cron.add");
   cJSON_AddStringToObject(req, "job_id", id);
   cJSON_AddStringToObject(req, "schedule", schedule);

   const char *v;
   if ((v = cli_args_get(&opts, "mode")) && v[0])
      cJSON_AddStringToObject(req, "mode", v);
   if ((v = cli_args_get(&opts, "script")) && v[0])
      cJSON_AddStringToObject(req, "script", v);
   if ((v = cli_args_get(&opts, "prompt")) && v[0])
      cJSON_AddStringToObject(req, "prompt", v);
   if ((v = cli_args_get(&opts, "workdir")) && v[0])
      cJSON_AddStringToObject(req, "workdir", v);
   if ((v = cli_args_get(&opts, "target")) && v[0])
      cJSON_AddStringToObject(req, "deliver_target", v);
   if ((v = cli_args_get(&opts, "context-from")) && v[0])
      cJSON_AddStringToObject(req, "context_from", v);
   if ((v = cli_args_get(&opts, "when-context-contains")) && v[0])
      cJSON_AddStringToObject(req, "when_context_contains", v);
   if (cli_args_get(&opts, "only-if-changed"))
      cJSON_AddBoolToObject(req, "deliver_only_if_changed", 1);
   if (cli_args_get(&opts, "first-run-silent"))
      cJSON_AddBoolToObject(req, "deliver_first_run_silent", 1);
   if (cli_args_get(&opts, "pre-wake-gate"))
      cJSON_AddBoolToObject(req, "pre_wake_gate", 1);
   if (cli_args_get(&opts, "disabled"))
      cJSON_AddBoolToObject(req, "enabled", 0);

   cJSON *skills = cJSON_CreateArray();
   for (int i = 0; i < opts.flag_count; i++)
   {
      const char *raw = opts.flags[i].raw;
      const char *eq = strchr(raw, '=');
      size_t rlen = eq ? (size_t)(eq - raw) : strlen(raw);
      if (rlen == strlen("skill") && memcmp(raw, "skill", rlen) == 0 && opts.flags[i].value &&
          opts.flags[i].value[0])
         cJSON_AddItemToArray(skills, cJSON_CreateString(opts.flags[i].value));
   }
   if (cJSON_GetArraySize(skills) > 0)
      cJSON_AddItemToObject(req, "skills", skills);
   else
      cJSON_Delete(skills);
   return req;
}

static cJSON *marshal_agent_args(const char *method, int argc, char **argv)
{
   cJSON *req = cJSON_CreateObject();
   cJSON_AddStringToObject(req, "method", method);
   cJSON_AddNumberToObject(req, "protocol_version", V1_PROTOCOL_VERSION);
   cJSON *args = cJSON_CreateArray();
   for (int i = 0; i < argc; i++)
      cJSON_AddItemToArray(args, cJSON_CreateString(argv[i]));
   cJSON_AddItemToObject(req, "args", args);
   return req;
}

/* workspace.add: keep the {method,args} body the socket dispatch parses, but also
 * surface root + provider as top-level fields so the first-class REST route
 * POST /v1/workspaces (which reads {root, provider}) accepts the same body when
 * the thin client routes over a remote /v1 endpoint. The route's response is the
 * raw dispatch result (ws_dispatch_args -> loopback_rpc), so it parses identically
 * to the socket path. */
cJSON *marshal_workspace_add(int argc, char **argv)
{
   cJSON *req = marshal_agent_args("workspace.add", argc, argv);
   if (!req)
      return NULL;
   cli_args_t opts;
   /* --no-scan takes no value, so it must be declared boolean: cli_args_parse would
    * otherwise treat the following argument as its value, and swallow the flag
    * entirely when it is last. */
   static const char *ws_bool_flags[] = {"no-scan", NULL};
   cli_args_parse(argc, argv, ws_bool_flags, &opts);
   if (opts.pos_count > 0)
      cJSON_AddStringToObject(req, "root", opts.positional[0]);
   const char *prov = cli_args_get(&opts, "provider");
   if (prov)
      cJSON_AddStringToObject(req, "provider", prov);
   /* The mirror coordinates need surfacing for the same reason root and provider
    * do. This body reaches the server through POST /v1/workspaces -- including
    * locally, over the HTTP UDS -- and that route reads top-level fields, not
    * `args`. Left in args only, they were silently dropped, so
    * `aimee workspace add <path> --provider mirror --remote <url>` came back with
    * "--provider mirror requires --remote <url>" while staring at the flag the
    * user had just typed. The socket dispatch still parses argv, so both paths
    * now carry them. */
   const char *rem = cli_args_get(&opts, "remote");
   if (rem)
      cJSON_AddStringToObject(req, "remote", rem);
   const char *hd = cli_args_get(&opts, "head");
   if (hd)
      cJSON_AddStringToObject(req, "head", hd);
   /* --no-scan registers the workspace and returns instead of walking every
    * discovered project first. On a large tree the eager scan makes this RPC
    * take minutes, so a caller with a timeout abandons a registration that
    * already succeeded; the background ingest timer indexes them regardless.
    * Omitted (not sent as true) by default, so an older server sees exactly the
    * body it saw before. */
   if (cli_args_has_flag(&opts, "no-scan"))
      cJSON_AddBoolToObject(req, "scan", 0);
   return req;
}

/* `aimee workspace mirror-sync <root>`: ship the client's full working-tree patch
 * (see mirror_compute_diff) to the server, which stores it for the mirror
 * workspace's next reconstruct (workspace-resource-plane §3). A clean tree ships
 * an empty diff (the reconstruct is then a clean checkout at head). */
static cJSON *marshal_workspace_mirror_sync(int argc, char **argv)
{
   if (argc < 1 || !argv[0][0])
   {
      fprintf(stderr, "usage: aimee workspace mirror-sync <root>\n");
      return NULL;
   }
   const char *root = argv[0];
   /* The patch and the commit it applies to are one fact, so both are sent. The
    * base is the newest ancestor of HEAD that a remote has: unpushed commits
    * cannot be fetched server-side, so they travel inside the patch instead. */
   char base[64] = "";
   if (workspace_client_mirror_base(root, base, sizeof(base)) != 0)
   {
      fprintf(stderr,
              "aimee: %s has no commit that exists on a remote, so the server has nothing to "
              "reconstruct from. Push a commit (even an old one) and retry.\n",
              root);
      return NULL;
   }
   char *patch = workspace_client_diff_compute(root, base);
   if (!patch)
      fprintf(stderr,
              "warning: could not compute a diff for %s against %.10s; shipping an empty diff\n",
              root, base);

   cJSON *req = marshal_no_args("workspace.mirror-sync");
   cJSON *args = cJSON_CreateArray();
   cJSON_AddItemToArray(args, cJSON_CreateString(root));
   cJSON_AddItemToObject(req, "args", args);
   cJSON_AddStringToObject(req, "head", base);
   cJSON_AddStringToObject(req, "diff", patch ? patch : "");
   char *line = NULL;
   const char *br[] = {"git", "-C", root, "symbolic-ref", "--quiet", "--short", "HEAD", NULL};
   if (safe_exec_capture(br, &line, 512) == 0 && line)
   {
      line[strcspn(line, "\r\n")] = '\0';
      cJSON_AddStringToObject(req, "branch", line);
   }
   free(line);
   line = NULL;
   const char *up[] = {
       "git", "-C", root, "rev-parse", "--abbrev-ref", "--symbolic-full-name", "@{upstream}", NULL};
   if (safe_exec_capture(up, &line, 512) == 0 && line)
   {
      line[strcspn(line, "\r\n")] = '\0';
      cJSON_AddStringToObject(req, "upstream", line);
   }
   free(line);
   free(patch);
   return req;
}

static void add_verify_arg(cJSON *args, const char *name, const char *val)
{
   if (!name || !name[0] || !val)
      return;
   if (strcmp(val, "true") == 0)
      cJSON_AddBoolToObject(args, name, 1);
   else if (strcmp(val, "false") == 0)
      cJSON_AddBoolToObject(args, name, 0);
   else
   {
      char *end = NULL;
      long n = strtol(val, &end, 10);
      if (val[0] && end && *end == '\0')
         cJSON_AddNumberToObject(args, name, n);
      else
         cJSON_AddStringToObject(args, name, val);
   }
}

/* Every git subcommand except verify, from the CLI.
 *
 * The shape is uniform on purpose — `aimee git <command> [primary] [key=value]`,
 * the same form `aimee git verify action=... key=value` already taught — so there
 * is ONE parser here rather than a hand-written flag grammar per subcommand. The
 * only per-command knowledge is what a bare first (and sometimes second) word
 * means, which is the table below; everything else is key=value and is typed the
 * same way verify's arguments are.
 *
 * `aimee git merge origin/testing`, `aimee git sync`, `aimee git rebase continue`,
 * `aimee git add -A`, `aimee git pr create title="..."`. */
static const struct
{
   const char *sub;    /* CLI subcommand (dashes allowed) */
   const char *tool;   /* MCP tool it dispatches to */
   const char *first;  /* what a bare first word means, NULL if none */
   const char *second; /* what a bare second word means, NULL if none */
   int rest_files;     /* remaining bare words accumulate into `files` */
} GIT_CLI[] = {
    {"status", "git_status", NULL, NULL, 0},
    {"commit", "git_commit", "message", NULL, 1},
    {"push", "git_push", NULL, NULL, 0},
    {"pull", "git_pull", NULL, NULL, 0},
    {"fetch", "git_fetch", "remote", NULL, 0},
    {"branch", "git_branch", "action", "name", 0},
    {"log", "git_log", NULL, NULL, 0},
    {"diff", "git_diff_summary", "ref", NULL, 1},
    {"diff_summary", "git_diff_summary", "ref", NULL, 1},
    {"pr", "git_pr", "action", NULL, 0},
    {"issue", "git_issue", "action", NULL, 0},
    {"clone", "git_clone", "url", "path", 0},
    {"stash", "git_stash", "action", NULL, 0},
    {"tag", "git_tag", "action", "name", 0},
    {"reset", "git_reset", "ref", "mode", 0},
    {"restore", "git_restore", NULL, NULL, 1},
    {"add", "git_add", NULL, NULL, 1},
    {"merge", "git_merge", "ref", NULL, 0},
    {"rebase", "git_rebase", "base", NULL, 0},
    {"sync", "git_sync", "base", NULL, 0},
    {"cherry-pick", "git_cherry_pick", "ref", NULL, 0},
    {"cherry_pick", "git_cherry_pick", "ref", NULL, 0},
    {"revert", "git_revert", "ref", NULL, 0},
    {"switch", "git_switch", "ref", NULL, 0},
    {"checkout", "git_checkout", NULL, NULL, 1},
    {NULL, NULL, NULL, NULL, 0},
};

/* Bare words that mean an action rather than a ref, for the operations that can
 * stop mid-flight. Spelled with or without dashes. */
static int git_cli_is_resume_word(const char *w)
{
   while (*w == '-')
      w++;
   return strcmp(w, "continue") == 0 || strcmp(w, "abort") == 0 || strcmp(w, "skip") == 0;
}

/* Short flags worth keeping, because typing them is reflex. Everything else is
 * key=value, which needs no aliasing. */
static int git_cli_flag_alias(const char *arg, cJSON *args)
{
   static const struct
   {
      const char *flag;
      const char *key;
      int value;
   } aliases[] = {
       {"-A", "all", 1},
       {"--all", "all", 1},
       {"-f", "force", 1},
       {"--force", "force", 1},
       {"--rebase", "rebase", 1},
       {"--prune", "prune", 1},
       {"--auto", "auto", 1},
       {"--staged", "staged", 1},
       {"--keep-conflicts", "abort_on_conflict", 0},
       {NULL, NULL, 0},
   };
   for (int i = 0; aliases[i].flag; i++)
      if (strcmp(arg, aliases[i].flag) == 0)
      {
         cJSON_AddBoolToObject(args, aliases[i].key, aliases[i].value);
         return 1;
      }
   if (strcmp(arg, "--merge") == 0)
   {
      cJSON_AddStringToObject(args, "mode", "merge");
      return 1;
   }
   return 0;
}

static cJSON *marshal_git_cli(int argc, char **argv)
{
   if (argc < 1 || !argv[0] || !argv[0][0])
   {
      fprintf(stderr, "usage: aimee git <command> [primary] [key=value ...]\n"
                      "  status commit push pull fetch branch log diff pr issue clone stash\n"
                      "  tag reset restore verify add merge rebase sync cherry-pick revert\n"
                      "  switch checkout\n");
      return NULL;
   }

   const char *sub = argv[0];
   int row = -1;
   for (int i = 0; GIT_CLI[i].sub; i++)
      if (strcmp(sub, GIT_CLI[i].sub) == 0)
      {
         row = i;
         break;
      }
   if (row < 0)
   {
      fprintf(stderr,
              "aimee: '%s' is not a git command. Try: status commit push pull fetch "
              "branch log diff pr issue clone stash tag reset restore verify add merge "
              "rebase sync cherry-pick revert switch checkout\n",
              sub);
      return NULL;
   }

   cJSON *req = cJSON_CreateObject();
   cJSON_AddStringToObject(req, "method", "mcp.call");
   cJSON_AddStringToObject(req, "tool", GIT_CLI[row].tool);
   const char *sid = getenv("AIMEE_SESSION_ID");
   if (!sid || !sid[0])
      sid = getenv("CLAUDE_SESSION_ID");
   if (sid && sid[0])
      cJSON_AddStringToObject(req, "session_id", sid);

   cJSON *args = cJSON_CreateObject();
   cJSON *files = NULL;
   int bare = 0;

   for (int i = 1; i < argc; i++)
   {
      char *arg = argv[i];

      if (git_cli_flag_alias(arg, args))
         continue;

      /* key=value / --key=value, typed exactly as verify types its arguments. */
      const char *raw = arg;
      if (strncmp(raw, "--", 2) == 0)
         raw += 2;
      char *eq = strchr(raw, '=');
      if (eq)
      {
         *eq = '\0';
         add_verify_arg(args, raw, eq + 1);
         *eq = '=';
         continue;
      }

      if (arg[0] == '-')
      {
         /* A bare --flag is a boolean, which is how every boolean in the git
          * schema reads anyway. */
         cJSON_AddBoolToObject(args, raw, 1);
         continue;
      }

      /* continue/abort/skip is an action wherever an operation can stop. */
      if (git_cli_is_resume_word(arg) && !cJSON_GetObjectItemCaseSensitive(args, "action"))
      {
         cJSON_AddStringToObject(args, "action", arg);
         continue;
      }

      bare++;
      const char *key = (bare == 1) ? GIT_CLI[row].first : (bare == 2 ? GIT_CLI[row].second : NULL);
      if (key && !cJSON_GetObjectItemCaseSensitive(args, key))
      {
         cJSON_AddStringToObject(args, key, arg);
         continue;
      }
      if (GIT_CLI[row].rest_files)
      {
         if (!files)
            files = cJSON_AddArrayToObject(args, "files");
         cJSON_AddItemToArray(files, cJSON_CreateString(arg));
         continue;
      }
      fprintf(stderr,
              "aimee: `aimee git %s` does not take '%s' as a bare word; pass it as "
              "key=value (see `aimee git %s` with no arguments, or the git tool schema)\n",
              sub, arg, sub);
      cJSON_Delete(args);
      cJSON_Delete(req);
      return NULL;
   }

   /* Which checkout this is, as everywhere else: the caller's directory unless
    * they named one. Added last — cJSON keeps duplicates and readers take the
    * first, so an explicit path= must not be shadowed. */
   if (!cJSON_GetObjectItemCaseSensitive(args, "path"))
   {
      char cwd[4096];
      if (getcwd(cwd, sizeof(cwd)))
         cJSON_AddStringToObject(args, "path", cwd);
   }

   cJSON_AddItemToObject(req, "arguments", args);
   return req;
}

static cJSON *marshal_git_verify(int argc, char **argv)
{
   cJSON *req = cJSON_CreateObject();
   cJSON_AddStringToObject(req, "method", "mcp.call");
   cJSON_AddStringToObject(req, "tool", "git_verify");
   const char *sid = getenv("AIMEE_SESSION_ID");
   if (!sid || !sid[0])
      sid = getenv("CLAUDE_SESSION_ID");
   if (sid && sid[0])
      cJSON_AddStringToObject(req, "session_id", sid);

   cJSON *args = cJSON_CreateObject();
   cJSON_AddBoolToObject(args, "no_session_redirect", 1);
   /* The cwd default is filled in AFTER the caller's arguments, not before.
    *
    * cJSON permits duplicate keys and cJSON_GetObjectItemCaseSensitive returns
    * the FIRST match, so adding path=<cwd> up here made an explicit
    * `aimee git verify path=<repo>` a second, unreachable entry. The server read
    * the cwd every time and the user's path was silently discarded -- which made
    * the error message's own advice ("pass path=<repo> to target it explicitly")
    * impossible to act on. */

   int has_async = 0;
   for (int i = 0; i < argc; i++)
   {
      if (strcmp(argv[i], "--status") == 0)
      {
         if (i + 1 >= argc)
         {
            cJSON_Delete(req);
            cJSON_Delete(args);
            return NULL;
         }
         cJSON_AddStringToObject(args, "action", "status");
         cJSON_AddNumberToObject(args, "job_id", atoi(argv[++i]));
         continue;
      }
      if (strncmp(argv[i], "--status=", 9) == 0)
      {
         cJSON_AddStringToObject(args, "action", "status");
         cJSON_AddNumberToObject(args, "job_id", atoi(argv[i] + 9));
         continue;
      }

      const char *raw = argv[i];
      if (strncmp(raw, "--", 2) == 0)
         raw += 2;

      char *eq = strchr(raw, '=');
      if (!eq)
      {
         cJSON_Delete(req);
         cJSON_Delete(args);
         return NULL;
      }

      *eq = '\0';
      const char *val = eq + 1;
      if (strcmp(raw, "async") == 0)
         has_async = 1;
      add_verify_arg(args, raw, val);
      *eq = '=';
   }

   /* Default the target to the caller's shell directory only when they did not
    * name one. The server prefers a git root over a bare directory, so sending
    * the cwd is a useful default and a poor override. */
   if (!cJSON_GetObjectItemCaseSensitive(args, "path"))
   {
      char cwd[4096];
      if (getcwd(cwd, sizeof(cwd)))
         cJSON_AddStringToObject(args, "path", cwd);
   }

   if (!has_async)
      cJSON_AddBoolToObject(args, "async", 0);
   cJSON_AddItemToObject(req, "arguments", args);
   return req;
}

static cJSON *marshal_get_help(int argc, char **argv)
{
   cJSON *req = cJSON_CreateObject();
   cJSON_AddStringToObject(req, "method", "help.get");
   cJSON_AddStringToObject(req, "tool", "get_help");

   cJSON *args = cJSON_CreateObject();
   if (argc > 0)
   {
      size_t total = 1;
      for (int i = 0; i < argc; i++)
         total += strlen(argv[i]) + 1;
      char *topic = malloc(total);
      if (!topic)
      {
         cJSON_Delete(req);
         cJSON_Delete(args);
         return NULL;
      }
      topic[0] = '\0';
      for (int i = 0; i < argc; i++)
      {
         if (i > 0)
            strcat(topic, " ");
         strcat(topic, argv[i]);
      }
      cJSON_AddStringToObject(args, "topic", topic);
      free(topic);
   }

   cJSON_AddItemToObject(req, "arguments", args);
   return req;
}

static cJSON *marshal_provider_set(int argc, char **argv)
{
   cJSON *req = marshal_no_args("provider.set");
   if (argc > 0 && argv[0] && argv[0][0])
      cJSON_AddStringToObject(req, "name", argv[0]);
   return req;
}

static cJSON *marshal_provider_list(int argc, char **argv)
{
   static const char *bool_flags[] = {"available", "all", "json", NULL};
   cli_args_t opts;
   cli_args_parse(argc, argv, bool_flags, &opts);
   cJSON *req = marshal_no_args("provider.list");
   if (!req)
      return NULL;
   if (cli_args_get(&opts, "available"))
      cJSON_AddTrueToObject(req, "available_only");
   if (cli_args_get(&opts, "all"))
      cJSON_AddTrueToObject(req, "all");
   if (cli_args_get(&opts, "json"))
      cJSON_AddTrueToObject(req, "json");
   return req;
}

static cJSON *marshal_provider_name_method(const char *method, int argc, char **argv)
{
   cli_args_t opts;
   cli_args_parse(argc, argv, NULL, &opts);
   cJSON *req = marshal_no_args(method);
   if (!req)
      return NULL;
   if (opts.pos_count > 0 && opts.positional[0] && opts.positional[0][0])
      cJSON_AddStringToObject(req, "name", opts.positional[0]);
   return req;
}

static cJSON *marshal_provider_models(int argc, char **argv)
{
   static const char *bool_flags[] = {"json", NULL};
   cli_args_t opts;
   cli_args_parse(argc, argv, bool_flags, &opts);
   cJSON *req = marshal_no_args("provider.models");
   if (!req)
      return NULL;
   if (opts.pos_count > 0 && opts.positional[0] && opts.positional[0][0])
      cJSON_AddStringToObject(req, "name", opts.positional[0]);
   if (cli_args_get(&opts, "json"))
      cJSON_AddTrueToObject(req, "json");
   return req;
}

static cJSON *marshal_model_list(int argc, char **argv)
{
   static const char *bool_flags[] = {"json", "open-weights", NULL};
   cli_args_t opts;
   cli_args_parse(argc, argv, bool_flags, &opts);
   cJSON *req = marshal_no_args("catalog.list");
   if (!req)
      return NULL;
   const char *capability = cli_args_get(&opts, "capability");
   if (capability && capability[0])
      cJSON_AddStringToObject(req, "capability", capability);
   if (cli_args_get(&opts, "json"))
      cJSON_AddTrueToObject(req, "json");
   if (cli_args_has_flag(&opts, "open-weights"))
      cJSON_AddBoolToObject(req, "open_weights_only", 1);
   return req;
}

static cJSON *marshal_model_show(int argc, char **argv)
{
   cli_args_t opts;
   cli_args_parse(argc, argv, NULL, &opts);
   if (opts.pos_count < 1)
   {
      fprintf(stderr, "aimee: usage: aimee catalog show [provider:]<model>\n");
      return NULL;
   }
   cJSON *req = marshal_no_args("catalog.show");
   if (!req)
      return NULL;
   const char *spec = opts.positional[0];
   cJSON_AddStringToObject(req, "name", spec);
   const char *colon = strchr(spec, ':');
   if (colon && colon != spec && colon[1])
   {
      char provider[64];
      size_t plen = (size_t)(colon - spec);
      if (plen >= sizeof(provider))
         plen = sizeof(provider) - 1;
      memcpy(provider, spec, plen);
      provider[plen] = '\0';
      cJSON_AddStringToObject(req, "provider", provider);
      cJSON_AddStringToObject(req, "model", colon + 1);
   }
   else
      cJSON_AddStringToObject(req, "model", spec);
   return req;
}

static cJSON *marshal_dogfood_tag(int argc, char **argv)
{
   static const char *bool_flags[] = {"surprise", "no-surprise", NULL};
   cli_args_t opts;
   cli_args_parse(argc, argv, bool_flags, &opts);
   cJSON *req = marshal_no_args("dogfood.tag");
   if (opts.pos_count > 0)
      cJSON_AddStringToObject(req, "record_id", opts.positional[0]);
   const char *outcome = cli_args_get(&opts, "outcome");
   if (outcome)
      cJSON_AddStringToObject(req, "outcome", outcome);
   const char *notes = cli_args_get(&opts, "notes");
   if (notes)
      cJSON_AddStringToObject(req, "notes", notes);
   const char *richness = cli_args_get(&opts, "richness");
   if (richness)
      cJSON_AddNumberToObject(req, "richness", atoi(richness));
   if (cli_args_get(&opts, "surprise"))
      cJSON_AddTrueToObject(req, "surprise");
   else if (cli_args_get(&opts, "no-surprise"))
      cJSON_AddFalseToObject(req, "surprise");
   return req;
}

static cJSON *marshal_dogfood_report(int argc, char **argv)
{
   static const char *bool_flags[] = {"json", NULL};
   cli_args_t opts;
   cli_args_parse(argc, argv, bool_flags, &opts);
   cJSON *req = marshal_no_args("dogfood.report");
   const char *month = cli_args_get(&opts, "month");
   if (month)
      cJSON_AddStringToObject(req, "month", month);
   const char *dir = cli_args_get(&opts, "dir");
   if (dir)
      cJSON_AddStringToObject(req, "dir", dir);
   return req;
}

static cJSON *marshal_dogfood_review(int argc, char **argv)
{
   static const char *bool_flags[] = {"json", NULL};
   cli_args_t opts;
   cli_args_parse(argc, argv, bool_flags, &opts);
   cJSON *req = marshal_no_args("dogfood.review");
   const char *month = cli_args_get(&opts, "month");
   if (month)
      cJSON_AddStringToObject(req, "month", month);
   const char *dir = cli_args_get(&opts, "dir");
   if (dir)
      cJSON_AddStringToObject(req, "dir", dir);
   const char *limit = cli_args_get(&opts, "limit");
   if (limit)
      cJSON_AddNumberToObject(req, "limit", atoi(limit));
   return req;
}

static cJSON *marshal_identity_snapshot(int argc, char **argv)
{
   cli_args_t opts;
   cli_args_parse(argc, argv, NULL, &opts);
   cJSON *req = marshal_no_args("identity.snapshot");
   const char *out = cli_args_get(&opts, "out");
   if (out)
      cJSON_AddStringToObject(req, "out", out);
   char cwd[4096];
   if (getcwd(cwd, sizeof(cwd)))
      cJSON_AddStringToObject(req, "cwd", cwd);
   return req;
}

static cJSON *marshal_identity_diff(int argc, char **argv)
{
   cli_args_t opts;
   cli_args_parse(argc, argv, NULL, &opts);
   cJSON *req = marshal_no_args("identity.diff");
   if (opts.pos_count > 0)
      cJSON_AddStringToObject(req, "a", opts.positional[0]);
   if (opts.pos_count > 1)
      cJSON_AddStringToObject(req, "b", opts.positional[1]);
   const char *ft = cli_args_get(&opts, "flip-threshold");
   if (ft)
      cJSON_AddNumberToObject(req, "flip_threshold", atof(ft));
   char cwd[4096];
   if (getcwd(cwd, sizeof(cwd)))
      cJSON_AddStringToObject(req, "cwd", cwd);
   return req;
}

/* api.enable: forward the --vscode / --port / --rate-limit flags so the server
 * can apply them (the server owns the aimee.api config and mints the bearer). */
static cJSON *marshal_api_enable(int argc, char **argv)
{
   const char *bool_flags[] = {"vscode", NULL};
   cli_args_t opts;
   cli_args_parse(argc, argv, bool_flags, &opts);

   cJSON *req = marshal_no_args("api.enable");
   if (!req)
      return NULL;
   if (cli_args_has_flag(&opts, "vscode"))
      cJSON_AddBoolToObject(req, "vscode", 1);
   int port = cli_args_get_int(&opts, "port", 0);
   if (port > 0)
      cJSON_AddNumberToObject(req, "port", port);
   int rate = cli_args_get_int(&opts, "rate-limit", 0);
   if (rate > 0)
      cJSON_AddNumberToObject(req, "rate_limit", rate);
   return req;
}

/* graph sync-code <project> → graph.sync_code (server runs the code-graph
 * projection off-thread). Without this marshaler the route table entry existed
 * but the thin CLI reported "no /v1 route". */
static cJSON *marshal_graph_sync_code(int argc, char **argv)
{
   cli_args_t opts;
   cli_args_parse(argc, argv, NULL, &opts);
   cJSON *req = marshal_no_args("graph.sync_code");
   if (opts.pos_count >= 1)
      cJSON_AddStringToObject(req, "project", opts.positional[0]);
   return req;
}

/* graph explain <entity> [--limit N] → graph.explain. */
static cJSON *marshal_graph_explain(int argc, char **argv)
{
   cli_args_t opts;
   cli_args_parse(argc, argv, NULL, &opts);
   cJSON *req = marshal_no_args("graph.explain");
   if (opts.pos_count >= 1)
      cJSON_AddStringToObject(req, "entity", opts.positional[0]);
   cJSON_AddNumberToObject(req, "limit", cli_args_get_int(&opts, "limit", 40));
   return req;
}

/* Roundtable authoring pipeline (pipeline.*). */
static cJSON *marshal_pipeline_request(const char *method, int argc, char **argv)
{
   /* No bool flags: `--admin` was removed — a merge needing an admin override of
    * branch protection is human-only (operator ruling 2026-07-15). */
   static const char *bool_flags[] = {NULL};
   cli_args_t opts;
   cli_args_parse(argc, argv, bool_flags, &opts);

   cJSON *req = cJSON_CreateObject();
   cJSON_AddStringToObject(req, "method", method);
   cJSON_AddNumberToObject(req, "protocol_version", V1_PROTOCOL_VERSION);
   const char *v;

   if (strcmp(method, "pipeline.start") == 0)
   {
      if (opts.pos_count > 0)
         cJSON_AddStringToObject(req, "idea", opts.positional[0]);
      if ((v = cli_args_get(&opts, "done-bar")) && v[0])
         cJSON_AddStringToObject(req, "done_bar", v);
      if ((v = cli_args_get(&opts, "base-branch")) && v[0])
         cJSON_AddStringToObject(req, "base_branch", v);
      if ((v = cli_args_get(&opts, "repo-root")) && v[0])
         cJSON_AddStringToObject(req, "repo_root", v);
      if ((v = cli_args_get(&opts, "brief")) && v[0])
         cJSON_AddStringToObject(req, "brief", v);
      /* PR-lifecycle fields (#2). */
      if ((v = cli_args_get(&opts, "head-branch")) && v[0])
         cJSON_AddStringToObject(req, "head_branch", v);
      if ((v = cli_args_get(&opts, "remote")) && v[0])
         cJSON_AddStringToObject(req, "remote", v);
      if ((v = cli_args_get(&opts, "worktree-path")) && v[0])
         cJSON_AddStringToObject(req, "worktree_path", v);
      /* strict question-count gate (#3): --questions "q1||q2||q3" -> array. */
      if ((v = cli_args_get(&opts, "questions")) && v[0])
      {
         cJSON *qarr = cJSON_AddArrayToObject(req, "questions");
         char *dup = strdup(v);
         if (dup)
         {
            char *s = dup;
            while (s && *s)
            {
               char *sep = strstr(s, "||");
               if (sep)
                  *sep = '\0';
               while (*s == ' ')
                  s++;
               if (*s)
                  cJSON_AddItemToArray(qarr, cJSON_CreateString(s));
               s = sep ? sep + 2 : NULL;
            }
            free(dup);
         }
      }
   }
   else if (strcmp(method, "pipeline.list") == 0)
   {
      if ((v = cli_args_get(&opts, "state")) && v[0])
         cJSON_AddStringToObject(req, "state", v);
   }
   else if (strcmp(method, "pipeline.gate") == 0)
   {
      if (opts.pos_count > 0)
         cJSON_AddNumberToObject(req, "pipeline_id", atoi(opts.positional[0]));
      if (opts.pos_count > 1)
         cJSON_AddStringToObject(req, "verdict", opts.positional[1]);
      if ((v = cli_args_get(&opts, "reason")) && v[0])
         cJSON_AddStringToObject(req, "reason", v);
      if ((v = cli_args_get(&opts, "operator-principal")) && v[0])
         cJSON_AddStringToObject(req, "operator_principal", v);
   }
   else /* status / cancel / resume / advance */
   {
      if (opts.pos_count > 0)
         cJSON_AddNumberToObject(req, "pipeline_id", atoi(opts.positional[0]));
      if ((v = cli_args_get(&opts, "artifact")) && v[0])
         cJSON_AddStringToObject(req, "artifact", v);
      if ((v = cli_args_get(&opts, "artifact-hash")) && v[0])
         cJSON_AddStringToObject(req, "artifact_hash", v);
      /* resume can repair repo/workspace metadata after an impl-workspace failure
       * (#3): the same fields accepted by start. */
      if ((v = cli_args_get(&opts, "repo-root")) && v[0])
         cJSON_AddStringToObject(req, "repo_root", v);
      if ((v = cli_args_get(&opts, "remote")) && v[0])
         cJSON_AddStringToObject(req, "remote", v);
      if ((v = cli_args_get(&opts, "head-branch")) && v[0])
         cJSON_AddStringToObject(req, "head_branch", v);
      if ((v = cli_args_get(&opts, "worktree-path")) && v[0])
         cJSON_AddStringToObject(req, "worktree_path", v);
   }
   return req;
}

/* Load the client-held 32-byte vault root key (hex), generating + persisting it
 * at <aimee_home>/vault-root.key (0600) on first use. The root key NEVER leaves
 * the client except as the unlock payload (WP-C: the server stores only the
 * salt + ciphertext, never the root key). Returns 0 on success. */
static int vault_client_root_key_hex(char out[65])
{
   const char *home = aimee_home();
   if (!home || !home[0])
      return -1;
   char path[1024];
   if ((size_t)snprintf(path, sizeof(path), "%s/vault-root.key", home) >= sizeof(path))
      return -1;
   FILE *f = fopen(path, "r");
   if (f)
   {
      size_t n = fread(out, 1, 64, f);
      fclose(f);
      if (n == 64)
      {
         out[64] = '\0';
         return 0;
      }
   }
   /* Read 32 bytes of kernel entropy directly — the thin client links no crypto
    * or platform-random library. (On a platform without /dev/urandom this fails
    * closed: unlock sends no root key and the server rejects it.) */
   unsigned char raw[32];
   FILE *ur = fopen("/dev/urandom", "rb");
   if (!ur)
      return -1;
   size_t got = fread(raw, 1, sizeof(raw), ur);
   fclose(ur);
   if (got != sizeof(raw))
      return -1;
   for (size_t i = 0; i < sizeof(raw); i++)
      snprintf(out + i * 2, 3, "%02x", raw[i]);
   out[64] = '\0';
   f = fopen(path, "w");
   if (!f)
      return -1;
   size_t w = fwrite(out, 1, 64, f);
   fclose(f);
   if (w != 64)
      return -1;
   chmod(path, 0600);
   return 0;
}

/* `aimee vault unlock` — derive + cache the KEK server-side from the client root
 * key. The key is generated on first use and persisted at 0600 client-side. */
static cJSON *marshal_vault_unlock(int argc, char **argv)
{
   (void)argc;
   (void)argv;
   cJSON *req = marshal_no_args("vault.unlock");
   char rk[65];
   if (vault_client_root_key_hex(rk) == 0)
      cJSON_AddStringToObject(req, "root_key_hex", rk);
   return req;
}

/* `aimee vault set <agent> <cred> <secret>` */
static cJSON *marshal_vault_set(int argc, char **argv)
{
   cli_args_t opts;
   cli_args_parse(argc, argv, NULL, &opts);
   cJSON *req = marshal_no_args("vault.set");
   if (opts.pos_count > 0)
      cJSON_AddStringToObject(req, "agent", opts.positional[0]);
   if (opts.pos_count > 1)
      cJSON_AddStringToObject(req, "cred", opts.positional[1]);
   if (opts.pos_count > 2)
      cJSON_AddStringToObject(req, "secret", opts.positional[2]);
   return req;
}

/* `aimee vault set-server <agent> <cred> <secret>` */
static cJSON *marshal_vault_set_server(int argc, char **argv)
{
   cli_args_t opts;
   cli_args_parse(argc, argv, NULL, &opts);
   cJSON *req = marshal_no_args("vault.set_server");
   if (opts.pos_count > 0)
      cJSON_AddStringToObject(req, "agent", opts.positional[0]);
   if (opts.pos_count > 1)
      cJSON_AddStringToObject(req, "cred", opts.positional[1]);
   if (opts.pos_count > 2)
      cJSON_AddStringToObject(req, "secret", opts.positional[2]);
   return req;
}

/* `aimee vault capability <grant|revoke|list> [principal]` */
static cJSON *marshal_vault_capability(int argc, char **argv)
{
   cli_args_t opts;
   cli_args_parse(argc, argv, NULL, &opts);
   cJSON *req = marshal_no_args("vault.capability");
   if (opts.pos_count > 0)
      cJSON_AddStringToObject(req, "action", opts.positional[0]);
   if (opts.pos_count > 1)
      cJSON_AddStringToObject(req, "principal", opts.positional[1]);
   return req;
}

/* `aimee vault delete <agent> <cred>` */
static cJSON *marshal_vault_delete(int argc, char **argv)
{
   cli_args_t opts;
   cli_args_parse(argc, argv, NULL, &opts);
   cJSON *req = marshal_no_args("vault.delete");
   if (opts.pos_count > 0)
      cJSON_AddStringToObject(req, "agent", opts.positional[0]);
   if (opts.pos_count > 1)
      cJSON_AddStringToObject(req, "cred", opts.positional[1]);
   return req;
}

/* `aimee cert issue <cn> [--days N]` */
static cJSON *marshal_cert_issue(int argc, char **argv)
{
   cli_args_t opts;
   cli_args_parse(argc, argv, NULL, &opts);
   cJSON *req = marshal_no_args("cert.issue");
   if (opts.pos_count > 0)
      cJSON_AddStringToObject(req, "cn", opts.positional[0]);
   const char *days = cli_args_get(&opts, "days");
   if (days && days[0])
      cJSON_AddNumberToObject(req, "days", atoi(days));
   return req;
}

/* `aimee cert revoke <serial>` */
static cJSON *marshal_cert_revoke(int argc, char **argv)
{
   cli_args_t opts;
   cli_args_parse(argc, argv, NULL, &opts);
   cJSON *req = marshal_no_args("cert.revoke");
   if (opts.pos_count > 0)
      cJSON_AddStringToObject(req, "serial", opts.positional[0]);
   return req;
}

typedef cJSON *(*marshal_argv_fn)(int argc, char **argv);
typedef cJSON *(*marshal_method_fn)(const char *method, int argc, char **argv);

/* marshal_request dispatch tables. Generated-from and completeness-verified
 * against the original if/strcmp chain (see git history / the PR). Methods are
 * unique across all tables, so lookup order is irrelevant for correctness; the
 * exact tables are consulted before the prefix fallbacks so an exact method
 * (e.g. agent.episodes) is never shadowed by a prefix (agent.*). */
static const char *const MARSHAL_NO_ARGS[] = {
/* Rows live in server/cli_marshal_defs_data.h: the server serves them, and
 * this compiled-in copy is the last resort when it cannot be asked. */
#include "server/cli_marshal_defs_data.h"
};

static const struct
{
   const char *method;
   marshal_argv_fn fn;
} MARSHAL_ARGV[] = {
    {"model.episodes", marshal_agent_episodes},
    {"api.enable", marshal_api_enable},
    {"aux.test", marshal_aux_test},
    {"cert.issue", marshal_cert_issue},
    {"cert.revoke", marshal_cert_revoke},
    {"config.get", marshal_config_get},
    {"config.set", marshal_config_set},
    {"cron.add", marshal_cron_add},
    {"curator.contradictions", marshal_curator_contradictions},
    {"delegate", marshal_delegate},
    {"delegate.aggregate", marshal_delegate_aggregate},
    {"delegate.backend_exec", marshal_delegate_backend_exec},
    {"delegate.launch", marshal_delegate_launch},
    {"delegate.log", marshal_delegate_log},
    {"roundtable.review", marshal_roundtable_review},
    {"delegate.sandbox_gc", marshal_delegate_sandbox_gc},
    {"delegate.status", marshal_delegate_status},
    {"dogfood.report", marshal_dogfood_report},
    {"dogfood.review", marshal_dogfood_review},
    {"dogfood.tag", marshal_dogfood_tag},
    {"eval.candidates", marshal_eval_candidates},
    {"eval.candidates-update", marshal_eval_candidates_update},
    {"learning.approaches", marshal_learning_approaches},
    {"eval.results", marshal_eval_results},
    {"eval.run", marshal_eval_run},
    {"evidence.fidelity_retrieval_event", marshal_audit_fidelity},
    {"evidence.provenance_retrieval_event", marshal_audit_provenance},
    {"evidence.trace_retrieval_event", marshal_audit_trace},
    {"get_help", marshal_get_help},
    {"git.cli", marshal_git_cli},
    {"git.verify", marshal_git_verify},
    {"graph.explain", marshal_graph_explain},
    {"graph.sync_code", marshal_graph_sync_code},
    {"identity.diff", marshal_identity_diff},
    {"identity.snapshot", marshal_identity_snapshot},
    {"index.blast_radius", marshal_index_blast_radius},
    {"index.ast_grep", marshal_index_ast_grep},
    {"index.deps", marshal_index_deps},
    {"index.find", marshal_index_find},
    {"index.find_callers", marshal_index_find_callers},
    {"index.list", marshal_index_list},
    {"index.scan", marshal_index_scan},
    {"index.structure", marshal_index_structure},
    {"index.span", marshal_index_span},
    {"index.investigate", marshal_index_investigate},
    {"index.hybrid", marshal_index_hybrid},
    {"insights.overview", marshal_insights_overview},
    {"job.list", marshal_coord_jobs_list},
    {"job.start", marshal_coord_job_start},
    {"jobs.list", marshal_jobs_list},
    {"kb.build", marshal_kb_build},
    {"kb.docs.push", marshal_kb_docs_push},
    {"kb.ingest", marshal_kb_ingest},
    {"kb.reembed", marshal_kb_reembed},
    {"kb.health", marshal_kb_status},
    {"kb.search", marshal_kb_search},
    {"kb.status", marshal_kb_status},
    {"kb.update", marshal_kb_update},
    {"mcp.recheck", marshal_mcp_recheck},
    {"tool.call", marshal_tool_call},
    {"memory.embed", marshal_memory_embed},
    {"memory.archive", marshal_memory_archive},
    {"memory.benchmark", marshal_memory_benchmark},
    {"memory.delete", marshal_memory_delete},
    {"memory.supersede", marshal_memory_supersede},
    {"memory.get", marshal_memory_get},
    {"memory.identity", marshal_memory_identity},
    {"memory.list", marshal_memory_list},
    {"memory.prefer", marshal_memory_prefer},
    {"memory.read", marshal_memory_read},
    {"memory.recall", marshal_memory_recall},
    {"memory.search", marshal_memory_search},
    {"memory.store", marshal_memory_store},
    {"catalog.list", marshal_model_list},
    {"catalog.show", marshal_model_show},
    {"notes.search", marshal_notes_search},
    {"primary.set", marshal_primary},
    {"provider.list", marshal_provider_list},
    {"provider.models", marshal_provider_models},
    {"provider.set", marshal_provider_set},
    {"rules.delete", marshal_rules_delete},
    {"session.attach", marshal_session_attach},
    {"session.brief", marshal_session_brief},
    {"session.close", marshal_session_close},
    {"session.detach", marshal_session_detach},
    {"session.get", marshal_session_get},
    {"session.list", marshal_session_list},
    {"session.presence", marshal_session_presence},
    {"trajectory.batch", marshal_trajectory_batch},
    {"trajectory.export", marshal_trajectory_export},
    {"trigger.fire", marshal_trigger_fire},
    {"trigger.list", marshal_trigger_list},
    {"vault.capability", marshal_vault_capability},
    {"vault.delete", marshal_vault_delete},
    {"vault.set", marshal_vault_set},
    {"vault.set_server", marshal_vault_set_server},
    {"vault.unlock", marshal_vault_unlock},
    {"wm.get", marshal_wm_get},
    {"wm.list", marshal_wm_list},
    {"wm.set", marshal_wm_set},
    {"workspace.add", marshal_workspace_add},
    {"workspace.prepare", marshal_workspace_prepare},
    {"workspace.mirror-sync", marshal_workspace_mirror_sync},
    {"worktree.gc", marshal_worktree_gc},
};

static const struct
{
   const char *method;
   marshal_method_fn fn;
} MARSHAL_METHOD_ARGV[] = {
    {"cron.disable", marshal_cron_id},
    {"cron.enable", marshal_cron_id},
    {"cron.history", marshal_cron_id},
    {"cron.remove", marshal_cron_id},
    {"cron.run", marshal_cron_id},
    {"cron.show", marshal_cron_id},
    {"curator.implements", marshal_curator_topic},
    {"curator.synthesize", marshal_curator_topic},
    {"job.cancel", marshal_job_id_request},
    {"job.status", marshal_job_id_request},
    {"jobs.cancel", marshal_job_id_request},
    {"jobs.logs", marshal_job_id_request},
    {"jobs.status", marshal_job_id_request},
    {"provider.quota", marshal_provider_name_method},
    {"provider.show", marshal_provider_name_method},
    {"provider.test", marshal_provider_name_method},
    {"trigger.cancel", marshal_trigger_id},
    {"trigger.status", marshal_trigger_id},
    {"workspace.get", marshal_agent_args},
    {"workspace.remove", marshal_agent_args},
};

/* See marshal_request_take_reported in the header. Not thread-local: the CLI marshals one
 * command per process, and a global keeps the contract visible in one place. */
static int g_marshal_reported;

void marshal_request_note_reported(void)
{
   g_marshal_reported = 1;
}

int marshal_request_take_reported(void)
{
   int reported = g_marshal_reported;
   g_marshal_reported = 0;
   return reported;
}

/* Read the flag WITHOUT clearing it. marshal_request itself has to distinguish
 * "a served spec already told the operator what was missing" from "the spec was
 * uninterpretable", and taking the flag here would swallow the message the
 * forwarder is about to check for. */
int marshal_request_peek_reported(void)
{
   return g_marshal_reported;
}

cJSON *marshal_request(const char *method, int argc, char **argv)
{
   /* Cleared on entry so a previous command's flag cannot suppress this one's message. */
   g_marshal_reported = 0;

   /* What the SERVER says the body is, BEFORE anything this build compiled in --
    * including the custom bodies below.
    *
    * This consult used to sit under them, which made a served spec for
    * init.run, kb.grant.* or toolset.show/resolve dead code: the compiled body
    * answered first and the server could never override it. That contradicts
    * the property the whole exercise rests on -- a served row wins, which is
    * what lets the server change a command without a new client. It held for
    * every other method and silently did not for these.
    *
    * Safe at the top: cli_argspec_try_served returns 0 unless a spec exists AND
    * this build can interpret it, so anything unserved falls through to exactly
    * the code that ran before. */
   {
      cJSON *served = NULL;
      if (cli_argspec_try_served(method, argc, argv, &served))
         return served;
   }

   /* Custom-body cases (handled before the compiled tables). */
   if (strcmp(method, "init.run") == 0)
   {
      (void)argc;
      (void)argv;
      cJSON *req = marshal_no_args(method);
      char cwd[4096];
      if (req && getcwd(cwd, sizeof(cwd)))
         cJSON_AddStringToObject(req, "cwd", cwd);
      return req;
   }
   /* Write-tier grant administration. Flags rather than positionals, because four of them
    * are mandatory and their order would otherwise be load-bearing and unmemorable.
    *
    * team_id is parsed STRICTLY and refused rather than coerced: it selects the
    * authorization scope, so "770001x" quietly becoming 770001 would administer a team the
    * operator did not type. A missing or malformed one yields NULL, which the caller
    * reports as a usage error rather than sending to the server. */
   if (strncmp(method, "kb.grant.", 9) == 0)
   {
      const char *subject = NULL, *server_id = NULL, *team = NULL, *tier = NULL;
      int include_revoked = 0;
      for (int i = 0; i < argc; i++)
      {
         if (!strcmp(argv[i], "--include-revoked"))
            include_revoked = 1;
         else if (i + 1 < argc && !strcmp(argv[i], "--subject"))
            subject = argv[++i];
         else if (i + 1 < argc && !strcmp(argv[i], "--server"))
            server_id = argv[++i];
         else if (i + 1 < argc && !strcmp(argv[i], "--team"))
            team = argv[++i];
         else if (i + 1 < argc && !strcmp(argv[i], "--tier"))
            tier = argv[++i];
      }
      char *tail = NULL;
      long long team_id = team ? strtoll(team, &tail, 10) : 0;
      /* A marshal failure is a silent exit 2 in the shared forwarder, so each refusal says
       * what was wrong HERE: an operator who mistypes a flag on a security command must not
       * be left guessing whether anything happened. */
      if (!server_id || !team || !tail || *tail || team_id < 1)
      {
         fprintf(stderr, "aimee kb grant: --server ID and an integer --team N are required\n");
         if (team && (!tail || *tail))
            fprintf(stderr,
                    "  '%s' is not an integer. team_id selects the authorization scope, so\n"
                    "  it is refused rather than rounded.\n",
                    team);
         marshal_request_note_reported();
         return NULL;
      }
      int is_set = strcmp(method, "kb.grant.set") == 0;
      int is_revoke = strcmp(method, "kb.grant.revoke") == 0;
      /* `show` MUST keep its own identity through marshalling. It resolves to the same route
       * as `list` — it is that listing filtered to one subject — but if it shared the same
       * METHOD this marshaller could not tell them apart, and
       * `aimee kb grant show --server s --team 1` would silently issue an UNFILTERED LIST: a
       * command naming one subject answering with every grant on the server. A review caught
       * exactly that. */
      int is_show = strcmp(method, "kb.grant.show") == 0;
      /* set and revoke both name a subject; list does not require one, and uses it as a
       * filter when given (that is `show`). */
      if ((is_set || is_revoke || is_show) && !subject)
      {
         fprintf(stderr, "aimee kb grant %s: --subject S is required\n",
                 is_set ? "set" : (is_revoke ? "revoke" : "show"));
         fprintf(stderr,
                 "  A subject is owner, oidc:<iss>:<sub>, cert:<issuer>:<serial>, or a bare\n"
                 "  host account.\n");
         marshal_request_note_reported();
         return NULL;
      }
      if (is_set &&
          (!tier || (strcmp(tier, "off") && strcmp(tier, "data") && strcmp(tier, "full"))))
      {
         fprintf(stderr, "aimee kb grant set: --tier must be off, data or full%s%s%s\n",
                 tier ? " (got '" : "", tier ? tier : "", tier ? "')" : "");
         fprintf(stderr,
                 "  off is a real tier meaning explicitly denied, which is not the same as\n"
                 "  having no grant at all.\n");
         marshal_request_note_reported();
         return NULL;
      }
      cJSON *req = marshal_no_args(method);
      if (!req)
         return NULL;
      cJSON_AddStringToObject(req, "server_id", server_id);
      cJSON_AddNumberToObject(req, "team_id", (double)team_id);
      if (subject)
         cJSON_AddStringToObject(req, "subject", subject);
      if (is_set)
         cJSON_AddStringToObject(req, "tier", tier);
      if (!is_set && !is_revoke && include_revoked)
         cJSON_AddBoolToObject(req, "include_revoked", 1);
      return req;
   }
   if (strcmp(method, "toolset.show") == 0 || strcmp(method, "toolset.resolve") == 0)
   {
      if (argc < 1)
         return NULL;
      cJSON *req = marshal_no_args(method);
      cJSON_AddStringToObject(req, "name", argv[0]);
      return req;
   }
   /* What the SERVER says the body should be, before this build's own list.
    *
    * Without this, serving routes/catalogue/dispatch still left a new no-arg
    * command unusable: the client could find it and address it, then refused to
    * send anything because it had no row saying the body is empty. A client
    * preferring its own list here would reintroduce exactly that. */
   if (cli_v1_manifest_method_takes_no_args(method))
      return marshal_no_args(method);

   /* Exact-method tables (before the prefix fallbacks). */
   for (size_t i = 0; i < sizeof(MARSHAL_NO_ARGS) / sizeof(MARSHAL_NO_ARGS[0]); i++)
      if (strcmp(method, MARSHAL_NO_ARGS[i]) == 0)
         return marshal_no_args(method);
   for (size_t i = 0; i < sizeof(MARSHAL_METHOD_ARGV) / sizeof(MARSHAL_METHOD_ARGV[0]); i++)
      if (strcmp(method, MARSHAL_METHOD_ARGV[i].method) == 0)
         return MARSHAL_METHOD_ARGV[i].fn(method, argc, argv);
   for (size_t i = 0; i < sizeof(MARSHAL_ARGV) / sizeof(MARSHAL_ARGV[0]); i++)
      if (strcmp(method, MARSHAL_ARGV[i].method) == 0)
         return MARSHAL_ARGV[i].fn(argc, argv);
   /* Prefix fallbacks (after all exact matches). */
   if (strncmp(method, "skill.", 6) == 0)
      return marshal_skill_request(method, argc, argv);
   if (strncmp(method, "model.", 6) == 0 || strncmp(method, "agent.", 6) == 0)
      return marshal_agent_args(method, argc, argv);
   if (strncmp(method, "pipeline.", 9) == 0)
      return marshal_pipeline_request(method, argc, argv);
   return NULL;
}

/* --- Non-JSON output formatting --- */

void print_mcp_content(cJSON *resp)
{
   cJSON *content = cJSON_GetObjectItemCaseSensitive(resp, "content");
   if (!cJSON_IsArray(content))
      return;
   cJSON *item;
   cJSON_ArrayForEach(item, content)
   {
      cJSON *type = cJSON_GetObjectItemCaseSensitive(item, "type");
      cJSON *text = cJSON_GetObjectItemCaseSensitive(item, "text");
      if (cJSON_IsString(type) && strcmp(type->valuestring, "text") == 0 && cJSON_IsString(text))
      {
         printf("%s", text->valuestring);
         size_t len = strlen(text->valuestring);
         if (len == 0 || text->valuestring[len - 1] != '\n')
            putchar('\n');
      }
   }
}

static int git_verify_text_is_failure(const char *text)
{
   if (!text)
      return 0;
   /* format=json structured verdict: a JSON payload whose verdict is not "passed"
    * (i.e. failed or unavailable) is a failure for the CLI exit code, so a scripted
    * `aimee git verify ... format=json` still exits nonzero. The autonomous driver
    * reads the verdict field directly and does not rely on the exit code. Only the
    * leading region is scanned (the top-level verdict is the 2nd key, after
    * schema_version) so a `"verdict":"failed"` substring inside a step's log tail
    * cannot spuriously flip the exit code. */
   if (text[0] == '{')
   {
      char head[96];
      snprintf(head, sizeof head, "%s", text);
      if (strstr(head, "\"verdict\":\"failed\"") != NULL ||
          strstr(head, "\"verdict\":\"unavailable\"") != NULL)
         return 1;
   }
   return strncmp(text, "FAIL:", 5) == 0 || strncmp(text, "error:", 6) == 0 ||
          strncmp(text, "verify busy:", 12) == 0 || strstr(text, ": FAIL (exit ") != NULL ||
          strstr(text, " step(s) failed -- verified with failures") != NULL;
}

int git_verify_response_is_failure(cJSON *resp)
{
   cJSON *content = cJSON_GetObjectItemCaseSensitive(resp, "content");
   if (!cJSON_IsArray(content))
      return 0;

   cJSON *item;
   cJSON_ArrayForEach(item, content)
   {
      cJSON *type = cJSON_GetObjectItemCaseSensitive(item, "type");
      cJSON *text = cJSON_GetObjectItemCaseSensitive(item, "text");
      if (cJSON_IsString(type) && strcmp(type->valuestring, "text") == 0 && cJSON_IsString(text) &&
          git_verify_text_is_failure(text->valuestring))
         return 1;
   }
   return 0;
}

void print_server_health(cJSON *resp)
{
   cJSON *status = cJSON_GetObjectItemCaseSensitive(resp, "status");
   cJSON *state = cJSON_GetObjectItemCaseSensitive(resp, "state");
   cJSON *uptime = cJSON_GetObjectItemCaseSensitive(resp, "uptime");
   cJSON *connections = cJSON_GetObjectItemCaseSensitive(resp, "connections");

   printf("aimee-server: %s\n", cJSON_IsString(status) ? status->valuestring : "unknown");
   printf("  state:       %s\n", cJSON_IsString(state) ? state->valuestring : "unknown");
   if (cJSON_IsNumber(uptime))
      printf("  uptime:      %.0fs\n", uptime->valuedouble);
   if (cJSON_IsNumber(connections))
      printf("  connections: %d\n", (int)connections->valuedouble);

   /* The kb line is the point of running this command on a sick install; print it
    * whenever the server sent one, and say plainly when the kb is unreachable
    * rather than leaving its absence to be interpreted. */
   cJSON *kb = cJSON_GetObjectItemCaseSensitive(resp, "kb");
   if (cJSON_IsObject(kb))
   {
      const char *kbs = json_str(kb, "status");
      printf("aimee-kb: %s\n", (kbs && kbs[0]) ? kbs : "unknown");
      /* Why the kb cannot work, straight from the kb, before any detail line.
       * These are the sentences someone is running this command to find; burying
       * them under the store/vector/embedder triple means reading "ok" first and
       * inferring the rest. */
      cJSON *blockers = cJSON_GetObjectItemCaseSensitive(kb, "blockers");
      if (cJSON_IsArray(blockers) && cJSON_GetArraySize(blockers) > 0)
      {
         cJSON *b;
         cJSON_ArrayForEach(b, blockers) if (cJSON_IsString(b))
             printf("  BLOCKED: %s\n", b->valuestring);
      }
      /* Advisory findings. They do not move the verdict, which is exactly why they
       * need printing: a kb that is genuinely "ok" can still be accumulating work
       * nothing will process, and a status line that only ever renders blockers
       * reports that as a clean bill of health. Measured: a typed-fact backlog of
       * 4 jobs, unclaimable for 11.5 hours, with `aimee status` showing "aimee-kb:
       * ok" and nothing else. */
      cJSON *kbwarn = cJSON_GetObjectItemCaseSensitive(kb, "warnings");
      if (cJSON_IsArray(kbwarn) && cJSON_GetArraySize(kbwarn) > 0)
      {
         cJSON *w;
         cJSON_ArrayForEach(w, kbwarn) if (cJSON_IsString(w))
             printf("  note: %s\n", w->valuestring);
      }
      /* An open transport breaker refuses every call locally, so the kb can be
       * "ok" here while nothing works. Print it before the detail lines: this is
       * the line that explains an index that answers "unavailable" on a server
       * whose kb looks healthy. */
      if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(kb, "queries_suppressed")))
      {
         cJSON *retry = cJSON_GetObjectItemCaseSensitive(kb, "retry_after_ms");
         printf("  QUERIES SUPPRESSED: the transport breaker is open, so kb calls are being\n");
         printf("  refused locally without reaching the kb%s.\n",
                cJSON_IsNumber(retry) && retry->valuedouble > 0 ? "" : " (retrying shortly)");
         if (cJSON_IsNumber(retry) && retry->valuedouble > 0)
            printf("  next retry in %lldms; see the server log for the cause.\n",
                   (long long)retry->valuedouble);
      }
      /* "degraded" means the kb ANSWERED and told us what is broken, so the detail
       * lines below are real and worth printing. Only a kb that never answered
       * gets the "did not answer" text — testing `== "ok"` would have sent every
       * degraded install down that branch and reported a running kb as absent. */
      if (kbs && (strcmp(kbs, "ok") == 0 || strcmp(kbs, "degraded") == 0))
      {
         cJSON *vec = cJSON_GetObjectItemCaseSensitive(kb, "vectors");
         printf("  store:       %s\n",
                cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(kb, "store_ok")) ? "ok"
                                                                               : "unavailable");
         printf("  vector index:%s\n",
                cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(kb, "vectors_ok")) ? "ok"
                                                                                 : "unavailable");
         printf("  embedder:    %s\n",
                cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(kb, "embed_configured"))
                    ? "configured"
                    : "not configured");
         if (cJSON_IsNumber(vec))
            printf("  vectors:     %d\n", (int)vec->valuedouble);
      }
      else
      {
         printf("  the knowledge base did not answer; memory and kb search will not work.\n");
         printf("  `aimee kb status` has the detail.\n");
         /* "did not answer" reads as a network problem, and the most common cause
          * is not. A kb that refuses to start fails CLOSED before it ever binds
          * the health port, so its diagnosis never reaches this response and
          * exists only in the container log. Measured: booting a 768-dimension
          * embedder over a corpus recorded at 384 logs the width, both sides, and
          * the remedy, then holds DB2 unready until the container crashloops --
          * and every operator-facing surface said "unreachable", pointing away
          * from the one place that already knew the answer. Name that place. */
         printf("  if it never became healthy, the reason is in its own log and not\n");
         printf("  on the network: `docker logs aimee-kb` (compose) or the kb\n");
         printf("  service log for your deployment.\n");
      }
   }
}

const char *json_str(cJSON *obj, const char *key)
{
   cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
   return cJSON_IsString(v) ? v->valuestring : "";
}

int json_int(cJSON *obj, const char *key, int def)
{
   cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
   return cJSON_IsNumber(v) ? v->valueint : def;
}

double json_double(cJSON *obj, const char *key, double def)
{
   cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
   return cJSON_IsNumber(v) ? v->valuedouble : def;
}

void print_jobs_list(cJSON *resp)
{
   cJSON *jobs = cJSON_GetObjectItemCaseSensitive(resp, "jobs");
   if (!cJSON_IsArray(jobs) || cJSON_GetArraySize(jobs) == 0)
   {
      printf("No jobs.\n");
      return;
   }

   printf("%-6s %-12s %-10s %-6s %-12s %s\n", "ID", "Role", "Status", "Turn", "Agent", "Created");
   cJSON *job;
   cJSON_ArrayForEach(job, jobs)
   {
      int turn = json_int(job, "cursor_turn", 0);
      char turn_buf[16];
      if (turn > 0)
         snprintf(turn_buf, sizeof(turn_buf), "%d", turn);
      else
         snprintf(turn_buf, sizeof(turn_buf), "--");
      printf("%-6d %-12s %-10s %-6s %-12s %s\n", json_int(job, "id", 0), json_str(job, "role"),
             json_str(job, "status"), turn_buf, json_str(job, "agent_name"),
             json_str(job, "created_at"));
   }
}

void print_mcp_audit(cJSON *resp)
{
   cJSON *items = cJSON_GetObjectItemCaseSensitive(resp, "items");
   if (!cJSON_IsArray(items) || cJSON_GetArraySize(items) == 0)
   {
      printf("No MCP clients registered.\n");
      return;
   }
   printf("%-20s %-8s %-36s %-12s %-20s %s\n", "client", "eco", "package", "verdict", "checked_at",
          "advisories");
   cJSON *item;
   cJSON_ArrayForEach(item, items)
   {
      printf("%-20s %-8s %-36s %-12s %-20s %s\n", json_str(item, "client"),
             json_str(item, "ecosystem")[0] ? json_str(item, "ecosystem") : "-",
             json_str(item, "name")[0] ? json_str(item, "name") : "-", json_str(item, "verdict"),
             json_str(item, "checked_at"), json_str(item, "advisory_ids"));
   }
}

void print_mcp_recheck(cJSON *resp)
{
   cJSON *items = cJSON_GetObjectItemCaseSensitive(resp, "items");
   if (!cJSON_IsArray(items) || cJSON_GetArraySize(items) == 0)
   {
      printf("No matching MCP clients.\n");
      return;
   }
   cJSON *item;
   cJSON_ArrayForEach(item, items)
   {
      if (strcmp(json_str(item, "verdict"), "skipped") == 0)
      {
         printf("%s: skipped\n", json_str(item, "client"));
         continue;
      }
      printf("%s: %s:%s %s%s%s\n", json_str(item, "client"), json_str(item, "ecosystem"),
             json_str(item, "name"), json_str(item, "verdict"),
             json_str(item, "advisory_ids")[0] ? " " : "", json_str(item, "advisory_ids"));
   }
}

void print_jobs_status(cJSON *resp)
{
   cJSON *status = cJSON_GetObjectItemCaseSensitive(resp, "job_status");
   cJSON *id = cJSON_GetObjectItemCaseSensitive(resp, "job_id");
   if (cJSON_IsString(status) && strcmp(status->valuestring, "not_found") == 0)
   {
      printf("Job %d not found.\n", cJSON_IsNumber(id) ? id->valueint : 0);
      return;
   }

   cJSON *job = cJSON_GetObjectItemCaseSensitive(resp, "job");
   if (!cJSON_IsObject(job))
      return;

   printf("Job #%d\n", json_int(job, "id", 0));
   printf("Role:      %s\n", json_str(job, "role"));
   const char *prompt = json_str(job, "prompt");
   if (prompt[0])
      printf("Prompt:    %.200s\n", prompt);
   printf("Status:    %s\n", json_str(job, "status"));
   int cursor = json_int(job, "cursor_turn", 0);
   if (cursor > 0)
      printf("Cursor:    turn %d\n", cursor);
   printf("Agent:     %s\n", json_str(job, "agent_name"));
   const char *lease = json_str(job, "lease_owner");
   if (lease[0])
      printf("Lease:     %s\n", lease);
   const char *tool = json_str(job, "current_tool");
   if (tool[0])
      printf("Tool:      %s\n", tool);
   int calls = json_int(job, "api_call_count", 0);
   if (calls > 0)
      printf("API calls: %d\n", calls);
   int default_max_turns = json_int(job, "default_max_turns", 0);
   int final_after_turns = json_int(job, "final_after_turns", 0);
   if (default_max_turns > 0)
      printf("Max turns: %d\n", default_max_turns);
   if (final_after_turns > 0)
      printf("Final after: turn %d\n", final_after_turns);
   const char *result = json_str(job, "result");
   if (result[0])
      printf("Result:    %.500s\n", result);
   const char *heartbeat = json_str(job, "heartbeat_at");
   if (heartbeat[0])
      printf("Heartbeat: %s\n", heartbeat);
   printf("Created:   %s\n", json_str(job, "created_at"));
   printf("Updated:   %s\n", json_str(job, "updated_at"));
}

void print_jobs_logs(cJSON *resp)
{
   cJSON *status = cJSON_GetObjectItemCaseSensitive(resp, "job_status");
   cJSON *id = cJSON_GetObjectItemCaseSensitive(resp, "job_id");
   int job_id = cJSON_IsNumber(id) ? id->valueint : 0;
   if (cJSON_IsString(status) && strcmp(status->valuestring, "not_found") == 0)
   {
      printf("Job %d not found.\n", job_id);
      return;
   }

   const char *log = json_str(resp, "log");
   if (!log[0])
   {
      cJSON *job = cJSON_GetObjectItemCaseSensitive(resp, "job");
      log = json_str(job, "result");
   }
   if (log[0])
      printf("%s%s", log, log[strlen(log) - 1] == '\n' ? "" : "\n");
   else
      printf("Job %d has no recorded result yet.\n", job_id);
}

void print_jobs_cancel(cJSON *resp)
{
   int job_id = json_int(resp, "job_id", 0);
   cJSON *cancelled = cJSON_GetObjectItemCaseSensitive(resp, "cancelled");
   if (cJSON_IsTrue(cancelled))
      printf("Job %d cancelled.\n", job_id);
   else
   {
      const char *msg = json_str(resp, "message");
      printf("%s\n", msg[0] ? msg : "No pending or running job found.");
   }
}

void print_toolset_list(cJSON *resp)
{
   cJSON *sets = cJSON_GetObjectItemCaseSensitive(resp, "toolsets");
   cJSON *item = NULL;
   cJSON_ArrayForEach(item, sets)
   {
      printf("%-16s %3d%s\n", json_str(item, "name"), json_int(item, "count", 0),
             cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(item, "builtin")) ? "  built-in" : "");
   }
}

void print_toolset_show(cJSON *resp)
{
   printf("%s%s\n", json_str(resp, "name"),
          cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(resp, "builtin")) ? " (built-in)" : "");
   cJSON *includes = cJSON_GetObjectItemCaseSensitive(resp, "include");
   if (cJSON_IsArray(includes) && cJSON_GetArraySize(includes) > 0)
   {
      printf("include:");
      cJSON *item = NULL;
      cJSON_ArrayForEach(item, includes)
      {
         if (cJSON_IsString(item))
            printf(" %s", item->valuestring);
      }
      putchar('\n');
   }
   cJSON *tools = cJSON_GetObjectItemCaseSensitive(resp, "tools");
   if (cJSON_IsArray(tools) && cJSON_GetArraySize(tools) > 0)
   {
      printf("tools:");
      cJSON *item = NULL;
      cJSON_ArrayForEach(item, tools)
      {
         if (cJSON_IsString(item))
            printf(" %s", item->valuestring);
      }
      putchar('\n');
   }
}

void print_toolset_resolve(cJSON *resp)
{
   cJSON *tools = cJSON_GetObjectItemCaseSensitive(resp, "tools");
   cJSON *item = NULL;
   cJSON_ArrayForEach(item, tools)
   {
      if (cJSON_IsString(item))
         printf("%s\n", item->valuestring);
   }
}

void print_coord_jobs_list(cJSON *resp)
{
   cJSON *jobs = cJSON_GetObjectItemCaseSensitive(resp, "jobs");
   if (!cJSON_IsArray(jobs) || cJSON_GetArraySize(jobs) == 0)
   {
      printf("No coordinated jobs found.\n");
      return;
   }

   printf("%-6s %-8s %-12s %-6s %s\n", "ID", "Plan", "Status", "Para", "Created");
   cJSON *job;
   cJSON_ArrayForEach(job, jobs)
   {
      printf("%-6d %-8d %-12s %-6d %s\n", json_int(job, "id", 0), json_int(job, "plan_id", 0),
             json_str(job, "status"), json_int(job, "max_concurrent", 0),
             json_str(job, "created_at"));
   }
}

void print_coord_job_status(cJSON *resp)
{
   cJSON *status = cJSON_GetObjectItemCaseSensitive(resp, "job_status");
   cJSON *id = cJSON_GetObjectItemCaseSensitive(resp, "job_id");
   if (cJSON_IsString(status) && strcmp(status->valuestring, "not_found") == 0)
   {
      printf("Coordinated job %d not found.\n", cJSON_IsNumber(id) ? id->valueint : 0);
      return;
   }

   cJSON *job = cJSON_GetObjectItemCaseSensitive(resp, "job");
   if (!cJSON_IsObject(job))
      return;

   int total = json_int(job, "total", 0);
   int done = json_int(job, "done", 0);
   int failed = json_int(job, "failed", 0);
   int running = json_int(job, "running", 0);

   printf("Coordinated job #%d (plan #%d): %s\n", json_int(job, "id", 0),
          json_int(job, "plan_id", 0), json_str(job, "status"));
   printf("  Max concurrent: %d\n", json_int(job, "max_concurrent", 0));
   printf("  Total:   %d\n", total);
   printf("  Done:    %d\n", done);
   printf("  Running: %d\n", running);
   printf("  Failed:  %d\n", failed);
   printf("  Pending: %d\n", total - done - failed - running);

   cJSON *tasks = cJSON_GetObjectItemCaseSensitive(resp, "tasks");
   if (cJSON_IsArray(tasks) && cJSON_GetArraySize(tasks) > 0)
   {
      printf("\nTasks:\n");
      cJSON *task;
      cJSON_ArrayForEach(task, tasks)
      {
         printf("  #%-4d %-10s", json_int(task, "id", 0), json_str(task, "status"));
         const char *claimed = json_str(task, "claimed_by");
         if (claimed[0])
            printf(" [%s]", claimed);
         int step_id = json_int(task, "step_id", 0);
         if (step_id > 0)
            printf(" step:%d", step_id);
         const char *files = json_str(task, "files");
         if (files[0] && strcmp(files, "[]") != 0)
            printf(" files:%s", files);
         const char *error = json_str(task, "error");
         if (error[0])
            printf(" error:%.120s", error);
         printf("\n");
      }
   }
}

void print_coord_job_cancel(cJSON *resp)
{
   int job_id = json_int(resp, "job_id", 0);
   cJSON *cancelled = cJSON_GetObjectItemCaseSensitive(resp, "cancelled");
   if (cJSON_IsTrue(cancelled))
      printf("Coordinated job %d cancelled.\n", job_id);
   else
   {
      const char *msg = json_str(resp, "message");
      printf("%s\n", msg[0] ? msg : "No coordinated job found.");
   }
}

void print_coord_job_start(cJSON *resp)
{
   int job_id = json_int(resp, "job_id", 0);
   printf("Queued coord job #%d from plan #%d: %d tasks, max %d concurrent\n", job_id,
          json_int(resp, "plan_id", 0), json_int(resp, "tasks", 0),
          json_int(resp, "max_concurrent", 0));
   printf("Inspect packet progress with: aimee job status %d\n", job_id);
}

void print_aux_config(cJSON *resp)
{
   cJSON *aux = cJSON_GetObjectItemCaseSensitive(resp, "auxiliary");
   if (!cJSON_IsObject(aux))
      return;

   cJSON *enabled = cJSON_GetObjectItemCaseSensitive(aux, "enabled");
   const char *provider = json_str(aux, "default_provider");
   const char *model = json_str(aux, "default_model");
   printf("auxiliary.enabled:          %s\n", cJSON_IsTrue(enabled) ? "true" : "false");
   printf("auxiliary.default_provider: %s\n", provider[0] ? provider : "(none)");
   printf("auxiliary.default_model:    %s\n", model[0] ? model : "(none)");
   printf("auxiliary.default_max_tokens: %d\n", json_int(aux, "default_max_tokens", 0));

   cJSON *tasks = cJSON_GetObjectItemCaseSensitive(aux, "tasks");
   if (!cJSON_IsArray(tasks) || cJSON_GetArraySize(tasks) == 0)
   {
      printf("(no per-task overrides)\n");
      return;
   }

   printf("\n%-30s  %-20s  %-40s  %s\n", "task", "provider", "model", "max_tokens");
   printf("%-30s  %-20s  %-40s  %s\n", "----", "--------", "-----", "----------");
   cJSON *task;
   cJSON_ArrayForEach(task, tasks)
   {
      const char *name = json_str(task, "task");
      const char *prov = json_str(task, "provider");
      const char *mod = json_str(task, "model");
      int max_tokens = json_int(task, "max_tokens", 0);
      if (max_tokens > 0)
         printf("%-30s  %-20s  %-40s  %d\n", name, prov[0] ? prov : "(default)",
                mod[0] ? mod : "(default)", max_tokens);
      else
         printf("%-30s  %-20s  %-40s  (default)\n", name, prov[0] ? prov : "(default)",
                mod[0] ? mod : "(default)");
   }
}

void print_session_list(cJSON *resp)
{
   cJSON *sessions = cJSON_GetObjectItemCaseSensitive(resp, "sessions");
   if (!cJSON_IsArray(sessions) || cJSON_GetArraySize(sessions) == 0)
   {
      printf("No sessions.\n");
      return;
   }

   printf("%-36s %-10s %-19s %-19s %s\n", "SESSION", "CLIENT", "CREATED", "LAST ACTIVE", "TITLE");
   printf("%-36s %-10s %-19s %-19s %s\n", "-------", "------", "-------", "-----------", "-----");
   cJSON *s;
   cJSON_ArrayForEach(s, sessions)
   {
      const char *title = json_str(s, "title");
      printf("%-36s %-10s %-19s %-19s %s\n", json_str(s, "id"), json_str(s, "client_type"),
             json_str(s, "created_at"), json_str(s, "last_activity_at"), title[0] ? title : "-");
   }
}

void print_session_get(cJSON *resp)
{
   cJSON *s = cJSON_GetObjectItemCaseSensitive(resp, "session");
   if (!cJSON_IsObject(s))
      return;

   printf("session: %s\n", json_str(s, "id"));
   printf("  client:      %s\n", json_str(s, "client_type"));
   printf("  principal:   %s\n", json_str(s, "principal"));
   printf("  created:     %s\n", json_str(s, "created_at"));
   printf("  last active: %s\n", json_str(s, "last_activity_at"));

   const char *title = json_str(s, "title");
   const char *outcome = json_str(s, "outcome");
   const char *claude = json_str(s, "claude_session_id");
   if (title[0])
      printf("  title:       %s\n", title);
   if (outcome[0])
      printf("  outcome:     %s\n", outcome);
   if (claude[0])
      printf("  claude sid:  %s\n", claude);
}

void print_session_brief(cJSON *resp)
{
   cJSON *briefs = cJSON_GetObjectItemCaseSensitive(resp, "briefs");
   if (cJSON_IsArray(briefs))
   {
      if (cJSON_GetArraySize(briefs) == 0)
      {
         printf("No session briefs found.\n");
         return;
      }

      cJSON *b;
      cJSON_ArrayForEach(b, briefs)
      {
         cJSON *bytes = cJSON_GetObjectItemCaseSensitive(b, "bytes");
         printf("%s  %s  %lld bytes\n", json_str(b, "session_id"), json_str(b, "modified_at"),
                cJSON_IsNumber(bytes) ? (long long)bytes->valuedouble : 0LL);
      }
      return;
   }

   cJSON *brief = cJSON_GetObjectItemCaseSensitive(resp, "brief");
   if (cJSON_IsString(brief))
      printf("%s", brief->valuestring);
   else
   {
      cJSON *message = cJSON_GetObjectItemCaseSensitive(resp, "message");
      if (cJSON_IsString(message))
         printf("%s\n", message->valuestring);
   }
}

void print_insights_overview(cJSON *resp)
{
   if (!resp)
      return;

   cJSON *jdays = cJSON_GetObjectItemCaseSensitive(resp, "days");
   int days = cJSON_IsNumber(jdays) ? (int)jdays->valuedouble : 0;

   long long total_calls = 0, prompt = 0, completion = 0, cw = 0, cr = 0;
   double cost = 0.0;
   cJSON *j;
   if ((j = cJSON_GetObjectItemCaseSensitive(resp, "total_calls")) && cJSON_IsNumber(j))
      total_calls = (long long)j->valuedouble;
   if ((j = cJSON_GetObjectItemCaseSensitive(resp, "prompt_tokens")) && cJSON_IsNumber(j))
      prompt = (long long)j->valuedouble;
   if ((j = cJSON_GetObjectItemCaseSensitive(resp, "completion_tokens")) && cJSON_IsNumber(j))
      completion = (long long)j->valuedouble;
   if ((j = cJSON_GetObjectItemCaseSensitive(resp, "cache_write_tokens")) && cJSON_IsNumber(j))
      cw = (long long)j->valuedouble;
   if ((j = cJSON_GetObjectItemCaseSensitive(resp, "cache_read_tokens")) && cJSON_IsNumber(j))
      cr = (long long)j->valuedouble;
   if ((j = cJSON_GetObjectItemCaseSensitive(resp, "estimated_cost_usd")) && cJSON_IsNumber(j))
      cost = j->valuedouble;

   printf("Insights — last %d days\n", days);
   printf("  calls:           %lld\n", total_calls);
   printf("  prompt tokens:   %lld\n", prompt);
   printf("  completion:      %lld\n", completion);
   printf("  cache write:     %lld\n", cw);
   printf("  cache read:      %lld\n", cr);
   printf("  cost:            $%.4f\n", cost);

   /* Spend breakdown by usage_kind (§7), when the server reports it. The
    * billable figure is spend.total (excludes dedup-avoided cost). */
   cJSON *spend = cJSON_GetObjectItemCaseSensitive(resp, "spend");
   if (cJSON_IsObject(spend))
   {
      double s_real = 0.0, s_est = 0.0, s_avoid = 0.0, s_total = 0.0;
      if ((j = cJSON_GetObjectItemCaseSensitive(spend, "realized")) && cJSON_IsNumber(j))
         s_real = j->valuedouble;
      if ((j = cJSON_GetObjectItemCaseSensitive(spend, "estimated")) && cJSON_IsNumber(j))
         s_est = j->valuedouble;
      if ((j = cJSON_GetObjectItemCaseSensitive(spend, "avoided")) && cJSON_IsNumber(j))
         s_avoid = j->valuedouble;
      if ((j = cJSON_GetObjectItemCaseSensitive(spend, "total")) && cJSON_IsNumber(j))
         s_total = j->valuedouble;
      printf("  spend (billable):$%.4f  (realized $%.4f, estimated $%.4f", s_total, s_real, s_est);
      if (s_avoid > 0.0)
         printf(", avoided $%.4f", s_avoid);
      printf(")\n");
   }

   cJSON *models = cJSON_GetObjectItemCaseSensitive(resp, "models");
   int n_models = cJSON_IsArray(models) ? cJSON_GetArraySize(models) : 0;
   if (n_models == 0)
      printf("\n  Models: (none)\n");
   else
   {
      printf("\n  Models (top %d):\n", n_models);
      cJSON *m;
      cJSON_ArrayForEach(m, models)
      {
         const char *name = json_str(m, "model");
         long long mc = 0, mp = 0, mco = 0;
         double mcost = 0.0;
         if ((j = cJSON_GetObjectItemCaseSensitive(m, "calls")) && cJSON_IsNumber(j))
            mc = (long long)j->valuedouble;
         if ((j = cJSON_GetObjectItemCaseSensitive(m, "prompt_tokens")) && cJSON_IsNumber(j))
            mp = (long long)j->valuedouble;
         if ((j = cJSON_GetObjectItemCaseSensitive(m, "completion_tokens")) && cJSON_IsNumber(j))
            mco = (long long)j->valuedouble;
         if ((j = cJSON_GetObjectItemCaseSensitive(m, "estimated_cost_usd")) && cJSON_IsNumber(j))
            mcost = j->valuedouble;
         printf("    %-40s calls=%lld prompt=%lld completion=%lld cost=$%.4f\n",
                name && name[0] ? name : "(unknown)", mc, mp, mco, mcost);
      }
   }

   cJSON *roles = cJSON_GetObjectItemCaseSensitive(resp, "roles");
   int n_roles = cJSON_IsArray(roles) ? cJSON_GetArraySize(roles) : 0;
   if (n_roles > 0)
   {
      printf("\n  Roles (top %d):\n", n_roles);
      cJSON *r;
      cJSON_ArrayForEach(r, roles)
      {
         const char *name = json_str(r, "role");
         long long rc2 = 0;
         double rcost = 0.0;
         if ((j = cJSON_GetObjectItemCaseSensitive(r, "calls")) && cJSON_IsNumber(j))
            rc2 = (long long)j->valuedouble;
         if ((j = cJSON_GetObjectItemCaseSensitive(r, "estimated_cost_usd")) && cJSON_IsNumber(j))
            rcost = j->valuedouble;
         printf("    %-24s calls=%lld cost=$%.4f\n", name && name[0] ? name : "(unknown)", rc2,
                rcost);
      }
   }

   cJSON *tools = cJSON_GetObjectItemCaseSensitive(resp, "tools");
   int n_tools = cJSON_IsArray(tools) ? cJSON_GetArraySize(tools) : 0;
   if (n_tools > 0)
   {
      printf("\n  Tools (top %d):\n", n_tools);
      cJSON *t;
      cJSON_ArrayForEach(t, tools)
      {
         const char *name = json_str(t, "tool");
         long long tc = 0;
         double tcost = 0.0;
         if ((j = cJSON_GetObjectItemCaseSensitive(t, "calls")) && cJSON_IsNumber(j))
            tc = (long long)j->valuedouble;
         if ((j = cJSON_GetObjectItemCaseSensitive(t, "estimated_cost_usd")) && cJSON_IsNumber(j))
            tcost = j->valuedouble;
         printf("    %-32s calls=%lld cost=$%.4f\n", name && name[0] ? name : "(unknown)", tc,
                tcost);
      }
   }

   cJSON *platforms = cJSON_GetObjectItemCaseSensitive(resp, "platforms");
   int n_platforms = cJSON_IsArray(platforms) ? cJSON_GetArraySize(platforms) : 0;
   if (n_platforms > 0)
   {
      printf("\n  Platforms:\n");
      cJSON *p;
      cJSON_ArrayForEach(p, platforms)
      {
         const char *name = json_str(p, "platform");
         int sc = 0;
         if ((j = cJSON_GetObjectItemCaseSensitive(p, "session_count")) && cJSON_IsNumber(j))
            sc = (int)j->valuedouble;
         printf("    %-20s sessions=%d\n", name && name[0] ? name : "(unknown)", sc);
      }
   }

   cJSON *top_sessions = cJSON_GetObjectItemCaseSensitive(resp, "top_sessions");
   int n_top = cJSON_IsArray(top_sessions) ? cJSON_GetArraySize(top_sessions) : 0;
   if (n_top > 0)
   {
      printf("\n  Top sessions by cost:\n");
      cJSON *s;
      cJSON_ArrayForEach(s, top_sessions)
      {
         const char *sid = json_str(s, "session_id");
         const char *title = json_str(s, "title");
         double scost = 0.0;
         if ((j = cJSON_GetObjectItemCaseSensitive(s, "estimated_cost_usd")) && cJSON_IsNumber(j))
            scost = j->valuedouble;
         char sid8[9] = "(none)  ";
         if (sid && sid[0])
            snprintf(sid8, sizeof(sid8), "%.8s", sid);
         printf("    %-8s %-32s cost=$%.4f\n", sid8, (title && title[0]) ? title : "(untitled)",
                scost);
      }
   }

   cJSON *delegates = cJSON_GetObjectItemCaseSensitive(resp, "delegates");
   int n_deleg = cJSON_IsArray(delegates) ? cJSON_GetArraySize(delegates) : 0;
   if (n_deleg > 0)
   {
      printf("\n  Delegates by role:\n");
      cJSON *d;
      cJSON_ArrayForEach(d, delegates)
      {
         const char *name = json_str(d, "role");
         int dtotal = 0, dcompleted = 0;
         if ((j = cJSON_GetObjectItemCaseSensitive(d, "total")) && cJSON_IsNumber(j))
            dtotal = (int)j->valuedouble;
         if ((j = cJSON_GetObjectItemCaseSensitive(d, "completed")) && cJSON_IsNumber(j))
            dcompleted = (int)j->valuedouble;
         printf("    %-20s total=%d completed=%d\n", name && name[0] ? name : "(unknown)", dtotal,
                dcompleted);
      }
   }
}
