/* ===================================================================
 * RPC thin-client routing: route CLI subcommands through native server RPCs.
 * Unported commands fail in cli_main before reaching the server.
 * =================================================================== */

#include "util.h"          /* safe_exec_capture (workspace.mirror-sync ships the client diff) */
#include "aimee_client.h"  /* aimee_client_request: transport-agnostic /v1 client (Windows path) */
#if !defined(_WIN32) && !defined(_WIN64)
#include "aimee_home.h"
#include <dirent.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#endif

/* Only the POSIX /v1 retry loop calls this now (the Windows path has no retry),
 * so mark it maybe-unused to stay clean under the Windows build's -Werror. */
static void __attribute__((unused)) cli_rpc_sleep_ms(int ms)
{
#if defined(_WIN32) || defined(_WIN64)
   Sleep((DWORD)ms);
#else
   usleep((useconds_t)ms * 1000);
#endif
}

/* --- Minimal arg parser (no util.h dependency) --- */

#define RPC_MAX_POS   16
#define RPC_MAX_FLAGS 32

typedef struct
{
   const char *positional[RPC_MAX_POS];
   int pos_count;
   struct
   {
      const char *raw; /* flag name from argv (after --), may include =val */
      const char *value;
   } flags[RPC_MAX_FLAGS];
   int flag_count;
} rpc_opts_t;

static int rpc_is_bool(const char *name, const char **bool_flags)
{
   if (!bool_flags)
      return 0;
   for (int i = 0; bool_flags[i]; i++)
      if (strcmp(name, bool_flags[i]) == 0)
         return 1;
   return 0;
}

static void rpc_parse(int argc, char **argv, const char **bool_flags, rpc_opts_t *out)
{
   memset(out, 0, sizeof(*out));
   for (int i = 0; i < argc; i++)
   {
      if (argv[i][0] == '-' && argv[i][1] == '-')
      {
         const char *flag = argv[i] + 2;
         const char *eq = strchr(flag, '=');
         if (eq)
         {
            if (out->flag_count < RPC_MAX_FLAGS)
            {
               out->flags[out->flag_count].raw = flag;
               out->flags[out->flag_count].value = eq + 1;
               out->flag_count++;
            }
         }
         else if (rpc_is_bool(flag, bool_flags))
         {
            if (out->flag_count < RPC_MAX_FLAGS)
            {
               out->flags[out->flag_count].raw = flag;
               out->flags[out->flag_count].value = "1";
               out->flag_count++;
            }
         }
         else if (i + 1 < argc)
         {
            if (out->flag_count < RPC_MAX_FLAGS)
            {
               out->flags[out->flag_count].raw = flag;
               out->flags[out->flag_count].value = argv[i + 1];
               out->flag_count++;
            }
            i++;
         }
      }
      else
      {
         if (out->pos_count < RPC_MAX_POS)
            out->positional[out->pos_count++] = argv[i];
      }
   }
}

static const char *rpc_get(const rpc_opts_t *opts, const char *name)
{
   size_t nlen = strlen(name);
   for (int i = 0; i < opts->flag_count; i++)
   {
      const char *raw = opts->flags[i].raw;
      const char *eq = strchr(raw, '=');
      size_t rlen = eq ? (size_t)(eq - raw) : strlen(raw);
      if (rlen == nlen && memcmp(raw, name, nlen) == 0)
         return opts->flags[i].value;
   }
   return NULL;
}

static int rpc_has_flag(const rpc_opts_t *opts, const char *name)
{
   return rpc_get(opts, name) != NULL;
}

static int cli_rpc_args_request_json(int argc, char **argv)
{
   for (int i = 0; i < argc; i++)
   {
      if (argv[i] && strcmp(argv[i], "--json") == 0)
         return 1;
   }
   return 0;
}

static int rpc_get_int(const rpc_opts_t *opts, const char *name, int def)
{
   const char *v = rpc_get(opts, name);
   return v ? atoi(v) : def;
}

/* Resolve the active session id from --session / $AIMEE_SESSION_ID / "default"
 * (defined below; forward-declared for the session marshalers above it). */
static const char *resolve_session_env(const rpc_opts_t *opts);

/* --- RPC route table --- */

static const struct
{
   const char *cmd;
   const char *subcmd; /* NULL = match any (first arg is NOT a subcmd) */
   const char *method;
   const char *server_method; /* NULL = same as method */
   const char *extract;       /* response array field to extract, or NULL */
   int timeout_ms;            /* 0 = CLIENT_DEFAULT_TIMEOUT_MS */
} rpc_routes[] = {
    {"init", "", "init.run", NULL, NULL, 0},
    {"memory", "search", "memory.search", NULL, NULL, 60000},
    {"memory", "store", "memory.store", NULL, NULL, 60000},
    {"memory", "list", "memory.list", NULL, "memories", 60000},
    {"memory", "get", "memory.get", NULL, NULL, 60000},
    {"memory", "show", "memory.get", NULL, NULL, 60000},
    {"memory", "read", "memory.read", NULL, NULL, 60000},
    {"memory", "stats", "memory.stats", NULL, NULL, 60000},
    {"memory", "benchmark", "memory.benchmark", NULL, NULL, 600000},
    {"index", "scan", "index.scan", NULL, NULL, 300000},
    {"index", "overview", "index.list", NULL, "projects", 0},
    {"index", "list", "index.list", NULL, "projects", 0},
    {"index", "find", "index.find", NULL, NULL, 0},
    {"curator", "implements", "curator.implements", NULL, NULL, 0},
    {"curator", "synthesize", "curator.synthesize", NULL, NULL, 0},
    {"curator", "contradictions", "curator.contradictions", NULL, NULL, 0},
    {"index", "blast-radius", "index.blast_radius", NULL, NULL, 0},
    {"index", "structure", "index.structure", NULL, NULL, 0},
    {"index", "callers", "index.find_callers", NULL, NULL, 0},
    {"workspace", "add", "workspace.add", NULL, NULL, 300000},
    {"workspace", "list", "workspace.list", NULL, NULL, 0},
    {"workspace", "get", "workspace.get", NULL, NULL, 0},
    {"workspace", "remove", "workspace.remove", NULL, NULL, 0},
    {"workspace", "mirror-sync", "workspace.mirror-sync", NULL, NULL, 60000},
    {"graph", "sync-code", "graph.sync_code", NULL, NULL, 300000},
    {"graph", "explain", "graph.explain", NULL, NULL, 60000},
    {"session", "list", "session.list", NULL, "sessions", 0},
    {"session", "show", "session.get", NULL, NULL, 0},
    {"session", "get", "session.get", NULL, NULL, 0},
    {"session", "close", "session.close", NULL, NULL, 0},
    {"session", "brief", "session.brief", NULL, NULL, 0},
    {"session", "attach", "session.attach", NULL, NULL, 0},
    {"session", "detach", "session.detach", NULL, NULL, 0},
    {"presence", NULL, "session.presence", NULL, "presences", 0},
    {"trajectory", "export", "trajectory.export", NULL, NULL, 0},
    {"trajectory", "batch", "trajectory.batch", NULL, NULL, 900000},
    {"rules", "list", "rules.list", NULL, "rules", 0},
    {"rules", "generate", "rules.generate", NULL, NULL, 0},
    {"rules", "delete", "rules.delete", NULL, NULL, 0},
    {"skill", "list", "skill.list", NULL, "skills", 0},
    {"skill", "show", "skill.show", NULL, NULL, 0},
    {"skill", "lint", "skill.lint", NULL, NULL, 0},
    {"skill", "eval", "skill.eval", NULL, NULL, 0},
    {"skill", "create", "skill.create", NULL, NULL, 0},
    {"skill", "edit", "skill.edit", NULL, NULL, 0},
    {"skill", "patch", "skill.patch", NULL, NULL, 0},
    {"skill", "archive", "skill.archive", NULL, NULL, 0},
    {"skill", "lifecycle", "skill.lifecycle", NULL, NULL, 0},
    {"skill", "autostub", "skill.autostub", NULL, NULL, 60000},
    {"skill", "pin", "skill.pin", NULL, NULL, 0},
    {"skill", "unpin", "skill.unpin", NULL, NULL, 0},
    {"toolset", "list", "toolset.list", NULL, "toolsets", 0},
    {"toolset", "show", "toolset.show", NULL, NULL, 0},
    {"toolset", "resolve", "toolset.resolve", NULL, "tools", 0},
    {"wm", "set", "wm.set", NULL, NULL, 0},
    {"wm", "get", "wm.get", NULL, NULL, 0},
    {"wm", "list", "wm.list", NULL, "entries", 0},
    {"primary", NULL, "primary.set", NULL, NULL, 0},
    {"kb", "search", "kb.search", NULL, NULL, 60000},
    {"kb", "build", "kb.build", NULL, NULL, 900000},
    {"kb", "update", "kb.update", NULL, NULL, 900000},
    {"kb", "docs push", "kb.docs.push", NULL, NULL, 900000},
    {"kb", "ingest", "kb.ingest", NULL, NULL, 30000},
    {"kb", "ingest status", "kb.ingest.status", NULL, NULL, 0},
    {"kb", "status", "kb.status", NULL, NULL, 0},
    {"workers", "", "workers", NULL, NULL, 0},
    {"insights", NULL, "insights.overview", NULL, NULL, 0},
    {"worktree", "gc", "worktree.gc", NULL, NULL, 60000},
    {"work", "add", "work.add", NULL, NULL, 0},
    {"work", "add-batch", "work.add_batch", NULL, NULL, 0},
    {"work", "claim", "work.claim", NULL, NULL, 0},
    {"work", "complete", "work.complete", NULL, NULL, 0},
    {"work", "fail", "work.fail", NULL, NULL, 0},
    {"work", "list", "work.list", NULL, "items", 0},
    {"work", "board", "work.board", NULL, NULL, 0},
    {"work", "cancel", "work.cancel", NULL, NULL, 0},
    {"work", "release", "work.release", NULL, NULL, 0},
    {"work", "clear", "work.clear", NULL, NULL, 0},
    {"work", "gc", "work.gc", NULL, NULL, 0},
    {"work", "sync-proposals", "work.sync_proposals", NULL, NULL, 0},
    {"work", "stats", "work.stats", NULL, NULL, 0},
    {"delegate", "launch", "delegate.launch", NULL, NULL, 300000},
    {"delegate", "status", "delegate.status", NULL, NULL, 0},
    {"delegate", "--list-roles", "agent.list", NULL, "agents", 0},
    {"delegate", "log", "delegate.log", NULL, "episodes", 0},
    {"delegate", "history", "delegate.log", NULL, "episodes", 0},
    /* Codex/openai delegate agents have agent->timeout_ms == 900000 server-side.
     * The CLI must outlast that, otherwise we report "no response" while the
     * server is still genuinely working. */
    {"delegate", NULL, "delegate", NULL, NULL, 900000},
    {"jobs", "list", "jobs.list", NULL, "jobs", 0},
    {"jobs", "status", "jobs.status", NULL, NULL, 0},
    {"jobs", "show", "jobs.status", NULL, NULL, 0},
    {"jobs", "log", "jobs.logs", NULL, NULL, 0},
    {"jobs", "logs", "jobs.logs", NULL, NULL, 0},
    {"jobs", "cancel", "jobs.cancel", NULL, NULL, 0},
    {"job", "start", "job.start", NULL, NULL, 0},
    {"job", "list", "job.list", NULL, "jobs", 0},
    {"job", "status", "job.status", NULL, NULL, 0},
    {"job", "show", "job.status", NULL, NULL, 0},
    {"job", "cancel", "job.cancel", NULL, NULL, 0},
    {"aux", "config show", "aux.config_show", NULL, NULL, 0},
    {"aux", "config", "aux.config_show", NULL, NULL, 0},
    {"aux", "test", "aux.test", NULL, NULL, 300000},
    {"aux", "", "aux.config_show", NULL, NULL, 0},
    {"config", "show", "config.show", NULL, NULL, 0},
    {"config", "get", "config.get", NULL, NULL, 0},
    {"config", "set", "config.set", NULL, NULL, 0},
    {"config", "", "config.show", NULL, NULL, 0},
    {"episode", "list", "episode.list", NULL, "episodes", 0},
    {"trigger", "list", "trigger.list", NULL, "triggers", 0},
    {"trigger", "status", "trigger.status", NULL, NULL, 0},
    {"trigger", "cancel", "trigger.cancel", NULL, NULL, 0},
    {"trigger", "fire", "trigger.fire", NULL, NULL, 0},
    {"cron", "list", "cron.list", NULL, "jobs", 0},
    {"cron", "add", "cron.add", NULL, NULL, 0},
    {"cron", "show", "cron.show", NULL, NULL, 0},
    {"cron", "history", "cron.history", NULL, "runs", 0},
    {"cron", "run", "cron.run", NULL, NULL, 60000},
    {"cron", "enable", "cron.enable", NULL, NULL, 0},
    {"cron", "disable", "cron.disable", NULL, NULL, 0},
    {"cron", "remove", "cron.remove", NULL, NULL, 0},
    {"agent", "list", "agent.list", NULL, "agents", 0},
    {"agent", "episodes", "agent.episodes", NULL, "episodes", 0},
    {"agent", "add", "agent.add", NULL, NULL, 300000},
    {"agent", "local", "agent.local", NULL, NULL, 300000},
    {"agent", "remove", "agent.remove", NULL, NULL, 0},
    {"agent", "enable", "agent.enable", NULL, NULL, 0},
    {"agent", "disable", "agent.disable", NULL, NULL, 0},
    {"agent", "probe", "agent.probe", NULL, NULL, 300000},
    {"mcp", "audit", "mcp.audit", NULL, "items", 0},
    {"mcp", "recheck", "mcp.recheck", NULL, "items", 300000},
    {"get_help", NULL, "get_help", "mcp.call", NULL, 0},
    {"get-help", NULL, "get_help", "mcp.call", NULL, 0},
    {"verify", NULL, "git.verify", "mcp.call", NULL, 900000},
    {"git", "verify", "git.verify", "mcp.call", NULL, 900000},
    {"migrate", "v2", "migrate.v2", NULL, NULL, 900000},
    {"provider", "list", "provider.list", NULL, "providers", 300000},
    {"provider", "show", "provider.show", NULL, NULL, 0},
    {"provider", "models", "provider.models", NULL, "models", 300000},
    {"provider", "test", "provider.test", NULL, NULL, 300000},
    {"provider", "quota", "provider.quota", NULL, NULL, 0},
    {"provider", "", "provider.get", NULL, NULL, 0},
    {"provider", NULL, "provider.set", NULL, NULL, 0},
    {"use", NULL, "provider.set", NULL, NULL, 0},
    {"model", "list", "model.list", NULL, "models", 0},
    {"model", "show", "model.show", NULL, NULL, 0},
    {"model", "refresh", "model.refresh", NULL, NULL, 300000},
    {"server", "status", "server.health", NULL, NULL, 0},
    {"server", "health", "server.health", NULL, NULL, 0},
    {"status", "", "server.health", NULL, NULL, 0},
    {"api", "status", "api.status", NULL, NULL, 0},
    {"api", "enable", "api.enable", NULL, NULL, 0},
    {"api", "disable", "api.disable", NULL, NULL, 0},
    {"api", "", "api.status", NULL, NULL, 0},
    {"hud", "", "hud.status", NULL, NULL, 0},
    {"dogfood", "tag", "dogfood.tag", NULL, NULL, 0},
    /* Month-scale dogfood logs can take longer than the default RPC timeout. */
    {"dogfood", "review", "dogfood.review", NULL, NULL, 300000},
    {"dogfood", "report", "dogfood.report", NULL, NULL, 300000},
    {"eval", "run", "eval.run", NULL, NULL, 900000},
    {"eval", "results", "eval.results", NULL, NULL, 0},
    {"identity", "show", "identity.show", NULL, NULL, 0},
    {"identity", "snapshot", "identity.snapshot", NULL, NULL, 0},
    {"identity", "diff", "identity.diff", NULL, NULL, 0},
    {"delegate-backend", "list", "delegate.backend_list", NULL, "backends", 0},
    {"delegate-backend", "exec", "delegate.backend_exec", NULL, NULL, 90000},
    {NULL, NULL, NULL, NULL, NULL, 0},
};

int cli_rpc_lookup(const char *cmd, int sub_argc, char **sub_argv, cli_rpc_route_t *route)
{
   if (!cmd)
      return 0;

   /* Pass 1: compound subcmd (e.g. "ingest status" for `aimee kb ingest status`).
    * This must run before the single-word pass so more-specific routes win. */
   if (sub_argc >= 2)
   {
      char compound[256];
      snprintf(compound, sizeof(compound), "%s %s", sub_argv[0], sub_argv[1]);
      for (int i = 0; rpc_routes[i].cmd; i++)
      {
         if (strcmp(cmd, rpc_routes[i].cmd) != 0)
            continue;
         if (!rpc_routes[i].subcmd)
            continue;
         if (strcmp(compound, rpc_routes[i].subcmd) == 0)
         {
            route->method = rpc_routes[i].method;
            route->server_method = rpc_routes[i].server_method;
            route->extract = rpc_routes[i].extract;
            route->skip_subcmd = 2;
            route->timeout_ms = rpc_routes[i].timeout_ms;
            return 1;
         }
      }
   }

   /* Pass 2: single subcmd (or empty/wildcard). */
   const char *subcmd = sub_argc > 0 ? sub_argv[0] : NULL;
   for (int i = 0; rpc_routes[i].cmd; i++)
   {
      if (strcmp(cmd, rpc_routes[i].cmd) != 0)
         continue;
      if (rpc_routes[i].subcmd == NULL)
      {
         route->method = rpc_routes[i].method;
         route->server_method = rpc_routes[i].server_method;
         route->extract = rpc_routes[i].extract;
         route->skip_subcmd = 0;
         route->timeout_ms = rpc_routes[i].timeout_ms;
         return 1;
      }
      if (rpc_routes[i].subcmd[0] == '\0')
      {
         if (!subcmd || subcmd[0] == '-')
         {
            route->method = rpc_routes[i].method;
            route->server_method = rpc_routes[i].server_method;
            route->extract = rpc_routes[i].extract;
            route->skip_subcmd = 0;
            route->timeout_ms = rpc_routes[i].timeout_ms;
            return 1;
         }
         continue;
      }
      if (subcmd && strcmp(subcmd, rpc_routes[i].subcmd) == 0)
      {
         route->method = rpc_routes[i].method;
         route->server_method = rpc_routes[i].server_method;
         route->extract = rpc_routes[i].extract;
         route->skip_subcmd = 1;
         route->timeout_ms = rpc_routes[i].timeout_ms;
         return 1;
      }
   }
   return 0;
}

/* --- Per-method argv marshaling --- */

/* Builds a bare request envelope ({method, protocol_version}); defined below,
 * forward-declared here because the per-method marshalers above its definition
 * use it to open their request object. */
static cJSON *marshal_no_args(const char *method);

/* Marshal `aimee delegate-backend exec --backend X --task-id Y
 * [--image I] [--host H] [--no-hibernate] "<cmd>"` into the
 * delegate.backend_exec request JSON. The command is the (last)
 * positional arg. */
static cJSON *marshal_delegate_backend_exec(int argc, char **argv)
{
   static const char *bools[] = {"no-hibernate", NULL};
   rpc_opts_t opts;
   rpc_parse(argc, argv, bools, &opts);

   cJSON *req = marshal_no_args("delegate.backend_exec");

   const char *backend = rpc_get(&opts, "backend");
   if (backend)
      cJSON_AddStringToObject(req, "backend", backend);
   const char *task_id = rpc_get(&opts, "task-id");
   if (task_id)
      cJSON_AddStringToObject(req, "task_id", task_id);
   const char *image = rpc_get(&opts, "image");
   if (image)
      cJSON_AddStringToObject(req, "image", image);
   const char *host = rpc_get(&opts, "host");
   if (host)
      cJSON_AddStringToObject(req, "host", host);
   if (rpc_get(&opts, "no-hibernate"))
      cJSON_AddBoolToObject(req, "no_hibernate", 1);

   /* Command is the last positional arg (typically quoted on the
    * command line, so it's a single argv slot). */
   if (opts.pos_count > 0)
      cJSON_AddStringToObject(req, "command", opts.positional[opts.pos_count - 1]);
   return req;
}

static cJSON *marshal_curator_topic(const char *method, int argc, char **argv)
{
   rpc_opts_t opts;
   rpc_parse(argc, argv, NULL, &opts);
   cJSON *req = cJSON_CreateObject();
   cJSON_AddStringToObject(req, "method", method);
   cJSON_AddNumberToObject(req, "protocol_version", RPC_PROTOCOL_VERSION);
   if (opts.pos_count > 0)
      cJSON_AddStringToObject(req, "topic", opts.positional[0]);
   return req;
}

static cJSON *marshal_curator_contradictions(int argc, char **argv)
{
   rpc_opts_t opts;
   rpc_parse(argc, argv, NULL, &opts);
   cJSON *req = marshal_no_args("curator.contradictions");
   cJSON_AddNumberToObject(req, "limit", rpc_get_int(&opts, "limit", 20));
   return req;
}

static cJSON *marshal_memory_search(int argc, char **argv)
{
   rpc_opts_t opts;
   rpc_parse(argc, argv, NULL, &opts);

   cJSON *req = marshal_no_args("memory.search");

   cJSON *kw = cJSON_CreateArray();
   for (int i = 0; i < opts.pos_count; i++)
      cJSON_AddItemToArray(kw, cJSON_CreateString(opts.positional[i]));
   cJSON_AddItemToObject(req, "keywords", kw);
   cJSON_AddNumberToObject(req, "limit", rpc_get_int(&opts, "limit", 10));
   return req;
}

static cJSON *marshal_memory_store(int argc, char **argv)
{
   rpc_opts_t opts;
   rpc_parse(argc, argv, NULL, &opts);

   cJSON *req = marshal_no_args("memory.store");

   /* Accept both positional (`memory store <key> <content>`) and flag
    * (`--key` / `--content`) forms; positional wins when present. Previously the
    * flags were silently ignored, yielding a confusing "missing key or content". */
   const char *key = opts.pos_count > 0 ? opts.positional[0] : rpc_get(&opts, "key");
   const char *content = opts.pos_count > 1 ? opts.positional[1] : rpc_get(&opts, "content");
   if (key)
      cJSON_AddStringToObject(req, "key", key);
   if (content)
      cJSON_AddStringToObject(req, "content", content);

   const char *v;
   if ((v = rpc_get(&opts, "tier")))
      cJSON_AddStringToObject(req, "tier", v);
   if ((v = rpc_get(&opts, "kind")))
      cJSON_AddStringToObject(req, "kind", v);
   if ((v = rpc_get(&opts, "session")))
      cJSON_AddStringToObject(req, "session_id", v);
   if ((v = rpc_get(&opts, "confidence")))
      cJSON_AddNumberToObject(req, "confidence", atof(v));
   return req;
}

static cJSON *marshal_memory_list(int argc, char **argv)
{
   rpc_opts_t opts;
   rpc_parse(argc, argv, NULL, &opts);

   cJSON *req = marshal_no_args("memory.list");

   const char *v;
   if ((v = rpc_get(&opts, "tier")))
      cJSON_AddStringToObject(req, "tier", v);
   if ((v = rpc_get(&opts, "kind")))
      cJSON_AddStringToObject(req, "kind", v);
   cJSON_AddNumberToObject(req, "limit", rpc_get_int(&opts, "limit", 20));
   return req;
}

static cJSON *marshal_memory_get(int argc, char **argv)
{
   cJSON *req = marshal_no_args("memory.get");
   if (argc > 0)
      cJSON_AddNumberToObject(req, "id", atoll(argv[0]));
   return req;
}

static cJSON *marshal_memory_read(int argc, char **argv)
{
   (void)argc;
   (void)argv;
   cJSON *req = marshal_no_args("memory.read");
   return req;
}

/* Add `field` to req as an absolute path. The benchmark asset paths are
 * resolved against the client's cwd because aimee-server may run with a
 * different working directory; absolute paths are forwarded unchanged. */
static void marshal_add_abs_path(cJSON *req, const char *field, const char *path)
{
   if (!path || !path[0])
      return;
   if (path[0] == '/')
   {
      cJSON_AddStringToObject(req, field, path);
      return;
   }
   char cwd[4096];
   if (!getcwd(cwd, sizeof(cwd)))
   {
      cJSON_AddStringToObject(req, field, path);
      return;
   }
   char abs[4608];
   snprintf(abs, sizeof(abs), "%s/%s", cwd, path);
   cJSON_AddStringToObject(req, field, abs);
}

/* Marshal `aimee memory benchmark [code-graph-fusion] [--arm NAME]
 * [--corpus PATH] [--matrix PATH] [--fusion-state off|shadow|on]` into the
 * memory.benchmark request. Asset paths default on the server to the committed
 * benchmark files; when given here they are made absolute for the server. */
static cJSON *marshal_memory_benchmark(int argc, char **argv)
{
   rpc_opts_t opts;
   rpc_parse(argc, argv, NULL, &opts);

   cJSON *req = marshal_no_args("memory.benchmark");

   const char *suite = (opts.pos_count >= 1) ? opts.positional[0] : "code-graph-fusion";
   cJSON_AddStringToObject(req, "suite", suite);
   const char *arm = rpc_get(&opts, "arm");
   if (arm)
      cJSON_AddStringToObject(req, "arm", arm);
   const char *fstate = rpc_get(&opts, "fusion-state");
   if (fstate)
      cJSON_AddStringToObject(req, "fusion_state", fstate);
   marshal_add_abs_path(req, "corpus", rpc_get(&opts, "corpus"));
   marshal_add_abs_path(req, "matrix", rpc_get(&opts, "matrix"));
   return req;
}

static cJSON *marshal_index_scan(int argc, char **argv)
{
   static const char *bool_flags[] = {"force", NULL};
   rpc_opts_t opts;
   rpc_parse(argc, argv, bool_flags, &opts);

   cJSON *req = marshal_no_args("index.scan");
   if (opts.pos_count >= 1)
      cJSON_AddStringToObject(req, "name", opts.positional[0]);
   if (opts.pos_count >= 2)
      cJSON_AddStringToObject(req, "root", opts.positional[1]);
   if (rpc_get(&opts, "force"))
      cJSON_AddTrueToObject(req, "force");
   return req;
}

static cJSON *marshal_index_find(int argc, char **argv)
{
   cJSON *req = marshal_no_args("index.find");
   if (argc > 0)
      cJSON_AddStringToObject(req, "identifier", argv[0]);
   return req;
}

static cJSON *marshal_index_list(int argc, char **argv)
{
   (void)argc;
   (void)argv;
   cJSON *req = marshal_no_args("index.list");
   return req;
}

static cJSON *marshal_index_file_request(const char *method, int argc, char **argv)
{
   cJSON *req = cJSON_CreateObject();
   cJSON_AddStringToObject(req, "method", method);
   cJSON_AddNumberToObject(req, "protocol_version", RPC_PROTOCOL_VERSION);
   if (argc > 0)
      cJSON_AddStringToObject(req, "project", argv[0]);
   if (argc > 1)
      cJSON_AddStringToObject(req, "file_path", argv[1]);
   return req;
}

static cJSON *marshal_index_blast_radius(int argc, char **argv)
{
   return marshal_index_file_request("index.blast_radius", argc, argv);
}

static cJSON *marshal_index_structure(int argc, char **argv)
{
   return marshal_index_file_request("index.structure", argc, argv);
}

static cJSON *marshal_index_find_callers(int argc, char **argv)
{
   cJSON *req = marshal_no_args("index.find_callers");
   if (argc > 0)
      cJSON_AddStringToObject(req, "symbol", argv[0]);
   if (argc > 1)
      cJSON_AddStringToObject(req, "project", argv[1]);
   return req;
}

static cJSON *marshal_session_list(int argc, char **argv)
{
   rpc_opts_t opts;
   rpc_parse(argc, argv, NULL, &opts);

   cJSON *req = marshal_no_args("session.list");

   int limit = rpc_get_int(&opts, "limit", 0);
   if (limit > 0)
      cJSON_AddNumberToObject(req, "limit", limit);
   return req;
}

static cJSON *marshal_session_id_request(const char *method, int argc, char **argv)
{
   rpc_opts_t opts;
   rpc_parse(argc, argv, NULL, &opts);

   cJSON *req = cJSON_CreateObject();
   cJSON_AddStringToObject(req, "method", method);
   cJSON_AddNumberToObject(req, "protocol_version", RPC_PROTOCOL_VERSION);

   const char *sid = opts.pos_count > 0 ? opts.positional[0] : rpc_get(&opts, "session");
   if (sid && sid[0])
      cJSON_AddStringToObject(req, "session_id", sid);
   return req;
}

static cJSON *marshal_session_get(int argc, char **argv)
{
   return marshal_session_id_request("session.get", argc, argv);
}

static cJSON *marshal_session_close(int argc, char **argv)
{
   return marshal_session_id_request("session.close", argc, argv);
}

static cJSON *marshal_session_brief(int argc, char **argv)
{
   static const char *bool_flags[] = {"list", NULL};
   rpc_opts_t opts;
   rpc_parse(argc, argv, bool_flags, &opts);

   cJSON *req = marshal_no_args("session.brief");

   const char *sid = opts.pos_count > 0 ? opts.positional[0] : rpc_get(&opts, "session");
   if (sid && sid[0])
      cJSON_AddStringToObject(req, "session_id", sid);
   if (rpc_has_flag(&opts, "list"))
      cJSON_AddBoolToObject(req, "list", 1);
   return req;
}

/* `aimee session attach <session_id> [--surface S] [--target spec]
 * [--subscribe MASK] [--persistent]` — register this surface as an attachment
 * of the session's presence (unified-presence). The server mints the
 * attach_id. */
static cJSON *marshal_session_attach(int argc, char **argv)
{
   static const char *bool_flags[] = {"persistent", NULL};
   rpc_opts_t opts;
   rpc_parse(argc, argv, bool_flags, &opts);

   cJSON *req = marshal_no_args("session.attach");

   const char *sid = opts.pos_count > 0 ? opts.positional[0] : resolve_session_env(&opts);
   cJSON_AddStringToObject(req, "session_id", sid ? sid : "");

   const char *surface = rpc_get(&opts, "surface");
   cJSON_AddStringToObject(req, "surface", (surface && surface[0]) ? surface : "cli");

   const char *target = rpc_get(&opts, "target");
   if (target && target[0])
      cJSON_AddStringToObject(req, "target", target);
   const char *owner = rpc_get(&opts, "owner");
   if (owner && owner[0])
      cJSON_AddStringToObject(req, "owner", owner);
   int mask = rpc_get_int(&opts, "subscribe", -1);
   if (mask >= 0)
      cJSON_AddNumberToObject(req, "subscribe_mask", mask);
   if (rpc_has_flag(&opts, "persistent"))
      cJSON_AddBoolToObject(req, "persistent", 1);
   return req;
}

/* `aimee session detach <session_id> --attach-id <id>` — drop an attachment. */
static cJSON *marshal_session_detach(int argc, char **argv)
{
   rpc_opts_t opts;
   rpc_parse(argc, argv, NULL, &opts);

   cJSON *req = marshal_no_args("session.detach");

   const char *sid = opts.pos_count > 0 ? opts.positional[0] : resolve_session_env(&opts);
   cJSON_AddStringToObject(req, "session_id", sid ? sid : "");
   const char *aid = rpc_get(&opts, "attach-id");
   cJSON_AddStringToObject(req, "attach_id", aid ? aid : "");
   return req;
}

/* `aimee presence [--owner P]` — list the owner's live presences. */
static cJSON *marshal_session_presence(int argc, char **argv)
{
   rpc_opts_t opts;
   rpc_parse(argc, argv, NULL, &opts);

   cJSON *req = marshal_no_args("session.presence");
   const char *owner = rpc_get(&opts, "owner");
   if (owner && owner[0])
      cJSON_AddStringToObject(req, "owner", owner);
   return req;
}

static cJSON *marshal_trajectory_export(int argc, char **argv)
{
   static const char *bool_flags[] = {"no-compress", NULL};
   rpc_opts_t opts;
   rpc_parse(argc, argv, bool_flags, &opts);

   const char *sid = opts.pos_count > 0 ? opts.positional[0] : rpc_get(&opts, "session");
   if (!sid || !sid[0])
   {
      fprintf(stderr, "aimee: usage: aimee trajectory export <session_id> [--no-compress] "
                      "[--max-result-bytes N]\n");
      return NULL;
   }

   cJSON *req = marshal_no_args("trajectory.export");
   cJSON_AddStringToObject(req, "session_id", sid);
   cJSON_AddBoolToObject(req, "compress", rpc_get(&opts, "no-compress") ? 0 : 1);

   int max_bytes = rpc_get_int(&opts, "max-result-bytes", 512);
   if (max_bytes > 0)
      cJSON_AddNumberToObject(req, "max_result_bytes", max_bytes);
   return req;
}

static cJSON *marshal_trajectory_batch(int argc, char **argv)
{
   static const char *bool_flags[] = {"no-compress", NULL};
   rpc_opts_t opts;
   rpc_parse(argc, argv, bool_flags, &opts);

   const char *tasks = rpc_get(&opts, "tasks");
   if (!tasks || !tasks[0])
   {
      fprintf(stderr, "aimee: usage: aimee trajectory batch --tasks corpus.jsonl|suite_dir "
                      "[--toolset-dist research] [--out dir]\n");
      return NULL;
   }

   cJSON *req = marshal_no_args("trajectory.batch");
   cJSON_AddStringToObject(req, "tasks_path", tasks);
   const char *dist = rpc_get(&opts, "toolset-dist");
   if (dist && dist[0])
      cJSON_AddStringToObject(req, "toolset_dist", dist);
   const char *out = rpc_get(&opts, "out");
   if (out && out[0])
      cJSON_AddStringToObject(req, "out_dir", out);
   cJSON_AddBoolToObject(req, "compress", rpc_get(&opts, "no-compress") ? 0 : 1);
   int max_bytes = rpc_get_int(&opts, "max-result-bytes", 512);
   if (max_bytes > 0)
      cJSON_AddNumberToObject(req, "max_result_bytes", max_bytes);
   return req;
}

static cJSON *marshal_no_args(const char *method)
{
   cJSON *req = cJSON_CreateObject();
   cJSON_AddStringToObject(req, "method", method);
   cJSON_AddNumberToObject(req, "protocol_version", RPC_PROTOCOL_VERSION);
   return req;
}

static cJSON *marshal_mcp_recheck(int argc, char **argv)
{
   cJSON *req = marshal_no_args("mcp.recheck");
   if (argc > 0 && argv && argv[0] && argv[0][0] && argv[0][0] != '-')
      cJSON_AddStringToObject(req, "name", argv[0]);
   return req;
}

static cJSON *marshal_rules_delete(int argc, char **argv)
{
   rpc_opts_t opts;
   rpc_parse(argc, argv, NULL, &opts);

   cJSON *req = marshal_no_args("rules.delete");

   const char *id = opts.pos_count > 0 ? opts.positional[0] : rpc_get(&opts, "id");
   if (id && id[0])
      cJSON_AddNumberToObject(req, "id", atoi(id));
   return req;
}

static const char *resolve_session_env(const rpc_opts_t *opts)
{
   const char *s = rpc_get(opts, "session");
   if (s)
      return s;
   s = getenv("AIMEE_SESSION_ID");
   if (s && s[0])
      return s;
   return "default";
}

static cJSON *marshal_wm_set(int argc, char **argv)
{
   rpc_opts_t opts;
   rpc_parse(argc, argv, NULL, &opts);

   cJSON *req = marshal_no_args("wm.set");

   if (opts.pos_count > 0)
      cJSON_AddStringToObject(req, "key", opts.positional[0]);
   if (opts.pos_count > 1)
      cJSON_AddStringToObject(req, "value", opts.positional[1]);

   cJSON_AddStringToObject(req, "session_id", resolve_session_env(&opts));

   const char *v;
   if ((v = rpc_get(&opts, "category")))
      cJSON_AddStringToObject(req, "category", v);
   int ttl = rpc_get_int(&opts, "ttl", 0);
   if (ttl > 0)
      cJSON_AddNumberToObject(req, "ttl", ttl);
   return req;
}

static cJSON *marshal_wm_get(int argc, char **argv)
{
   rpc_opts_t opts;
   rpc_parse(argc, argv, NULL, &opts);

   cJSON *req = marshal_no_args("wm.get");

   if (opts.pos_count > 0)
      cJSON_AddStringToObject(req, "key", opts.positional[0]);
   cJSON_AddStringToObject(req, "session_id", resolve_session_env(&opts));
   return req;
}

static cJSON *marshal_wm_list(int argc, char **argv)
{
   rpc_opts_t opts;
   rpc_parse(argc, argv, NULL, &opts);

   cJSON *req = marshal_no_args("wm.list");

   cJSON_AddStringToObject(req, "session_id", resolve_session_env(&opts));
   const char *v;
   if ((v = rpc_get(&opts, "category")))
      cJSON_AddStringToObject(req, "category", v);
   return req;
}

static cJSON *marshal_config_get(int argc, char **argv)
{
   rpc_opts_t opts;
   rpc_parse(argc, argv, NULL, &opts);

   cJSON *req = marshal_no_args("config.get");
   if (opts.pos_count > 0)
      cJSON_AddStringToObject(req, "key", opts.positional[0]);
   return req;
}

static cJSON *marshal_config_set(int argc, char **argv)
{
   rpc_opts_t opts;
   rpc_parse(argc, argv, NULL, &opts);

   cJSON *req = marshal_no_args("config.set");
   if (opts.pos_count > 0)
      cJSON_AddStringToObject(req, "key", opts.positional[0]);
   if (opts.pos_count > 1)
      cJSON_AddStringToObject(req, "value", opts.positional[1]);
   return req;
}

/* `aimee primary <name>` sets, `--show` (or no arg) reads, `--clear` clears the
 * session's active primary agent. One route ("primary.set") dispatches here and
 * the real RPC method is chosen from the args. */
static cJSON *marshal_primary(int argc, char **argv)
{
   static const char *bools[] = {"show", "clear", NULL};
   rpc_opts_t opts;
   rpc_parse(argc, argv, bools, &opts);

   cJSON *req = cJSON_CreateObject();
   cJSON_AddNumberToObject(req, "protocol_version", RPC_PROTOCOL_VERSION);

   if (rpc_get(&opts, "clear"))
      cJSON_AddStringToObject(req, "method", "primary.clear");
   else if (rpc_get(&opts, "show") || opts.pos_count == 0)
      cJSON_AddStringToObject(req, "method", "primary.get");
   else
   {
      cJSON_AddStringToObject(req, "method", "primary.set");
      cJSON_AddStringToObject(req, "agent", opts.positional[0]);
   }
   cJSON_AddStringToObject(req, "session_id", resolve_session_env(&opts));
   return req;
}

static cJSON *marshal_kb_search(int argc, char **argv)
{
   rpc_opts_t opts;
   rpc_parse(argc, argv, NULL, &opts);

   cJSON *req = marshal_no_args("kb.search");

   if (opts.pos_count > 0)
      cJSON_AddStringToObject(req, "query", opts.positional[0]);

   const char *v;
   if ((v = rpc_get(&opts, "project")))
      cJSON_AddStringToObject(req, "project", v);
   cJSON_AddNumberToObject(req, "max_results", rpc_get_int(&opts, "max", 10));
   if ((v = rpc_get(&opts, "fusion-mode")))
      cJSON_AddStringToObject(req, "fusion_mode", v);
   if ((v = rpc_get(&opts, "embed")))
      cJSON_AddStringToObject(req, "embedding_command", v);
   return req;
}

static cJSON *marshal_kb_build(int argc, char **argv)
{
   static const char *bool_flags[] = {"force", NULL};
   rpc_opts_t opts;
   rpc_parse(argc, argv, bool_flags, &opts);

   cJSON *req = marshal_no_args("kb.build");

   if (opts.pos_count >= 1)
      cJSON_AddStringToObject(req, "path", opts.positional[0]);
   if (opts.pos_count >= 2)
      cJSON_AddStringToObject(req, "project", opts.positional[1]);
   if (rpc_get(&opts, "force"))
      cJSON_AddTrueToObject(req, "force");
   const char *v;
   if ((v = rpc_get(&opts, "embed")))
      cJSON_AddStringToObject(req, "embedding_command", v);
   return req;
}

static cJSON *marshal_kb_update(int argc, char **argv)
{
   rpc_opts_t opts;
   rpc_parse(argc, argv, NULL, &opts);

   cJSON *req = marshal_no_args("kb.update");

   if (opts.pos_count >= 1)
      cJSON_AddStringToObject(req, "path", opts.positional[0]);
   if (opts.pos_count >= 2)
      cJSON_AddStringToObject(req, "project", opts.positional[1]);
   const char *v;
   if ((v = rpc_get(&opts, "embed")))
      cJSON_AddStringToObject(req, "embedding_command", v);
   return req;
}

static cJSON *marshal_kb_ingest(int argc, char **argv)
{
   static const char *bool_flags[] = {"force", NULL};
   rpc_opts_t opts;
   rpc_parse(argc, argv, bool_flags, &opts);

   cJSON *req = marshal_no_args("kb.ingest");

   if (opts.pos_count >= 1)
      cJSON_AddStringToObject(req, "workspace", opts.positional[0]);
   if (rpc_get(&opts, "force"))
      cJSON_AddTrueToObject(req, "force");
   const char *v;
   if ((v = rpc_get(&opts, "embed")))
      cJSON_AddStringToObject(req, "embedding_command", v);
   return req;
}

static cJSON *marshal_kb_docs_push(int argc, char **argv)
{
   static const char *bool_flags[] = {"json", NULL};
   rpc_opts_t opts;
   rpc_parse(argc, argv, bool_flags, &opts);

   cJSON *req = marshal_no_args("kb.docs.push");

   const char *scope = rpc_get(&opts, "scope");
   if (!scope || !scope[0])
      scope = rpc_get(&opts, "project");
   if (scope && scope[0])
      cJSON_AddStringToObject(req, "scope", scope);

   cJSON *paths = cJSON_AddArrayToObject(req, "paths");
   for (int i = 0; paths && i < opts.pos_count; i++)
   {
      const char *path = opts.positional[i];
      char abs_path[4096];
      if (path && path[0] != '/')
      {
         char cwd_buf[4096];
         if (getcwd(cwd_buf, sizeof(cwd_buf)))
         {
            snprintf(abs_path, sizeof(abs_path), "%s/%s", cwd_buf, path);
            path = abs_path;
         }
      }
      cJSON_AddItemToArray(paths, cJSON_CreateString(path ? path : ""));
   }
   return req;
}

static cJSON *marshal_kb_status(int argc, char **argv)
{
   rpc_opts_t opts;
   rpc_parse(argc, argv, NULL, &opts);

   cJSON *req = marshal_no_args("kb.status");

   if (opts.pos_count >= 1)
      cJSON_AddStringToObject(req, "project", opts.positional[0]);
   return req;
}

static cJSON *marshal_worktree_gc(int argc, char **argv)
{
   static const char *bool_flags[] = {"force", "dry-run", NULL};
   rpc_opts_t opts;
   rpc_parse(argc, argv, bool_flags, &opts);

   cJSON *req = marshal_no_args("worktree.gc");

   /* Server side resolves git_root from this client_cwd (the worktrees
    * being GC'd live on the operator's filesystem; the daemon needs to
    * be told which repo the operator is in). */
   char cwd_buf[4096];
   if (getcwd(cwd_buf, sizeof(cwd_buf)))
      cJSON_AddStringToObject(req, "client_cwd", cwd_buf);

   int days = rpc_get_int(&opts, "days", 14);
   if (days < 1)
      days = 1;
   if (days > 365)
      days = 365;
   cJSON_AddNumberToObject(req, "max_age_days", days);
   if (rpc_get(&opts, "force"))
      cJSON_AddTrueToObject(req, "force");
   if (rpc_get(&opts, "dry-run"))
      cJSON_AddTrueToObject(req, "dry_run");
   return req;
}

static const char *resolve_work_session_env(void)
{
   const char *s = getenv("AIMEE_SESSION_ID");
   if (s && s[0])
      return s;
   s = getenv("CLAUDE_SESSION_ID");
   if (s && s[0])
      return s;
   return "default";
}

static const char *resolve_delegate_session_env(void)
{
   const char *s = getenv("AIMEE_SESSION_ID");
   if (s && s[0])
      return s;
   s = getenv("CLAUDE_SESSION_ID");
   if (s && s[0])
      return s;

   static char sid[64];
   sid[0] = '\0';
   char cwd[4096];
   if (!getcwd(cwd, sizeof(cwd)))
      return NULL;

   const char marker[] = "/.aimee/worktrees/";
   char *p = strstr(cwd, marker);
   if (!p)
      return NULL;
   p += strlen(marker);
   char *slash = strchr(p, '/');
   if (!slash || slash == p)
      return NULL;

   size_t len = (size_t)(slash - p);
   if (len >= sizeof(sid))
      len = sizeof(sid) - 1;
   memcpy(sid, p, len);
   sid[len] = '\0';
   return sid[0] ? sid : NULL;
}

static void marshal_add_client_cwd(cJSON *req)
{
   char cwd_buf[4096];
   if (req && getcwd(cwd_buf, sizeof(cwd_buf)))
      cJSON_AddStringToObject(req, "client_cwd", cwd_buf);
}

static cJSON *marshal_work_request(const char *method, int argc, char **argv)
{
   static const char *bool_flags[] = {"from-proposals", "no-sync-proposals", NULL};
   rpc_opts_t opts;
   rpc_parse(argc, argv, bool_flags, &opts);

   cJSON *req = cJSON_CreateObject();
   cJSON_AddStringToObject(req, "method", method);
   cJSON_AddNumberToObject(req, "protocol_version", RPC_PROTOCOL_VERSION);

   const char *v;
   if (strcmp(method, "work.add") == 0)
   {
      if (opts.pos_count > 0)
         cJSON_AddStringToObject(req, "title", opts.positional[0]);
      if ((v = rpc_get(&opts, "desc")) && v[0])
         cJSON_AddStringToObject(req, "description", v);
      if ((v = rpc_get(&opts, "source")) && v[0])
         cJSON_AddStringToObject(req, "source", v);
      cJSON_AddNumberToObject(req, "priority", rpc_get_int(&opts, "priority", 0));
      cJSON_AddStringToObject(req, "session_id", resolve_work_session_env());
   }
   else if (strcmp(method, "work.add_batch") == 0)
   {
      if (rpc_get(&opts, "from-proposals"))
         cJSON_AddTrueToObject(req, "from_proposals");
      if ((v = rpc_get(&opts, "dir")) && v[0])
         cJSON_AddStringToObject(req, "dir", v);
      cJSON_AddStringToObject(req, "session_id", resolve_work_session_env());
      marshal_add_client_cwd(req);
   }
   else if (strcmp(method, "work.claim") == 0)
   {
      v = rpc_get(&opts, "id");
      if (!v && opts.pos_count > 0)
         v = opts.positional[0];
      if (v && v[0])
         cJSON_AddStringToObject(req, "id", v);
      if ((v = rpc_get(&opts, "effort")) && v[0])
         cJSON_AddStringToObject(req, "effort", v);
      if ((v = rpc_get(&opts, "tag")) && v[0])
         cJSON_AddStringToObject(req, "tag", v);
      if ((v = rpc_get(&opts, "exclude-tag")) && v[0])
         cJSON_AddStringToObject(req, "exclude_tag", v);
      cJSON_AddNumberToObject(req, "skip", rpc_get_int(&opts, "skip", 0));
      cJSON_AddStringToObject(req, "session_id", resolve_work_session_env());
   }
   else if (strcmp(method, "work.complete") == 0 || strcmp(method, "work.fail") == 0)
   {
      v = rpc_get(&opts, "id");
      if (!v && opts.pos_count > 0)
         v = opts.positional[0];
      if (v && v[0])
         cJSON_AddStringToObject(req, "id", v);
      if ((v = rpc_get(&opts, "result")) && v[0])
         cJSON_AddStringToObject(req, "result", v);
      cJSON_AddStringToObject(req, "session_id", resolve_work_session_env());
   }
   else if (strcmp(method, "work.list") == 0)
   {
      if ((v = rpc_get(&opts, "status")) && v[0])
         cJSON_AddStringToObject(req, "status_filter", v);
   }
   else if (strcmp(method, "work.cancel") == 0 || strcmp(method, "work.release") == 0)
   {
      v = rpc_get(&opts, "id");
      if (!v && opts.pos_count > 0)
         v = opts.positional[0];
      if (v && v[0])
         cJSON_AddStringToObject(req, "id", v);
      cJSON_AddStringToObject(req, "session_id", resolve_work_session_env());
   }
   else if (strcmp(method, "work.clear") == 0)
   {
      if ((v = rpc_get(&opts, "status")) && v[0])
         cJSON_AddStringToObject(req, "status_filter", v);
      cJSON_AddStringToObject(req, "session_id", resolve_work_session_env());
   }
   else if (strcmp(method, "work.gc") == 0)
   {
      int hours = rpc_get_int(&opts, "max-age", 2);
      if (hours < 1)
         hours = 1;
      cJSON_AddNumberToObject(req, "max_age_hours", hours);
      if (rpc_get(&opts, "no-sync-proposals"))
         cJSON_AddTrueToObject(req, "no_sync_proposals");
      cJSON_AddStringToObject(req, "session_id", resolve_work_session_env());
      marshal_add_client_cwd(req);
   }
   else if (strcmp(method, "work.sync_proposals") == 0)
   {
      if ((v = rpc_get(&opts, "base")) && v[0])
         cJSON_AddStringToObject(req, "base", v);
      cJSON_AddStringToObject(req, "session_id", resolve_work_session_env());
      marshal_add_client_cwd(req);
   }

   return req;
}

static cJSON *marshal_insights_overview(int argc, char **argv)
{
   rpc_opts_t opts;
   rpc_parse(argc, argv, NULL, &opts);

   cJSON *req = marshal_no_args("insights.overview");

   int days = rpc_get_int(&opts, "days", 30);
   if (days < 1)
      days = 1;
   if (days > 365)
      days = 365;
   cJSON_AddNumberToObject(req, "days", days);
   return req;
}

static char *marshal_read_prompt_file(const char *path)
{
   FILE *f;
   long len;
   char *buf;
   size_t nread;

   if (!path || !path[0])
      return NULL;

   f = fopen(path, "r");
   if (!f)
      return NULL;
   fseek(f, 0, SEEK_END);
   len = ftell(f);
   fseek(f, 0, SEEK_SET);
   if (len <= 0 || len > 2 * 1024 * 1024)
   {
      fclose(f);
      return NULL;
   }
   buf = malloc((size_t)len + 1);
   if (!buf)
   {
      fclose(f);
      return NULL;
   }
   nread = fread(buf, 1, (size_t)len, f);
   buf[nread] = '\0';
   fclose(f);
   return buf;
}

static char *marshal_read_prompt_stdin(void)
{
   size_t cap = 4096;
   size_t len = 0;
   char *buf = malloc(cap);
   if (!buf)
      return NULL;
   for (;;)
   {
      if (len + 1024 >= cap)
      {
         if (cap >= 2 * 1024 * 1024)
         {
            free(buf);
            return NULL;
         }
         size_t next = cap * 2;
         if (next > 2 * 1024 * 1024 + 1)
            next = 2 * 1024 * 1024 + 1;
         char *nb = realloc(buf, next);
         if (!nb)
         {
            free(buf);
            return NULL;
         }
         buf = nb;
         cap = next;
      }
      size_t n = fread(buf + len, 1, cap - len - 1, stdin);
      len += n;
      if (n == 0)
      {
         if (ferror(stdin))
         {
            free(buf);
            return NULL;
         }
         break;
      }
   }
   buf[len] = '\0';
   if (len == 0)
   {
      free(buf);
      return NULL;
   }
   return buf;
}

static char *client_strdup(const char *s)
{
   if (!s)
      s = "";
   size_t len = strlen(s);
   char *copy = malloc(len + 1);
   if (!copy)
      return NULL;
   memcpy(copy, s, len + 1);
   return copy;
}

static char *marshal_compose_delegate_prompt(const char *cli_prompt, const char *file_prompt)
{
   if (file_prompt && file_prompt[0])
   {
      if (cli_prompt && cli_prompt[0])
      {
         size_t cap = strlen(cli_prompt) + strlen(file_prompt) + 32;
         char *combined = malloc(cap);
         if (!combined)
            return NULL;
         snprintf(combined, cap, "%s\n\n# Prompt File\n%s", cli_prompt, file_prompt);
         return combined;
      }
      return client_strdup(file_prompt);
   }

   if (cli_prompt && cli_prompt[0])
      return client_strdup(cli_prompt);

   return NULL;
}

/* Append one file's contents to the preload block, bounded by `cap`.
 * Emits a "--- File: <path> ---" header (or a "(not found)" marker) so the
 * delegate can attribute the snippet. Reads relative to the operator's cwd,
 * which is the correct anchor: the server may run under a different cwd (or on
 * a different host), so resolving these paths server-side would be wrong. */
static void marshal_preload_append_file(char *block, size_t cap, size_t *pos, const char *path)
{
   if (!path || !path[0] || *pos + 256 >= cap)
      return;
   char *contents = marshal_read_prompt_file(path);
   if (contents)
   {
      *pos += (size_t)snprintf(block + *pos, cap - *pos, "\n--- File: %s ---\n", path);
      size_t avail = cap - *pos - 1;
      size_t clen = strlen(contents);
      if (clen > avail)
         clen = avail;
      memcpy(block + *pos, contents, clen);
      *pos += clen;
      block[*pos] = '\0';
      free(contents);
   }
   else
   {
      *pos += (size_t)snprintf(block + *pos, cap - *pos, "\n--- File: %s (not found) ---\n", path);
   }
}

/* Resolve a symbol to its defining file via `aimee index find` and append a
 * ~70-line excerpt around the definition. Mirrors the legacy local-path
 * --context behaviour so the deployed thin-client path restores it. */
/* Escape a token for safe interpolation inside the single quotes of a shell
 * command: a literal ' becomes '\'' so it cannot break out and inject commands.
 * Caller frees; NULL on OOM. Kept local to this .inc (rather than calling
 * util.c's shell_escape) so the thin-client TUs that include it — and the
 * unit-test target that compiles it — need no extra link dependency. */
/* Caller (marshal_preload_append_symbol) is POSIX-only, so this is unused on the
 * Windows build — mark maybe-unused to stay -Werror-clean there. */
__attribute__((unused)) static char *cli_rpc_shell_quote_inner(const char *raw)
{
   if (!raw)
      return NULL;
   size_t len = strlen(raw);
   char *esc = malloc(len * 4 + 1);
   if (!esc)
      return NULL;
   size_t j = 0;
   for (size_t i = 0; i < len; i++)
   {
      if (raw[i] == '\'')
      {
         esc[j++] = '\'';
         esc[j++] = '\\';
         esc[j++] = '\'';
         esc[j++] = '\'';
      }
      else
         esc[j++] = raw[i];
   }
   esc[j] = '\0';
   return esc;
}

static void marshal_preload_append_symbol(char *block, size_t cap, size_t *pos, const char *sym)
{
#if defined(_WIN32) || defined(_WIN64)
   (void)block;
   (void)cap;
   (void)pos;
   (void)sym;
   return; /* index-resolved symbol preload is POSIX-only (popen) */
#else
   if (!sym || !sym[0] || *pos + 512 >= cap)
      return;
   /* shell_escape the symbol: it is interpolated into a popen() command, so a
    * raw single quote would break out of the quotes and inject shell commands. */
   char *esc_sym = cli_rpc_shell_quote_inner(sym);
   if (!esc_sym)
      return;
   char find_cmd[512];
   snprintf(find_cmd, sizeof(find_cmd), "aimee index find '%s' 2>/dev/null", esc_sym);
   free(esc_sym);
   FILE *fp = popen(find_cmd, "r");
   if (!fp)
      return;
   char best_file[1024] = "";
   int best_lineno = 0;
   char line[2048];
   while (fgets(line, sizeof(line), fp))
   {
      char *p = line;
      while (*p == ' ')
         p++;
      char *colon = strrchr(p, ':');
      if (!colon)
         continue;
      char *sp = colon + 1;
      int lineno = (int)strtol(sp, NULL, 10);
      if (lineno <= 0)
         continue;
      *colon = '\0';
      /* p is "file:line  definition [scope]"; cut at the first space so only
       * the file path survives. */
      char *trim = strchr(p, ' ');
      if (trim)
         *trim = '\0';
      snprintf(best_file, sizeof(best_file), "%s", p);
      best_lineno = lineno;
      break; /* first match is the highest-ranked definition */
   }
   pclose(fp);
   if (!best_file[0] || best_lineno <= 0)
      return;
   FILE *src = fopen(best_file, "r");
   if (!src)
      return;
   int start = best_lineno > 5 ? best_lineno - 5 : 1;
   *pos += (size_t)snprintf(block + *pos, cap - *pos, "\n## %s (%s:%d)\n```\n", sym, best_file,
                            best_lineno);
   int cur = 1;
   char srcline[2048];
   while (fgets(srcline, sizeof(srcline), src) && cur < start + 65 && *pos + 256 < cap)
   {
      if (cur >= start)
      {
         size_t sll = strlen(srcline);
         if (*pos + sll + 8 < cap)
         {
            memcpy(block + *pos, srcline, sll);
            *pos += sll;
            block[*pos] = '\0';
         }
      }
      cur++;
   }
   fclose(src);
   if (*pos + 8 < cap)
      *pos += (size_t)snprintf(block + *pos, cap - *pos, "```\n");
#endif /* POSIX */
}

/* Build the "preloaded context" block from the delegate's file/dir/symbol
 * flags: --context-file (repeatable), --files (comma-separated), --context-dir
 * (regular files in the directory), and --context (comma-separated symbols
 * resolved via the index). Returns a malloc'd string the caller frees, or NULL
 * when no preload flags are present. Resolved client-side so relative paths
 * anchor to the operator's cwd. */
static char *marshal_build_preload_context(const rpc_opts_t *opts)
{
   const size_t cap = 120 * 1024;
   char *block = malloc(cap);
   if (!block)
      return NULL;
   size_t pos = 0;
   block[0] = '\0';

   /* --context-file: repeatable, so scan every parsed flag entry. */
   const char *cf = "context-file";
   size_t cflen = strlen(cf);
   for (int i = 0; i < opts->flag_count; i++)
   {
      const char *raw = opts->flags[i].raw;
      const char *eq = strchr(raw, '=');
      size_t rlen = eq ? (size_t)(eq - raw) : strlen(raw);
      if (rlen == cflen && memcmp(raw, cf, cflen) == 0)
         marshal_preload_append_file(block, cap, &pos, opts->flags[i].value);
   }

   /* --files: comma-separated list. */
   const char *files = rpc_get(opts, "files");
   if (files && files[0])
   {
      char copy[4096];
      snprintf(copy, sizeof(copy), "%s", files);
      for (char *tok = strtok(copy, ","); tok; tok = strtok(NULL, ","))
      {
         while (*tok == ' ')
            tok++;
         marshal_preload_append_file(block, cap, &pos, tok);
      }
   }

   /* --context-dir: regular files directly under the directory (POSIX only). */
#if !defined(_WIN32) && !defined(_WIN64)
   const char *dir = rpc_get(opts, "context-dir");
   if (dir && dir[0])
   {
      DIR *d = opendir(dir);
      if (d)
      {
         struct dirent *ent;
         while ((ent = readdir(d)) && pos + 512 < cap)
         {
            if (ent->d_name[0] == '.')
               continue;
            char fpath[2048];
            snprintf(fpath, sizeof(fpath), "%s/%s", dir, ent->d_name);
            struct stat st;
            if (stat(fpath, &st) == 0 && S_ISREG(st.st_mode))
               marshal_preload_append_file(block, cap, &pos, fpath);
         }
         closedir(d);
      }
   }
#endif /* POSIX context-dir */

   /* --context: comma-separated symbol names resolved via the index. */
   const char *syms = rpc_get(opts, "context");
   if (syms && syms[0])
   {
      char copy[2048];
      snprintf(copy, sizeof(copy), "%s", syms);
      for (char *tok = strtok(copy, ","); tok; tok = strtok(NULL, ","))
      {
         while (*tok == ' ')
            tok++;
         marshal_preload_append_symbol(block, cap, &pos, tok);
      }
   }

   if (pos == 0)
   {
      free(block);
      return NULL;
   }
   return block;
}

static const char *marshal_delegate_toolset_arg(int argc, char **argv)
{
   for (int i = 0; i < argc; i++)
   {
      if (strncmp(argv[i], "--tools=", 8) == 0 && argv[i][8])
         return argv[i] + 8;
      if (strcmp(argv[i], "--tools") != 0 || i + 1 >= argc || argv[i + 1][0] == '-')
         continue;
      for (int j = i + 2; j < argc; j++)
      {
         if (strcmp(argv[j], "--prompt-file") == 0 || strcmp(argv[j], "--prompt-stdin") == 0)
            return argv[i + 1];
      }
   }
   return NULL;
}

static cJSON *marshal_delegate(int argc, char **argv)
{
   static const char *bool_flags[] = {"json",         "background", "durable", "coordination",
                                      "plan",         "dry-run",    "tools",   "handoff-json",
                                      "prompt-stdin", NULL};
   rpc_opts_t opts;
   rpc_parse(argc, argv, bool_flags, &opts);

   cJSON *req = marshal_no_args("delegate");

   if (opts.pos_count > 0)
      cJSON_AddStringToObject(req, "role", opts.positional[0]);

   const char *cli_prompt = opts.pos_count > 1 ? opts.positional[1] : NULL;
   const char *toolset_override = marshal_delegate_toolset_arg(argc, argv);
   if (!toolset_override && rpc_get(&opts, "tools") && opts.pos_count > 2)
      toolset_override = opts.positional[1];
   if (toolset_override && opts.pos_count > 2 && strcmp(opts.positional[1], toolset_override) == 0)
      cli_prompt = opts.positional[2];
   else if (toolset_override && opts.pos_count > 1 &&
            strcmp(opts.positional[1], toolset_override) == 0)
      cli_prompt = NULL;
   char *file_prompt = NULL;
   if (rpc_get(&opts, "prompt-stdin") && rpc_get(&opts, "prompt-file"))
   {
      cJSON_Delete(req);
      return NULL;
   }
   if (rpc_get(&opts, "prompt-stdin"))
      file_prompt = marshal_read_prompt_stdin();
   else
      file_prompt = marshal_read_prompt_file(rpc_get(&opts, "prompt-file"));
   char *delegate_prompt = marshal_compose_delegate_prompt(cli_prompt, file_prompt);

   /* Fold any --context-file / --files / --context-dir / --context preloads
    * into the prompt. These are advertised flags that the server delegate
    * handler does not interpret, so they must be resolved client-side (where
    * the operator's cwd anchors relative paths) and shipped in the prompt. */
   char *preload = marshal_build_preload_context(&opts);
   if (preload)
   {
      const char *base = (delegate_prompt && delegate_prompt[0]) ? delegate_prompt : "";
      size_t cap = strlen(base) + strlen(preload) + 64;
      char *combined = malloc(cap);
      if (combined)
      {
         snprintf(combined, cap, "%s\n\n# Source Packet: Preloaded Context\n%s", base, preload);
         free(delegate_prompt);
         delegate_prompt = combined;
      }
      free(preload);
   }

   if (delegate_prompt && delegate_prompt[0])
      cJSON_AddStringToObject(req, "prompt", delegate_prompt);

   const char *v;
   if ((v = rpc_get(&opts, "system")))
      cJSON_AddStringToObject(req, "system_prompt", v);
   if ((v = resolve_delegate_session_env()) && v[0])
      cJSON_AddStringToObject(req, "session_id", v);
   /* Propagate chain context only as a pair. A leaked depth without a parent
    * delegation id is stale process state and must not make an operator shell
    * look like a nested delegate. */
   const char *env_parent = getenv("AIMEE_PARENT_DELEGATION_ID");
   if (env_parent && env_parent[0] && (v = getenv("AIMEE_DELEGATE_DEPTH")) && v[0] && atoi(v) > 0)
      cJSON_AddNumberToObject(req, "delegation_depth", atoi(v));
   if (rpc_get(&opts, "background"))
      cJSON_AddTrueToObject(req, "background");
   if (rpc_get(&opts, "tools"))
      cJSON_AddTrueToObject(req, "tools");
   if (toolset_override && toolset_override[0])
      cJSON_AddStringToObject(req, "toolset", toolset_override);
   if (rpc_get(&opts, "handoff-json"))
      cJSON_AddTrueToObject(req, "handoff_json");
   int mt = rpc_get_int(&opts, "max-tokens", 0);
   if (mt > 0)
      cJSON_AddNumberToObject(req, "max_tokens", mt);
   if ((v = rpc_get(&opts, "max-turns")))
      cJSON_AddNumberToObject(req, "max_turns", atoi(v));
   int timeout = rpc_get_int(&opts, "timeout", 0);
   if (timeout > 0)
      cJSON_AddNumberToObject(req, "timeout_ms", timeout);
   if ((v = rpc_get(&opts, "worktree")) && v[0])
      cJSON_AddStringToObject(req, "branch", v);
   if ((v = rpc_get(&opts, "via")) && v[0])
      cJSON_AddStringToObject(req, "via", v);
   /* Inline ACP transport: --acp <command> [--acp-args <args>] routes this
    * delegate through an ephemeral kind:acp agent (the external agent runs its
    * own model; aimee's tools/worktree still apply). */
   if ((v = rpc_get(&opts, "acp")) && v[0])
      cJSON_AddStringToObject(req, "acp_command", v);
   if ((v = rpc_get(&opts, "acp-args")) && v[0])
      cJSON_AddStringToObject(req, "acp_args", v);
   if ((v = rpc_get(&opts, "provider")) && v[0])
      cJSON_AddStringToObject(req, "provider", v);
   if ((v = rpc_get(&opts, "model")) && v[0])
      cJSON_AddStringToObject(req, "model", v);
   if ((v = rpc_get(&opts, "persona")) && v[0])
      cJSON_AddStringToObject(req, "persona", v);
   {
      int tier = rpc_get_int(&opts, "tier", -1);
      if (tier >= 0)
         cJSON_AddNumberToObject(req, "tier", tier);
   }
   if (env_parent && env_parent[0])
      cJSON_AddStringToObject(req, "parent_delegation_id", env_parent);
   /* Anchor the delegate worktree at the operator's cwd, not the
    * server process's startup cwd. Without this, a client running in
    * /home/foo/dev/X delegates a job that lands in whatever repo the
    * server happened to start under. server_compute.c's worktree
    * resolver (handle_delegate_launch) reads this field per its own
    * comment "Resolution order for the anchor git root: 1.
    * caller-supplied `cwd` ...". */
   {
      char cwd_buf[4096];
      if (getcwd(cwd_buf, sizeof(cwd_buf)))
         cJSON_AddStringToObject(req, "cwd", cwd_buf);
   }
   free(delegate_prompt);
   free(file_prompt);
   return req;
}

static cJSON *marshal_delegate_status(int argc, char **argv)
{
   rpc_opts_t opts;
   const char *bool_flags[] = {"full", NULL};
   rpc_parse(argc, argv, bool_flags, &opts);

   cJSON *req = marshal_no_args("delegate.status");

   if (opts.pos_count > 1)
   {
      cJSON *job_ids = cJSON_AddArrayToObject(req, "job_ids");
      for (int i = 0; i < opts.pos_count; i++)
         cJSON_AddItemToArray(job_ids, cJSON_CreateNumber(atoi(opts.positional[i])));
   }
   else
   {
      const char *job_id = opts.pos_count > 0 ? opts.positional[0] : rpc_get(&opts, "job-id");
      if (job_id && job_id[0])
         cJSON_AddNumberToObject(req, "job_id", atoi(job_id));
   }
   if (rpc_get(&opts, "full"))
   {
      cJSON_AddBoolToObject(req, "full_result", 1);
      cJSON_AddNumberToObject(req, "result_limit", -1);
   }
   else
   {
      int result_limit = rpc_get_int(&opts, "result-limit", 200);
      if (result_limit < 0)
         result_limit = 0;
      cJSON_AddNumberToObject(req, "result_limit", result_limit);
   }

   return req;
}

static cJSON *marshal_delegate_log(int argc, char **argv)
{
   rpc_opts_t opts;
   rpc_parse(argc, argv, NULL, &opts);
   if (opts.pos_count > 0)
   {
      fprintf(stderr, "aimee: usage: aimee delegate log [--json]\n"
                      "aimee: for a background job log, use `aimee jobs logs <job_id>`.\n");
      return NULL;
   }
   return marshal_no_args("delegate.log");
}

static cJSON *marshal_jobs_list(int argc, char **argv)
{
   rpc_opts_t opts;
   rpc_parse(argc, argv, NULL, &opts);

   cJSON *req = marshal_no_args("jobs.list");
   cJSON_AddNumberToObject(req, "limit", rpc_get_int(&opts, "limit", 20));
   return req;
}

static cJSON *marshal_coord_jobs_list(int argc, char **argv)
{
   rpc_opts_t opts;
   rpc_parse(argc, argv, NULL, &opts);

   cJSON *req = marshal_no_args("job.list");
   cJSON_AddNumberToObject(req, "limit", rpc_get_int(&opts, "limit", 20));
   return req;
}

static cJSON *marshal_coord_job_start(int argc, char **argv)
{
   rpc_opts_t opts;
   rpc_parse(argc, argv, NULL, &opts);

   cJSON *req = marshal_no_args("job.start");

   const char *plan_id = opts.pos_count > 0 ? opts.positional[0] : rpc_get(&opts, "plan-id");
   if (plan_id && plan_id[0])
      cJSON_AddNumberToObject(req, "plan_id", atoi(plan_id));
   int parallel = rpc_get_int(&opts, "parallel", 0);
   if (parallel > 0)
      cJSON_AddNumberToObject(req, "parallel", parallel);
   return req;
}

static cJSON *marshal_job_id_request(const char *method, int argc, char **argv)
{
   rpc_opts_t opts;
   rpc_parse(argc, argv, NULL, &opts);

   cJSON *req = cJSON_CreateObject();
   cJSON_AddStringToObject(req, "method", method);
   cJSON_AddNumberToObject(req, "protocol_version", RPC_PROTOCOL_VERSION);

   const char *job_id = opts.pos_count > 0 ? opts.positional[0] : rpc_get(&opts, "job-id");
   if (job_id && job_id[0])
      cJSON_AddNumberToObject(req, "job_id", atoi(job_id));
   const char *reason = rpc_get(&opts, "reason");
   if (reason && reason[0])
      cJSON_AddStringToObject(req, "reason", reason);
   return req;
}

static cJSON *marshal_aux_test(int argc, char **argv)
{
   rpc_opts_t opts;
   rpc_parse(argc, argv, NULL, &opts);

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
   rpc_opts_t opts;
   rpc_parse(argc, argv, NULL, &opts);

   cJSON *req = marshal_no_args("agent.episodes");

   const char *agent = opts.pos_count > 0 ? opts.positional[0] : rpc_get(&opts, "agent");
   if (agent && agent[0])
      cJSON_AddStringToObject(req, "agent", agent);

   return req;
}

static char *rpc_read_file_limited(const char *path, size_t limit, char *err, size_t err_len)
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
   cJSON_AddNumberToObject(req, "protocol_version", RPC_PROTOCOL_VERSION);
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
         char *content = rpc_read_file_limited(argv[1], 100 * 1024, err, sizeof(err));
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
   rpc_opts_t opts;
   rpc_parse(argc, argv, bool_flags, &opts);

   const char *path = opts.pos_count > 0 ? opts.positional[0] : NULL;
   if (!path || !path[0])
   {
      fprintf(stderr, "aimee: usage: aimee delegate launch <plan.json> [--parallel N]\n");
      return NULL;
   }

   char err[256] = "";
   char *text = rpc_read_file_limited(path, 2 * 1024 * 1024, err, sizeof(err));
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

   int parallel = rpc_get_int(&opts, "parallel", 3);
   if (parallel > 0)
      cJSON_AddNumberToObject(req, "parallel", parallel);
   return req;
}

static cJSON *marshal_trigger_list(int argc, char **argv)
{
   rpc_opts_t opts;
   rpc_parse(argc, argv, NULL, &opts);

   cJSON *req = marshal_no_args("trigger.list");
   const char *status = rpc_get(&opts, "status");
   if (status && status[0])
      cJSON_AddStringToObject(req, "status", status);
   return req;
}

static cJSON *marshal_trigger_id(const char *method, int argc, char **argv)
{
   rpc_opts_t opts;
   rpc_parse(argc, argv, NULL, &opts);

   const char *id = opts.pos_count > 0 ? opts.positional[0] : rpc_get(&opts, "id");
   if (!id || !id[0])
   {
      fprintf(stderr, "aimee: usage: aimee trigger %s <id>\n",
              strcmp(method, "trigger.status") == 0 ? "status" : "cancel");
      return NULL;
   }

   cJSON *req = cJSON_CreateObject();
   cJSON_AddStringToObject(req, "method", method);
   cJSON_AddNumberToObject(req, "protocol_version", RPC_PROTOCOL_VERSION);
   cJSON_AddStringToObject(req, "id", id);
   return req;
}

static cJSON *marshal_trigger_fire(int argc, char **argv)
{
   rpc_opts_t opts;
   rpc_parse(argc, argv, NULL, &opts);

   const char *source = rpc_get(&opts, "source");
   const char *task = rpc_get(&opts, "task");
   if (!source || !source[0] || !task || !task[0])
   {
      fprintf(stderr, "aimee: usage: aimee trigger fire --source <source> --task <task> "
                      "[--event <event>] [--workspace <ws>] [--token <auth_token>]\n");
      return NULL;
   }

   cJSON *req = marshal_no_args("trigger.fire");
   cJSON_AddStringToObject(req, "source", source);
   cJSON_AddStringToObject(req, "task", task);

   const char *v;
   if ((v = rpc_get(&opts, "event")) && v[0])
      cJSON_AddStringToObject(req, "event", v);
   if ((v = rpc_get(&opts, "workspace")) && v[0])
      cJSON_AddStringToObject(req, "workspace", v);
   if ((v = rpc_get(&opts, "token")) && v[0])
      cJSON_AddStringToObject(req, "auth_token", v);
   if ((v = rpc_get(&opts, "metadata")) && v[0])
      cJSON_AddStringToObject(req, "metadata", v);
   return req;
}

static cJSON *marshal_cron_id(const char *method, int argc, char **argv)
{
   static const char *bools[] = {"all", NULL};
   rpc_opts_t opts;
   rpc_parse(argc, argv, bools, &opts);

   const char *id = opts.pos_count > 0 ? opts.positional[0] : rpc_get(&opts, "id");
   int all = (strcmp(method, "cron.enable") == 0 || strcmp(method, "cron.disable") == 0) &&
             rpc_get(&opts, "all") != NULL;
   if (!id || !id[0])
   {
      if (all)
      {
         cJSON *req = cJSON_CreateObject();
         cJSON_AddStringToObject(req, "method", method);
         cJSON_AddNumberToObject(req, "protocol_version", RPC_PROTOCOL_VERSION);
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
   cJSON_AddNumberToObject(req, "protocol_version", RPC_PROTOCOL_VERSION);
   cJSON_AddStringToObject(req, "job_id", id);
   if (strcmp(method, "cron.history") == 0)
      cJSON_AddNumberToObject(req, "limit", rpc_get_int(&opts, "limit", 20));
   return req;
}

static cJSON *marshal_cron_add(int argc, char **argv)
{
   static const char *bools[] = {"only-if-changed", "first-run-silent", "pre-wake-gate", "disabled",
                                 NULL};
   rpc_opts_t opts;
   rpc_parse(argc, argv, bools, &opts);

   const char *id = opts.pos_count > 0 ? opts.positional[0] : rpc_get(&opts, "id");
   const char *schedule = rpc_get(&opts, "schedule");
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
   if ((v = rpc_get(&opts, "mode")) && v[0])
      cJSON_AddStringToObject(req, "mode", v);
   if ((v = rpc_get(&opts, "script")) && v[0])
      cJSON_AddStringToObject(req, "script", v);
   if ((v = rpc_get(&opts, "prompt")) && v[0])
      cJSON_AddStringToObject(req, "prompt", v);
   if ((v = rpc_get(&opts, "workdir")) && v[0])
      cJSON_AddStringToObject(req, "workdir", v);
   if ((v = rpc_get(&opts, "target")) && v[0])
      cJSON_AddStringToObject(req, "deliver_target", v);
   if ((v = rpc_get(&opts, "context-from")) && v[0])
      cJSON_AddStringToObject(req, "context_from", v);
   if ((v = rpc_get(&opts, "when-context-contains")) && v[0])
      cJSON_AddStringToObject(req, "when_context_contains", v);
   if (rpc_get(&opts, "only-if-changed"))
      cJSON_AddBoolToObject(req, "deliver_only_if_changed", 1);
   if (rpc_get(&opts, "first-run-silent"))
      cJSON_AddBoolToObject(req, "deliver_first_run_silent", 1);
   if (rpc_get(&opts, "pre-wake-gate"))
      cJSON_AddBoolToObject(req, "pre_wake_gate", 1);
   if (rpc_get(&opts, "disabled"))
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
   cJSON_AddNumberToObject(req, "protocol_version", RPC_PROTOCOL_VERSION);
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
static cJSON *marshal_workspace_add(int argc, char **argv)
{
   cJSON *req = marshal_agent_args("workspace.add", argc, argv);
   if (!req)
      return NULL;
   rpc_opts_t opts;
   rpc_parse(argc, argv, NULL, &opts);
   if (opts.pos_count > 0)
      cJSON_AddStringToObject(req, "root", opts.positional[0]);
   const char *prov = rpc_get(&opts, "provider");
   if (prov)
      cJSON_AddStringToObject(req, "provider", prov);
   return req;
}

/* Compute the client's FULL working-tree patch vs HEAD — tracked modifications,
 * deletions, AND untracked (non-ignored) files as additions — without touching
 * the client's real index: stage everything into a throwaway index
 * (GIT_INDEX_FILE) seeded from HEAD, then `diff --cached --binary HEAD`. git's
 * --binary patch format is ASCII (base85 hunks), so the result is JSON-safe even
 * for binary files. Returns a malloc'd patch (caller frees), or NULL when the
 * root is not a repo / has no HEAD. POSIX-only (the thin Windows client cannot
 * fork git); Windows ships no diff. */
#if !defined(_WIN32) && !defined(_WIN64)
extern char **environ;
static char *mirror_compute_diff(const char *root)
{
   char idx[] = "/tmp/aimee-msync-idx-XXXXXX";
   int fd = mkstemp(idx);
   if (fd < 0)
      return NULL;
   close(fd); /* git read-tree overwrites it */

   int n = 0;
   while (environ[n])
      n++;
   char **envp = calloc((size_t)n + 2, sizeof(char *));
   if (!envp)
   {
      unlink(idx);
      return NULL;
   }
   char giv[300];
   snprintf(giv, sizeof(giv), "GIT_INDEX_FILE=%s", idx);
   for (int i = 0; i < n; i++)
      envp[i] = environ[i];
   envp[n] = giv;
   envp[n + 1] = NULL;

   char *out = NULL;
   const char *rt[] = {"git", "-C", root, "read-tree", "HEAD", NULL};
   int rc = safe_exec_capture_env(rt, envp, &out, 4096);
   free(out);
   out = NULL;
   char *patch = NULL;
   if (rc == 0)
   {
      const char *add[] = {"git", "-C", root, "add", "-A", NULL};
      safe_exec_capture_env(add, envp, &out, 4096);
      free(out);
      out = NULL;
      const char *df[] = {"git", "-C", root, "diff", "--cached", "--binary", "HEAD", NULL};
      safe_exec_capture_env(df, envp, &patch, 16 * 1024 * 1024);
   }
   free(envp);
   unlink(idx);
   return patch; /* may be "" (clean tree) */
}
#else
static char *mirror_compute_diff(const char *root)
{
   (void)root;
   return NULL;
}
#endif

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
   char *patch = mirror_compute_diff(root);
   if (!patch)
      fprintf(stderr,
              "warning: could not compute a diff for %s (not a git repo / no HEAD?); "
              "shipping an empty diff\n",
              root);

   cJSON *req = marshal_no_args("workspace.mirror-sync");
   cJSON *args = cJSON_CreateArray();
   cJSON_AddItemToArray(args, cJSON_CreateString(root));
   cJSON_AddItemToObject(req, "args", args);
   cJSON_AddStringToObject(req, "diff", patch ? patch : "");
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
   char cwd[4096];
   if (getcwd(cwd, sizeof(cwd)))
      cJSON_AddStringToObject(args, "path", cwd);
   cJSON_AddBoolToObject(args, "no_session_redirect", 1);

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

   if (!has_async)
      cJSON_AddBoolToObject(args, "async", 0);
   cJSON_AddItemToObject(req, "arguments", args);
   return req;
}

static cJSON *marshal_get_help(int argc, char **argv)
{
   cJSON *req = cJSON_CreateObject();
   cJSON_AddStringToObject(req, "method", "mcp.call");
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

static cJSON *marshal_storage_migrate(int argc, char **argv)
{
   if (argc < 1 || !argv[0] || !argv[0][0])
   {
      fprintf(stderr, "usage: aimee migrate v2 <legacy-store>\n");
      return NULL;
   }

   cJSON *req = marshal_no_args("migrate.v2");
   if (!req)
      return NULL;
   cJSON_AddStringToObject(req, "source_path", argv[0]);

   char cwd[4096];
   if (getcwd(cwd, sizeof(cwd)))
      cJSON_AddStringToObject(req, "cwd", cwd);
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
   static const char *bool_flags[] = {"available", "json", NULL};
   rpc_opts_t opts;
   rpc_parse(argc, argv, bool_flags, &opts);
   cJSON *req = marshal_no_args("provider.list");
   if (!req)
      return NULL;
   if (rpc_get(&opts, "available"))
      cJSON_AddTrueToObject(req, "available_only");
   if (rpc_get(&opts, "json"))
      cJSON_AddTrueToObject(req, "json");
   return req;
}

static cJSON *marshal_provider_name_method(const char *method, int argc, char **argv)
{
   rpc_opts_t opts;
   rpc_parse(argc, argv, NULL, &opts);
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
   rpc_opts_t opts;
   rpc_parse(argc, argv, bool_flags, &opts);
   cJSON *req = marshal_no_args("provider.models");
   if (!req)
      return NULL;
   if (opts.pos_count > 0 && opts.positional[0] && opts.positional[0][0])
      cJSON_AddStringToObject(req, "name", opts.positional[0]);
   if (rpc_get(&opts, "json"))
      cJSON_AddTrueToObject(req, "json");
   return req;
}

static cJSON *marshal_model_list(int argc, char **argv)
{
   static const char *bool_flags[] = {"json", "open-weights", NULL};
   rpc_opts_t opts;
   rpc_parse(argc, argv, bool_flags, &opts);
   cJSON *req = marshal_no_args("model.list");
   if (!req)
      return NULL;
   const char *capability = rpc_get(&opts, "capability");
   if (capability && capability[0])
      cJSON_AddStringToObject(req, "capability", capability);
   if (rpc_get(&opts, "json"))
      cJSON_AddTrueToObject(req, "json");
   if (rpc_has_flag(&opts, "open-weights"))
      cJSON_AddBoolToObject(req, "open_weights_only", 1);
   return req;
}

static cJSON *marshal_model_show(int argc, char **argv)
{
   rpc_opts_t opts;
   rpc_parse(argc, argv, NULL, &opts);
   if (opts.pos_count < 1)
   {
      fprintf(stderr, "aimee: usage: aimee model show [provider:]<model>\n");
      return NULL;
   }
   cJSON *req = marshal_no_args("model.show");
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
   rpc_opts_t opts;
   rpc_parse(argc, argv, bool_flags, &opts);
   cJSON *req = cJSON_CreateObject();
   cJSON_AddStringToObject(req, "method", "dogfood.tag");
   if (opts.pos_count > 0)
      cJSON_AddStringToObject(req, "record_id", opts.positional[0]);
   const char *outcome = rpc_get(&opts, "outcome");
   if (outcome)
      cJSON_AddStringToObject(req, "outcome", outcome);
   const char *notes = rpc_get(&opts, "notes");
   if (notes)
      cJSON_AddStringToObject(req, "notes", notes);
   const char *richness = rpc_get(&opts, "richness");
   if (richness)
      cJSON_AddNumberToObject(req, "richness", atoi(richness));
   if (rpc_get(&opts, "surprise"))
      cJSON_AddTrueToObject(req, "surprise");
   else if (rpc_get(&opts, "no-surprise"))
      cJSON_AddFalseToObject(req, "surprise");
   return req;
}

static cJSON *marshal_dogfood_report(int argc, char **argv)
{
   static const char *bool_flags[] = {"json", NULL};
   rpc_opts_t opts;
   rpc_parse(argc, argv, bool_flags, &opts);
   cJSON *req = cJSON_CreateObject();
   cJSON_AddStringToObject(req, "method", "dogfood.report");
   const char *month = rpc_get(&opts, "month");
   if (month)
      cJSON_AddStringToObject(req, "month", month);
   const char *dir = rpc_get(&opts, "dir");
   if (dir)
      cJSON_AddStringToObject(req, "dir", dir);
   return req;
}

static cJSON *marshal_dogfood_review(int argc, char **argv)
{
   static const char *bool_flags[] = {"json", NULL};
   rpc_opts_t opts;
   rpc_parse(argc, argv, bool_flags, &opts);
   cJSON *req = cJSON_CreateObject();
   cJSON_AddStringToObject(req, "method", "dogfood.review");
   const char *month = rpc_get(&opts, "month");
   if (month)
      cJSON_AddStringToObject(req, "month", month);
   const char *dir = rpc_get(&opts, "dir");
   if (dir)
      cJSON_AddStringToObject(req, "dir", dir);
   const char *limit = rpc_get(&opts, "limit");
   if (limit)
      cJSON_AddNumberToObject(req, "limit", atoi(limit));
   return req;
}

static cJSON *marshal_eval_run(int argc, char **argv)
{
   rpc_opts_t opts;
   rpc_parse(argc, argv, NULL, &opts);
   cJSON *req = cJSON_CreateObject();
   cJSON_AddStringToObject(req, "method", "eval.run");
   if (opts.pos_count > 0)
      cJSON_AddStringToObject(req, "suite_dir", opts.positional[0]);
   const char *ablation = rpc_get(&opts, "ablation");
   if (ablation)
      cJSON_AddStringToObject(req, "ablation", ablation);
   const char *runs = rpc_get(&opts, "runs");
   if (runs)
      cJSON_AddNumberToObject(req, "runs", atoi(runs));
   const char *seed = rpc_get(&opts, "seed");
   if (seed)
      cJSON_AddNumberToObject(req, "seed", strtoul(seed, NULL, 10));
   char cwd[4096];
   if (getcwd(cwd, sizeof(cwd)))
      cJSON_AddStringToObject(req, "cwd", cwd);
   return req;
}

static cJSON *marshal_eval_results(int argc, char **argv)
{
   rpc_opts_t opts;
   rpc_parse(argc, argv, NULL, &opts);
   cJSON *req = cJSON_CreateObject();
   cJSON_AddStringToObject(req, "method", "eval.results");
   if (opts.pos_count > 0)
      cJSON_AddStringToObject(req, "suite", opts.positional[0]);
   return req;
}

static cJSON *marshal_identity_snapshot(int argc, char **argv)
{
   rpc_opts_t opts;
   rpc_parse(argc, argv, NULL, &opts);
   cJSON *req = cJSON_CreateObject();
   cJSON_AddStringToObject(req, "method", "identity.snapshot");
   const char *out = rpc_get(&opts, "out");
   if (out)
      cJSON_AddStringToObject(req, "out", out);
   char cwd[4096];
   if (getcwd(cwd, sizeof(cwd)))
      cJSON_AddStringToObject(req, "cwd", cwd);
   return req;
}

static cJSON *marshal_identity_diff(int argc, char **argv)
{
   rpc_opts_t opts;
   rpc_parse(argc, argv, NULL, &opts);
   cJSON *req = cJSON_CreateObject();
   cJSON_AddStringToObject(req, "method", "identity.diff");
   if (opts.pos_count > 0)
      cJSON_AddStringToObject(req, "a", opts.positional[0]);
   if (opts.pos_count > 1)
      cJSON_AddStringToObject(req, "b", opts.positional[1]);
   const char *ft = rpc_get(&opts, "flip-threshold");
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
   rpc_opts_t opts;
   rpc_parse(argc, argv, bool_flags, &opts);

   cJSON *req = marshal_no_args("api.enable");
   if (!req)
      return NULL;
   if (rpc_has_flag(&opts, "vscode"))
      cJSON_AddBoolToObject(req, "vscode", 1);
   int port = rpc_get_int(&opts, "port", 0);
   if (port > 0)
      cJSON_AddNumberToObject(req, "port", port);
   int rate = rpc_get_int(&opts, "rate-limit", 0);
   if (rate > 0)
      cJSON_AddNumberToObject(req, "rate_limit", rate);
   return req;
}

/* graph sync-code <project> → graph.sync_code (server runs the code-graph
 * projection off-thread). Without this marshaler the route table entry existed
 * but the thin CLI reported "no typed server RPC route". */
static cJSON *marshal_graph_sync_code(int argc, char **argv)
{
   rpc_opts_t opts;
   rpc_parse(argc, argv, NULL, &opts);
   cJSON *req = marshal_no_args("graph.sync_code");
   if (opts.pos_count >= 1)
      cJSON_AddStringToObject(req, "project", opts.positional[0]);
   return req;
}

/* graph explain <entity> [--limit N] → graph.explain. */
static cJSON *marshal_graph_explain(int argc, char **argv)
{
   rpc_opts_t opts;
   rpc_parse(argc, argv, NULL, &opts);
   cJSON *req = marshal_no_args("graph.explain");
   if (opts.pos_count >= 1)
      cJSON_AddStringToObject(req, "entity", opts.positional[0]);
   cJSON_AddNumberToObject(req, "limit", rpc_get_int(&opts, "limit", 40));
   return req;
}

static cJSON *marshal_request(const char *method, int argc, char **argv)
{
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
   if (strcmp(method, "api.status") == 0)
      return marshal_no_args(method);
   if (strcmp(method, "api.enable") == 0)
      return marshal_api_enable(argc, argv);
   if (strcmp(method, "api.disable") == 0)
      return marshal_no_args(method);
   if (strcmp(method, "delegate.backend_exec") == 0)
      return marshal_delegate_backend_exec(argc, argv);
   if (strcmp(method, "delegate.backend_list") == 0)
      return marshal_no_args(method);
   if (strcmp(method, "memory.search") == 0)
      return marshal_memory_search(argc, argv);
   if (strcmp(method, "memory.store") == 0)
      return marshal_memory_store(argc, argv);
   if (strcmp(method, "memory.list") == 0)
      return marshal_memory_list(argc, argv);
   if (strcmp(method, "memory.get") == 0)
      return marshal_memory_get(argc, argv);
   if (strcmp(method, "memory.read") == 0)
      return marshal_memory_read(argc, argv);
   if (strcmp(method, "memory.stats") == 0)
      return marshal_no_args(method);
   if (strcmp(method, "memory.benchmark") == 0)
      return marshal_memory_benchmark(argc, argv);
   if (strcmp(method, "index.scan") == 0)
      return marshal_index_scan(argc, argv);
   if (strcmp(method, "index.find") == 0)
      return marshal_index_find(argc, argv);
   if (strcmp(method, "index.list") == 0)
      return marshal_index_list(argc, argv);
   if (strcmp(method, "index.blast_radius") == 0)
      return marshal_index_blast_radius(argc, argv);
   if (strcmp(method, "index.structure") == 0)
      return marshal_index_structure(argc, argv);
   if (strcmp(method, "index.find_callers") == 0)
      return marshal_index_find_callers(argc, argv);
   if (strcmp(method, "graph.sync_code") == 0)
      return marshal_graph_sync_code(argc, argv);
   if (strcmp(method, "graph.explain") == 0)
      return marshal_graph_explain(argc, argv);
   if (strcmp(method, "curator.implements") == 0)
      return marshal_curator_topic("curator.implements", argc, argv);
   if (strcmp(method, "curator.synthesize") == 0)
      return marshal_curator_topic("curator.synthesize", argc, argv);
   if (strcmp(method, "curator.contradictions") == 0)
      return marshal_curator_contradictions(argc, argv);
   if (strcmp(method, "workspace.add") == 0)
      return marshal_workspace_add(argc, argv);
   if (strcmp(method, "workspace.remove") == 0 || strcmp(method, "workspace.get") == 0)
      return marshal_agent_args(method, argc, argv);
   if (strcmp(method, "workspace.mirror-sync") == 0)
      return marshal_workspace_mirror_sync(argc, argv);
   if (strcmp(method, "workspace.list") == 0)
      return marshal_no_args(method);
   if (strcmp(method, "session.list") == 0)
      return marshal_session_list(argc, argv);
   if (strcmp(method, "session.get") == 0)
      return marshal_session_get(argc, argv);
   if (strcmp(method, "session.close") == 0)
      return marshal_session_close(argc, argv);
   if (strcmp(method, "session.brief") == 0)
      return marshal_session_brief(argc, argv);
   if (strcmp(method, "session.attach") == 0)
      return marshal_session_attach(argc, argv);
   if (strcmp(method, "session.detach") == 0)
      return marshal_session_detach(argc, argv);
   if (strcmp(method, "session.presence") == 0)
      return marshal_session_presence(argc, argv);
   if (strcmp(method, "trajectory.export") == 0)
      return marshal_trajectory_export(argc, argv);
   if (strcmp(method, "trajectory.batch") == 0)
      return marshal_trajectory_batch(argc, argv);
   if (strcmp(method, "rules.delete") == 0)
      return marshal_rules_delete(argc, argv);
   if (strncmp(method, "skill.", 6) == 0)
      return marshal_skill_request(method, argc, argv);
   if (strcmp(method, "rules.list") == 0 || strcmp(method, "rules.generate") == 0 ||
       strcmp(method, "server.health") == 0 || strcmp(method, "hud.status") == 0 ||
       strcmp(method, "provider.get") == 0)
      return marshal_no_args(method);
   if (strcmp(method, "provider.set") == 0)
      return marshal_provider_set(argc, argv);
   if (strcmp(method, "provider.list") == 0)
      return marshal_provider_list(argc, argv);
   if (strcmp(method, "provider.show") == 0)
      return marshal_provider_name_method(method, argc, argv);
   if (strcmp(method, "provider.models") == 0)
      return marshal_provider_models(argc, argv);
   if (strcmp(method, "provider.test") == 0)
      return marshal_provider_name_method(method, argc, argv);
   if (strcmp(method, "provider.quota") == 0)
      return marshal_provider_name_method(method, argc, argv);
   if (strcmp(method, "model.list") == 0)
      return marshal_model_list(argc, argv);
   if (strcmp(method, "model.show") == 0)
      return marshal_model_show(argc, argv);
   if (strcmp(method, "model.refresh") == 0)
      return marshal_no_args(method);
   if (strcmp(method, "wm.set") == 0)
      return marshal_wm_set(argc, argv);
   if (strcmp(method, "wm.get") == 0)
      return marshal_wm_get(argc, argv);
   if (strcmp(method, "wm.list") == 0)
      return marshal_wm_list(argc, argv);
   if (strcmp(method, "primary.set") == 0)
      return marshal_primary(argc, argv);
   if (strcmp(method, "kb.search") == 0)
      return marshal_kb_search(argc, argv);
   if (strcmp(method, "kb.build") == 0)
      return marshal_kb_build(argc, argv);
   if (strcmp(method, "kb.update") == 0)
      return marshal_kb_update(argc, argv);
   if (strcmp(method, "kb.ingest") == 0)
      return marshal_kb_ingest(argc, argv);
   if (strcmp(method, "kb.docs.push") == 0)
      return marshal_kb_docs_push(argc, argv);
   if (strcmp(method, "kb.ingest.status") == 0 || strcmp(method, "workers") == 0)
      return marshal_no_args(method);
   if (strcmp(method, "kb.status") == 0)
      return marshal_kb_status(argc, argv);
   if (strcmp(method, "insights.overview") == 0)
      return marshal_insights_overview(argc, argv);
   if (strcmp(method, "worktree.gc") == 0)
      return marshal_worktree_gc(argc, argv);
   if (strncmp(method, "work.", 5) == 0)
      return marshal_work_request(method, argc, argv);
   if (strcmp(method, "delegate") == 0)
      return marshal_delegate(argc, argv);
   if (strcmp(method, "delegate.launch") == 0)
      return marshal_delegate_launch(argc, argv);
   if (strcmp(method, "delegate.status") == 0)
      return marshal_delegate_status(argc, argv);
   if (strcmp(method, "jobs.list") == 0)
      return marshal_jobs_list(argc, argv);
   if (strcmp(method, "jobs.status") == 0 || strcmp(method, "jobs.logs") == 0 ||
       strcmp(method, "jobs.cancel") == 0)
      return marshal_job_id_request(method, argc, argv);
   if (strcmp(method, "job.start") == 0)
      return marshal_coord_job_start(argc, argv);
   if (strcmp(method, "job.list") == 0)
      return marshal_coord_jobs_list(argc, argv);
   if (strcmp(method, "job.status") == 0 || strcmp(method, "job.cancel") == 0)
      return marshal_job_id_request(method, argc, argv);
   if (strcmp(method, "aux.config_show") == 0)
      return marshal_no_args(method);
   if (strcmp(method, "config.show") == 0)
      return marshal_no_args(method);
   if (strcmp(method, "config.get") == 0)
      return marshal_config_get(argc, argv);
   if (strcmp(method, "config.set") == 0)
      return marshal_config_set(argc, argv);
   if (strcmp(method, "aux.test") == 0)
      return marshal_aux_test(argc, argv);
   if (strcmp(method, "delegate.log") == 0)
      return marshal_delegate_log(argc, argv);
   if (strcmp(method, "episode.list") == 0)
      return marshal_no_args(method);
   if (strcmp(method, "agent.episodes") == 0)
      return marshal_agent_episodes(argc, argv);
   if (strcmp(method, "trigger.list") == 0)
      return marshal_trigger_list(argc, argv);
   if (strcmp(method, "trigger.status") == 0 || strcmp(method, "trigger.cancel") == 0)
      return marshal_trigger_id(method, argc, argv);
   if (strcmp(method, "trigger.fire") == 0)
      return marshal_trigger_fire(argc, argv);
   if (strcmp(method, "cron.list") == 0)
      return marshal_no_args(method);
   if (strcmp(method, "cron.add") == 0)
      return marshal_cron_add(argc, argv);
   if (strcmp(method, "cron.show") == 0 || strcmp(method, "cron.history") == 0 ||
       strcmp(method, "cron.run") == 0 || strcmp(method, "cron.enable") == 0 ||
       strcmp(method, "cron.disable") == 0 || strcmp(method, "cron.remove") == 0)
      return marshal_cron_id(method, argc, argv);
   if (strncmp(method, "agent.", 6) == 0)
      return marshal_agent_args(method, argc, argv);
   if (strcmp(method, "mcp.audit") == 0)
      return marshal_no_args(method);
   if (strcmp(method, "mcp.recheck") == 0)
      return marshal_mcp_recheck(argc, argv);
   if (strcmp(method, "toolset.list") == 0)
      return marshal_no_args(method);
   if (strcmp(method, "toolset.show") == 0 || strcmp(method, "toolset.resolve") == 0)
   {
      if (argc < 1)
         return NULL;
      cJSON *req = marshal_no_args(method);
      cJSON_AddStringToObject(req, "name", argv[0]);
      return req;
   }
   if (strcmp(method, "get_help") == 0)
      return marshal_get_help(argc, argv);
   if (strcmp(method, "git.verify") == 0)
      return marshal_git_verify(argc, argv);
   if (strcmp(method, "migrate.v2") == 0)
      return marshal_storage_migrate(argc, argv);
   if (strcmp(method, "dogfood.tag") == 0)
      return marshal_dogfood_tag(argc, argv);
   if (strcmp(method, "dogfood.review") == 0)
      return marshal_dogfood_review(argc, argv);
   if (strcmp(method, "dogfood.report") == 0)
      return marshal_dogfood_report(argc, argv);
   if (strcmp(method, "eval.run") == 0)
      return marshal_eval_run(argc, argv);
   if (strcmp(method, "eval.results") == 0)
      return marshal_eval_results(argc, argv);
   if (strcmp(method, "identity.show") == 0)
      return marshal_no_args(method);
   if (strcmp(method, "identity.snapshot") == 0)
      return marshal_identity_snapshot(argc, argv);
   if (strcmp(method, "identity.diff") == 0)
      return marshal_identity_diff(argc, argv);
   return NULL;
}

/* --- Non-JSON output formatting --- */

static void print_mcp_content(cJSON *resp)
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
   return strncmp(text, "FAIL:", 5) == 0 || strncmp(text, "error:", 6) == 0 ||
          strncmp(text, "verify busy:", 12) == 0 || strstr(text, ": FAIL (exit ") != NULL ||
          strstr(text, " step(s) failed -- verified with failures") != NULL;
}

static int git_verify_response_is_failure(cJSON *resp)
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

static void print_server_health(cJSON *resp)
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
}

static const char *json_str(cJSON *obj, const char *key)
{
   cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
   return cJSON_IsString(v) ? v->valuestring : "";
}

static int json_int(cJSON *obj, const char *key, int def)
{
   cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
   return cJSON_IsNumber(v) ? v->valueint : def;
}

static double json_double(cJSON *obj, const char *key, double def)
{
   cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
   return cJSON_IsNumber(v) ? v->valuedouble : def;
}

static void print_jobs_list(cJSON *resp)
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

static void print_mcp_audit(cJSON *resp)
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

static void print_mcp_recheck(cJSON *resp)
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

static void print_jobs_status(cJSON *resp)
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

static void print_jobs_logs(cJSON *resp)
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

static void print_jobs_cancel(cJSON *resp)
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

static void print_toolset_list(cJSON *resp)
{
   cJSON *sets = cJSON_GetObjectItemCaseSensitive(resp, "toolsets");
   cJSON *item = NULL;
   cJSON_ArrayForEach(item, sets)
   {
      printf("%-16s %3d%s\n", json_str(item, "name"), json_int(item, "count", 0),
             cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(item, "builtin")) ? "  built-in" : "");
   }
}

static void print_toolset_show(cJSON *resp)
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

static void print_toolset_resolve(cJSON *resp)
{
   cJSON *tools = cJSON_GetObjectItemCaseSensitive(resp, "tools");
   cJSON *item = NULL;
   cJSON_ArrayForEach(item, tools)
   {
      if (cJSON_IsString(item))
         printf("%s\n", item->valuestring);
   }
}

static void print_coord_jobs_list(cJSON *resp)
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

static void print_coord_job_status(cJSON *resp)
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

static void print_coord_job_cancel(cJSON *resp)
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

static void print_coord_job_start(cJSON *resp)
{
   int job_id = json_int(resp, "job_id", 0);
   printf("Queued coord job #%d from plan #%d: %d tasks, max %d concurrent\n", job_id,
          json_int(resp, "plan_id", 0), json_int(resp, "tasks", 0),
          json_int(resp, "max_concurrent", 0));
   printf("Inspect packet progress with: aimee job status %d\n", job_id);
}

static void print_aux_config(cJSON *resp)
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

static void print_session_list(cJSON *resp)
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

static void print_session_get(cJSON *resp)
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

static void print_session_brief(cJSON *resp)
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

static void print_insights_overview(cJSON *resp)
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

static void print_worktree_gc(cJSON *resp)
{
   if (!resp)
      return;
   const char *git_root = json_str(resp, "git_root");
   int dry_run = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(resp, "dry_run"));
   int force = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(resp, "force"));
   cJSON *jdays = cJSON_GetObjectItemCaseSensitive(resp, "max_age_days");
   int days = cJSON_IsNumber(jdays) ? (int)jdays->valuedouble : 0;
   cJSON *jscan = cJSON_GetObjectItemCaseSensitive(resp, "scanned");
   int scanned = cJSON_IsNumber(jscan) ? (int)jscan->valuedouble : 0;
   cJSON *jrem = cJSON_GetObjectItemCaseSensitive(resp, "removed");
   int removed = cJSON_IsNumber(jrem) ? (int)jrem->valuedouble : 0;

   printf("Worktree GC — %s\n", git_root && git_root[0] ? git_root : "(unknown)");
   printf("  threshold: idle %dd%s%s\n", days, force ? ", --force" : "",
          dry_run ? ", --dry-run" : "");
   printf("  scanned:   %d worktree(s)\n", scanned);
   printf("  %s:   %d\n", dry_run ? "would remove" : "removed", removed);

   cJSON *cands = cJSON_GetObjectItemCaseSensitive(resp, "candidates");
   if (!cJSON_IsArray(cands) || cJSON_GetArraySize(cands) == 0)
   {
      printf("\n  No candidates.\n");
      return;
   }
   printf("\n  Candidates:\n");
   cJSON *c;
   cJSON_ArrayForEach(c, cands)
   {
      const char *path = json_str(c, "path");
      const char *branch = json_str(c, "branch");
      const char *reason = json_str(c, "reason");
      int eligible = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(c, "eligible"));
      printf("    [%s] %s\n", eligible ? "x" : " ", path && path[0] ? path : "(unknown)");
      if (branch && branch[0])
         printf("        branch: %s\n", branch);
      if (reason && reason[0])
         printf("        %s\n", reason);
   }
}

static void print_work_item_line(cJSON *item)
{
   const char *id = json_str(item, "id");
   const char *title = json_str(item, "title");
   const char *source = json_str(item, "source");
   const char *status = json_str(item, "status");
   const char *claimed = json_str(item, "claimed_by");
   const char *claimed_at = json_str(item, "claimed_at");
   const char *result = json_str(item, "result");

   printf("[%s] %-10s %s", id[0] ? id : "?", status[0] ? status : "?", title[0] ? title : "?");
   if (claimed[0])
      printf("  (session: %.8s)", claimed);
   if (claimed_at[0] && status[0] && strcmp(status, "claimed") == 0)
      printf("  (%s)", claimed_at);
   if (source[0])
      printf("  [%s]", source);
   if (result[0])
      printf("  -> %s", result);
   printf("\n");
}

static void print_work_list(cJSON *resp)
{
   cJSON *items = cJSON_GetObjectItemCaseSensitive(resp, "items");
   if (!cJSON_IsArray(items) || cJSON_GetArraySize(items) == 0)
   {
      printf("No work items found.\n");
      return;
   }

   cJSON *item;
   cJSON_ArrayForEach(item, items) print_work_item_line(item);
}

static void format_work_board_age(cJSON *item, char *buf, size_t cap)
{
   if (!buf || cap == 0)
      return;
   buf[0] = '\0';

   cJSON *age = cJSON_GetObjectItemCaseSensitive(item, "age_minutes");
   if (!cJSON_IsNumber(age))
      return;

   int age_min = (int)age->valuedouble;
   if (age_min < 60)
      snprintf(buf, cap, "%dm", age_min);
   else if (age_min < 24 * 60)
      snprintf(buf, cap, "%dh%dm", age_min / 60, age_min % 60);
   else
      snprintf(buf, cap, "%dd%dh", age_min / (24 * 60), (age_min / 60) % 24);
}

static void print_work_board(cJSON *resp)
{
   static const char *statuses[] = {"pending", "claimed", "done", "failed", "cancelled"};
   cJSON *board = cJSON_GetObjectItemCaseSensitive(resp, "board");
   if (!cJSON_IsObject(board))
   {
      printf("No work items found.\n");
      return;
   }

   printf("Work board\n");
   int total = 0;
   for (int s = 0; s < 5; s++)
   {
      cJSON *items = cJSON_GetObjectItemCaseSensitive(board, statuses[s]);
      int count = cJSON_IsArray(items) ? cJSON_GetArraySize(items) : 0;
      total += count;
      printf("\n%s (%d)\n", statuses[s], count);
      if (count == 0)
      {
         printf("  (empty)\n");
         continue;
      }

      cJSON *item;
      cJSON_ArrayForEach(item, items)
      {
         char age[32];
         format_work_board_age(item, age, sizeof(age));
         printf("  [%s] %s", json_str(item, "id"), json_str(item, "title"));
         if (age[0])
            printf("  age:%s", age);
         if (json_str(item, "claimed_by")[0])
            printf("  owner:%.8s", json_str(item, "claimed_by"));
         if (json_str(item, "source")[0])
            printf("  source:%s", json_str(item, "source"));
         if (json_str(item, "result")[0])
            printf("  result:%s", json_str(item, "result"));
         printf("\n");
      }
   }

   if (total == 0)
      printf("\nNo work items found.\n");
}

static void print_work_claim(cJSON *resp)
{
   if (!cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(resp, "claimed")))
   {
      const char *msg = json_str(resp, "message");
      printf("%s\n", msg[0] ? msg : "No pending work items to claim.");
      return;
   }

   printf("Claimed: [%s] %s\n", json_str(resp, "id"), json_str(resp, "title"));
   const char *source = json_str(resp, "source");
   const char *desc = json_str(resp, "description");
   if (source[0])
      printf("  Source: %s\n", source);
   if (desc[0])
      printf("  Description: %s\n", desc);
}

static void print_work_finish(cJSON *resp, const char *status_name)
{
   if (!cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(resp, "updated")))
   {
      const char *msg = json_str(resp, "message");
      printf("%s\n", msg[0] ? msg : "No matching claimed work item found for this session.");
      return;
   }

   printf("Marked %s: [%s] %s\n", status_name, json_str(resp, "id"), json_str(resp, "title"));
}

static void print_work_simple_item(cJSON *resp, const char *field, const char *verb,
                                   const char *missing)
{
   if (!cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(resp, field)))
   {
      const char *msg = json_str(resp, "message");
      printf("%s\n", msg[0] ? msg : missing);
      return;
   }
   printf("%s: [%s] %s\n", verb, json_str(resp, "id"), json_str(resp, "title"));
}

static void print_work_stats(cJSON *resp)
{
   int total = (int)cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(resp, "total"));
   int pending = (int)cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(resp, "pending"));
   int claimed = (int)cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(resp, "claimed"));
   int done = (int)cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(resp, "done"));
   int failed = (int)cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(resp, "failed"));
   int cancelled = (int)cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(resp, "cancelled"));
   int timed =
       (int)cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(resp, "completed_with_timing"));
   double avg =
       cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(resp, "avg_completion_minutes"));

   printf("Queue stats:\n");
   printf("  Total items: %d\n", total);
   printf("  Pending:     %d\n", pending);
   printf("  Claimed:     %d\n", claimed);
   printf("  Completed:   %d", done);
   if (timed > 0)
      printf(" (avg %.0fm)", avg);
   printf("\n");
   printf("  Failed:      %d\n", failed);
   printf("  Cancelled:   %d\n", cancelled);
}

static void print_work_gc(cJSON *resp)
{
   cJSON *released_items = cJSON_GetObjectItemCaseSensitive(resp, "released_items");
   if (cJSON_IsArray(released_items))
   {
      cJSON *item;
      cJSON_ArrayForEach(item, released_items)
          printf("Released stale claim: [%s] %s\n", json_str(item, "id"), json_str(item, "title"));
   }

   int released = (int)cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(resp, "released"));
   int closed = (int)cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(resp, "closed"));
   int cancelled = (int)cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(resp, "cancelled"));
   int hours = (int)cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(resp, "max_age_hours"));

   if (released == 0 && closed == 0 && cancelled == 0)
      printf("No stale claims found (threshold: %d hours).\n", hours);
   else
   {
      if (released > 0)
         printf("Released %d stale claims.\n", released);
      if (closed > 0 || cancelled > 0)
         printf("Synced proposals: %d closed as done, %d cancelled.\n", closed, cancelled);
   }
}

static void print_work_sync_proposals(cJSON *resp)
{
   int closed = (int)cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(resp, "closed"));
   int cancelled = (int)cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(resp, "cancelled"));
   if (closed == 0 && cancelled == 0)
      printf("No work items to sync - all sources are still in pending/.\n");
   else
      printf("Synced work queue: %d closed as done, %d cancelled.\n", closed, cancelled);
}

static void print_session_close(cJSON *resp)
{
   cJSON *sid = cJSON_GetObjectItemCaseSensitive(resp, "session_id");
   if (cJSON_IsString(sid) && sid->valuestring[0])
      printf("closed session %s\n", sid->valuestring);
   else
      printf("closed session\n");
}

static void print_rules_delete(cJSON *resp)
{
   cJSON *id = cJSON_GetObjectItemCaseSensitive(resp, "id");
   if (cJSON_IsNumber(id))
      printf("deleted rule %d\n", (int)id->valuedouble);
   else
      printf("deleted rule\n");
}

static void print_agent_list(cJSON *resp)
{
   cJSON *agents = cJSON_GetObjectItemCaseSensitive(resp, "agents");
   if (!cJSON_IsArray(agents) || cJSON_GetArraySize(agents) == 0)
   {
      printf("No agents configured.\n");
      return;
   }
   cJSON *ag;
   cJSON_ArrayForEach(ag, agents)
   {
      cJSON *name = cJSON_GetObjectItemCaseSensitive(ag, "name");
      cJSON *enabled = cJSON_GetObjectItemCaseSensitive(ag, "enabled");
      cJSON *tier = cJSON_GetObjectItemCaseSensitive(ag, "cost_tier");
      cJSON *parallel = cJSON_GetObjectItemCaseSensitive(ag, "max_parallel");
      cJSON *model = cJSON_GetObjectItemCaseSensitive(ag, "model");
      cJSON *endpoint = cJSON_GetObjectItemCaseSensitive(ag, "endpoint");
      cJSON *tools = cJSON_GetObjectItemCaseSensitive(ag, "tools_enabled");
      printf("%-16s %-6s tier=%d parallel=%d model=%s endpoint=%s%s\n",
             cJSON_IsString(name) ? name->valuestring : "?", cJSON_IsTrue(enabled) ? "ON" : "OFF",
             cJSON_IsNumber(tier) ? tier->valueint : 0,
             cJSON_IsNumber(parallel) ? parallel->valueint : 0,
             cJSON_IsString(model) ? model->valuestring : "",
             cJSON_IsString(endpoint) ? endpoint->valuestring : "",
             cJSON_IsTrue(tools) ? " [tools]" : "");
   }
}

static void print_agent_local(cJSON *resp)
{
   printf("Local delegate '%s' registered: %s @ %s\n", json_str(resp, "name"),
          json_str(resp, "model"), json_str(resp, "endpoint"));
   cJSON *parallel = cJSON_GetObjectItemCaseSensitive(resp, "max_parallel");
   cJSON *ctx = cJSON_GetObjectItemCaseSensitive(resp, "context_window");
   cJSON *tools = cJSON_GetObjectItemCaseSensitive(resp, "tools_enabled");
   printf("  max_parallel=%d", cJSON_IsNumber(parallel) ? parallel->valueint : 0);
   if (cJSON_IsNumber(ctx))
      printf(" context_window=%d", ctx->valueint);
   printf(" tools=%s\n", cJSON_IsTrue(tools) ? "on" : "off");
   cJSON *mp = cJSON_GetObjectItemCaseSensitive(resp, "model_probe");
   cJSON *sp = cJSON_GetObjectItemCaseSensitive(resp, "slot_probe");
   if (cJSON_IsString(mp) && mp->valuestring[0])
      printf("  warning: %s\n", mp->valuestring);
   if (cJSON_IsString(sp) && sp->valuestring[0])
      printf("  warning: %s\n", sp->valuestring);
}

static void print_agent_add(cJSON *resp)
{
   printf("Delegate '%s' saved: %s @ %s\n", json_str(resp, "name"), json_str(resp, "model"),
          json_str(resp, "endpoint"));
}

static void print_agent_remove(cJSON *resp)
{
   printf("Delegate '%s' removed.\n", json_str(resp, "name"));
}

static void print_agent_enabled(cJSON *resp, int enabled)
{
   printf("Delegate '%s' %s.\n", json_str(resp, "name"), enabled ? "enabled" : "disabled");
}

static void print_agent_probe(cJSON *resp)
{
   printf("%s\n", json_str(resp, "name"));
   cJSON *models_status = cJSON_GetObjectItemCaseSensitive(resp, "models_status");
   cJSON *model_available = cJSON_GetObjectItemCaseSensitive(resp, "model_available");
   cJSON *slots_status = cJSON_GetObjectItemCaseSensitive(resp, "slots_status");
   cJSON *slots = cJSON_GetObjectItemCaseSensitive(resp, "detected_slots");
   cJSON *ctx = cJSON_GetObjectItemCaseSensitive(resp, "detected_context_window");
   cJSON *slots_source = cJSON_GetObjectItemCaseSensitive(resp, "slots_source");
   printf("  models: %s (%d)\n", cJSON_IsTrue(model_available) ? "ok" : "warn",
          cJSON_IsNumber(models_status) ? models_status->valueint : 0);
   cJSON *mp = cJSON_GetObjectItemCaseSensitive(resp, "model_probe");
   if (cJSON_IsString(mp) && mp->valuestring[0])
      printf("  model_probe: %s\n", mp->valuestring);
   printf("  slots: %d", cJSON_IsNumber(slots) ? slots->valueint : 0);
   if (cJSON_IsNumber(ctx))
      printf(" context_window=%d", ctx->valueint);
   if (cJSON_IsString(slots_source) && strcmp(slots_source->valuestring, "config") == 0)
      printf(" (configured)\n");
   else
      printf(" (%d)\n", cJSON_IsNumber(slots_status) ? slots_status->valueint : 0);
   cJSON *sp = cJSON_GetObjectItemCaseSensitive(resp, "slot_probe");
   if (cJSON_IsString(sp) && sp->valuestring[0])
      printf("  slot_probe: %s\n", sp->valuestring);
   cJSON *run = cJSON_GetObjectItemCaseSensitive(resp, "execution_ok");
   if (run)
   {
      cJSON *lat = cJSON_GetObjectItemCaseSensitive(resp, "latency_ms");
      printf("  execution: %s latency=%dms %s\n", cJSON_IsTrue(run) ? "ok" : "failed",
             cJSON_IsNumber(lat) ? lat->valueint : 0, json_str(resp, "execution_message"));
   }
}

static void print_trigger_list(cJSON *resp)
{
   cJSON *triggers = cJSON_GetObjectItemCaseSensitive(resp, "triggers");
   if (!cJSON_IsArray(triggers) || cJSON_GetArraySize(triggers) == 0)
   {
      printf("No trigger runs found.\n");
      return;
   }

   printf("%-16s %-16s %-10s %-40s %s\n", "ID", "SOURCE", "STATUS", "TASK", "QUEUED_AT");
   printf("%-16s %-16s %-10s %-40s %s\n", "--", "------", "------", "----", "---------");
   cJSON *t;
   cJSON_ArrayForEach(t, triggers)
   {
      const char *task = json_str(t, "task");
      char task_buf[41];
      if (strlen(task) > 40)
      {
         memcpy(task_buf, task, 37);
         task_buf[37] = '.';
         task_buf[38] = '.';
         task_buf[39] = '.';
         task_buf[40] = '\0';
         task = task_buf;
      }
      printf("%-16s %-16s %-10s %-40s %s\n", json_str(t, "id"), json_str(t, "source"),
             json_str(t, "status"), task, json_str(t, "queued_at"));
   }
}

static cJSON *trigger_object(cJSON *resp)
{
   cJSON *trigger = cJSON_GetObjectItemCaseSensitive(resp, "trigger");
   return cJSON_IsObject(trigger) ? trigger : resp;
}

static void print_trigger_status(cJSON *resp)
{
   cJSON *t = trigger_object(resp);
   printf("id:          %s\n", json_str(t, "id"));
   printf("source:      %s\n", json_str(t, "source"));
   printf("event:       %s\n", json_str(t, "event"));
   printf("status:      %s\n", json_str(t, "status"));
   printf("task:        %s\n", json_str(t, "task"));
   printf("workspace:   %s\n", json_str(t, "workspace"));
   printf("pipeline_id: %s\n", json_str(t, "pipeline_id"));
   printf("queued_at:   %s\n", json_str(t, "queued_at"));
   printf("started_at:  %s\n", json_str(t, "started_at"));
   printf("finished_at: %s\n", json_str(t, "finished_at"));
   if (json_str(t, "error")[0])
      printf("error:       %s\n", json_str(t, "error"));
}

static void print_trigger_fire(cJSON *resp)
{
   printf("trigger_id: %s\n", json_str(resp, "trigger_id"));
   if (json_str(resp, "pipeline_id")[0])
      printf("pipeline_id: %s\n", json_str(resp, "pipeline_id"));
   printf("status: %s\n", json_str(resp, "trigger_status")[0] ? json_str(resp, "trigger_status")
                                                              : json_str(resp, "status"));
}

static void print_trigger_cancel(cJSON *resp)
{
   const char *id = json_str(resp, "trigger_id");
   if (id[0])
      printf("cancelled trigger %s\n", id);
   else
      printf("cancelled trigger\n");
}

static void print_cron_list(cJSON *resp)
{
   cJSON *jobs = cJSON_GetObjectItemCaseSensitive(resp, "jobs");
   if (!cJSON_IsArray(jobs) || cJSON_GetArraySize(jobs) == 0)
   {
      printf("No cron jobs configured.\n");
      return;
   }

   printf("%-24s %-12s %-14s %-8s %-18s %s\n", "ID", "MODE", "SCHEDULE", "ENABLED", "LAST_STATUS",
          "TARGET");
   cJSON *job;
   cJSON_ArrayForEach(job, jobs)
   {
      cJSON *enabled = cJSON_GetObjectItemCaseSensitive(job, "enabled");
      printf("%-24s %-12s %-14s %-8s %-18s %s\n", json_str(job, "id"), json_str(job, "mode"),
             json_str(job, "schedule"), cJSON_IsTrue(enabled) ? "yes" : "no",
             json_str(job, "last_run_status"), json_str(job, "deliver_target"));
   }
}

static void print_cron_show(cJSON *resp)
{
   cJSON *job = cJSON_GetObjectItemCaseSensitive(resp, "job");
   if (!cJSON_IsObject(job))
      job = resp;
   printf("id:                    %s\n", json_str(job, "id"));
   printf("schedule:              %s\n", json_str(job, "schedule"));
   printf("mode:                  %s\n", json_str(job, "mode"));
   printf("enabled:               %s\n",
          cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(job, "enabled")) ? "true" : "false");
   printf("workdir:               %s\n", json_str(job, "workdir"));
   printf("deliver_target:        %s\n", json_str(job, "deliver_target"));
   printf("skills:                %s\n", json_str(job, "skills_csv"));
   printf("context_from:          %s\n", json_str(job, "context_from"));
   printf("when_context_contains: %s\n", json_str(job, "when_context_contains"));
}

static void print_cron_history(cJSON *resp)
{
   cJSON *runs = cJSON_GetObjectItemCaseSensitive(resp, "runs");
   if (!cJSON_IsArray(runs) || cJSON_GetArraySize(runs) == 0)
   {
      printf("No cron job runs found.\n");
      return;
   }

   printf("%-6s %-24s %-10s %-7s %-9s %s\n", "ID", "JOB", "STATUS", "SILENT", "DELIVERED",
          "COMPLETED");
   cJSON *run;
   cJSON_ArrayForEach(run, runs)
   {
      printf("%-6d %-24s %-10s %-7s %-9s %d\n", json_int(run, "id", 0), json_str(run, "job_id"),
             json_str(run, "status"),
             cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(run, "silent")) ? "yes" : "no",
             cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(run, "delivered")) ? "yes" : "no",
             json_int(run, "completed_at", 0));
   }
}

static void print_cron_run(cJSON *resp)
{
   printf("job_id: %s\n", json_str(resp, "job_id"));
   printf("run_id: %d\n", json_int(resp, "run_id", 0));
   printf("status: %s\n", json_str(resp, "run_status"));
   printf("silent: %s\n",
          cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(resp, "silent")) ? "true" : "false");
   if (json_str(resp, "trigger_id")[0])
      printf("trigger_id: %s\n", json_str(resp, "trigger_id"));
   if (json_str(resp, "pipeline_id")[0])
      printf("pipeline_id: %s\n", json_str(resp, "pipeline_id"));
   if (json_str(resp, "error")[0])
      printf("error: %s\n", json_str(resp, "error"));
}

static void print_memory_row(cJSON *m)
{
   if (!cJSON_IsObject(m))
      return;
   cJSON *id = cJSON_GetObjectItemCaseSensitive(m, "id");
   const char *tier = json_str(m, "tier");
   const char *kind = json_str(m, "kind");
   const char *key = json_str(m, "key");
   const char *content = json_str(m, "content");
   printf("%lld  %-3s %-12s %s", cJSON_IsNumber(id) ? (long long)id->valuedouble : 0, tier, kind,
          key[0] ? key : "(no key)");
   if (content[0])
      printf(": %s", content);
   putchar('\n');
}

static void print_memory_search(cJSON *resp)
{
   cJSON *facts = cJSON_GetObjectItemCaseSensitive(resp, "facts");
   cJSON *windows = cJSON_GetObjectItemCaseSensitive(resp, "windows");
   int fact_count = cJSON_IsArray(facts) ? cJSON_GetArraySize(facts) : 0;
   int window_count = cJSON_IsArray(windows) ? cJSON_GetArraySize(windows) : 0;
   if (fact_count == 0 && window_count == 0)
   {
      printf("No memories found.\n");
      return;
   }
   if (fact_count > 0)
   {
      printf("Facts:\n");
      cJSON *m;
      cJSON_ArrayForEach(m, facts)
      {
         printf("  ");
         print_memory_row(m);
      }
   }
   if (window_count > 0)
   {
      printf("Conversation windows:\n");
      cJSON *w;
      cJSON_ArrayForEach(w, windows)
      {
         cJSON *score = cJSON_GetObjectItemCaseSensitive(w, "score");
         printf("  %.4f  %s:%lld  %s\n", cJSON_IsNumber(score) ? score->valuedouble : 0.0,
                json_str(w, "session_id"),
                (long long)cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(w, "seq")),
                json_str(w, "summary"));
      }
   }
}

static void print_memory_list(cJSON *resp)
{
   cJSON *memories = cJSON_GetObjectItemCaseSensitive(resp, "memories");
   if (!cJSON_IsArray(memories) || cJSON_GetArraySize(memories) == 0)
   {
      printf("No memories.\n");
      return;
   }
   cJSON *m;
   cJSON_ArrayForEach(m, memories)
   {
      print_memory_row(m);
   }
}

static void print_memory_stats(cJSON *resp)
{
   cJSON *stats = cJSON_GetObjectItemCaseSensitive(resp, "stats");
   if (!cJSON_IsObject(stats))
   {
      printf("No memory statistics available.\n");
      return;
   }
   cJSON *total = cJSON_GetObjectItemCaseSensitive(stats, "total");
   cJSON *conflicts = cJSON_GetObjectItemCaseSensitive(stats, "conflicts");
   printf("Memory statistics:\n");
   printf("  total:     %lld\n", (long long)(cJSON_IsNumber(total) ? total->valuedouble : 0));
   printf("  conflicts: %lld\n",
          (long long)(cJSON_IsNumber(conflicts) ? conflicts->valuedouble : 0));
   cJSON *tiers = cJSON_GetObjectItemCaseSensitive(stats, "tier_counts");
   if (cJSON_IsArray(tiers) && cJSON_GetArraySize(tiers) > 0)
   {
      printf("  tier counts:");
      cJSON *t;
      cJSON_ArrayForEach(t, tiers)
          printf(" %lld", (long long)(cJSON_IsNumber(t) ? t->valuedouble : 0));
      printf("\n");
   }
   cJSON *kinds = cJSON_GetObjectItemCaseSensitive(stats, "kind_counts");
   if (cJSON_IsArray(kinds) && cJSON_GetArraySize(kinds) > 0)
   {
      printf("  kind counts:");
      cJSON *k;
      cJSON_ArrayForEach(k, kinds)
          printf(" %lld", (long long)(cJSON_IsNumber(k) ? k->valuedouble : 0));
      printf("\n");
   }
}

static void print_index_scan(cJSON *resp)
{
   cJSON *skipped = cJSON_GetObjectItemCaseSensitive(resp, "skipped");
   if (cJSON_IsTrue(skipped))
   {
      const char *reason = json_str(resp, "reason");
      cJSON *retry = cJSON_GetObjectItemCaseSensitive(resp, "retry_after");
      if (strcmp(reason, "cooldown") == 0 && cJSON_IsNumber(retry))
         printf("==> Scan skipped: cooldown active (%lds remaining)\n", (long)retry->valuedouble);
      else if (strcmp(reason, "busy") == 0)
         printf("==> Scan skipped: another scan is already running\n");
      else
         printf("==> Scan skipped: %s\n", reason[0] ? reason : "no reason given");
      return;
   }

   int projects = (int)cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(resp, "projects"));
   int files = (int)cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(resp, "files"));
   int inspected = (int)cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(resp, "inspected"));
   int unchanged = inspected - files;
   if (inspected > 0 && unchanged > 0)
      printf("==> Scan complete: %d project(s), %d file(s) re-indexed (%d unchanged)\n", projects,
             files, unchanged);
   else
      printf("==> Scan complete: %d project(s), %d file(s) re-indexed\n", projects, files);
}

static void print_index_list(cJSON *resp)
{
   cJSON *projects = cJSON_GetObjectItemCaseSensitive(resp, "projects");
   if (!cJSON_IsArray(projects) || cJSON_GetArraySize(projects) == 0)
   {
      printf("No indexed projects.\n");
      return;
   }
   printf("%-30s %s\n", "PROJECT", "ROOT");
   printf("%-30s %s\n", "-------", "----");
   cJSON *p;
   cJSON_ArrayForEach(p, projects)
   {
      printf("%-30s %s\n", json_str(p, "name"), json_str(p, "root"));
   }
}

static void print_index_find(cJSON *resp)
{
   cJSON *hits = cJSON_GetObjectItemCaseSensitive(resp, "hits");
   if (!cJSON_IsArray(hits) || cJSON_GetArraySize(hits) == 0)
   {
      printf("No matches.\n");
      return;
   }
   cJSON *h;
   cJSON_ArrayForEach(h, hits)
   {
      cJSON *line = cJSON_GetObjectItemCaseSensitive(h, "line");
      printf("  %s:%d  %-12s [%s]\n", json_str(h, "file_path"),
             cJSON_IsNumber(line) ? (int)line->valuedouble : 0, json_str(h, "kind"),
             json_str(h, "project"));
   }
}

static void print_index_blast_radius(cJSON *resp)
{
   printf("Blast radius for %s:\n", json_str(resp, "file"));
   cJSON *dependents = cJSON_GetObjectItemCaseSensitive(resp, "dependents");
   cJSON *dependencies = cJSON_GetObjectItemCaseSensitive(resp, "dependencies");
   int shown = 0;
   if (cJSON_IsArray(dependents) && cJSON_GetArraySize(dependents) > 0)
   {
      printf("  Dependents (%d):\n", cJSON_GetArraySize(dependents));
      cJSON *d;
      cJSON_ArrayForEach(d, dependents)
      {
         if (cJSON_IsString(d))
            printf("    %s\n", d->valuestring);
      }
      shown = 1;
   }
   if (cJSON_IsArray(dependencies) && cJSON_GetArraySize(dependencies) > 0)
   {
      printf("  Dependencies (%d):\n", cJSON_GetArraySize(dependencies));
      cJSON *d;
      cJSON_ArrayForEach(d, dependencies)
      {
         if (cJSON_IsString(d))
            printf("    %s\n", d->valuestring);
      }
      shown = 1;
   }
   if (!shown)
      printf("  No dependents or dependencies found.\n");
}

static void print_index_structure(cJSON *resp)
{
   cJSON *defs = cJSON_GetObjectItemCaseSensitive(resp, "definitions");
   if (!cJSON_IsArray(defs) || cJSON_GetArraySize(defs) == 0)
   {
      printf("No definitions found.\n");
      return;
   }
   cJSON *d;
   cJSON_ArrayForEach(d, defs)
   {
      cJSON *line = cJSON_GetObjectItemCaseSensitive(d, "line");
      printf("  %4d  %-12s %s\n", cJSON_IsNumber(line) ? (int)line->valuedouble : 0,
             json_str(d, "kind"), json_str(d, "name"));
   }
}

static void print_index_callers(cJSON *resp)
{
   cJSON *hits = cJSON_GetObjectItemCaseSensitive(resp, "hits");
   if (!cJSON_IsArray(hits) || cJSON_GetArraySize(hits) == 0)
   {
      printf("No callers found.\n");
      return;
   }
   cJSON *h;
   cJSON_ArrayForEach(h, hits)
   {
      cJSON *line = cJSON_GetObjectItemCaseSensitive(h, "line");
      const char *caller = json_str(h, "caller");
      if (caller[0])
         printf("  %s:%d in %s() [%s]\n", json_str(h, "file_path"),
                cJSON_IsNumber(line) ? (int)line->valuedouble : 0, caller, json_str(h, "project"));
      else
         printf("  %s:%d (file scope) [%s]\n", json_str(h, "file_path"),
                cJSON_IsNumber(line) ? (int)line->valuedouble : 0, json_str(h, "project"));
   }
}

static void print_hud_status(cJSON *resp)
{
   cJSON *mode = cJSON_GetObjectItemCaseSensitive(resp, "mode");
   cJSON *total_calls = cJSON_GetObjectItemCaseSensitive(resp, "total_calls");
   cJSON *successful_calls = cJSON_GetObjectItemCaseSensitive(resp, "successful_calls");
   cJSON *failed_calls = cJSON_GetObjectItemCaseSensitive(resp, "failed_calls");
   cJSON *prompt = cJSON_GetObjectItemCaseSensitive(resp, "total_prompt_tokens");
   cJSON *completion = cJSON_GetObjectItemCaseSensitive(resp, "total_completion_tokens");
   cJSON *cache_w = cJSON_GetObjectItemCaseSensitive(resp, "total_cache_write_tokens");
   cJSON *cache_r = cJSON_GetObjectItemCaseSensitive(resp, "total_cache_read_tokens");
   cJSON *cost = cJSON_GetObjectItemCaseSensitive(resp, "total_estimated_cost_usd");
   cJSON *turns = cJSON_GetObjectItemCaseSensitive(resp, "total_turns");
   cJSON *tools = cJSON_GetObjectItemCaseSensitive(resp, "total_tool_calls");
   cJSON *latency = cJSON_GetObjectItemCaseSensitive(resp, "avg_latency_ms");
   cJSON *recent = cJSON_GetObjectItemCaseSensitive(resp, "recent_calls");
   cJSON *recent_ok = cJSON_GetObjectItemCaseSensitive(resp, "recent_successes");

   long long prompt_tokens = cJSON_IsNumber(prompt) ? (long long)prompt->valuedouble : 0;
   long long completion_tokens =
       cJSON_IsNumber(completion) ? (long long)completion->valuedouble : 0;
   long long cache_write = cJSON_IsNumber(cache_w) ? (long long)cache_w->valuedouble : 0;
   long long cache_read = cJSON_IsNumber(cache_r) ? (long long)cache_r->valuedouble : 0;
   const char *mode_value = "unknown";

   if (cJSON_IsString(mode) && mode->valuestring[0])
      mode_value = mode->valuestring;

   printf("Session Status\n");
   printf("  Mode:        %s\n", mode_value);
   printf("  Delegates:   %d total  (%d ok, %d failed)\n",
          cJSON_IsNumber(total_calls) ? (int)total_calls->valuedouble : 0,
          cJSON_IsNumber(successful_calls) ? (int)successful_calls->valuedouble : 0,
          cJSON_IsNumber(failed_calls) ? (int)failed_calls->valuedouble : 0);
   printf("  Tokens:      %lld prompt / %lld completion", prompt_tokens, completion_tokens);
   if (cache_write > 0 || cache_read > 0)
      printf(" | cache: %lldw / %lldr", cache_write, cache_read);
   if (cJSON_IsNumber(cost) && cost->valuedouble > 0.0)
      printf(" | ~$%.4f", cost->valuedouble);
   printf("\n");
   printf("  Turns:       %d  |  Tool calls: %d\n",
          cJSON_IsNumber(turns) ? (int)turns->valuedouble : 0,
          cJSON_IsNumber(tools) ? (int)tools->valuedouble : 0);
   printf("  Avg latency: %.0f ms\n", cJSON_IsNumber(latency) ? latency->valuedouble : 0.0);
   printf("  Recent (5m): %d calls  (%d ok)\n",
          cJSON_IsNumber(recent) ? (int)recent->valuedouble : 0,
          cJSON_IsNumber(recent_ok) ? (int)recent_ok->valuedouble : 0);
}

static void print_init_run(cJSON *resp)
{
   cJSON *local = cJSON_GetObjectItemCaseSensitive(resp, "local_ready");
   cJSON *knowledge = cJSON_GetObjectItemCaseSensitive(resp, "knowledge_ready");
   cJSON *rules_generated = cJSON_GetObjectItemCaseSensitive(resp, "rules_generated");
   cJSON *rules_exists = cJSON_GetObjectItemCaseSensitive(resp, "rules_already_exists");

   printf("Initialized\n");
   printf("  Local:      %s\n", cJSON_IsTrue(local) ? "ready" : "unavailable");
   printf("  Knowledge:  %s\n", cJSON_IsTrue(knowledge) ? "ready" : "not ready");
   if (cJSON_IsTrue(rules_generated))
      printf("  Rules:      generated .aimee-rules\n");
   else if (cJSON_IsTrue(rules_exists))
      printf("  Rules:      existing .aimee-rules preserved\n");

   cJSON *k = cJSON_GetObjectItemCaseSensitive(resp, "knowledge");
   cJSON *msg = cJSON_IsObject(k) ? cJSON_GetObjectItemCaseSensitive(k, "message") : NULL;
   cJSON *rem = cJSON_IsObject(k) ? cJSON_GetObjectItemCaseSensitive(k, "remediation") : NULL;
   if (!cJSON_IsTrue(knowledge) && cJSON_IsString(msg) && msg->valuestring[0])
      printf("  Note:       %s\n", msg->valuestring);
   if (!cJSON_IsTrue(knowledge) && cJSON_IsString(rem) && rem->valuestring[0])
      printf("  Fix:        %s\n", rem->valuestring);
}

static void print_storage_migrate(cJSON *resp)
{
   cJSON *source = cJSON_GetObjectItemCaseSensitive(resp, "source");
   cJSON *backup = cJSON_GetObjectItemCaseSensitive(resp, "backup_path");
   cJSON *tables = cJSON_GetObjectItemCaseSensitive(resp, "tables_found");
   cJSON *source_rows = cJSON_GetObjectItemCaseSensitive(resp, "source_rows");
   cJSON *copied = cJSON_GetObjectItemCaseSensitive(resp, "rows_copied");
   cJSON *skipped = cJSON_GetObjectItemCaseSensitive(resp, "rows_skipped");
   cJSON *table_errors = cJSON_GetObjectItemCaseSensitive(resp, "table_errors");
   cJSON *regressions = cJSON_GetObjectItemCaseSensitive(resp, "row_count_regressions");

   printf("Migration complete\n");
   if (cJSON_IsString(source) && source->valuestring[0])
      printf("  Source:      %s\n", source->valuestring);
   printf("  Tables:      %d\n", cJSON_IsNumber(tables) ? tables->valueint : 0);
   printf("  Source rows: %d\n", cJSON_IsNumber(source_rows) ? source_rows->valueint : 0);
   printf("  Copied:      %d\n", cJSON_IsNumber(copied) ? copied->valueint : 0);
   printf("  Skipped:     %d\n", cJSON_IsNumber(skipped) ? skipped->valueint : 0);
   printf("  Errors:      %d\n", cJSON_IsNumber(table_errors) ? table_errors->valueint : 0);
   printf("  Regressions: %d\n", cJSON_IsNumber(regressions) ? regressions->valueint : 0);
   if (cJSON_IsString(backup) && backup->valuestring[0])
      printf("  Backup:      %s\n", backup->valuestring);
}

static void print_episode_list(cJSON *resp)
{
   cJSON *episodes = cJSON_GetObjectItemCaseSensitive(resp, "episodes");
   if (!cJSON_IsArray(episodes) || cJSON_GetArraySize(episodes) == 0)
   {
      printf("No episodes recorded.\n");
      return;
   }
   cJSON *ep;
   cJSON_ArrayForEach(ep, episodes)
   {
      const char *agent = json_str(ep, "agent");
      const char *role = json_str(ep, "role");
      const char *ts = json_str(ep, "created_at");
      int turns = (int)cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(ep, "turns"));
      int tools = (int)cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(ep, "tool_calls"));
      int conf = (int)cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(ep, "confidence"));
      int lat = (int)cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(ep, "latency_ms"));
      int ok = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(ep, "success"));
      printf("%-16s [%-8s] %2d turns %2d tools %s conf=%d %dms  %s\n", agent[0] ? agent : "?",
             role[0] ? role : "?", turns, tools, ok ? "ok  " : "FAIL", conf, lat, ts[0] ? ts : "?");
   }
}

static void print_eval_run(cJSON *resp)
{
   cJSON *rows = cJSON_GetObjectItemCaseSensitive(resp, "results");
   printf("%-30s %-12s %-12s %-6s %-6s %-8s %-10s\n", "Task", "Agent", "Ablation", "Pass", "Turns",
          "ToolOK", "Latency");
   if (cJSON_IsArray(rows))
   {
      cJSON *row;
      cJSON_ArrayForEach(row, rows)
      {
         double tool_ok =
             cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(row, "tool_call_success_rate"));
         printf("%-30s %-12s %-12s %-6s %-6d %-7.2f%% %-10dms\n", json_str(row, "task_name"),
                json_str(row, "agent_name"), json_str(row, "ablation"),
                json_int(row, "success", 0) ? "PASS" : "FAIL", json_int(row, "turns", 0),
                tool_ok * 100.0, json_int(row, "latency_ms", 0));
      }
   }
   printf("\n%d/%d passed.\n", json_int(resp, "passes", 0), json_int(resp, "total", 0));
}

static void print_eval_results(cJSON *resp)
{
   cJSON *rows = cJSON_GetObjectItemCaseSensitive(resp, "results");
   if (!cJSON_IsArray(rows) || cJSON_GetArraySize(rows) == 0)
   {
      printf("No eval results.\n");
      return;
   }
   printf("%-15s %-25s %-12s %-12s %-6s %-6s %-6s %-10s %s\n", "Suite", "Task", "Agent", "Ablation",
          "Pass", "Turns", "Tools", "Latency", "Time");
   cJSON *row;
   cJSON_ArrayForEach(row, rows)
   {
      printf("%-15s %-25s %-12s %-12s %-6s %-6d %-6d %-10dms %s\n", json_str(row, "suite"),
             json_str(row, "task_name"), json_str(row, "agent_name"), json_str(row, "ablation"),
             json_int(row, "success", 0) ? "PASS" : "FAIL", json_int(row, "turns", 0),
             json_int(row, "tool_calls", 0), json_int(row, "latency_ms", 0),
             json_str(row, "created_at"));
   }
}

static void print_delegate_status_object(cJSON *job, cJSON *full_result, cJSON *result_limit)
{
   cJSON *id = cJSON_GetObjectItemCaseSensitive(job, "job_id");
   cJSON *status = cJSON_GetObjectItemCaseSensitive(job, "job_status");
   cJSON *role = cJSON_GetObjectItemCaseSensitive(job, "role");
   cJSON *agent = cJSON_GetObjectItemCaseSensitive(job, "agent_name");
   cJSON *turns = cJSON_GetObjectItemCaseSensitive(job, "cursor_turn");
   cJSON *result = cJSON_GetObjectItemCaseSensitive(job, "result");
   cJSON *tool = cJSON_GetObjectItemCaseSensitive(job, "current_tool");
   cJSON *calls = cJSON_GetObjectItemCaseSensitive(job, "api_call_count");
   cJSON *default_max_turns = cJSON_GetObjectItemCaseSensitive(job, "default_max_turns");
   cJSON *final_after_turns = cJSON_GetObjectItemCaseSensitive(job, "final_after_turns");
   cJSON *heartbeat = cJSON_GetObjectItemCaseSensitive(job, "heartbeat_at");
   cJSON *created = cJSON_GetObjectItemCaseSensitive(job, "created_at");
   cJSON *updated = cJSON_GetObjectItemCaseSensitive(job, "updated_at");

   printf("job_id: %d\n", cJSON_IsNumber(id) ? id->valueint : 0);
   printf("status: %s\n", cJSON_IsString(status) ? status->valuestring : "unknown");
   if (cJSON_IsString(role) && role->valuestring[0])
      printf("role: %s\n", role->valuestring);
   if (cJSON_IsString(agent) && agent->valuestring[0])
      printf("agent: %s\n", agent->valuestring);
   if (cJSON_IsNumber(turns))
      printf("turns: %d\n", turns->valueint);
   if (cJSON_IsString(tool) && tool->valuestring[0])
      printf("tool: %s\n", tool->valuestring);
   if (cJSON_IsNumber(calls) && calls->valueint > 0)
      printf("api_calls: %d\n", calls->valueint);
   if (cJSON_IsNumber(default_max_turns) && default_max_turns->valueint > 0)
      printf("max_turns: %d\n", default_max_turns->valueint);
   if (cJSON_IsNumber(final_after_turns) && final_after_turns->valueint > 0)
      printf("final_after: turn %d\n", final_after_turns->valueint);
   if (cJSON_IsString(heartbeat) && heartbeat->valuestring[0])
      printf("heartbeat_at: %s\n", heartbeat->valuestring);
   if (cJSON_IsString(updated) && updated->valuestring[0])
      printf("updated_at: %s\n", updated->valuestring);
   if (cJSON_IsString(created) && created->valuestring[0])
      printf("created_at: %s\n", created->valuestring);
   if (cJSON_IsString(result) && result->valuestring[0])
   {
      if (cJSON_IsTrue(full_result))
      {
         printf("result: %s\n", result->valuestring);
      }
      else
      {
         int limit = cJSON_IsNumber(result_limit) ? result_limit->valueint : 200;
         if (limit < 0)
            limit = 0;
         int len = (int)strlen(result->valuestring);
         printf("result: %.*s\n", limit, result->valuestring);
         if (len > limit)
            printf("result_truncated: true\nresult_chars: %d\n", len);
      }
   }
}

static void print_workspace_add(cJSON *resp)
{
   const char *path = json_str(resp, "path");
   cJSON *projects = cJSON_GetObjectItemCaseSensitive(resp, "projects");
   int count = cJSON_IsArray(projects) ? cJSON_GetArraySize(projects) : 0;
   printf("workspace: added %s (%d project(s) discovered)\n", path, count);
   if (cJSON_IsArray(projects))
   {
      cJSON *p;
      cJSON_ArrayForEach(p, projects)
      {
         const char *name = json_str(p, "name");
         if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(p, "indexed")))
            printf("  indexed: %s\n", name);
         else
            printf("  %s (not indexed: %s)\n", name, json_str(p, "reason"));
      }
   }
}

static void print_workspace_get(cJSON *resp)
{
   cJSON *m = cJSON_GetObjectItemCaseSensitive(resp, "manifest");
   if (!cJSON_IsObject(m))
   {
      printf("workspace: no manifest returned\n");
      return;
   }
   printf("root:     %s\n", json_str(m, "root"));
   printf("provider: %s\n", json_str(m, "provider"));
   printf("exists:   %s\n",
          cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(m, "exists")) ? "yes" : "no");
   cJSON *vcs = cJSON_GetObjectItemCaseSensitive(m, "vcs");
   if (cJSON_IsObject(vcs))
   {
      const char *remote = json_str(vcs, "remote");
      const char *head = json_str(vcs, "head");
      const char *branch = json_str(vcs, "branch");
      if ((remote && remote[0]) || (head && head[0]) || (branch && branch[0]))
      {
         printf("vcs:\n");
         if (remote && remote[0])
            printf("  remote: %s\n", remote);
         if (branch && branch[0])
            printf("  branch: %s\n", branch);
         if (head && head[0])
            printf("  head:   %s\n", head);
      }
      else
         printf("vcs:      (not a git repo)\n");
   }
}

static void print_workspace_list(cJSON *resp)
{
   cJSON *workspaces = cJSON_GetObjectItemCaseSensitive(resp, "workspaces");
   if (!cJSON_IsArray(workspaces) || cJSON_GetArraySize(workspaces) == 0)
   {
      printf("No workspaces configured. Use 'aimee workspace add <path>' to add one.\n");
      return;
   }
   cJSON *ws;
   cJSON_ArrayForEach(ws, workspaces)
   {
      /* Surface a non-default provider and (for mirror) its vcs coordinates so a
       * detached/mirror workspace is distinguishable from a plain co-located one. */
      const char *prov = json_str(ws, "provider");
      const char *remote = json_str(ws, "remote");
      const char *head = json_str(ws, "head");
      if (prov && prov[0] && strcmp(prov, "shared") != 0)
      {
         printf("%s  [%s", json_str(ws, "path"), prov);
         if (remote && remote[0])
            printf(" remote=%s", remote);
         if (head && head[0])
            printf(" head=%.10s", head);
         printf("]\n");
      }
      else
         printf("%s\n", json_str(ws, "path"));
      cJSON *projs = cJSON_GetObjectItemCaseSensitive(ws, "projects");
      if (cJSON_IsArray(projs))
      {
         cJSON *pr;
         cJSON_ArrayForEach(pr, projs) if (cJSON_IsString(pr)) printf("  %s\n", pr->valuestring);
      }
   }
}

/* --- print_text_output dispatch: one printer per CLI method, table-driven.
 * Extracted verbatim from the former ~1200-line if/else-if ladder. Adding a
 * method's text rendering is now one printer + one table row. Each printer reads
 * only `resp` (plus `method` for the few that need it). The skill.* prefix branch
 * keeps its internal sub-dispatch and is matched after the exact table — exactly
 * as the ladder did (skill.list / skill.show were exact branches before it). --- */
static void pt_print_init_run(const char *method, cJSON *resp)
{
   print_init_run(resp);
}
static void pt_print_migrate_v2(const char *method, cJSON *resp)
{
   print_storage_migrate(resp);
}
static void pt_print_rules_generate(const char *method, cJSON *resp)
{
   cJSON *c = cJSON_GetObjectItemCaseSensitive(resp, "content");
   if (cJSON_IsString(c) && c->valuestring[0])
      printf("%s", c->valuestring);
}
static void pt_print_skill_list(const char *method, cJSON *resp)
{
   cJSON *skills = cJSON_GetObjectItemCaseSensitive(resp, "skills");
   cJSON *item;
   cJSON_ArrayForEach(item, skills)
   {
      printf("%-32s  %s\n", json_str(item, "name"), json_str(item, "source"));
   }
}
static void pt_print_skill_show(const char *method, cJSON *resp)
{
   cJSON *c = cJSON_GetObjectItemCaseSensitive(resp, "content");
   if (cJSON_IsString(c) && c->valuestring)
   {
      fputs(c->valuestring, stdout);
      size_t len = strlen(c->valuestring);
      if (len == 0 || c->valuestring[len - 1] != '\n')
         fputc('\n', stdout);
   }
}
static void pt_print_skill_group(const char *method, cJSON *resp)
{
   const char *status = json_str(resp, "status");
   if (strcmp(method, "skill.lint") == 0)
   {
      cJSON *report = cJSON_GetObjectItemCaseSensitive(resp, "report");
      if (cJSON_IsString(report) && report->valuestring[0])
         fputs(report->valuestring, stderr);
      else
         printf("skill lint: %d skills passed\n", json_int(resp, "checked", 0));
   }
   else if (strcmp(method, "skill.eval") == 0)
   {
      printf("skill eval: %s %s\n", json_str(resp, "name"),
             strcmp(status, "ok") == 0 ? "PASS" : "FAIL");
      printf("scenarios: %d\n", json_int(resp, "scenarios", 0));
      printf("baseline violations: %d/%d\n", json_int(resp, "baseline_violations", 0),
             json_int(resp, "scenarios", 0));
      printf("treatment compliance: %d/%d\n", json_int(resp, "treatment_compliances", 0),
             json_int(resp, "scenarios", 0));
      printf("compliance delta: %.2f\n", json_double(resp, "compliance_delta", 0.0));
      if (strcmp(status, "ok") != 0)
         fprintf(stderr, "skill eval: %s\n", json_str(resp, "message"));
   }
   else if (strcmp(method, "skill.lifecycle") == 0)
   {
      printf("skill lifecycle: %d considered, %d stale, %d archived, %d pinned skipped\n",
             json_int(resp, "considered", 0), json_int(resp, "stale_marked", 0),
             json_int(resp, "archived", 0), json_int(resp, "skipped_pinned", 0));
   }
   else if (strcmp(method, "skill.autostub") == 0)
   {
      if (strcmp(status, "disabled") == 0)
         printf("skill autostub: disabled (set skills.capability.autostub or pass --force)\n");
      else if (strcmp(status, "ok") == 0)
         printf("skill autostub: %d scanned, %d covered, %d proposed, %d skipped\n",
                json_int(resp, "scanned", 0), json_int(resp, "existing", 0),
                json_int(resp, "proposed", 0), json_int(resp, "skipped", 0));
      else
         fprintf(stderr, "skill autostub: %s\n", json_str(resp, "message"));
   }
   else if (strcmp(status, "ok") == 0)
      printf("skill %s: %s\n", json_str(resp, "action"), json_str(resp, "name"));
   else
      fprintf(stderr, "skill: %s\n", json_str(resp, "message"));
}
static void pt_print_git_verify(const char *method, cJSON *resp)
{
   print_mcp_content(resp);
}
static void pt_print_get_help(const char *method, cJSON *resp)
{
   print_mcp_content(resp);
}
static void pt_print_server_health(const char *method, cJSON *resp)
{
   print_server_health(resp);
}
static void pt_print_session_list(const char *method, cJSON *resp)
{
   print_session_list(resp);
}
static void pt_print_session_get(const char *method, cJSON *resp)
{
   print_session_get(resp);
}
static void pt_print_session_close(const char *method, cJSON *resp)
{
   print_session_close(resp);
}
static void pt_print_session_brief(const char *method, cJSON *resp)
{
   print_session_brief(resp);
}
static void pt_print_session_attach(const char *method, cJSON *resp)
{
   const char *aid = json_str(resp, "attach_id");
   printf("attached: %s\n", (aid && aid[0]) ? aid : "(none)");
}
static void pt_print_session_detach(const char *method, cJSON *resp)
{
   cJSON *d = cJSON_GetObjectItemCaseSensitive(resp, "detached");
   printf("%s\n", cJSON_IsTrue(d) ? "detached" : "no such attachment");
}
static void pt_print_session_presence(const char *method, cJSON *resp)
{
   cJSON *arr = cJSON_GetObjectItemCaseSensitive(resp, "presences");
   int n = cJSON_IsArray(arr) ? cJSON_GetArraySize(arr) : 0;
   if (n == 0)
      printf("(no live presences)\n");
   cJSON *p = NULL;
   cJSON_ArrayForEach(p, arr)
   {
      const char *sid = json_str(p, "session_id");
      const char *owner = json_str(p, "owner");
      cJSON *att = cJSON_GetObjectItemCaseSensitive(p, "attachments");
      cJSON *tif = cJSON_GetObjectItemCaseSensitive(p, "turn_in_flight");
      const char *tid = json_str(p, "turn_id");
      printf("%-32s owner=%-12s attachments=%d%s%s\n", sid ? sid : "(?)",
             (owner && owner[0]) ? owner : "-", cJSON_IsNumber(att) ? att->valueint : 0,
             cJSON_IsTrue(tif) ? " turn=" : "", cJSON_IsTrue(tif) && tid ? tid : "");
   }
}
static void pt_print_trajectory_export(const char *method, cJSON *resp)
{
   cJSON *trajectory = cJSON_GetObjectItemCaseSensitive(resp, "trajectory");
   char *json = trajectory ? cJSON_PrintUnformatted(trajectory) : NULL;
   if (json)
   {
      puts(json);
      free(json);
   }
}
static void pt_print_trajectory_batch(const char *method, cJSON *resp)
{
   char *json = cJSON_PrintUnformatted(resp);
   if (json)
   {
      puts(json);
      free(json);
   }
}
static void pt_print_insights_overview(const char *method, cJSON *resp)
{
   print_insights_overview(resp);
}
static void pt_print_worktree_gc(const char *method, cJSON *resp)
{
   print_worktree_gc(resp);
}
static void pt_print_work_add(const char *method, cJSON *resp)
{
   printf("Added work item %s: %s\n", json_str(resp, "id"), json_str(resp, "title"));
}
static void pt_print_work_add_batch(const char *method, cJSON *resp)
{
   int added = (int)cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(resp, "added"));
   int skipped = (int)cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(resp, "skipped"));
   printf("Added %d work items from proposals (%d already queued).\n", added, skipped);
}
static void pt_print_work_claim(const char *method, cJSON *resp)
{
   print_work_claim(resp);
}
static void pt_print_work_complete(const char *method, cJSON *resp)
{
   print_work_finish(resp, "done");
}
static void pt_print_work_fail(const char *method, cJSON *resp)
{
   print_work_finish(resp, "failed");
}
static void pt_print_work_list(const char *method, cJSON *resp)
{
   print_work_list(resp);
}
static void pt_print_work_board(const char *method, cJSON *resp)
{
   print_work_board(resp);
}
static void pt_print_work_cancel(const char *method, cJSON *resp)
{
   print_work_simple_item(resp, "cancelled", "Cancelled", "No pending item found.");
}
static void pt_print_work_release(const char *method, cJSON *resp)
{
   print_work_simple_item(resp, "released", "Released", "No claimed item found.");
}
static void pt_print_work_clear(const char *method, cJSON *resp)
{
   int cleared = (int)cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(resp, "cleared"));
   printf("Cleared %d %s items.\n", cleared, json_str(resp, "status_filter"));
}
static void pt_print_work_gc(const char *method, cJSON *resp)
{
   print_work_gc(resp);
}
static void pt_print_work_sync_proposals(const char *method, cJSON *resp)
{
   print_work_sync_proposals(resp);
}
static void pt_print_work_stats(const char *method, cJSON *resp)
{
   print_work_stats(resp);
}
static void pt_print_rules_delete(const char *method, cJSON *resp)
{
   print_rules_delete(resp);
}
static void pt_print_memory_search(const char *method, cJSON *resp)
{
   print_memory_search(resp);
}
static void pt_print_memory_store(const char *method, cJSON *resp)
{
   cJSON *id = cJSON_GetObjectItemCaseSensitive(resp, "id");
   if (cJSON_IsNumber(id))
      printf("stored memory %lld\n", (long long)id->valuedouble);
   else
      printf("stored memory\n");
}
static void pt_print_memory_list(const char *method, cJSON *resp)
{
   print_memory_list(resp);
}
static void pt_print_memory_get(const char *method, cJSON *resp)
{
   print_memory_row(resp);
}
static void pt_print_memory_read(const char *method, cJSON *resp)
{
   cJSON *context = cJSON_GetObjectItemCaseSensitive(resp, "context");
   if (cJSON_IsString(context))
      printf("%s", context->valuestring);
}
static void pt_print_memory_stats(const char *method, cJSON *resp)
{
   print_memory_stats(resp);
}
static void pt_print_index_scan(const char *method, cJSON *resp)
{
   print_index_scan(resp);
}
static void pt_print_index_list(const char *method, cJSON *resp)
{
   print_index_list(resp);
}
static void pt_print_index_find(const char *method, cJSON *resp)
{
   print_index_find(resp);
}
static void pt_print_index_blast_radius(const char *method, cJSON *resp)
{
   print_index_blast_radius(resp);
}
static void pt_print_index_structure(const char *method, cJSON *resp)
{
   print_index_structure(resp);
}
static void pt_print_index_find_callers(const char *method, cJSON *resp)
{
   print_index_callers(resp);
}
static void pt_print_graph_sync_code(const char *method, cJSON *resp)
{
   printf("graph sync-code: project=%s generation=%s edges=%s\n", json_str(resp, "project"),
          json_str(resp, "generation_id"), json_str(resp, "edge_count"));
}
static void pt_print_workspace_add(const char *method, cJSON *resp)
{
   print_workspace_add(resp);
}
static void pt_print_workspace_list(const char *method, cJSON *resp)
{
   print_workspace_list(resp);
}
static void pt_print_workspace_get(const char *method, cJSON *resp)
{
   print_workspace_get(resp);
}
static void pt_print_workspace_remove(const char *method, cJSON *resp)
{
   printf("workspace: removed %s\n", json_str(resp, "removed"));
}
static void pt_print_workspace_mirror_sync(const char *method, cJSON *resp)
{
   printf("workspace: synced client diff (%d bytes)\n", json_int(resp, "bytes", 0));
}
static void pt_print_hud_status(const char *method, cJSON *resp)
{
   print_hud_status(resp);
}
static void pt_print_agent_list(const char *method, cJSON *resp)
{
   print_agent_list(resp);
}
static void pt_print_agent_local(const char *method, cJSON *resp)
{
   print_agent_local(resp);
}
static void pt_print_agent_add(const char *method, cJSON *resp)
{
   print_agent_add(resp);
}
static void pt_print_agent_remove(const char *method, cJSON *resp)
{
   print_agent_remove(resp);
}
static void pt_print_agent_enable(const char *method, cJSON *resp)
{
   print_agent_enabled(resp, 1);
}
static void pt_print_agent_disable(const char *method, cJSON *resp)
{
   print_agent_enabled(resp, 0);
}
static void pt_print_agent_probe(const char *method, cJSON *resp)
{
   print_agent_probe(resp);
}
static void pt_print_mcp_audit(const char *method, cJSON *resp)
{
   print_mcp_audit(resp);
}
static void pt_print_mcp_recheck(const char *method, cJSON *resp)
{
   print_mcp_recheck(resp);
}
static void pt_print_toolset_list(const char *method, cJSON *resp)
{
   print_toolset_list(resp);
}
static void pt_print_toolset_show(const char *method, cJSON *resp)
{
   print_toolset_show(resp);
}
static void pt_print_toolset_resolve(const char *method, cJSON *resp)
{
   print_toolset_resolve(resp);
}
static void pt_print_trigger_list(const char *method, cJSON *resp)
{
   print_trigger_list(resp);
}
static void pt_print_trigger_status(const char *method, cJSON *resp)
{
   print_trigger_status(resp);
}
static void pt_print_trigger_fire(const char *method, cJSON *resp)
{
   print_trigger_fire(resp);
}
static void pt_print_trigger_cancel(const char *method, cJSON *resp)
{
   print_trigger_cancel(resp);
}
static void pt_print_cron_list(const char *method, cJSON *resp)
{
   print_cron_list(resp);
}
static void pt_print_cron_add(const char *method, cJSON *resp)
{
   printf("cron job %s: %s\n", json_str(resp, "job_id"), json_str(resp, "status"));
}
static void pt_print_cron_show(const char *method, cJSON *resp)
{
   print_cron_show(resp);
}
static void pt_print_cron_history(const char *method, cJSON *resp)
{
   print_cron_history(resp);
}
static void pt_print_cron_run(const char *method, cJSON *resp)
{
   print_cron_run(resp);
}
static void pt_print_wm_get(const char *method, cJSON *resp)
{
   cJSON *v = cJSON_GetObjectItemCaseSensitive(resp, "value");
   if (cJSON_IsString(v))
      printf("%s\n", v->valuestring);
}
static void pt_print_wm_set(const char *method, cJSON *resp)
{
   cJSON *k = cJSON_GetObjectItemCaseSensitive(resp, "key");
   cJSON *v = cJSON_GetObjectItemCaseSensitive(resp, "value");
   if (cJSON_IsString(k) && cJSON_IsString(v))
      printf("set %s = %s\n", k->valuestring, v->valuestring);
}
static void pt_print_config_value(cJSON *v)
{
   if (cJSON_IsString(v))
      printf("%s\n", v->valuestring[0] ? v->valuestring : "(unset)");
   else if (cJSON_IsBool(v))
      printf("%s\n", cJSON_IsTrue(v) ? "true" : "false");
   else if (cJSON_IsNumber(v))
   {
      double d = v->valuedouble;
      if (d == (double)(long)d)
         printf("%ld\n", (long)d);
      else
         printf("%g\n", d);
   }
   else
      printf("(unset)\n");
}
static void pt_print_config_get(const char *method, cJSON *resp)
{
   (void)method;
   pt_print_config_value(cJSON_GetObjectItemCaseSensitive(resp, "value"));
}
static void pt_print_config_set(const char *method, cJSON *resp)
{
   (void)method;
   cJSON *k = cJSON_GetObjectItemCaseSensitive(resp, "key");
   cJSON *v = cJSON_GetObjectItemCaseSensitive(resp, "value");
   printf("set %s = ", cJSON_IsString(k) ? k->valuestring : "?");
   pt_print_config_value(v);
}
static void pt_print_config_show(const char *method, cJSON *resp)
{
   (void)method;
   cJSON *cfg = cJSON_GetObjectItemCaseSensitive(resp, "config");
   char *s = cJSON_Print(cfg ? cfg : resp);
   if (s)
   {
      printf("%s\n", s);
      free(s);
   }
}
static void pt_print_wm_list(const char *method, cJSON *resp)
{
   cJSON *entries = cJSON_GetObjectItemCaseSensitive(resp, "entries");
   if (cJSON_IsArray(entries))
   {
      cJSON *el;
      int n = 0;
      cJSON_ArrayForEach(el, entries)
      {
         cJSON *cat = cJSON_GetObjectItemCaseSensitive(el, "category");
         cJSON *key = cJSON_GetObjectItemCaseSensitive(el, "key");
         cJSON *val = cJSON_GetObjectItemCaseSensitive(el, "value");
         if (cJSON_IsString(key) && cJSON_IsString(val))
            printf("[%s] %s: %s\n", cJSON_IsString(cat) ? cat->valuestring : "general",
                   key->valuestring, val->valuestring);
         n++;
      }
      if (n == 0)
         printf("(no entries)\n");
   }
}
static void pt_print_delegate(const char *method, cJSON *resp)
{
   cJSON *job_id = cJSON_GetObjectItemCaseSensitive(resp, "job_id");
   if (cJSON_IsNumber(job_id))
   {
      cJSON *status = cJSON_GetObjectItemCaseSensitive(resp, "job_status");
      printf("job_id: %d\n", job_id->valueint);
      printf("status: %s\n", cJSON_IsString(status) ? status->valuestring : "running");
      return;
   }
   cJSON *r = cJSON_GetObjectItemCaseSensitive(resp, "response");
   if (cJSON_IsString(r) && r->valuestring[0])
      printf("%s\n", r->valuestring);
}
static void pt_print_delegate_status(const char *method, cJSON *resp)
{
   cJSON *full_result = cJSON_GetObjectItemCaseSensitive(resp, "full_result");
   cJSON *result_limit = cJSON_GetObjectItemCaseSensitive(resp, "result_limit");
   cJSON *jobs = cJSON_GetObjectItemCaseSensitive(resp, "jobs");

   if (cJSON_IsArray(jobs))
   {
      int n = cJSON_GetArraySize(jobs);
      for (int i = 0; i < n; i++)
      {
         if (i > 0)
            printf("---\n");
         cJSON *job = cJSON_GetArrayItem(jobs, i);
         print_delegate_status_object(job, full_result, result_limit);
      }
      return;
   }

   print_delegate_status_object(resp, full_result, result_limit);
}
static void pt_print_jobs_list(const char *method, cJSON *resp)
{
   print_jobs_list(resp);
}
static void pt_print_jobs_status(const char *method, cJSON *resp)
{
   print_jobs_status(resp);
}
static void pt_print_jobs_logs(const char *method, cJSON *resp)
{
   print_jobs_logs(resp);
}
static void pt_print_jobs_cancel(const char *method, cJSON *resp)
{
   print_jobs_cancel(resp);
}
static void pt_print_job_start(const char *method, cJSON *resp)
{
   print_coord_job_start(resp);
}
static void pt_print_job_list(const char *method, cJSON *resp)
{
   print_coord_jobs_list(resp);
}
static void pt_print_job_status(const char *method, cJSON *resp)
{
   print_coord_job_status(resp);
}
static void pt_print_job_cancel(const char *method, cJSON *resp)
{
   print_coord_job_cancel(resp);
}
static void pt_print_aux_config_show(const char *method, cJSON *resp)
{
   print_aux_config(resp);
}
static void pt_print_aux_test(const char *method, cJSON *resp)
{
   cJSON *r = cJSON_GetObjectItemCaseSensitive(resp, "response");
   if (cJSON_IsString(r))
      printf("%s\n", r->valuestring);
}
static void pt_print_delegate_log(const char *method, cJSON *resp)
{
   print_episode_list(resp);
}
static void pt_print_delegate_launch(const char *method, cJSON *resp)
{
   cJSON *job_id = cJSON_GetObjectItemCaseSensitive(resp, "job_id");
   cJSON *plan_id = cJSON_GetObjectItemCaseSensitive(resp, "plan_id");
   cJSON *tasks = cJSON_GetObjectItemCaseSensitive(resp, "tasks");
   cJSON *max = cJSON_GetObjectItemCaseSensitive(resp, "max_concurrent");
   int id = cJSON_IsNumber(job_id) ? job_id->valueint : 0;
   printf("Queued coord job #%d from delegate plan #%d: %d tasks, max %d concurrent\n", id,
          cJSON_IsNumber(plan_id) ? plan_id->valueint : 0,
          cJSON_IsNumber(tasks) ? tasks->valueint : 0, cJSON_IsNumber(max) ? max->valueint : 0);
   printf("Inspect packet progress with: aimee job status %d\n", id);
   printf("Note: coord jobs are queued packet plans. `aimee jobs status` and "
          "`aimee delegate status` show durable background delegate jobs, not coord jobs.\n");
}
static void pt_print_kb_search(const char *method, cJSON *resp)
{
   cJSON *results = cJSON_GetObjectItemCaseSensitive(resp, "results");
   if (!cJSON_IsArray(results) || cJSON_GetArraySize(results) == 0)
   {
      printf("No results.\n");
      return;
   }
   cJSON *r;
   cJSON_ArrayForEach(r, results)
   {
      cJSON *score = cJSON_GetObjectItemCaseSensitive(r, "score");
      cJSON *fp = cJSON_GetObjectItemCaseSensitive(r, "file_path");
      cJSON *hp = cJSON_GetObjectItemCaseSensitive(r, "heading_path");
      printf("  %.4f  %s  %s\n", cJSON_IsNumber(score) ? score->valuedouble : 0.0,
             cJSON_IsString(fp) ? fp->valuestring : "?", cJSON_IsString(hp) ? hp->valuestring : "");
   }
}
static void pt_print_kb_build(const char *method, cJSON *resp)
{
   cJSON *proj = cJSON_GetObjectItemCaseSensitive(resp, "project");
   cJSON *fi = cJSON_GetObjectItemCaseSensitive(resp, "files_indexed");
   cJSON *ca = cJSON_GetObjectItemCaseSensitive(resp, "chunks_added");
   cJSON *ea = cJSON_GetObjectItemCaseSensitive(resp, "embeddings_added");
   printf("project: %s\n", cJSON_IsString(proj) ? proj->valuestring : "?");
   printf("files indexed:    %d\n", cJSON_IsNumber(fi) ? (int)fi->valuedouble : 0);
   printf("chunks added:     %d\n", cJSON_IsNumber(ca) ? (int)ca->valuedouble : 0);
   printf("embeddings added: %d\n", cJSON_IsNumber(ea) ? (int)ea->valuedouble : 0);
}
static void pt_print_kb_ingest(const char *method, cJSON *resp)
{
   cJSON *msg = cJSON_GetObjectItemCaseSensitive(resp, "message");
   if (cJSON_IsString(msg))
      printf("%s\n", msg->valuestring);
   else
   {
      cJSON *nq = cJSON_GetObjectItemCaseSensitive(resp, "projects_queued");
      printf("%d project(s) queued for ingest. Run `aimee kb ingest status` to monitor.\n",
             cJSON_IsNumber(nq) ? (int)nq->valuedouble : 0);
   }
}
static void pt_print_kb_docs_push(const char *method, cJSON *resp)
{
   int total = (int)cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(resp, "total"));
   int uploaded = (int)cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(resp, "uploaded"));
   int skipped = (int)cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(resp, "skipped"));
   int failed = (int)cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(resp, "failed"));
   printf("Docs push complete: %d uploaded, %d skipped, %d failed (%d total).\n", uploaded, skipped,
          failed, total);
}
static void pt_print_kb_ingest_status(const char *method, cJSON *resp)
{
   cJSON *queue = cJSON_GetObjectItemCaseSensitive(resp, "queue");
   cJSON *workers_obj = cJSON_GetObjectItemCaseSensitive(resp, "workers");
   cJSON *recent = cJSON_GetObjectItemCaseSensitive(resp, "recent");

   int pending = 0, running = 0, done24 = 0, failed24 = 0, configured = 0;
   if (cJSON_IsObject(queue))
   {
      pending = (int)cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(queue, "pending"));
      running = (int)cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(queue, "running"));
      done24 = (int)cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(queue, "done_last_24h"));
      failed24 =
          (int)cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(queue, "failed_last_24h"));
   }
   cJSON *slots = NULL;
   if (cJSON_IsObject(workers_obj))
   {
      configured =
          (int)cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(workers_obj, "configured"));
      slots = cJSON_GetObjectItemCaseSensitive(workers_obj, "slots");
   }

   if (cJSON_IsArray(recent) && cJSON_GetArraySize(recent) > 0)
   {
      printf("Jobs:\n");
      cJSON *item;
      cJSON_ArrayForEach(item, recent)
      {
         cJSON *pn = cJSON_GetObjectItemCaseSensitive(item, "project");
         cJSON *st = cJSON_GetObjectItemCaseSensitive(item, "status");
         cJSON *at = cJSON_GetObjectItemCaseSensitive(item, "completed_at");
         cJSON *fi = cJSON_GetObjectItemCaseSensitive(item, "files_indexed");
         cJSON *ca = cJSON_GetObjectItemCaseSensitive(item, "chunks_added");
         cJSON *er = cJSON_GetObjectItemCaseSensitive(item, "error");
         const char *status_str = cJSON_IsString(st) ? st->valuestring : "?";
         if (cJSON_IsString(er) && er->valuestring[0])
            printf("  %-24s  %-6s  %s  ERROR: %s\n", cJSON_IsString(pn) ? pn->valuestring : "?",
                   status_str, cJSON_IsString(at) ? at->valuestring : "", er->valuestring);
         else
            printf("  %-24s  %-6s  %s  %d files  %d chunks\n",
                   cJSON_IsString(pn) ? pn->valuestring : "?", status_str,
                   cJSON_IsString(at) ? at->valuestring : "",
                   cJSON_IsNumber(fi) ? (int)fi->valuedouble : 0,
                   cJSON_IsNumber(ca) ? (int)ca->valuedouble : 0);
      }
      printf("\n");
   }
   printf("Queue:    %d pending  %d running  %d done (24h)  %d failed (24h)\n", pending, running,
          done24, failed24);
   printf("Workers:  %d configured\n", configured);
   if (cJSON_IsArray(slots))
   {
      cJSON *slot;
      cJSON_ArrayForEach(slot, slots)
      {
         int idx = (int)cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(slot, "index"));
         int active = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(slot, "active"));
         if (active)
         {
            const char *proj =
                cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(slot, "project"));
            const char *phase =
                cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(slot, "phase"));
            int elapsed =
                (int)cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(slot, "elapsed_secs"));
            printf("  [%d] %-24s  %-10s  %ds\n", idx, proj ? proj : "?", phase ? phase : "?",
                   elapsed);
         }
         else
         {
            printf("  [%d] idle\n", idx);
         }
      }
   }
}
static void pt_print_kb_status(const char *method, cJSON *resp)
{
   cJSON *proj = cJSON_GetObjectItemCaseSensitive(resp, "project");
   cJSON *chunks = cJSON_GetObjectItemCaseSensitive(resp, "chunks");
   cJSON *vpts = cJSON_GetObjectItemCaseSensitive(resp, "vector_points");
   if (cJSON_IsString(proj))
      printf("project: %s\n", proj->valuestring);
   if (cJSON_IsNumber(chunks))
      printf("chunks:        %d\n", (int)chunks->valuedouble);
   if (cJSON_IsNumber(vpts))
      printf("vector points: %d\n", (int)vpts->valuedouble);
   cJSON *iq = cJSON_GetObjectItemCaseSensitive(resp, "ingest_queue");
   if (cJSON_IsObject(iq))
   {
      cJSON *pending = cJSON_GetObjectItemCaseSensitive(iq, "pending");
      cJSON *running = cJSON_GetObjectItemCaseSensitive(iq, "running");
      cJSON *done24 = cJSON_GetObjectItemCaseSensitive(iq, "done_last_24h");
      int n_pending = cJSON_IsNumber(pending) ? (int)pending->valuedouble : 0;
      int n_running = cJSON_IsNumber(running) ? (int)running->valuedouble : 0;
      int n_done24 = cJSON_IsNumber(done24) ? (int)done24->valuedouble : 0;
      printf("Background ingest: %d pending", n_pending + n_running);
      if (n_running > 0)
         printf(" (%d running)", n_running);
      printf(", %d done last 24h\n", n_done24);
   }
}
static void pt_print_workers(const char *method, cJSON *resp)
{
   cJSON *server_bg_obj = cJSON_GetObjectItemCaseSensitive(resp, "server_background");
   cJSON *request_obj = cJSON_GetObjectItemCaseSensitive(resp, "request");
   cJSON *compute_obj = cJSON_GetObjectItemCaseSensitive(resp, "compute");
   cJSON *kb_obj = cJSON_GetObjectItemCaseSensitive(resp, "kb");
   int ingest_cap =
       (int)cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(resp, "ingest_max_concurrent"));

   if (cJSON_IsObject(server_bg_obj))
   {
      printf("aimee-server  background threads:\n");
      cJSON *threads = cJSON_GetObjectItemCaseSensitive(server_bg_obj, "threads");
      if (cJSON_IsArray(threads))
      {
         cJSON *thread;
         cJSON_ArrayForEach(thread, threads)
         {
            const char *name =
                cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(thread, "name"));
            const char *state =
                cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(thread, "state"));
            int active = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(thread, "active"));
            cJSON *in_flight = cJSON_GetObjectItemCaseSensitive(thread, "in_flight");
            cJSON *max_concurrent = cJSON_GetObjectItemCaseSensitive(thread, "max_concurrent");
            if (cJSON_IsNumber(in_flight) || cJSON_IsNumber(max_concurrent))
               printf("  %-18s %-8s %-10s %d/%d in-flight\n", name ? name : "?",
                      active ? "active" : "idle", state ? state : "-",
                      cJSON_IsNumber(in_flight) ? (int)in_flight->valuedouble : 0,
                      cJSON_IsNumber(max_concurrent) ? (int)max_concurrent->valuedouble : 0);
            else
               printf("  %-18s %-8s %s\n", name ? name : "?", active ? "active" : "idle",
                      state ? state : "-");
         }
      }
   }

   if (cJSON_IsObject(request_obj))
   {
      int cfg =
          (int)cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(request_obj, "configured"));
      printf("aimee-server  request threads: %d configured\n", cfg);
      cJSON *slots = cJSON_GetObjectItemCaseSensitive(request_obj, "slots");
      if (cJSON_IsArray(slots))
      {
         cJSON *slot;
         cJSON_ArrayForEach(slot, slots)
         {
            int idx = (int)cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(slot, "index"));
            int active = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(slot, "active"));
            if (active)
            {
               const char *desc =
                   cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(slot, "descriptor"));
               int elapsed = (int)cJSON_GetNumberValue(
                   cJSON_GetObjectItemCaseSensitive(slot, "elapsed_secs"));
               printf("  [%d] %-40s  %ds\n", idx, desc && desc[0] ? desc : "-", elapsed);
            }
            else
            {
               printf("  [%d] idle\n", idx);
            }
         }
      }
   }

   if (cJSON_IsObject(compute_obj))
   {
      int cfg =
          (int)cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(compute_obj, "configured"));
      if (ingest_cap > 0)
         printf("aimee-server  compute pool:   %d configured (ingest cap %d)\n", cfg, ingest_cap);
      else
         printf("aimee-server  compute pool:   %d configured\n", cfg);
      cJSON *slots = cJSON_GetObjectItemCaseSensitive(compute_obj, "slots");
      if (cJSON_IsArray(slots))
      {
         cJSON *slot;
         cJSON_ArrayForEach(slot, slots)
         {
            int idx = (int)cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(slot, "index"));
            int active = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(slot, "active"));
            if (active)
            {
               const char *kind =
                   cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(slot, "kind"));
               const char *desc =
                   cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(slot, "descriptor"));
               int elapsed = (int)cJSON_GetNumberValue(
                   cJSON_GetObjectItemCaseSensitive(slot, "elapsed_secs"));
               printf("  [%d] %-10s  %-40s  %ds\n", idx, kind ? kind : "?",
                      desc && desc[0] ? desc : "-", elapsed);
            }
            else
            {
               printf("  [%d] idle\n", idx);
            }
         }
      }
   }

   cJSON *async_obj = cJSON_GetObjectItemCaseSensitive(resp, "async");
   if (cJSON_IsObject(async_obj))
   {
      int chat_active =
          (int)cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(async_obj, "chat_active"));
      int chat_limit =
          (int)cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(async_obj, "chat_limit"));
      int tool_active =
          (int)cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(async_obj, "tool_active"));
      int tool_limit =
          (int)cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(async_obj, "tool_limit"));
      if (chat_limit > 0)
         printf("aimee-server  async lanes:    chat %d/%d active, tool %d/%d active\n", chat_active,
                chat_limit, tool_active, tool_limit);
      else
         printf("aimee-server  async lanes:    chat %d active (uncapped), tool %d/%d active\n",
                chat_active, tool_active, tool_limit);
      cJSON *slots = cJSON_GetObjectItemCaseSensitive(async_obj, "slots");
      if (cJSON_IsArray(slots))
      {
         cJSON *slot;
         cJSON_ArrayForEach(slot, slots)
         {
            const char *lane = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(slot, "lane"));
            const char *sid =
                cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(slot, "session_id"));
            const char *psid =
                cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(slot, "provider_session_id"));
            const char *desc =
                cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(slot, "descriptor"));
            int elapsed =
                (int)cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(slot, "elapsed_secs"));
            if (psid && psid[0])
               printf("  %-5s session=%-18s provider=%-18s %-24s %ds\n", lane ? lane : "?",
                      sid ? sid : "?", psid, desc && desc[0] ? desc : "-", elapsed);
            else
               printf("  %-5s session=%-18s %-44s %ds\n", lane ? lane : "?", sid ? sid : "?",
                      desc && desc[0] ? desc : "-", elapsed);
         }
      }
   }

   cJSON *secondary = cJSON_GetObjectItemCaseSensitive(resp, "secondary_pools");
   if (cJSON_IsArray(secondary))
   {
      cJSON *pool_entry;
      cJSON_ArrayForEach(pool_entry, secondary)
      {
         const char *name =
             cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(pool_entry, "name"));
         int cfg =
             (int)cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(pool_entry, "configured"));
         printf("aimee-server  %s pool:%*s%d configured (ephemeral)\n", name ? name : "?",
                (int)(11 - strlen(name ? name : "?")), "", cfg);
         cJSON *slots = cJSON_GetObjectItemCaseSensitive(pool_entry, "slots");
         if (cJSON_IsArray(slots))
         {
            cJSON *slot;
            cJSON_ArrayForEach(slot, slots)
            {
               int idx = (int)cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(slot, "index"));
               int active = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(slot, "active"));
               if (active)
               {
                  const char *kind =
                      cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(slot, "kind"));
                  const char *desc =
                      cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(slot, "descriptor"));
                  int elapsed = (int)cJSON_GetNumberValue(
                      cJSON_GetObjectItemCaseSensitive(slot, "elapsed_secs"));
                  printf("  [%d] %-10s  %-40s  %ds\n", idx, kind ? kind : "?",
                         desc && desc[0] ? desc : "-", elapsed);
               }
               else
               {
                  printf("  [%d] idle\n", idx);
               }
            }
         }
      }
   }

   if (cJSON_IsObject(kb_obj))
   {
      int cfg = (int)cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(kb_obj, "configured"));
      printf("aimee-kb      connection workers: %d configured\n", cfg);
      cJSON *threads = cJSON_GetObjectItemCaseSensitive(kb_obj, "threads");
      if (cJSON_IsArray(threads))
      {
         printf("aimee-kb      worker threads:\n");
         cJSON *thread;
         cJSON_ArrayForEach(thread, threads)
         {
            const char *name =
                cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(thread, "name"));
            const char *state =
                cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(thread, "state"));
            int active = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(thread, "active"));
            printf("  %-22s %-8s %s\n", name ? name : "?", active ? "active" : "idle",
                   state ? state : "-");
         }
      }
      cJSON *slots = cJSON_GetObjectItemCaseSensitive(kb_obj, "slots");
      if (cJSON_IsArray(slots))
      {
         cJSON *slot;
         cJSON_ArrayForEach(slot, slots)
         {
            int idx = (int)cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(slot, "index"));
            int active = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(slot, "active"));
            if (active)
            {
               const char *meth =
                   cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(slot, "method"));
               /* kb.workers is the introspection query itself — skip it */
               if (meth && strcmp(meth, "kb.workers") == 0)
                  continue;
               int elapsed = (int)cJSON_GetNumberValue(
                   cJSON_GetObjectItemCaseSensitive(slot, "elapsed_secs"));
               printf("  [%d] %-36s  %ds\n", idx, meth ? meth : "?", elapsed);
            }
            else
            {
               printf("  [%d] idle\n", idx);
            }
         }
      }
      /* aimee-kb autonomous tasks (curator drain, maintenance timer, etc.). */
      cJSON *bg = cJSON_GetObjectItemCaseSensitive(kb_obj, "background");
      if (cJSON_IsArray(bg) && cJSON_GetArraySize(bg) > 0)
      {
         printf("aimee-kb      autonomous tasks:\n");
         cJSON *slot;
         cJSON_ArrayForEach(slot, bg)
         {
            const char *name = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(slot, "name"));
            const char *desc =
                cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(slot, "descriptor"));
            int elapsed =
                (int)cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(slot, "elapsed_secs"));
            printf("  %-12s  %-40s  %ds\n", name ? name : "?", desc && desc[0] ? desc : "-",
                   elapsed);
         }
      }
   }
}
static void pt_print_provider_list(const char *method, cJSON *resp)
{
   cJSON *providers = cJSON_GetObjectItemCaseSensitive(resp, "providers");
   if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(resp, "json")))
   {
      char *json = cJSON_Print(providers);
      if (json)
      {
         printf("%s\n", json);
         free(json);
      }
   }
   else if (cJSON_IsArray(providers))
   {
      printf("%-20s  %-24s  %-10s  %s\n", "provider", "display", "auth", "status");
      cJSON *p;
      cJSON_ArrayForEach(p, providers)
      {
         const char *name = json_str(p, "name");
         const char *display = json_str(p, "display_name");
         const char *auth = json_str(p, "auth_type");
         const char *env_var = json_str(p, "env_var");
         int available = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(p, "available"));
         printf("%-20s  %-24s  %-10s  %s\n", name, display, auth,
                env_var[0] ? (available ? "[key set]" : "[no key]") : "[local]");
      }
   }
}
static void pt_print_provider_show(const char *method, cJSON *resp)
{
   cJSON *p = cJSON_GetObjectItemCaseSensitive(resp, "provider");
   if (cJSON_IsObject(p))
   {
      printf("name:          %s\n", json_str(p, "name"));
      printf("display_name:  %s\n", json_str(p, "display_name"));
      printf("description:   %s\n", json_str(p, "description"));
      printf("base_url:      %s\n", json_str(p, "base_url"));
      printf("models_url:    %s\n", json_str(p, "models_url"));
      printf("auth_type:     %s\n", json_str(p, "auth_type"));
      printf("default_model: %s\n", json_str(p, "default_model"));
      printf("aux_model:     %s\n", json_str(p, "default_aux_model"));
      cJSON *envs = cJSON_GetObjectItemCaseSensitive(p, "env_vars");
      if (cJSON_IsArray(envs))
      {
         cJSON *env;
         int idx = 0;
         cJSON_ArrayForEach(env, envs)
         {
            if (cJSON_IsString(env))
               printf("env[%d]:        %s\n", idx++, env->valuestring);
         }
      }
   }
}
static void pt_print_provider_models(const char *method, cJSON *resp)
{
   cJSON *models = cJSON_GetObjectItemCaseSensitive(resp, "models");
   if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(resp, "json")))
   {
      char *json = cJSON_Print(models);
      if (json)
      {
         printf("%s\n", json);
         free(json);
      }
   }
   else if (cJSON_IsArray(models))
   {
      cJSON *m;
      cJSON_ArrayForEach(m, models)
      {
         if (cJSON_IsString(m))
            printf("%s\n", m->valuestring);
      }
      printf("%d models\n", cJSON_GetArraySize(models));
   }
}
static void pt_print_provider_test(const char *method, cJSON *resp)
{
   const char *message = json_str(resp, "message");
   if (message[0])
      printf("%s\n", message);
}
static void pt_print_provider_quota(const char *method, cJSON *resp)
{
   const char *quota = json_str(resp, "quota");
   if (quota[0])
      printf("%s", quota);
}
static void pt_print_model_show(const char *method, cJSON *resp)
{
   cJSON *m = cJSON_GetObjectItemCaseSensitive(resp, "model");
   if (cJSON_IsObject(m))
   {
      printf("provider:       %s\n", json_str(m, "provider"));
      printf("model:          %s\n", json_str(m, "model"));
      printf("context_window: %.0f\n",
             cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(m, "context_window")));
      printf("max_output:     %.0f\n",
             cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(m, "max_output")));
      printf("cost_in/mtok:   %.6f\n",
             cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(m, "cost_in_per_mtok")));
      printf("cost_out/mtok:  %.6f\n",
             cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(m, "cost_out_per_mtok")));
      printf("capabilities:   %s\n", json_str(m, "capabilities"));
      printf("modalities:     %s\n", json_str(m, "modalities"));
      printf("cutoff:         %s\n", json_str(m, "knowledge_cutoff"));
      printf("open_weights:   %s\n",
             cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(m, "open_weights")) ? "yes" : "no");
      printf("deprecated:     %s\n",
             cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(m, "deprecated")) ? "yes" : "no");
   }
}
static void pt_print_model_list(const char *method, cJSON *resp)
{
   cJSON *models = cJSON_GetObjectItemCaseSensitive(resp, "models");
   if (cJSON_IsArray(models))
   {
      printf("%-20s  %-14s  %-32s  %-8s  %s\n", "agent", "provider", "model", "context",
             "capabilities");
      cJSON *m;
      cJSON_ArrayForEach(m, models)
      {
         printf("%-20s  %-14s  %-32s  %-8.0f  %s\n", json_str(m, "agent"), json_str(m, "provider"),
                json_str(m, "model"),
                cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(m, "context_window")),
                json_str(m, "capabilities"));
      }
   }
}
static void pt_print_provider_get(const char *method, cJSON *resp)
{
   cJSON *p = cJSON_GetObjectItemCaseSensitive(resp, "provider");
   if (cJSON_IsString(p))
      printf("%s\n", p->valuestring);
}
static void pt_print_provider_set(const char *method, cJSON *resp)
{
   cJSON *p = cJSON_GetObjectItemCaseSensitive(resp, "provider");
   if (cJSON_IsString(p))
      printf("provider: %s\n", p->valuestring);
}
static void pt_print_model_refresh(const char *method, cJSON *resp)
{
   const char *message = json_str(resp, "message");
   if (message[0])
      printf("%s\n", message);
}
static void pt_print_dogfood_tag(const char *method, cJSON *resp)
{
   cJSON *id = cJSON_GetObjectItemCaseSensitive(resp, "record_id");
   printf("tagged %s\n", cJSON_IsString(id) ? id->valuestring : "(unknown)");
}
static void pt_print_dogfood_report(const char *method, cJSON *resp)
{
   cJSON *month = cJSON_GetObjectItemCaseSensitive(resp, "month");
   cJSON *dir = cJSON_GetObjectItemCaseSensitive(resp, "log_dir");
   cJSON *rt = cJSON_GetObjectItemCaseSensitive(resp, "records_total");
   cJSON *rl = cJSON_GetObjectItemCaseSensitive(resp, "records_labelled");
   cJSON *ra = cJSON_GetObjectItemCaseSensitive(resp, "records_auto_labelled");
   cJSON *rz = cJSON_GetObjectItemCaseSensitive(resp, "records_retrieved_zero");
   printf("dogfood report for %s (%s)\n", cJSON_IsString(month) ? month->valuestring : "?",
          cJSON_IsString(dir) ? dir->valuestring : "?");
   printf("  records: %d (labelled: %d, auto-labelled: %d, retrieved-zero: %d)\n",
          cJSON_IsNumber(rt) ? rt->valueint : 0, cJSON_IsNumber(rl) ? rl->valueint : 0,
          cJSON_IsNumber(ra) ? ra->valueint : 0, cJSON_IsNumber(rz) ? rz->valueint : 0);
   cJSON *outcomes = cJSON_GetObjectItemCaseSensitive(resp, "outcomes");
   if (cJSON_IsObject(outcomes))
   {
      printf("  outcomes:\n");
      cJSON *entry;
      cJSON_ArrayForEach(entry, outcomes)
      {
         printf("    %-16s %d\n", entry->string, entry->valueint);
      }
   }
   cJSON *al = cJSON_GetObjectItemCaseSensitive(resp, "auto_labels");
   if (cJSON_IsObject(al) && cJSON_GetArraySize(al) > 0)
   {
      printf("  auto-labels:\n");
      cJSON *entry;
      cJSON_ArrayForEach(entry, al)
      {
         printf("    %-18s %d\n", entry->string, entry->valueint);
      }
   }
   cJSON *armed = cJSON_GetObjectItemCaseSensitive(resp, "review_reminder_armed");
   if (cJSON_IsTrue(armed))
      printf("  review reminder armed\n");
}
static void pt_print_eval_run(const char *method, cJSON *resp)
{
   print_eval_run(resp);
}
static void pt_print_eval_results(const char *method, cJSON *resp)
{
   print_eval_results(resp);
}
static void pt_print_identity_show(const char *method, cJSON *resp)
{
   cJSON *charter = cJSON_GetObjectItemCaseSensitive(resp, "charter");
   cJSON *wp = cJSON_GetObjectItemCaseSensitive(resp, "working_profile");
   char *s = charter ? cJSON_Print(charter) : NULL;
   if (s)
   {
      printf("charter:\n%s\n", s);
      free(s);
   }
   s = wp ? cJSON_Print(wp) : NULL;
   if (s)
   {
      printf("working_profile:\n%s\n", s);
      free(s);
   }
}
static void pt_print_api_status(const char *method, cJSON *resp)
{
   cJSON *report = cJSON_GetObjectItemCaseSensitive(resp, "report");
   if (cJSON_IsString(report) && report->valuestring[0])
   {
      fputs(report->valuestring, stdout);
      size_t len = strlen(report->valuestring);
      if (len == 0 || report->valuestring[len - 1] != '\n')
         fputc('\n', stdout);
   }
}
static void pt_print_primary_set(const char *method, cJSON *resp)
{
   const char *agent = json_str(resp, "agent");
   printf("%s\n", (agent && agent[0]) ? agent : "(none — using default provider)");
}
static void pt_print_identity_snapshot(const char *method, cJSON *resp)
{
   cJSON *path = cJSON_GetObjectItemCaseSensitive(resp, "path");
   printf("wrote %s\n", cJSON_IsString(path) ? path->valuestring : "(unknown)");
}
static void pt_print_identity_diff(const char *method, cJSON *resp)
{
   cJSON *added = cJSON_GetObjectItemCaseSensitive(resp, "added");
   cJSON *removed = cJSON_GetObjectItemCaseSensitive(resp, "removed");
   cJSON *changed = cJSON_GetObjectItemCaseSensitive(resp, "changed");
   cJSON *flips = cJSON_GetObjectItemCaseSensitive(resp, "high_confidence_flips");
   printf("identity diff:\n");
   printf("  added:   %d\n", cJSON_IsArray(added) ? cJSON_GetArraySize(added) : 0);
   printf("  removed: %d\n", cJSON_IsArray(removed) ? cJSON_GetArraySize(removed) : 0);
   printf("  changed: %d\n", cJSON_IsArray(changed) ? cJSON_GetArraySize(changed) : 0);
   int nflips = cJSON_IsArray(flips) ? cJSON_GetArraySize(flips) : 0;
   if (nflips > 0)
      printf("  HIGH CONFIDENCE FLIPS: %d (investigate before continuing)\n", nflips);
}

typedef void (*pt_print_fn)(const char *method, cJSON *resp);
static const struct
{
   const char *method;
   pt_print_fn fn;
} pt_print_table[] = {
    {"init.run", pt_print_init_run},
    {"migrate.v2", pt_print_migrate_v2},
    {"rules.generate", pt_print_rules_generate},
    {"skill.list", pt_print_skill_list},
    {"skill.show", pt_print_skill_show},
    {"git.verify", pt_print_git_verify},
    {"get_help", pt_print_get_help},
    {"server.health", pt_print_server_health},
    {"session.list", pt_print_session_list},
    {"session.get", pt_print_session_get},
    {"session.close", pt_print_session_close},
    {"session.brief", pt_print_session_brief},
    {"session.attach", pt_print_session_attach},
    {"session.detach", pt_print_session_detach},
    {"session.presence", pt_print_session_presence},
    {"trajectory.export", pt_print_trajectory_export},
    {"trajectory.batch", pt_print_trajectory_batch},
    {"insights.overview", pt_print_insights_overview},
    {"worktree.gc", pt_print_worktree_gc},
    {"work.add", pt_print_work_add},
    {"work.add_batch", pt_print_work_add_batch},
    {"work.claim", pt_print_work_claim},
    {"work.complete", pt_print_work_complete},
    {"work.fail", pt_print_work_fail},
    {"work.list", pt_print_work_list},
    {"work.board", pt_print_work_board},
    {"work.cancel", pt_print_work_cancel},
    {"work.release", pt_print_work_release},
    {"work.clear", pt_print_work_clear},
    {"work.gc", pt_print_work_gc},
    {"work.sync_proposals", pt_print_work_sync_proposals},
    {"work.stats", pt_print_work_stats},
    {"rules.delete", pt_print_rules_delete},
    {"memory.search", pt_print_memory_search},
    {"memory.store", pt_print_memory_store},
    {"memory.list", pt_print_memory_list},
    {"memory.get", pt_print_memory_get},
    {"memory.read", pt_print_memory_read},
    {"memory.stats", pt_print_memory_stats},
    {"index.scan", pt_print_index_scan},
    {"index.list", pt_print_index_list},
    {"index.find", pt_print_index_find},
    {"index.blast_radius", pt_print_index_blast_radius},
    {"index.structure", pt_print_index_structure},
    {"index.find_callers", pt_print_index_find_callers},
    {"graph.sync_code", pt_print_graph_sync_code},
    {"workspace.add", pt_print_workspace_add},
    {"workspace.list", pt_print_workspace_list},
    {"workspace.get", pt_print_workspace_get},
    {"workspace.remove", pt_print_workspace_remove},
    {"workspace.mirror-sync", pt_print_workspace_mirror_sync},
    {"hud.status", pt_print_hud_status},
    {"agent.list", pt_print_agent_list},
    {"agent.local", pt_print_agent_local},
    {"agent.add", pt_print_agent_add},
    {"agent.remove", pt_print_agent_remove},
    {"agent.enable", pt_print_agent_enable},
    {"agent.disable", pt_print_agent_disable},
    {"agent.probe", pt_print_agent_probe},
    {"mcp.audit", pt_print_mcp_audit},
    {"mcp.recheck", pt_print_mcp_recheck},
    {"toolset.list", pt_print_toolset_list},
    {"toolset.show", pt_print_toolset_show},
    {"toolset.resolve", pt_print_toolset_resolve},
    {"trigger.list", pt_print_trigger_list},
    {"trigger.status", pt_print_trigger_status},
    {"trigger.fire", pt_print_trigger_fire},
    {"trigger.cancel", pt_print_trigger_cancel},
    {"cron.list", pt_print_cron_list},
    {"cron.add", pt_print_cron_add},
    {"cron.enable", pt_print_cron_add},
    {"cron.disable", pt_print_cron_add},
    {"cron.remove", pt_print_cron_add},
    {"cron.show", pt_print_cron_show},
    {"cron.history", pt_print_cron_history},
    {"cron.run", pt_print_cron_run},
    {"wm.get", pt_print_wm_get},
    {"wm.set", pt_print_wm_set},
    {"wm.list", pt_print_wm_list},
    {"delegate", pt_print_delegate},
    {"delegate.status", pt_print_delegate_status},
    {"jobs.list", pt_print_jobs_list},
    {"jobs.status", pt_print_jobs_status},
    {"jobs.logs", pt_print_jobs_logs},
    {"jobs.cancel", pt_print_jobs_cancel},
    {"job.start", pt_print_job_start},
    {"job.list", pt_print_job_list},
    {"job.status", pt_print_job_status},
    {"job.cancel", pt_print_job_cancel},
    {"aux.config_show", pt_print_aux_config_show},
    {"config.show", pt_print_config_show},
    {"config.get", pt_print_config_get},
    {"config.set", pt_print_config_set},
    {"aux.test", pt_print_aux_test},
    {"delegate.log", pt_print_delegate_log},
    {"episode.list", pt_print_delegate_log},
    {"agent.episodes", pt_print_delegate_log},
    {"delegate.launch", pt_print_delegate_launch},
    {"kb.search", pt_print_kb_search},
    {"kb.build", pt_print_kb_build},
    {"kb.update", pt_print_kb_build},
    {"kb.ingest", pt_print_kb_ingest},
    {"kb.docs.push", pt_print_kb_docs_push},
    {"kb.ingest.status", pt_print_kb_ingest_status},
    {"kb.status", pt_print_kb_status},
    {"workers", pt_print_workers},
    {"provider.list", pt_print_provider_list},
    {"provider.show", pt_print_provider_show},
    {"provider.models", pt_print_provider_models},
    {"provider.test", pt_print_provider_test},
    {"provider.quota", pt_print_provider_quota},
    {"model.show", pt_print_model_show},
    {"model.list", pt_print_model_list},
    {"provider.get", pt_print_provider_get},
    {"provider.set", pt_print_provider_set},
    {"model.refresh", pt_print_model_refresh},
    {"dogfood.tag", pt_print_dogfood_tag},
    {"dogfood.report", pt_print_dogfood_report},
    {"eval.run", pt_print_eval_run},
    {"eval.results", pt_print_eval_results},
    {"identity.show", pt_print_identity_show},
    {"api.status", pt_print_api_status},
    {"api.enable", pt_print_api_status},
    {"api.disable", pt_print_api_status},
    {"primary.set", pt_print_primary_set},
    {"identity.snapshot", pt_print_identity_snapshot},
    {"identity.diff", pt_print_identity_diff},
};

static void print_text_output(const char *method, cJSON *resp)
{
   if (!resp)
      return;
   for (size_t i = 0; i < sizeof(pt_print_table) / sizeof(pt_print_table[0]); i++)
      if (strcmp(method, pt_print_table[i].method) == 0)
      {
         pt_print_table[i].fn(method, resp);
         return;
      }
   if (strncmp(method, "skill.", 6) == 0)
      pt_print_skill_group(method, resp);
}

static const char *delegate_output_path_from_args(int argc, char **argv)
{
   static const char *bool_flags[] = {"json",         "background",   "durable",
                                      "coordination", "plan",         "dry-run",
                                      "tools",        "handoff-json", NULL};
   rpc_opts_t opts;
   rpc_parse(argc, argv, bool_flags, &opts);
   return rpc_get(&opts, "output");
}

static int delegate_timeout_from_args(int argc, char **argv)
{
   static const char *bool_flags[] = {"json",         "background",   "durable",
                                      "coordination", "plan",         "dry-run",
                                      "tools",        "handoff-json", NULL};
   rpc_opts_t opts;
   rpc_parse(argc, argv, bool_flags, &opts);
   return rpc_get_int(&opts, "timeout", 0);
}

static int write_delegate_output_file(const char *path, const char *text)
{
   if (!path || !path[0] || !text)
      return -1;

   char parent[4096];
   snprintf(parent, sizeof(parent), "%s", path);
   char *slash = strrchr(parent, '/');
   if (slash && slash != parent)
   {
      *slash = '\0';
      if (platform_mkdir_p(parent, 0755) != 0)
         return -1;
   }

   FILE *f = fopen(path, "w");
   if (!f)
      return -1;
   fputs(text, f);
   fputc('\n', f);
   fclose(f);
   return 0;
}

#if !defined(_WIN32) && !defined(_WIN64)
/* cli_rpc_http_request: minimal HTTP/1.1 request (any verb) over aimee-server's
 * /v1 Unix socket (<aimee_home>/aimee-http.sock). Kept local to the thin client
 * so the api.client_transport cutover adds no new link dependency
 * (http_uds_client.c is not linked into the CLI). Returns the response body
 * (heap; caller frees) and sets *status_out to the HTTP status; NULL on
 * transport failure. Mirrors http_uds_client.c, which serves the same role for
 * the TUI. */
static char *cli_rpc_http_request(const char *verb, const char *path, const char *body,
                                  int *status_out)
{
   if (status_out)
      *status_out = 0;
   const char *home = aimee_home();
   if (!home || !home[0])
      return NULL;

   char sock_path[512];
   snprintf(sock_path, sizeof(sock_path), "%s/aimee-http.sock", home);

   int fd = socket(AF_UNIX, SOCK_STREAM, 0);
   if (fd < 0)
      return NULL;
   struct sockaddr_un addr;
   memset(&addr, 0, sizeof(addr));
   addr.sun_family = AF_UNIX;
   snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", sock_path);
   if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0)
   {
      close(fd);
      return NULL;
   }

   int blen = body ? (int)strlen(body) : 0;
   char head[512];
   int hlen = snprintf(head, sizeof(head),
                       "%s %s HTTP/1.1\r\nHost: localhost\r\nContent-Type: application/json\r\n"
                       "Content-Length: %d\r\nConnection: close\r\n\r\n",
                       verb, path, blen);
   if (hlen <= 0 || hlen >= (int)sizeof(head))
   {
      close(fd);
      return NULL;
   }

   int off = 0;
   while (off < hlen)
   {
      int n = (int)write(fd, head + off, (size_t)(hlen - off));
      if (n <= 0)
      {
         close(fd);
         return NULL;
      }
      off += n;
   }
   off = 0;
   while (off < blen)
   {
      int n = (int)write(fd, body + off, (size_t)(blen - off));
      if (n <= 0)
      {
         close(fd);
         return NULL;
      }
      off += n;
   }

   size_t cap = 8192, len = 0;
   char *resp = malloc(cap);
   if (!resp)
   {
      close(fd);
      return NULL;
   }
   for (;;)
   {
      if (len + 4096 > cap)
      {
         cap *= 2;
         char *grown = realloc(resp, cap);
         if (!grown)
         {
            free(resp);
            close(fd);
            return NULL;
         }
         resp = grown;
      }
      int n = (int)read(fd, resp + len, 4096);
      if (n <= 0)
         break;
      len += (size_t)n;
   }
   close(fd);
   resp[len] = '\0';

   int status = 0;
   if (sscanf(resp, "HTTP/1.%*d %d", &status) != 1)
   {
      free(resp);
      return NULL;
   }
   if (status_out)
      *status_out = status;
   char *bstart = strstr(resp, "\r\n\r\n");
   char *out = bstart ? strdup(bstart + 4) : strdup("");
   free(resp);
   return out;
}

/* The thin client is a strict /v1 consumer: there is no socket/auto transport
 * selection any more (the legacy NDJSON transport was removed). One-shot
 * commands go over the local aimee-http.sock, or a configured remote /v1
 * endpoint (cli_rpc_client_endpoint) — see cli_rpc_forward below. */
#endif /* !_WIN32 — the aimee-http.sock helpers above are POSIX-only */

/* The remote-endpoint resolvers below are portable (env + aimee.yaml, no UDS)
 * and MUST compile on Windows too: cli_rpc_forward calls them unconditionally
 * and the Windows thin client always takes the remote /v1 path. */

/* cli_rpc_aimee_yaml_value: scan <aimee_home>/aimee.yaml for the first
 * (non-comment) line containing <key> and return its malloc'd, unquoted value,
 * or NULL when absent/empty. The full config_t parser is not linked into the
 * thin client, so the api.client_* settings are read with this lightweight
 * scan. Caller frees. */
static char *cli_rpc_aimee_yaml_value(const char *key)
{
   const char *home = aimee_home();
   if (!home || !home[0])
      return NULL;
   char path[512];
   snprintf(path, sizeof(path), "%s/aimee.yaml", home);
   FILE *fp = fopen(path, "r");
   if (!fp)
      return NULL;

   char *result = NULL;
   char line[512];
   while (fgets(line, sizeof(line), fp))
   {
      const char *p = line;
      while (*p == ' ' || *p == '\t')
         p++;
      if (*p == '#')
         continue; /* comment line */
      char *k = strstr(line, key);
      if (!k)
         continue;
      k += strlen(key);
      while (*k == ' ' || *k == '\t')
         k++;
      char val[256];
      int i = 0;
      while (*k && *k != '\n' && *k != '\r' && *k != '#' && i < (int)sizeof(val) - 1)
      {
         if (*k != '"' && *k != '\'')
            val[i++] = *k;
         k++;
      }
      while (i > 0 && (val[i - 1] == ' ' || val[i - 1] == '\t'))
         i--;
      val[i] = '\0';
      if (val[0])
         result = strdup(val);
      break;
   }
   fclose(fp);
   return result;
}

/* cli_rpc_client_endpoint: a remote aimee-server /v1 endpoint for the HTTP
 * transport — "tcp:host:port" (or an explicit "unix:/path"). AIMEE_API_ENDPOINT
 * overrides; otherwise aimee.api.client_endpoint in aimee.yaml. NULL means no
 * remote is configured and the caller falls back to the local aimee-http.sock.
 * This is the only client path that reaches an aimee-server on another host
 * (e.g. a container's published port); the aimee-http.sock helpers above are
 * loopback-only. Caller frees. */
char *cli_rpc_client_endpoint(void)
{
   const char *env = getenv("AIMEE_API_ENDPOINT");
   if (env && env[0])
      return strdup(env);
   return cli_rpc_aimee_yaml_value("client_endpoint:");
}

/* cli_rpc_client_bearer: bearer token sent with the remote HTTP transport.
 * AIMEE_API_BEARER overrides; otherwise the `bearer_token:` key in aimee.yaml
 * (the same value aimee-server reads for aimee.api.bearer_token). NULL means no
 * Authorization header. Caller frees. */
char *cli_rpc_client_bearer(void)
{
   const char *env = getenv("AIMEE_API_BEARER");
   if (env && env[0])
      return strdup(env);
   return cli_rpc_aimee_yaml_value("bearer_token:");
}

/* cli_rpc_has_remote_endpoint: true when the thin client is configured to reach
 * a remote aimee-server over /v1 (client_transport != socket AND an endpoint is
 * set). The dispatcher uses this to skip the local-socket preflight, which would
 * otherwise fail with "server unavailable" because there is no aimee.sock to
 * find. Non-static so cli_main.c (a separate TU) can call it; mirrors the
 * linkage of cli_rpc_forward below. */
int cli_rpc_has_remote_endpoint(void)
{
#if !defined(_WIN32) && !defined(_WIN64)
   char *ep = cli_rpc_client_endpoint();
   if (ep)
   {
      free(ep);
      return 1;
   }
   return 0;
#else
   /* Windows has no UDS path and no AIMEE_API_ENDPOINT/aimee.yaml config: its
    * remote target comes from aimee_client (AIMEE_SERVER_URL or --server). Report
    * that so the dispatcher skips the (always-failing) local-socket preflight and
    * lets cli_rpc_forward route over the remote /v1 via aimee_client_request. */
   return aimee_client_has_remote();
#endif
}

/* cli_rpc_remote_endpoint_is_tcp: like cli_rpc_has_remote_endpoint, but true only
 * for a genuinely remote "tcp:host:port" endpoint (not a local "unix:" one).
 * Interactive/co-located commands (chat, launch) use this to refuse a remote
 * server cleanly — the agent + tools + worktree run on this host. Non-static so
 * cli_main.c can call it. */
int cli_rpc_remote_endpoint_is_tcp(void)
{
#if !defined(_WIN32) && !defined(_WIN64)
   char *ep = cli_rpc_client_endpoint();
   int is_tcp = ep && strncmp(ep, "tcp:", 4) == 0;
   free(ep);
   return is_tcp;
#else
   return 0;
#endif
}

/* The complete method -> first-class /v1 route map, generated from the server
 * registry (every synchronous rh_dispatch_op route). DO NOT EDIT — see
 * scripts/gen-cli-v1-routes.py; scripts/check-cli-v1-routes.py guards drift. */
#include "cli_v1_routes_gen.inc"

/* First-class /v1 REST route resolvers — SHARED across POSIX and Windows so both
 * thin clients route a method to its dedicated dispatch-backed endpoint instead
 * of tunnelling through POST /v1/rpc. Pure string-map lookups (no platform deps).
 * The HTTP response is byte-identical to the NDJSON socket, and data-write routes
 * are reachable under aimee.api.remote_writes=data (vs /v1/rpc's CAPS_ALL). Both
 * GET and POST routes carry the request body — the server reads it via
 * Content-Length regardless of verb — so param-bearing GET reads (skill.list,
 * work.list, …) keep their filters. */
const char *cli_v1_route_for_method(const char *method, const char **verb_out)
{
   if (verb_out)
      *verb_out = "POST";
   if (!method || !method[0])
      return NULL;
   /* Bespoke-but-dispatch-compatible routes the generator can't auto-detect:
    * the server handler is custom but still echoes the raw dispatch response
    * (e.g. ws_dispatch_args -> loopback_rpc for workspace.add). */
   static const struct
   {
      const char *method;
      const char *verb;
      const char *path;
   } bespoke[] = {
       {"workspace.add", "POST", "/v1/workspaces"},
       /* Detached-workspace runner reverse channel (aimee workspace serve); the
        * REST twins return the same {ok, have_op, op?} / {ok} as the NDJSON ops. */
       {"runner.poll", "POST", "/v1/runner/poll"},
       {"runner.respond", "POST", "/v1/runner/respond"},
       /* Custom-handler routes whose response still matches the dispatch method. */
       {"session.list", "GET", "/v1/sessions"},
       {"kb.search", "POST", "/v1/kb/search"},
       {"kb.status", "GET", "/v1/kb/status"},
       {"memory.recall", "POST", "/v1/memory/recall"},
       {"rules.list", "GET", "/v1/rules"},
   };
   for (size_t i = 0; i < sizeof(bespoke) / sizeof(bespoke[0]); i++)
   {
      if (strcmp(method, bespoke[i].method) == 0)
      {
         if (verb_out)
            *verb_out = bespoke[i].verb;
         return bespoke[i].path;
      }
   }
   for (size_t i = 0; i < sizeof(CLI_V1_GEN_ROUTES) / sizeof(CLI_V1_GEN_ROUTES[0]); i++)
   {
      if (strcmp(method, CLI_V1_GEN_ROUTES[i].method) == 0)
      {
         if (verb_out)
            *verb_out = CLI_V1_GEN_ROUTES[i].verb;
         return CLI_V1_GEN_ROUTES[i].path;
      }
   }
   return NULL;
}

/* {id}-bearing /v1 routes: PREFIX{id}SUFFIX (e.g. /v1/workspaces/{path},
 * /v1/sessions/{id}/attach, /v1/sessions/{id}/primary). Returns the static
 * prefix and fills the verb, the suffix after {id}, and the request field that
 * carries the id ("session_id"; NULL means the first positional arg). NULL = not
 * a path-id route. Dispatch-backed responses, so they parse like the socket. */
const char *cli_v1_pathid_route_for_method(const char *method, const char **verb_out,
                                           const char **suffix_out, const char **id_field_out)
{
   if (verb_out)
      *verb_out = "GET";
   if (suffix_out)
      *suffix_out = "";
   if (id_field_out)
      *id_field_out = NULL;
   if (!method || !method[0])
      return NULL;
   static const struct
   {
      const char *method;
      const char *verb;
      const char *prefix;
      const char *suffix;
      const char *id_field;
   } map[] = {
       {"workspace.get", "GET", "/v1/workspaces/", "", NULL},
       {"workspace.remove", "DELETE", "/v1/workspaces/", "", NULL},
       {"session.attach", "POST", "/v1/sessions/", "/attach", "session_id"},
       {"session.detach", "POST", "/v1/sessions/", "/detach", "session_id"},
       {"primary.get", "GET", "/v1/sessions/", "/primary", "session_id"},
       {"primary.set", "POST", "/v1/sessions/", "/primary", "session_id"},
       {"primary.clear", "DELETE", "/v1/sessions/", "/primary", "session_id"},
   };
   for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++)
   {
      if (strcmp(method, map[i].method) == 0)
      {
         if (verb_out)
            *verb_out = map[i].verb;
         if (suffix_out)
            *suffix_out = map[i].suffix;
         if (id_field_out)
            *id_field_out = map[i].id_field;
         return map[i].prefix;
      }
   }
   return NULL;
}

/* Build the full {id}-bearing URL for `method` into buf (PREFIX + pct(id) +
 * SUFFIX), returning buf and the verb, or NULL if `method` is not a path-id
 * route or its id (req's id_field, else the first positional arg) is missing or
 * too long. */
static const char *cli_v1_pathid_build(const char *method, cJSON *req, char *buf, size_t cap,
                                       const char **verb_out)
{
   const char *suffix = NULL;
   const char *id_field = NULL;
   const char *prefix = cli_v1_pathid_route_for_method(method, verb_out, &suffix, &id_field);
   if (!prefix)
      return NULL;
   const char *id = NULL;
   if (id_field)
   {
      cJSON *f = cJSON_GetObjectItemCaseSensitive(req, id_field);
      id = (cJSON_IsString(f) && f->valuestring) ? f->valuestring : NULL;
   }
   else
   {
      cJSON *aarr = cJSON_GetObjectItemCaseSensitive(req, "args");
      cJSON *a0 = cJSON_IsArray(aarr) ? cJSON_GetArrayItem(aarr, 0) : NULL;
      id = (cJSON_IsString(a0) && a0->valuestring) ? a0->valuestring : NULL;
   }
   if (!id || !id[0])
      return NULL;
   char enc[1024];
   if (cli_v1_pct_encode(id, enc, sizeof(enc)) != 0)
      return NULL;
   if (snprintf(buf, cap, "%s%s%s", prefix, enc, suffix) >= (int)cap)
      return NULL;
   return buf;
}

/* cli_v1_async_route_for_method: a queued (rh_dispatch_op_async) route. POSTing
 * returns a run handle to poll at GET /v1/runs/{id}. NULL = not async. */
const char *cli_v1_async_route_for_method(const char *method, const char **verb_out)
{
   if (verb_out)
      *verb_out = "POST";
   if (!method || !method[0])
      return NULL;
   for (size_t i = 0; i < sizeof(CLI_V1_ASYNC_ROUTES) / sizeof(CLI_V1_ASYNC_ROUTES[0]); i++)
   {
      if (strcmp(method, CLI_V1_ASYNC_ROUTES[i].method) == 0)
      {
         if (verb_out)
            *verb_out = CLI_V1_ASYNC_ROUTES[i].verb;
         return CLI_V1_ASYNC_ROUTES[i].path;
      }
   }
   return NULL;
}

/* cli_v1_send: one /v1 request over the active transport — a remote endpoint
 * (cli_http_request), the co-located local UDS (cli_rpc_http_request), or the
 * Windows remote client (aimee_client_request) — returning the parsed JSON
 * response (caller frees) or NULL on a transport error. Unifies the platform
 * branches so cli_rpc_forward and the async poller share one code path. */
static cJSON *cli_v1_send(const char *remote, const char *bearer, const char *verb,
                          const char *path, const char *body, int timeout_ms, int *status_out)
{
   int status = 0;
   cJSON *resp = NULL;
#if !defined(_WIN32) && !defined(_WIN64)
   if (remote)
   {
      resp = cli_http_request(remote, verb, path, body, bearer, timeout_ms, &status);
   }
   else
   {
      char *r = cli_rpc_http_request(verb, path, body, &status);
      if (r)
      {
         resp = cJSON_Parse(r);
         free(r);
      }
   }
#else
   (void)remote;
   (void)bearer;
   (void)timeout_ms;
   char *r = aimee_client_request(verb, path, body, &status);
   if (r)
   {
      if (status >= 200 && status < 300)
         resp = cJSON_Parse(r);
      free(r);
   }
#endif
   if (status_out)
      *status_out = status;
   return resp;
}

/* cli_v1_run_and_poll: POST an async (rh_dispatch_op_async) route, then poll
 * GET /v1/runs/{id} until the run is terminal and return its `result` — the raw
 * dispatch payload, identical to what the old synchronous /v1/rpc dispatch
 * returned, so the CLI's response handling is unchanged. Caller frees. */
static cJSON *cli_v1_run_and_poll(const char *remote, const char *bearer, const char *verb,
                                  const char *path, const char *body, int timeout_ms)
{
   int status = 0;
   cJSON *queued = cli_v1_send(remote, bearer, verb, path, body, 30000, &status);
   if (!queued)
      return NULL;
   cJSON *idj = cJSON_GetObjectItemCaseSensitive(queued, "id");
   if (!cJSON_IsString(idj) || !idj->valuestring || !idj->valuestring[0])
      return queued; /* not a run handle (e.g. a synchronous error body) — surface it */
   char runpath[160];
   /* run ids are oprun_<digits>_<digits>: URL-safe, no percent-encoding needed. */
   snprintf(runpath, sizeof(runpath), "/v1/runs/%s", idj->valuestring);
   cJSON_Delete(queued);

   int waited = 0;
   const int step_ms = 500;
   for (;;)
   {
      cJSON *snap = cli_v1_send(remote, bearer, "GET", runpath, NULL, 15000, &status);
      if (!snap)
         return NULL;
      cJSON *st = cJSON_GetObjectItemCaseSensitive(snap, "status");
      if (cJSON_IsString(st) &&
          (strcmp(st->valuestring, "completed") == 0 || strcmp(st->valuestring, "failed") == 0 ||
           strcmp(st->valuestring, "cancelled") == 0))
      {
         cJSON *result = cJSON_DetachItemFromObjectCaseSensitive(snap, "result");
         cJSON_Delete(snap);
         /* Completed runs carry the raw dispatch result; a failed/empty run yields
          * an empty object so the caller still gets a well-formed response. */
         return result ? result : cJSON_CreateObject();
      }
      cJSON_Delete(snap);
      if (timeout_ms > 0 && waited >= timeout_ms)
         return NULL;
      cli_rpc_sleep_ms(step_ms);
      waited += step_ms;
   }
}

/* cli_v1_rpc_local: dispatch a pre-marshalled {method, ...params} request to its
 * first-class /v1 route over the co-located transport — the local aimee-http.sock
 * on POSIX, or the configured remote on Windows (no UDS). Replaces the old POST
 * /v1/rpc bridge: the interactive co-located callers (chat, launch, mcp serve,
 * hooks, triggers, gateway, workspace serve) now reach the same dispatch surface
 * via dedicated routes (async methods are polled to completion). Returns the
 * parsed response (caller frees) or NULL. */
cJSON *cli_v1_rpc_local(cJSON *req, int timeout_ms)
{
   if (!req)
      return NULL;
   cJSON *mj = cJSON_GetObjectItemCaseSensitive(req, "method");
   const char *method = (cJSON_IsString(mj) && mj->valuestring) ? mj->valuestring : NULL;
   if (!method)
      return NULL;
   if (timeout_ms <= 0)
      timeout_ms = CLIENT_DEFAULT_TIMEOUT_MS;

   const char *async_verb = NULL;
   const char *async_path = cli_v1_async_route_for_method(method, &async_verb);
   const char *rest_verb = NULL;
   const char *rest_path = NULL;
   char pathid_buf[1100];
   if (!async_path)
      rest_path = cli_v1_pathid_build(method, req, pathid_buf, sizeof(pathid_buf), &rest_verb);
   if (!async_path && !rest_path)
      rest_path = cli_v1_route_for_method(method, &rest_verb);

   char *body = cJSON_PrintUnformatted(req);
   if (!body)
      return NULL;
   cJSON *resp = NULL;
   int status = 0;
   if (async_path)
      resp = cli_v1_run_and_poll(NULL, NULL, async_verb, async_path, body, timeout_ms);
   else if (rest_path)
      resp = cli_v1_send(NULL, NULL, rest_verb, rest_path, body, timeout_ms, &status);
   /* else: no first-class route (e.g. the fire-and-forget chat.graceful_cancel,
    * which has no dispatch handler) — return NULL; callers already tolerate it. */
   free(body);
   return resp;
}

/* --- Main RPC forward function --- */

int cli_rpc_forward(const char *socket_path, const cli_rpc_route_t *route, int json_output,
                    const char *json_fields, const char *response_profile, int argc, char **argv)
{
   (void)json_fields;
   (void)response_profile;

   if (!route || !route->method)
      return -1;
   (void)socket_path; /* strict /v1: local aimee-http.sock or a remote /v1 endpoint */

   /* Skip subcmd args consumed by the route match (1 for single, 2 for compound) */
   int fwd_argc = argc;
   char **fwd_argv = argv;
   for (int skip = route->skip_subcmd; skip > 0 && fwd_argc > 0; skip--)
   {
      fwd_argc--;
      fwd_argv++;
   }
   int effective_json_output = json_output || cli_rpc_args_request_json(fwd_argc, fwd_argv);

   cJSON *req = marshal_request(route->method, fwd_argc, fwd_argv);
   if (!req)
      return 2;

   int timeout = route->timeout_ms > 0 ? route->timeout_ms : CLIENT_DEFAULT_TIMEOUT_MS;
   if (strcmp(route->method, "delegate") == 0)
   {
      int delegate_timeout = delegate_timeout_from_args(fwd_argc, fwd_argv);
      if (delegate_timeout > timeout)
         timeout = delegate_timeout < 2147480000 ? delegate_timeout + 5000 : delegate_timeout;
   }

   cJSON *resp = NULL;

   /* Resolve the method's first-class /v1 route. Strict /v1, no /v1/rpc bridge:
    * every dispatch method has a route (the generated sync map, the async set, the
    * {id}-path map, or the bespoke supplement), enforced by the cli-v1-routes +
    * v1-method-coverage gates. Async (rh_dispatch_op_async) methods return a run
    * handle the poller below drives to completion; sync routes echo the raw
    * dispatch response, so the post-processing is unchanged from the socket path. */
   /* Resolve by the method the request actually dispatches — the body's "method"
    * field, which marshalling may set to a server_method twin (e.g. `git verify`
    * dispatches as mcp.call) — not the CLI route name. */
   cJSON *bm = cJSON_GetObjectItemCaseSensitive(req, "method");
   const char *disp_method =
       (cJSON_IsString(bm) && bm->valuestring && bm->valuestring[0]) ? bm->valuestring
                                                                     : route->method;
   const char *rest_verb = NULL;
   const char *rest_path = NULL;
   const char *async_verb = NULL;
   const char *async_path = cli_v1_async_route_for_method(disp_method, &async_verb);
   char pathid_buf[1100];
   if (!async_path)
      rest_path = cli_v1_pathid_build(disp_method, req, pathid_buf, sizeof(pathid_buf), &rest_verb);
   if (!async_path && !rest_path)
      rest_path = cli_v1_route_for_method(disp_method, &rest_verb);

   char *http_body = cJSON_PrintUnformatted(req);
   cJSON_Delete(req);
   /* Transport target: a configured remote /v1 endpoint ("tcp:host:port" /
    * "unix:path", authed by its bearer) reaches an aimee-server on another host;
    * otherwise the co-located server over the local aimee-http.sock. Windows is
    * always remote (resolved from --server / AIMEE_SERVER_URL inside cli_v1_send). */
   char *remote = cli_rpc_client_endpoint();
   char *bearer = remote ? cli_rpc_client_bearer() : NULL;

   if (http_body && async_path)
   {
      resp = cli_v1_run_and_poll(remote, bearer, async_verb, async_path, http_body, timeout);
   }
   else if (http_body && rest_path)
   {
      /* Send the marshalled body on every first-class route, GET included: the
       * server reads it via Content-Length regardless of verb (rh_dispatch_op), so
       * param-bearing GET reads and {id}-in-URL routes that also carry fields keep
       * them. Retry on transient unavailability (a server restart drops the
       * connection; a full compute queue returns {status:error,"compute queue
       * full"}) — both clear fast. */
      const char *body = http_body;
      static const int retry_delays_ms[] = {200, 500, 1000};
      const int max_retries = (int)(sizeof(retry_delays_ms) / sizeof(retry_delays_ms[0]));
      for (int attempt = 0; attempt <= max_retries; attempt++)
      {
         if (resp)
         {
            cJSON_Delete(resp);
            resp = NULL;
         }
         int http_status = 0;
         resp = cli_v1_send(remote, bearer, rest_verb, rest_path, body, timeout, &http_status);
         if (resp)
         {
            cJSON *st = cJSON_GetObjectItemCaseSensitive(resp, "status");
            cJSON *msg = cJSON_GetObjectItemCaseSensitive(resp, "message");
            int queue_full = http_status >= 200 && http_status < 300 && cJSON_IsString(st) &&
                             strcmp(st->valuestring, "error") == 0 && cJSON_IsString(msg) &&
                             strstr(msg->valuestring, "compute queue full");
            if (!queue_full || attempt == max_retries)
               break;
         }
         if (attempt < max_retries)
            cli_rpc_sleep_ms(retry_delays_ms[attempt]);
      }
   }
   else if (http_body)
   {
      /* No first-class route — unreachable given the coverage gates; surface it
       * rather than silently dropping the command. */
      fprintf(stderr, "aimee: '%s' has no /v1 route\n", route->method);
   }

   free(http_body);
   free(bearer);
   free(remote);

   if (resp == NULL)
   {
      fprintf(stderr,
              "aimee: '%s' could not reach the aimee-server /v1 endpoint (is the server "
              "running? check aimee.api.client_endpoint / AIMEE_API_ENDPOINT for a remote "
              "target).\n",
              route->method);
      return -1;
   }

   /* Protocol version mismatch → fall back */
   cJSON *err = cJSON_GetObjectItemCaseSensitive(resp, "error");
   if (cJSON_IsString(err) && strstr(err->valuestring, "protocol version"))
   {
      cJSON_Delete(resp);
      return -1;
   }

   /* Server-side error */
   cJSON *status = cJSON_GetObjectItemCaseSensitive(resp, "status");
   if (cJSON_IsString(status) && strcmp(status->valuestring, "error") == 0)
   {
      cJSON *msg = cJSON_GetObjectItemCaseSensitive(resp, "message");
      if (cJSON_IsString(msg) && msg->valuestring[0])
         fprintf(stderr, "aimee: %s\n", msg->valuestring);
      cJSON_Delete(resp);
      return 1;
   }

   int exit_rc = 0;
   if (strcmp(route->method, "init.run") == 0)
   {
      cJSON *knowledge = cJSON_GetObjectItemCaseSensitive(resp, "knowledge_ready");
      if (!cJSON_IsTrue(knowledge))
         exit_rc = 1;
   }
   else if (strcmp(route->method, "git.verify") == 0 && git_verify_response_is_failure(resp))
   {
      exit_rc = 1;
   }

   if (!effective_json_output && strcmp(route->method, "delegate") == 0)
   {
      const char *output_path = delegate_output_path_from_args(fwd_argc, fwd_argv);
      if (output_path && output_path[0])
      {
         cJSON *r = cJSON_GetObjectItemCaseSensitive(resp, "response");
         if (!cJSON_IsString(r))
         {
            cJSON_Delete(resp);
            fprintf(stderr, "aimee: delegate returned no response to write\n");
            return 1;
         }
         if (write_delegate_output_file(output_path, r->valuestring) != 0)
         {
            cJSON_Delete(resp);
            fprintf(stderr, "aimee: cannot write to --output path: %s\n", output_path);
            return 1;
         }
         cJSON_Delete(resp);
         fprintf(stderr, "%s\n", output_path);
         return exit_rc;
      }
   }

   if (effective_json_output)
   {
      if (route->extract)
      {
         /* Extract named array from response */
         cJSON *arr = cJSON_DetachItemFromObjectCaseSensitive(resp, route->extract);
         if (arr)
         {
            char *str = cJSON_PrintUnformatted(arr);
            if (str)
            {
               puts(str);
               free(str);
            }
            cJSON_Delete(arr);
         }
         cJSON_Delete(resp);
      }
      else
      {
         /* Strip "status", print remaining object */
         cJSON_DeleteItemFromObjectCaseSensitive(resp, "status");
         char *str = cJSON_PrintUnformatted(resp);
         if (str)
         {
            puts(str);
            free(str);
         }
         cJSON_Delete(resp);
      }
   }
   else
   {
      print_text_output(route->method, resp);
      cJSON_Delete(resp);
   }

   return exit_rc;
}
