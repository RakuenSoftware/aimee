/* ===================================================================
 * /v1 thin-client routing: route CLI subcommands through the server's native /v1 HTTP endpoints.
 * Unported commands fail in cli_main before reaching the server.
 * =================================================================== */

#include "cli_v1_routes_internal.h"
#include "platform_path.h"
#include "cli_client.h"
#define V1_PROTOCOL_VERSION 1
#include "util.h"         /* safe_exec_capture (workspace.mirror-sync ships the client diff) */
#include "aimee_client.h" /* aimee_client_request: transport-agnostic /v1 client (Windows path) */
#include "code_collect.h" /* code_collect_files + code_collect_discover_repos (thin-client push) */
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
void __attribute__((unused)) cli_v1_sleep_ms(int ms)
{
#if defined(_WIN32) || defined(_WIN64)
   Sleep((DWORD)ms);
#else
   usleep((useconds_t)ms * 1000);
#endif
}

/* --- Minimal arg parser (no util.h dependency) --- */

static int rpc_is_bool(const char *name, const char **bool_flags)
{
   if (!bool_flags)
      return 0;
   for (int i = 0; bool_flags[i]; i++)
      if (strcmp(name, bool_flags[i]) == 0)
         return 1;
   return 0;
}

void rpc_parse(int argc, char **argv, const char **bool_flags, rpc_opts_t *out)
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
            if (out->flag_count < V1_MAX_FLAGS)
            {
               out->flags[out->flag_count].raw = flag;
               out->flags[out->flag_count].value = eq + 1;
               out->flag_count++;
            }
         }
         else if (rpc_is_bool(flag, bool_flags))
         {
            if (out->flag_count < V1_MAX_FLAGS)
            {
               out->flags[out->flag_count].raw = flag;
               out->flags[out->flag_count].value = "1";
               out->flag_count++;
            }
         }
         else if (i + 1 < argc)
         {
            if (out->flag_count < V1_MAX_FLAGS)
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
         if (out->pos_count < V1_MAX_POS)
            out->positional[out->pos_count++] = argv[i];
      }
   }
}

const char *rpc_get(const rpc_opts_t *opts, const char *name)
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

int rpc_has_flag(const rpc_opts_t *opts, const char *name)
{
   return rpc_get(opts, name) != NULL;
}

int cli_v1_args_request_json(int argc, char **argv)
{
   for (int i = 0; i < argc; i++)
   {
      if (argv[i] && strcmp(argv[i], "--json") == 0)
         return 1;
   }
   return 0;
}

int rpc_get_int(const rpc_opts_t *opts, const char *name, int def)
{
   const char *v = rpc_get(opts, name);
   return v ? atoi(v) : def;
}

/* Resolve the active session id from --session / $AIMEE_SESSION_ID / "default"
 * (defined below; forward-declared for the session marshalers above it). */
static const char *resolve_session_env(const rpc_opts_t *opts);

/* --- /v1 route table --- */

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
    {"doctor", "forensics", "doctor.forensics", NULL, NULL, 0},
    {"memory", "search", "memory.search", NULL, NULL, 60000},
    {"memory", "recall", "memory.recall", NULL, NULL, 60000},
    {"memory", "store", "memory.store", NULL, NULL, 60000},
    {"memory", "identity", "memory.identity", "memory.user_capture", NULL, 60000},
    {"memory", "prefer", "memory.prefer", "memory.user_capture", NULL, 60000},
    {"memory", "archive", "memory.archive", "memory.user_capture", NULL, 60000},
    {"memory", "list", "memory.list", NULL, "memories", 60000},
    {"memory", "get", "memory.get", NULL, NULL, 60000},
    {"memory", "delete", "memory.delete", NULL, NULL, 60000},
    {"memory", "supersede", "memory.supersede", NULL, NULL, 60000},
    {"memory", "show", "memory.get", NULL, NULL, 60000},
    {"memory", "read", "memory.read", NULL, NULL, 60000},
    {"memory", "embed", "memory.embed", NULL, NULL, 900000},
    {"memory", "stats", "memory.stats", NULL, NULL, 60000},
    {"economizer", "stats", "economizer.stats", NULL, NULL, 60000},
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
    {"index", "span", "index.span", NULL, NULL, 0},
    {"index", "investigate", "index.investigate", NULL, NULL, 0},
    {"index", "hybrid", "index.hybrid", NULL, NULL, 0},
    {"index", "callers", "index.find_callers", NULL, NULL, 0},
    {"index", "deps", "index.deps", NULL, NULL, 0},
    {"workspace", "add", "workspace.add", NULL, NULL, 300000},
    {"workspace", "list", "workspace.list", NULL, NULL, 0},
    {"workspace", "get", "workspace.get", NULL, NULL, 0},
    {"workspace", "remove", "workspace.remove", NULL, NULL, 0},
    {"workspace", "mirror-sync", "workspace.mirror-sync", NULL, NULL, 60000},
    {"graph", "sync-code", "graph.sync_code", NULL, NULL, 300000},
    {"graph", "explain", "graph.explain", NULL, NULL, 60000},
    {"notes", "search", "notes.search", NULL, "notes", 0},
    {"notes", NULL, "notes.list", NULL, "notes", 0},
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
    /* Grant administration. `show` maps to the same method as `list` — it is that listing
     * filtered to one subject, so there is one row shape and one route. */
    /* A DISTINCT method from `list`, resolving to the same route. Sharing the method would
     * leave the marshaller unable to require --subject, and `show` with no subject would
     * silently list everything. */
    {"kb", "ingest", "kb.ingest", NULL, NULL, 30000},
    {"kb", "ingest status", "kb.ingest.status", NULL, NULL, 0},
    {"kb", "reembed", "kb.reembed", NULL, NULL, 900000},
    {"kb", "health", "kb.health", NULL, NULL, 0},
    {"kb", "status", "kb.status", NULL, NULL, 0},
    {"kb", "curator status", "kb.curator", NULL, NULL, 0},
    {"kb", "curator", "kb.curator", NULL, NULL, 0},
    {"kb", "calibrate", "calibration.readiness", NULL, NULL, 0},
    {"kb", "demote", "demotion.check", NULL, NULL, 0},
    {"kb", "ranker export-view", "ranker.export_view", NULL, NULL, 0},
    {"kb", "ranker fit", "ranker.fit", NULL, NULL, 900000},
    {"workers", "", "workers", NULL, NULL, 0},
    {"insights", NULL, "insights.overview", NULL, NULL, 0},
    {"worktree", "gc", "worktree.gc", NULL, NULL, 60000},
    {"pipeline", "start", "pipeline.start", NULL, NULL, 60000},
    {"pipeline", "status", "pipeline.status", NULL, NULL, 0},
    {"pipeline", "list", "pipeline.list", NULL, NULL, 0},
    {"pipeline", "cancel", "pipeline.cancel", NULL, NULL, 0},
    {"pipeline", "resume", "pipeline.resume", NULL, NULL, 0},
    {"pipeline", "advance", "pipeline.advance", NULL, NULL, 300000},
    {"pipeline", "gate", "pipeline.gate", NULL, NULL, 300000},
    {"delegate", "launch", "delegate.launch", NULL, NULL, 300000},
    {"delegate", "status", "delegate.status", NULL, NULL, 0},
    {"delegate", "--list-roles", "model.list", NULL, "agents", 0},
    {"delegate", "log", "delegate.log", NULL, "episodes", 0},
    {"delegate", "history", "delegate.log", NULL, "episodes", 0},
    {"delegate", "sandbox list", "delegate.sandbox_list", NULL, "images", 0},
    {"delegate", "sandbox gc", "delegate.sandbox_gc", NULL, NULL, 60000},
    /* MoA ensemble aggregate: positional[0] is the PROMPT (not a role). Async —
     * forwards to POST /v1/delegate/aggregate and polls GET /v1/runs/{id}. This
     * specific route is what makes `aimee delegate aggregate "<prompt>"` reach the
     * ensemble; without it the prompt fell through to the catch-all delegate
     * route, which maps positional[0] -> role and never ran the engine. */
    {"delegate", "aggregate", "delegate.aggregate", NULL, NULL, 600000},
    {"roundtable", "review", "roundtable.review", NULL, NULL, 900000},
    {"ensemble", "aggregate", "delegate.aggregate", NULL, NULL, 600000},
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
    {"config", "deploy-env", "config.deploy_env", NULL, NULL, 0},
    {"config", "set", "config.set", NULL, NULL, 0},
    {"config", "", "config.show", NULL, NULL, 0},
    {"vault", "unlock", "vault.unlock", NULL, NULL, 0},
    {"vault", "set", "vault.set", NULL, NULL, 0},
    {"vault", "set-server", "vault.set_server", NULL, NULL, 0},
    {"vault", "capability", "vault.capability", NULL, NULL, 0},
    {"vault", "list", "vault.list", NULL, "credentials", 0},
    {"vault", "delete", "vault.delete", NULL, NULL, 0},
    {"vault", "lock", "vault.lock", NULL, NULL, 0},
    {"cert", "issue", "cert.issue", NULL, NULL, 0},
    {"cert", "list", "cert.list", NULL, "certs", 0},
    {"cert", "revoke", "cert.revoke", NULL, NULL, 0},
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
    {"model", "list", "model.list", NULL, "agents", 0},
    {"audit", "verify", "audit.verify", NULL, NULL, 0},
    {"audit", "checkpoint", "audit.checkpoint", NULL, NULL, 0},
    {"audit", "seal", "audit.seal", NULL, NULL, 0},
    {"audit", "snapshot", "audit.snapshot", NULL, NULL, 0},
    {"audit", "", "audit.verify", NULL, NULL, 0},
    {"model", "episodes", "model.episodes", NULL, "episodes", 0},
    {"model", "add", "model.add", NULL, NULL, 300000},
    {"model", "local", "model.local", NULL, NULL, 300000},
    {"model", "remove", "model.remove", NULL, NULL, 0},
    {"model", "enable", "model.enable", NULL, NULL, 0},
    {"model", "roles", "model.roles", NULL, NULL, 0},
    {"model", "personas", "model.personas", NULL, NULL, 0},
    {"model", "disable", "model.disable", NULL, NULL, 0},
    {"model", "probe", "model.probe", NULL, NULL, 300000},
    /* Deprecated alias: the roster tab and its ops were called "agent" before
     * they were named for what a roster entry actually is -- one model. Kept so
     * existing scripts keep working; both spellings reach the same ops. */
    {"agent", "list", "model.list", NULL, "agents", 0},
    {"agent", "episodes", "model.episodes", NULL, "episodes", 0},
    {"agent", "add", "model.add", NULL, NULL, 300000},
    {"agent", "local", "model.local", NULL, NULL, 300000},
    {"agent", "remove", "model.remove", NULL, NULL, 0},
    {"agent", "enable", "model.enable", NULL, NULL, 0},
    {"agent", "roles", "model.roles", NULL, NULL, 0},
    {"agent", "personas", "model.personas", NULL, NULL, 0},
    {"agent", "disable", "model.disable", NULL, NULL, 0},
    {"agent", "probe", "model.probe", NULL, NULL, 300000},
    {"mcp", "audit", "mcp.audit", NULL, "items", 0},
    {"mcp", "recheck", "mcp.recheck", NULL, "items", 300000},
    {"audit", "trace", "evidence.trace_retrieval_event", NULL, NULL, 0},
    {"audit", "provenance", "evidence.provenance_retrieval_event", NULL, NULL, 0},
    {"audit", "fidelity", "evidence.fidelity_retrieval_event", NULL, NULL, 0},
    {"get_help", NULL, "get_help", "help.get", NULL, 0},
    {"get-help", NULL, "get_help", "help.get", NULL, 0},
    {"verify", NULL, "git.verify", "mcp.call", NULL, 900000},
    {"git", "verify", "git.verify", "mcp.call", NULL, 900000},
    /* Every other git subcommand, wildcard so `aimee git <anything>` reaches the
     * server's git tool instead of "not a subcommand of 'git'". MUST stay after
     * the verify row: pass 2 takes the first match, and verify has its own
     * marshaler (--status, async=). Wildcard also means argv[0] is the
     * subcommand, which is what marshal_git_cli needs to pick the tool. */
    {"git", NULL, "git.cli", "mcp.call", NULL, 300000},
    {"provider", "list", "provider.list", NULL, "providers", 300000},
    {"provider", "show", "provider.show", NULL, NULL, 0},
    {"provider", "models", "provider.models", NULL, "models", 300000},
    {"provider", "test", "provider.test", NULL, NULL, 300000},
    {"provider", "quota", "provider.quota", NULL, NULL, 0},
    {"provider", "", "provider.get", NULL, NULL, 0},
    {"provider", NULL, "provider.set", NULL, NULL, 0},
    {"use", NULL, "provider.set", NULL, NULL, 0},
    /* The model CAPABILITY CATALOG (what a model can do: context window,
     * pricing, flags). Distinct from the model ROSTER (`aimee model`),
     * which is the configured fleet. */
    {"catalog", "list", "catalog.list", NULL, "models", 0},
    {"catalog", "show", "catalog.show", NULL, NULL, 0},
    {"catalog", "refresh", "catalog.refresh", NULL, NULL, 300000},
    {"server", "status", "server.health", NULL, NULL, 0},
    {"server", "health", "server.health", NULL, NULL, 0},
    {"status", "", "server.health", NULL, NULL, 0},
    {"api", "status", "api.status", NULL, NULL, 0},
    {"api", "enable", "api.enable", NULL, NULL, 0},
    {"api", "disable", "api.disable", NULL, NULL, 0},
    {"api", "", "api.status", NULL, NULL, 0},
    {"hud", "", "hud.status", NULL, NULL, 0},
    {"dogfood", "tag", "dogfood.tag", NULL, NULL, 0},
    /* Month-scale dogfood logs can take longer than the default request timeout. */
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

/* Collect the subcommands registered for `cmd` into `out` as a comma-separated
 * list, so a failed lookup can say which ones exist instead of blaming the whole
 * command. Rows whose subcmd is NULL (match-any) or "" (bare command) are
 * skipped -- they are not names a user can type. Returns the number found. */
int cli_v1_subcommands(const char *cmd, char *out, size_t cap)
{
   if (out && cap)
      out[0] = '\0';
   if (!cmd)
      return 0;
   int n = 0;
   size_t len = 0;
   /* rpc_routes ends with a {NULL,...} sentinel — stop there, do not walk the
    * array by sizeof or the terminator's NULL cmd reaches strcmp. */
   for (size_t i = 0; rpc_routes[i].cmd; i++)
   {
      if (strcmp(rpc_routes[i].cmd, cmd) != 0)
         continue;
      const char *sub = rpc_routes[i].subcmd;
      if (!sub || !sub[0])
         continue;
      n++;
      if (!out || !cap)
         continue;
      size_t need = strlen(sub) + (len ? 2 : 0);
      if (len + need >= cap)
         continue; /* keep the list truncated rather than overflow */
      if (len)
      {
         memcpy(out + len, ", ", 2);
         len += 2;
      }
      memcpy(out + len, sub, strlen(sub));
      len += strlen(sub);
      out[len] = '\0';
   }
   return n;
}

int cli_v1_lookup(const char *cmd, int sub_argc, char **sub_argv, cli_v1_route_t *route)
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

/* Marshal `aimee delegate-backend exec --backend X --task-id Y
 * [--image I] [--host H] [--no-hibernate] "<cmd>"` into the
 * delegate.backend_exec request JSON. The command is the (last)
 * positional arg. */
cJSON *marshal_delegate_backend_exec(int argc, char **argv)
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

cJSON *marshal_curator_topic(const char *method, int argc, char **argv)
{
   rpc_opts_t opts;
   rpc_parse(argc, argv, NULL, &opts);
   cJSON *req = cJSON_CreateObject();
   cJSON_AddStringToObject(req, "method", method);
   cJSON_AddNumberToObject(req, "protocol_version", V1_PROTOCOL_VERSION);
   if (opts.pos_count > 0)
      cJSON_AddStringToObject(req, "topic", opts.positional[0]);
   return req;
}

cJSON *marshal_curator_contradictions(int argc, char **argv)
{
   rpc_opts_t opts;
   rpc_parse(argc, argv, NULL, &opts);
   cJSON *req = marshal_no_args("curator.contradictions");
   cJSON_AddNumberToObject(req, "limit", rpc_get_int(&opts, "limit", 20));
   return req;
}

cJSON *marshal_index_scan(int argc, char **argv)
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

cJSON *marshal_index_find(int argc, char **argv)
{
   static const char *bools[] = {"json", NULL};
   rpc_opts_t opts;
   rpc_parse(argc, argv, bools, &opts);

   cJSON *req = marshal_no_args("index.find");
   if (opts.pos_count > 0)
      cJSON_AddStringToObject(req, "identifier", opts.positional[0]);
   const char *scope = rpc_get(&opts, "scope");
   if (scope)
      cJSON_AddStringToObject(req, "scope", scope);
   char cwd[4096];
   if (getcwd(cwd, sizeof(cwd)))
      cJSON_AddStringToObject(req, "cwd", cwd);
   return req;
}

cJSON *marshal_index_list(int argc, char **argv)
{
   (void)argc;
   (void)argv;
   cJSON *req = marshal_no_args("index.list");
   return req;
}

static cJSON *marshal_index_file_request(const char *method, int argc, char **argv)
{
   static const char *bools[] = {"json", NULL};
   rpc_opts_t opts;
   rpc_parse(argc, argv, bools, &opts);

   cJSON *req = marshal_no_args(method);
   if (opts.pos_count > 1)
   {
      /* Compatibility: the historic form was <project> <file>. */
      cJSON_AddStringToObject(req, "project", opts.positional[0]);
      cJSON_AddStringToObject(req, "file_path", opts.positional[1]);
   }
   else if (opts.pos_count > 0)
      cJSON_AddStringToObject(req, "file_path", opts.positional[0]);
   char cwd[4096];
   if (getcwd(cwd, sizeof(cwd)))
      cJSON_AddStringToObject(req, "cwd", cwd);
   return req;
}

cJSON *marshal_index_blast_radius(int argc, char **argv)
{
   return marshal_index_file_request("index.blast_radius", argc, argv);
}

/* `aimee index span <file> [start] [end]`, or the historic <project> <file> form
 * with the range after it. Positional rather than flagged because this is meant
 * to be typed inline in a chain of && commands, where three short positionals
 * beat --line-start/--line-end. */
cJSON *marshal_index_span(int argc, char **argv)
{
   static const char *bools[] = {"json", NULL};
   rpc_opts_t opts;
   rpc_parse(argc, argv, bools, &opts);

   cJSON *req = marshal_no_args("index.span");
   int at = 0;
   /* A leading argument that is not a number and is followed by another
    * non-number is the project, matching the other index commands. */
   if (opts.pos_count > 1 && !isdigit((unsigned char)opts.positional[1][0]))
   {
      cJSON_AddStringToObject(req, "project", opts.positional[0]);
      at = 1;
   }
   if (opts.pos_count > at)
      cJSON_AddStringToObject(req, "file_path", opts.positional[at]);
   if (opts.pos_count > at + 1)
      cJSON_AddNumberToObject(req, "line_start", atoi(opts.positional[at + 1]));
   if (opts.pos_count > at + 2)
      cJSON_AddNumberToObject(req, "line_end", atoi(opts.positional[at + 2]));
   char cwd[4096];
   if (getcwd(cwd, sizeof(cwd)))
      cJSON_AddStringToObject(req, "cwd", cwd);
   return req;
}

/* `aimee index investigate "<question>" ["<question>" ...]`.
 *
 * EVERY positional is a question, so several become one invocation and one
 * round trip -- the same reason the MCP tool grew a `queries` array. A single
 * question still works and is sent as `query`, matching the tool's shape. */
/* `aimee index hybrid "<phrase>" ["<phrase>" ...]` -- same positional-as-query
 * shape as investigate, for the same reason: several phrases in one invocation
 * is one round trip. --scope all widens beyond the active project. */
cJSON *marshal_index_hybrid(int argc, char **argv)
{
   static const char *bools[] = {"json", NULL};
   rpc_opts_t opts;
   rpc_parse(argc, argv, bools, &opts);

   cJSON *req = marshal_no_args("index.hybrid");
   if (opts.pos_count == 1)
      cJSON_AddStringToObject(req, "query", opts.positional[0]);
   else if (opts.pos_count > 1)
   {
      cJSON *arr = cJSON_AddArrayToObject(req, "queries");
      for (int i = 0; i < opts.pos_count; i++)
         cJSON_AddItemToArray(arr, cJSON_CreateString(opts.positional[i]));
   }
   const char *scope = rpc_get(&opts, "scope");
   if (scope && scope[0])
      cJSON_AddStringToObject(req, "scope", scope);
   char cwd[4096];
   if (getcwd(cwd, sizeof(cwd)))
      cJSON_AddStringToObject(req, "cwd", cwd);
   return req;
}

cJSON *marshal_index_investigate(int argc, char **argv)
{
   static const char *bools[] = {"json", NULL};
   rpc_opts_t opts;
   rpc_parse(argc, argv, bools, &opts);

   cJSON *req = marshal_no_args("index.investigate");
   if (opts.pos_count == 1)
      cJSON_AddStringToObject(req, "query", opts.positional[0]);
   else if (opts.pos_count > 1)
   {
      cJSON *arr = cJSON_AddArrayToObject(req, "queries");
      for (int i = 0; i < opts.pos_count; i++)
         cJSON_AddItemToArray(arr, cJSON_CreateString(opts.positional[i]));
   }
   char cwd[4096];
   if (getcwd(cwd, sizeof(cwd)))
      cJSON_AddStringToObject(req, "cwd", cwd);
   return req;
}

cJSON *marshal_index_structure(int argc, char **argv)
{
   return marshal_index_file_request("index.structure", argc, argv);
}

cJSON *marshal_index_find_callers(int argc, char **argv)
{
   /* `project` is an OPTIONAL second positional, so an output-mode flag like
    * `--json` must not be mistaken for it: `aimee index callers <symbol> --json`
    * would otherwise send project="--json", which matches no project and returns
    * an empty result. Parse flags out (rpc_parse drops `--json` as a valueless
    * bool) and read only the true positionals. */
   static const char *bools[] = {"json", NULL};
   rpc_opts_t opts;
   rpc_parse(argc, argv, bools, &opts);

   cJSON *req = marshal_no_args("index.find_callers");
   if (opts.pos_count > 0)
      cJSON_AddStringToObject(req, "symbol", opts.positional[0]);
   if (opts.pos_count > 1)
      cJSON_AddStringToObject(req, "project", opts.positional[1]);
   const char *scope = rpc_get(&opts, "scope");
   if (scope)
      cJSON_AddStringToObject(req, "scope", scope);
   char cwd[4096];
   if (getcwd(cwd, sizeof(cwd)))
      cJSON_AddStringToObject(req, "cwd", cwd);
   return req;
}

/* Marshal `aimee index deps <project> [--tier T] [--review] [--reverse] [--dry-run]`
 * (§B). --tier sets the minimum emitted tier (high|medium|tentative); --review
 * lists the AMBIGUOUS queue (status=ambiguous); --reverse inverts direction to list
 * repos that depend ON <project> (direction=in); --dry-run emits every confidence
 * band (down to LOW) plus the AMBIGUOUS candidates inline for offline inspection,
 * writing nothing (acceptance #4). */
cJSON *marshal_index_deps(int argc, char **argv)
{
   static const char *bools[] = {"review", "reverse", "dry-run", NULL};
   rpc_opts_t opts;
   rpc_parse(argc, argv, bools, &opts);

   cJSON *req = marshal_no_args("index.deps");
   if (opts.pos_count > 0)
      cJSON_AddStringToObject(req, "project", opts.positional[0]);
   const char *tier = rpc_get(&opts, "tier");
   if (tier)
      cJSON_AddStringToObject(req, "min_tier", tier);
   if (rpc_get(&opts, "review"))
      cJSON_AddStringToObject(req, "status", "ambiguous");
   if (rpc_get(&opts, "reverse"))
      cJSON_AddStringToObject(req, "direction", "in");
   if (rpc_get(&opts, "dry-run"))
      cJSON_AddBoolToObject(req, "dry_run", 1);
   return req;
}

cJSON *marshal_session_list(int argc, char **argv)
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
   cJSON_AddNumberToObject(req, "protocol_version", V1_PROTOCOL_VERSION);

   const char *sid = opts.pos_count > 0 ? opts.positional[0] : rpc_get(&opts, "session");
   if (sid && sid[0])
      cJSON_AddStringToObject(req, "session_id", sid);
   return req;
}

/* `aimee notes search <query>` -> POST /v1/notes/search {query}.
 * (`aimee notes` -> notes.list is a GET and uses marshal_no_args.) */
cJSON *marshal_notes_search(int argc, char **argv)
{
   rpc_opts_t opts;
   rpc_parse(argc, argv, NULL, &opts);

   cJSON *req = marshal_no_args("notes.search");
   const char *q = opts.pos_count > 0 ? opts.positional[0] : rpc_get(&opts, "query");
   if (q)
      cJSON_AddStringToObject(req, "query", q);
   cJSON_AddNumberToObject(req, "limit", rpc_get_int(&opts, "limit", 20));
   return req;
}

cJSON *marshal_session_get(int argc, char **argv)
{
   return marshal_session_id_request("session.get", argc, argv);
}

cJSON *marshal_session_close(int argc, char **argv)
{
   return marshal_session_id_request("session.close", argc, argv);
}

cJSON *marshal_session_brief(int argc, char **argv)
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
cJSON *marshal_session_attach(int argc, char **argv)
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
cJSON *marshal_session_detach(int argc, char **argv)
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
cJSON *marshal_session_presence(int argc, char **argv)
{
   rpc_opts_t opts;
   rpc_parse(argc, argv, NULL, &opts);

   cJSON *req = marshal_no_args("session.presence");
   const char *owner = rpc_get(&opts, "owner");
   if (owner && owner[0])
      cJSON_AddStringToObject(req, "owner", owner);
   return req;
}

cJSON *marshal_trajectory_export(int argc, char **argv)
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

cJSON *marshal_trajectory_batch(int argc, char **argv)
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

cJSON *marshal_no_args(const char *method)
{
   cJSON *req = cJSON_CreateObject();
   cJSON_AddStringToObject(req, "method", method);
   cJSON_AddNumberToObject(req, "protocol_version", V1_PROTOCOL_VERSION);
   return req;
}

cJSON *marshal_mcp_recheck(int argc, char **argv)
{
   cJSON *req = marshal_no_args("mcp.recheck");
   if (argc > 0 && argv && argv[0] && argv[0][0] && argv[0][0] != '-')
      cJSON_AddStringToObject(req, "name", argv[0]);
   return req;
}

cJSON *marshal_rules_delete(int argc, char **argv)
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

cJSON *marshal_wm_set(int argc, char **argv)
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

cJSON *marshal_wm_get(int argc, char **argv)
{
   rpc_opts_t opts;
   rpc_parse(argc, argv, NULL, &opts);

   cJSON *req = marshal_no_args("wm.get");

   if (opts.pos_count > 0)
      cJSON_AddStringToObject(req, "key", opts.positional[0]);
   cJSON_AddStringToObject(req, "session_id", resolve_session_env(&opts));
   return req;
}

cJSON *marshal_wm_list(int argc, char **argv)
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

cJSON *marshal_config_get(int argc, char **argv)
{
   rpc_opts_t opts;
   rpc_parse(argc, argv, NULL, &opts);

   cJSON *req = marshal_no_args("config.get");
   if (opts.pos_count > 0)
      cJSON_AddStringToObject(req, "key", opts.positional[0]);
   return req;
}

/* Auditable-correctness P1: `aimee audit trace <turn_id>` → the turn_id param. */
cJSON *marshal_audit_trace(int argc, char **argv)
{
   rpc_opts_t opts;
   rpc_parse(argc, argv, NULL, &opts);

   cJSON *req = marshal_no_args("evidence.trace_retrieval_event");
   if (opts.pos_count > 0)
      cJSON_AddStringToObject(req, "turn_id", opts.positional[0]);
   return req;
}

/* Auditable-correctness P2: `aimee audit provenance <turn_id>` → the turn_id param. */
cJSON *marshal_audit_provenance(int argc, char **argv)
{
   rpc_opts_t opts;
   rpc_parse(argc, argv, NULL, &opts);

   cJSON *req = marshal_no_args("evidence.provenance_retrieval_event");
   if (opts.pos_count > 0)
      cJSON_AddStringToObject(req, "turn_id", opts.positional[0]);
   return req;
}

/* Auditable-correctness P3: `aimee audit fidelity <turn_id>` → the turn_id param. */
cJSON *marshal_audit_fidelity(int argc, char **argv)
{
   rpc_opts_t opts;
   rpc_parse(argc, argv, NULL, &opts);

   cJSON *req = marshal_no_args("evidence.fidelity_retrieval_event");
   if (opts.pos_count > 0)
      cJSON_AddStringToObject(req, "turn_id", opts.positional[0]);
   return req;
}

cJSON *marshal_config_set(int argc, char **argv)
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
 * the real /v1 method is chosen from the args. */
cJSON *marshal_primary(int argc, char **argv)
{
   static const char *bools[] = {"show", "clear", NULL};
   rpc_opts_t opts;
   rpc_parse(argc, argv, bools, &opts);

   cJSON *req = cJSON_CreateObject();
   cJSON_AddNumberToObject(req, "protocol_version", V1_PROTOCOL_VERSION);

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

cJSON *marshal_kb_search(int argc, char **argv)
{
   rpc_opts_t opts;
   rpc_parse(argc, argv, NULL, &opts);

   cJSON *req = marshal_no_args("kb.search");

   if (opts.pos_count > 0)
      cJSON_AddStringToObject(req, "query", opts.positional[0]);

   const char *v;
   if ((v = rpc_get(&opts, "project")))
      cJSON_AddStringToObject(req, "project", v);
   if ((v = rpc_get(&opts, "scope")))
      cJSON_AddStringToObject(req, "scope", v);
   char cwd[4096];
   if (getcwd(cwd, sizeof(cwd)))
      cJSON_AddStringToObject(req, "cwd", cwd);
   cJSON_AddNumberToObject(req, "max_results", rpc_get_int(&opts, "max", 10));
   if ((v = rpc_get(&opts, "fusion-mode")))
      cJSON_AddStringToObject(req, "fusion_mode", v);
   if ((v = rpc_get(&opts, "embed")))
      cJSON_AddStringToObject(req, "embedding_command", v);
   return req;
}

cJSON *marshal_kb_build(int argc, char **argv)
{
   static const char *bool_flags[] = {"force", NULL};
   rpc_opts_t opts;
   rpc_parse(argc, argv, bool_flags, &opts);

   cJSON *req = marshal_no_args("kb.build");

   /* MANUAL §7.16 documents `kb build [--path DIR] [--project NAME]`, but only the
    * positional forms were read, so the documented flags parsed into opts.flags and
    * were silently dropped — the build ran against no path at all. Accept the
    * documented flags, falling back to the positional forms that already shipped. */
   const char *path = rpc_get(&opts, "path");
   const char *project = rpc_get(&opts, "project");
   if (!path && opts.pos_count >= 1)
      path = opts.positional[0];
   if (!project && opts.pos_count >= 2)
      project = opts.positional[1];
   if (path)
      cJSON_AddStringToObject(req, "path", path);
   if (project)
      cJSON_AddStringToObject(req, "project", project);
   if (rpc_get(&opts, "force"))
      cJSON_AddTrueToObject(req, "force");
   const char *v;
   if ((v = rpc_get(&opts, "embed")))
      cJSON_AddStringToObject(req, "embedding_command", v);
   return req;
}

cJSON *marshal_kb_update(int argc, char **argv)
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

cJSON *marshal_kb_ingest(int argc, char **argv)
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

/* kb reembed [--confirm] [--force] [--dry-run] [--target-dim N] [--clear-maintenance]
 * The kb keeps the gating; this only carries the operator's intent across. */
cJSON *marshal_kb_reembed(int argc, char **argv)
{
   static const char *bool_flags[] = {"confirm", "force", "dry-run", "clear-maintenance", NULL};
   rpc_opts_t opts;
   rpc_parse(argc, argv, bool_flags, &opts);

   cJSON *req = marshal_no_args("kb.reembed");
   if (rpc_get(&opts, "confirm"))
      cJSON_AddTrueToObject(req, "confirm");
   if (rpc_get(&opts, "force"))
      cJSON_AddTrueToObject(req, "force");
   if (rpc_get(&opts, "dry-run"))
      cJSON_AddTrueToObject(req, "dry_run");
   if (rpc_get(&opts, "clear-maintenance"))
      cJSON_AddTrueToObject(req, "clear_maintenance");
   const char *v;
   if ((v = rpc_get(&opts, "target-dim")))
      cJSON_AddNumberToObject(req, "target_dim", atoi(v));
   return req;
}

/* memory embed --all | <id> — rebuild memory vectors, e.g. after a dim change
 * drops them. Defaults to nothing so a bare invocation is rejected server-side
 * rather than silently re-embedding an entire corpus. */
cJSON *marshal_memory_embed(int argc, char **argv)
{
   static const char *bool_flags[] = {"all", NULL};
   rpc_opts_t opts;
   rpc_parse(argc, argv, bool_flags, &opts);

   cJSON *req = marshal_no_args("memory.embed");
   if (rpc_get(&opts, "all"))
      cJSON_AddTrueToObject(req, "all");
   if (opts.pos_count >= 1)
      cJSON_AddNumberToObject(req, "memory_id", atof(opts.positional[0]));
   const char *v;
   if ((v = rpc_get(&opts, "version")))
      cJSON_AddStringToObject(req, "version", v);
   return req;
}

#define CLI_KB_DOCS_PUSH_MAX_BYTES (2U * 1024U * 1024U)

static char *marshal_read_kb_doc(const char *path, size_t *len_out)
{
   if (len_out)
      *len_out = 0;
   FILE *fp = path && path[0] ? fopen(path, "rb") : NULL;
   if (!fp || fseek(fp, 0, SEEK_END) != 0)
   {
      if (fp)
         fclose(fp);
      return NULL;
   }
   long len = ftell(fp);
   if (len <= 0 || len > (long)CLI_KB_DOCS_PUSH_MAX_BYTES || fseek(fp, 0, SEEK_SET) != 0)
   {
      fclose(fp);
      return NULL;
   }
   char *content = malloc((size_t)len + 1);
   if (!content)
   {
      fclose(fp);
      return NULL;
   }
   size_t nread = fread(content, 1, (size_t)len, fp);
   int failed = ferror(fp) || nread != (size_t)len || memchr(content, '\0', nread) != NULL;
   fclose(fp);
   if (failed)
   {
      free(content);
      return NULL;
   }
   content[nread] = '\0';
   if (len_out)
      *len_out = nread;
   return content;
}

cJSON *marshal_kb_docs_push(int argc, char **argv)
{
   static const char *bool_flags[] = {"json", NULL};
   rpc_opts_t opts;
   rpc_parse(argc, argv, bool_flags, &opts);

   if (opts.pos_count <= 0)
   {
      fprintf(stderr,
              "usage: aimee kb docs push [--scope SCOPE|--project NAME] <file> [file...]\n");
      return NULL;
   }

   cJSON *req = marshal_no_args("kb.docs.push");

   const char *scope = rpc_get(&opts, "scope");
   if (!scope || !scope[0])
      scope = rpc_get(&opts, "project");
   if (scope && scope[0])
      cJSON_AddStringToObject(req, "scope", scope);

   cJSON *paths = cJSON_AddArrayToObject(req, "paths");
   cJSON *documents = cJSON_AddArrayToObject(req, "documents");
   size_t total_bytes = 0;
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
      if (!path || !path[0] || strchr(path, '"') || strchr(path, '\r') || strchr(path, '\n'))
      {
         fprintf(stderr, "aimee: invalid docs path: %s\n", path ? path : "");
         cJSON_Delete(req);
         return NULL;
      }
      size_t content_len = 0;
      char *content = marshal_read_kb_doc(path, &content_len);
      if (!content || total_bytes + content_len > CLI_KB_DOCS_PUSH_MAX_BYTES)
      {
         fprintf(stderr,
                 "aimee: could not read docs path (non-empty text, 2 MiB command limit): %s\n",
                 path);
         free(content);
         cJSON_Delete(req);
         return NULL;
      }
      total_bytes += content_len;
      cJSON_AddItemToArray(paths, cJSON_CreateString(path ? path : ""));
      cJSON *doc = cJSON_CreateObject();
      cJSON_AddStringToObject(doc, "path", path);
      cJSON_AddStringToObject(doc, "content", content);
      cJSON_AddItemToArray(documents, doc);
      free(content);
   }
   return req;
}

cJSON *marshal_kb_status(int argc, char **argv)
{
   rpc_opts_t opts;
   rpc_parse(argc, argv, NULL, &opts);

   cJSON *req = marshal_no_args("kb.status");

   if (opts.pos_count >= 1)
      cJSON_AddStringToObject(req, "project", opts.positional[0]);
   return req;
}

cJSON *marshal_worktree_gc(int argc, char **argv)
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

cJSON *marshal_delegate_sandbox_gc(int argc, char **argv)
{
   static const char *bool_flags[] = {"dry-run", NULL};
   rpc_opts_t opts;
   rpc_parse(argc, argv, bool_flags, &opts);

   cJSON *req = marshal_no_args("delegate.sandbox_gc");

   /* Age floor in days (default 7). 0 means "any age" — prune everything not in use
    * beyond the keep-recent floor. */
   int days = rpc_get_int(&opts, "days", 7);
   if (days < 0)
      days = 0;
   if (days > 3650)
      days = 3650;
   cJSON_AddNumberToObject(req, "max_age_days", days);

   int keep = rpc_get_int(&opts, "keep", 3);
   if (keep < 0)
      keep = 0;
   cJSON_AddNumberToObject(req, "keep", keep);

   if (rpc_get(&opts, "dry-run"))
      cJSON_AddTrueToObject(req, "dry_run");
   return req;
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

cJSON *marshal_insights_overview(int argc, char **argv)
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

static char *marshal_read_file_limited(const char *path, size_t limit)
{
   FILE *f;
   long len;
   char *buf;
   size_t nread;

   if (!path || !path[0])
      return NULL;

   f = fopen(path, "rb");
   if (!f)
      return NULL;
   fseek(f, 0, SEEK_END);
   len = ftell(f);
   fseek(f, 0, SEEK_SET);
   if (len <= 0 || (size_t)len > limit)
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

static char *marshal_read_stdin_limited(size_t limit)
{
   size_t cap = limit < 4095 ? limit + 1 : 4096;
   size_t len = 0;
   char *buf = malloc(cap);
   if (!buf)
      return NULL;
   for (;;)
   {
      if (len == limit)
      {
         int extra = fgetc(stdin);
         if (extra == EOF && !ferror(stdin))
            break;
         free(buf);
         return NULL;
      }
      if (len + 1024 >= cap && cap < limit + 1)
      {
         size_t next = cap * 2;
         if (next > limit + 1)
            next = limit + 1;
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

static char *marshal_read_prompt_file(const char *path)
{
   return marshal_read_file_limited(path, 2u * 1024u * 1024u);
}

static char *marshal_read_prompt_stdin(void)
{
   return marshal_read_stdin_limited(2u * 1024u * 1024u);
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
__attribute__((unused)) static char *cli_v1_shell_quote_inner(const char *raw)
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
   char *esc_sym = cli_v1_shell_quote_inner(sym);
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

/* MoA ensemble aggregate. positional[0] is the prompt (the catch-all delegate
 * marshaller mapped positional[0] -> role, which is the §0.2 routing bug). */
cJSON *marshal_delegate_aggregate(int argc, char **argv)
{
   static const char *bool_flags[] = {"json", NULL};
   rpc_opts_t opts;
   rpc_parse(argc, argv, bool_flags, &opts);
   cJSON *req = marshal_no_args("delegate.aggregate");
   if (opts.pos_count > 0)
      cJSON_AddStringToObject(req, "prompt", opts.positional[0]);
   return req;
}

cJSON *marshal_roundtable_review(int argc, char **argv)
{
   static const char *bool_flags[] = {"json", "apply", "artifact-stdin", NULL};
   rpc_opts_t opts;
   rpc_parse(argc, argv, bool_flags, &opts);
   cJSON *req = marshal_no_args("roundtable.review");

   /* Explicit artifact bytes are authoritative. Paths are read by the thin
    * client and never sent to the server, so a remote appliance cannot replace
    * them with an unrelated workspace artifact. `--artifact -`, positional
    * `-`, and `--artifact-stdin` all read stdin. */
   const char *artifact_path = rpc_get(&opts, "artifact");
   int artifact_stdin = rpc_has_flag(&opts, "artifact-stdin");
   int artifact_positional = 0;
   if (artifact_path && strcmp(artifact_path, "-") == 0)
      artifact_stdin = 1;
   if (artifact_stdin && artifact_path && strcmp(artifact_path, "-") != 0)
   {
      cJSON_Delete(req);
      return NULL;
   }
   if (!artifact_path && opts.pos_count > 0 && opts.positional[0] &&
       strcmp(opts.positional[0], "-") == 0)
   {
      artifact_stdin = 1;
      artifact_positional = 1;
   }
   char *artifact_text = NULL;
   if (artifact_stdin)
      artifact_text = marshal_read_stdin_limited(CLI_V1_MAX_ROUNDTABLE_ARTIFACT);
   else if (artifact_path)
      artifact_text = marshal_read_file_limited(artifact_path, CLI_V1_MAX_ROUNDTABLE_ARTIFACT);
   else if (opts.pos_count > 0 && opts.positional[0])
   {
      FILE *probe = fopen(opts.positional[0], "rb");
      if (probe)
      {
         fclose(probe);
         artifact_positional = 1;
         artifact_text =
             marshal_read_file_limited(opts.positional[0], CLI_V1_MAX_ROUNDTABLE_ARTIFACT);
         if (!artifact_text)
         {
            cJSON_Delete(req);
            return NULL;
         }
      }
   }
   if ((artifact_stdin || artifact_path) && !artifact_text)
   {
      cJSON_Delete(req);
      return NULL;
   }
   if (artifact_text)
      cJSON_AddStringToObject(req, "artifact", artifact_text);

   /* Fold --context-file / --files / --context-dir / --context preloads into the
    * prompt, mirroring marshal_delegate() so both paths ship identical payloads. */
   int prompt_index = artifact_positional ? 1 : 0;
   char *prompt = (opts.pos_count > prompt_index && opts.positional[prompt_index])
                      ? strdup(opts.positional[prompt_index])
                      : NULL;
   char *preload = marshal_build_preload_context(&opts);
   if (preload)
   {
      const char *base = (prompt && prompt[0]) ? prompt : "";
      size_t cap = strlen(base) + strlen(preload) + 64;
      char *combined = malloc(cap);
      if (combined)
      {
         snprintf(combined, cap, "%s%s# Source Packet: Preloaded Context\n%s", base,
                  base[0] ? "\n\n" : "", preload);
         free(prompt);
         prompt = combined;
      }
      free(preload);
   }
   if (prompt && prompt[0])
   {
      if (artifact_text)
         cJSON_AddStringToObject(req, "original_request", prompt);
      else
         cJSON_AddStringToObject(req, "prompt", prompt);
   }
   free(prompt);
   free(artifact_text);
   const char *original_request = rpc_get(&opts, "original-request");
   if (original_request)
   {
      cJSON_DeleteItemFromObjectCaseSensitive(req, "original_request");
      cJSON_AddStringToObject(req, "original_request", original_request);
   }
   const char *mode = rpc_get(&opts, "mode");
   if (mode)
      cJSON_AddStringToObject(req, "mode", mode);
   const char *roundtable = rpc_get(&opts, "roundtable");
   if (roundtable)
      cJSON_AddStringToObject(req, "roundtable", roundtable);
   const char *artifact_stage = rpc_get(&opts, "artifact-stage");
   if (artifact_stage)
      cJSON_AddStringToObject(req, "artifact_stage", artifact_stage);
   const char *workdir = rpc_get(&opts, "workdir");
   if (workdir)
      cJSON_AddStringToObject(req, "workdir", workdir);
   const char *run_id = rpc_get(&opts, "run-id");
   if (run_id)
      cJSON_AddStringToObject(req, "run_id", run_id);
   const char *turns = rpc_get(&opts, "turns");
   if (turns)
      cJSON_AddStringToObject(req, "turns", turns);
   const char *rounds = rpc_get(&opts, "rounds");
   if (rounds)
      cJSON_AddNumberToObject(req, "rounds", atoi(rounds));
   const char *brief = rpc_get(&opts, "brief");
   if (brief)
      cJSON_AddStringToObject(req, "brief", brief);
   const char *brief_json = rpc_get(&opts, "brief-json");
   if (brief_json)
   {
      cJSON *parsed = cJSON_Parse(brief_json);
      if (!parsed)
      {
         fprintf(stderr, "aimee: roundtable review: --brief-json must be valid JSON\n");
         exit(1);
      }
      cJSON_DeleteItemFromObjectCaseSensitive(req, "brief");
      cJSON_AddItemToObject(req, "brief", parsed);
   }
   if (rpc_has_flag(&opts, "apply"))
      cJSON_AddBoolToObject(req, "apply", 1);
   return req;
}

cJSON *marshal_delegate(int argc, char **argv)
{
   static const char *bool_flags[] = {"json",         "background",   "durable", "coordination",
                                      "plan",         "dry-run",      "tools",   "no-tools",
                                      "handoff-json", "prompt-stdin", NULL};
   rpc_opts_t opts;
   rpc_parse(argc, argv, bool_flags, &opts);

   cJSON *req = marshal_no_args("delegate");

   /* --scope is pure ROUTING POLICY: it names a ceiling the seat choice must
    * respect, carries no caller-supplied code, and the server is already the
    * thing picking the seat. So it is forwarded and enforced server-side.
    * Validate here so a typo fails at the client with a usable message rather
    * than being silently ignored by a server that cannot parse it. */
   const char *scope = rpc_get(&opts, "scope");
   if (scope)
   {
      /* Spelled out rather than calling agent_scope_from_string: that lives in
       * agent_config.c, which the thin client deliberately does not link. The
       * server re-parses with the canonical function, so this is only a
       * fail-fast on a typo - keep the two names in step with agent_scope_t. */
      if (strcmp(scope, "bounded") != 0 && strcmp(scope, "whole_task") != 0)
      {
         fprintf(stderr,
                 "aimee: delegate --scope expects \"bounded\" or \"whole_task\", got '%s'\n",
                 scope);
         exit(1);
      }
      cJSON_AddStringToObject(req, "scope", scope);
   }

   /* --verify is NOT forwarded. Verification runs a caller-supplied shell
    * command and its exit status is the sole evidence used to decide a model was
    * inadequate - so honouring it server-side would both execute caller-supplied
    * code on a shared server and hand whoever passes the flag control of
    * escalation, and therefore of spend. The thin client cannot run it either:
    * it links no delegate engine (cmd_agent_delegate.c is in CMD_SRCS, not
    * CLI_SRCS). Until that has a designed home, refuse rather than accept a flag
    * that would be silently ignored. */
   if (rpc_get(&opts, "verify"))
   {
      fprintf(stderr, "aimee: delegate --verify is not supported for a server-routed run: "
                      "verification and one-shot escalation are not implemented server-side, and "
                      "the thin client has no local delegate engine to run them in.\n");
      exit(1);
   }

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
         snprintf(combined, cap, "%s%s# Source Packet: Preloaded Context\n%s", base,
                  base[0] ? "\n\n" : "", preload);
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
   /* Foreground delegate streams over a long-lived connection and has no /v1
    * route; against a remote aimee-server force background so the call uses the
    * /v1/delegate/run route (returns a job_id to poll via `aimee jobs`). */
   if (rpc_get(&opts, "background") || cli_v1_has_remote_endpoint())
      cJSON_AddTrueToObject(req, "background");
   /* --tools forces tools on; --no-tools forces them OFF even for a role that
    * enables tools by default (e.g. `review`) — the right mode for an
    * artifact-provided panel review of an inline diff, which otherwise wanders
    * reading files and returns nothing. Explicit false overrides the role default
    * server-side. */
   /* --no-tools wins over --tools when both are passed (fail-safe: the explicit
    * "off" is honored), matching the foreground delegate path's precedence. */
   if (rpc_get(&opts, "no-tools"))
      cJSON_AddFalseToObject(req, "tools");
   else if (rpc_get(&opts, "tools"))
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

cJSON *marshal_delegate_status(int argc, char **argv)
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

cJSON *marshal_delegate_log(int argc, char **argv)
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

cJSON *marshal_jobs_list(int argc, char **argv)
{
   rpc_opts_t opts;
   rpc_parse(argc, argv, NULL, &opts);

   cJSON *req = marshal_no_args("jobs.list");
   cJSON_AddNumberToObject(req, "limit", rpc_get_int(&opts, "limit", 20));
   return req;
}

cJSON *marshal_coord_jobs_list(int argc, char **argv)
{
   rpc_opts_t opts;
   rpc_parse(argc, argv, NULL, &opts);

   cJSON *req = marshal_no_args("job.list");
   cJSON_AddNumberToObject(req, "limit", rpc_get_int(&opts, "limit", 20));
   return req;
}

cJSON *marshal_coord_job_start(int argc, char **argv)
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

cJSON *marshal_job_id_request(const char *method, int argc, char **argv)
{
   rpc_opts_t opts;
   rpc_parse(argc, argv, NULL, &opts);

   cJSON *req = cJSON_CreateObject();
   cJSON_AddStringToObject(req, "method", method);
   cJSON_AddNumberToObject(req, "protocol_version", V1_PROTOCOL_VERSION);

   const char *job_id = opts.pos_count > 0 ? opts.positional[0] : rpc_get(&opts, "job-id");
   if (job_id && job_id[0])
      cJSON_AddNumberToObject(req, "job_id", atoi(job_id));
   const char *reason = rpc_get(&opts, "reason");
   if (reason && reason[0])
      cJSON_AddStringToObject(req, "reason", reason);
   return req;
}
