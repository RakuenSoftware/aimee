/* server_main.c: aimee-server entry point -- socket lifecycle, signal handling */
#include "aimee.h"
#include <aimee/tools/agent_tools.h>
#include "cli_client.h"
#include "commands.h"
#include "config.h"
#include "config_database.h" /* config_emit_deploy_env_current (--emit-deploy-env) */
#include "config_sections.h"
#include "forge_app_token.h"
#include "modules/git/forge_credentials.h"
#include "modules/git/git_host_cred.h"
#include "modules/git/git_host_resolve.h"
#include "modules/git/git_forge_vault.h"
#include <aimee/git/git_ops.h>
#include "guardrails.h"
#include <aimee/workspace/workspace.h>
#include "kb_client_cache.h"
#include "kb_client_mtls.h"
#include "kb_client_ws.h"
#include "db1.h"
#include "aimee/protocols/mcp/mcp_client_registry.h"
#include "server.h"
#include "server_http.h"
#include "server_kb_heartbeat.h"
#include "cli_session_pty.h"
#include "presence.h"
#include "turn_registry.h"
#include "events.h"
#include "agent_exec.h"
#include "log.h"
#include <aimee/audit/audit_action.h>
#include "platform_path.h"
#include "platform_process.h"
#include "platform_random.h"
#include "shutdown_forensics.h"
#include "headers/context_engine.h"
#include "headers/server_cli_oauth.h"
#include "vault_server_key.h"
#include "vault_service.h"          /* VAULT_SERVER_PRINCIPAL (rotation target) */
#include "vault_env_bootstrap.h"    /* first-boot credential env -> Vault */
#include "vault_config_bootstrap.h" /* legacy config credential -> Vault */
#include "vault_bootstrap_privilege.h"
#include "runtime_secret.h"       /* wipe Vault-sourced runtime cache at exit */
#include "vault_audit_bridge.h"   /* route vault credential-access events onto the audit bus */
#include "sandbox_audit_bridge.h" /* route sandbox degraded-isolation events onto the audit bus */
#include "memory_audit_bridge.h"  /* route server-side memory mutations onto the audit bus */
#include "tool_completion_audit_bridge.h" /* route tool-dispatch outcomes onto the audit bus */
#include "obs_bus_adapter.h"              /* bind shared bus events to server-owned durable sinks */
#include "module_routing_adapter.h"       /* route selection through the local routing process */
#include "module_stage_adapters.h"        /* process-owned stage decisions */
#include <aimee/audit/obs_bus.h>
#include <aimee/audit/audit_replay.h> /* --audit-replay: inspect a governed-action capture file */
#include <signal.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Platform-specific server helpers (posix/server_main.c, windows/server_main.c) */
void platform_server_redirect_stderr(FILE *log_fp);
void platform_server_install_signals(void (*handler)(int));
int platform_server_service_dispatch(const char *socket_path, log_level_t log_level,
                                     int (*run_server_fn)(const char *, log_level_t));

static server_ctx_t g_ctx;

/* Unified-presence outbound delivery: route a presence event (e.g. a delegate
 * completion) to a surface's persistent messaging target by reusing the
 * existing typed notify path (ntfy / local). Non-ntfy/local targets (e.g.
 * telegram, which needs the out-of-process gateway) are a no-op here. */
static int presence_deliver_via_notify(const delivery_target_t *target, const char *text,
                                       void *user)
{
   (void)user;
   if (!target)
      return -1;
   notify_target_t nt;
   memset(&nt, 0, sizeof(nt));
   nt.is_delivery = 1;
   nt.target = *target;
   return notify_deliver_target(&nt, "delegate_done", text);
}

/* The process-wide server context, for first-class /v1 route handlers that
 * dispatch through server_dispatch() in-process. */
server_ctx_t *server_active_ctx(void)
{
   return &g_ctx;
}

static int startup_notify_fd(void)
{
   const char *env = getenv("AIMEE_SERVER_STARTUP_FD");
   if (!env || !env[0])
      return -1;
   int fd = atoi(env);
   return fd >= 0 ? fd : -1;
}

static void startup_notify(int fd, const char *message)
{
   if (fd < 0)
      return;
   if (message && message[0])
      (void)write(fd, message, strlen(message));
   close(fd);
}

/* Set by SIGHUP (async-signal-safe: just a flag); the server main loop observes it and calls
 * config_reload() off the signal path (config_reload takes a mutex / does I/O). */
volatile sig_atomic_t g_config_reload_requested = 0;

#ifndef _WIN32
static void signal_handler_info(int sig, siginfo_t *info, void *ucontext)
{
   (void)ucontext;
#ifdef SIGHUP
   if (sig == SIGHUP)
   {
      g_config_reload_requested = 1; /* reload config, NOT shut down */
      return;
   }
#endif
   (void)shutdown_forensics_record_signal("server", sig, info, g_ctx.start_time,
                                          g_ctx.active_sessions, 0, g_ctx.session_threads);
   g_ctx.running = 0;
}

static void install_signal_handlers(void)
{
   struct sigaction sa;
   memset(&sa, 0, sizeof(sa));
   sa.sa_sigaction = signal_handler_info;
   sigemptyset(&sa.sa_mask);
   sa.sa_flags = SA_SIGINFO;
   sigaction(SIGTERM, &sa, NULL);
   sigaction(SIGINT, &sa, NULL);
#ifdef SIGHUP
   sigaction(SIGHUP, &sa, NULL);
#endif
   signal(SIGPIPE, SIG_IGN);
}
#else
static void signal_handler(int sig)
{
   (void)sig;
   g_ctx.running = 0;
}

static void install_signal_handlers(void)
{
   platform_server_install_signals(signal_handler);
}
#endif

static int run_server(const char *socket_path, log_level_t log_level)
{
   int notify_fd = startup_notify_fd();

   /* Redirect stderr to log file so server messages never leak to the user's
    * terminal -- regardless of how the server was started. */
   {
      char log_path[4096];
      snprintf(log_path, sizeof(log_path), "%s/server.log", config_default_dir());
      FILE *log_fp = fopen(log_path, "a");
      if (log_fp)
      {
         setvbuf(log_fp, NULL, _IOLBF, 0);
         platform_server_redirect_stderr(log_fp);
         fclose(log_fp);
         /* stderr is now this file, so it is ours to bound. Only registered
          * here: the CLI's stderr is the user's terminal. */
         log_set_rotating_sink(log_path);
      }
   }

   /* Initialize logging */
   log_init(log_level);
   audit_log_open();
   if (server_obs_bus_configure() != 0)
      LOG_WARN("obs_bus", "shared event bus was already started before server sink configuration");
   if (obs_bus_configure_daemon_module_runtime("server", config_default_dir()) != 0)
   {
      startup_notify(notify_fd, "error: invalid server module-bus path configuration\n");
      audit_log_close();
      return 1;
   }
   audit_ensure_key();             /* provision the per-action audit key (best-effort) */
   vault_audit_bridge_install();   /* route vault credential-access events onto the audit bus */
   sandbox_audit_bridge_install(); /* route sandbox degraded-isolation events onto the audit bus */
   memory_audit_bridge_install();  /* route server-side memory mutations onto the audit bus */
   tool_completion_audit_bridge_install(); /* route tool-dispatch outcomes onto the audit bus */

   /* Credential env vars are deployment bootstrap transport (for example a
    * Kubernetes Secret), never runtime storage. Seal and unset them before any
    * config or subsystem can read the process environment. */
   if (vault_env_bootstrap_init() < 0)
   {
      startup_notify(notify_fd, "error: credential Vault bootstrap failed\n");
      audit_log_close();
      return 1;
   }
   if (vault_config_bootstrap_init() < 0)
   {
      startup_notify(notify_fd, "error: credential config Vault migration failed\n");
      audit_log_close();
      return 1;
   }
   (void)atexit(runtime_secret_clear);

   /* Activate the GitHub App installation-token provider for the server's forge
    * identity. Inert unless AIMEE_FORGE_APP_* is set (see forge_app_token.c). */
   forge_cred_register_app_token_provider(forge_app_token_configured, forge_app_token_get);
   forge_cred_register_static_token_provider(git_forge_vault_server_token);

   /* Wire the per-host git credential vault into the credential resolvers so git
    * clone/fetch/push/PR authenticate with the stored token for the repo's host
    * (git_host_resolve.c). Without this the per-host step is skipped. */
   git_host_resolve_register(git_host_cred_for_url);

   /* Wire the per-session worktree resolver so the webchat git surfaces (git panel
    * + editor) act on the SAME isolated worktree the session's agent edits, rather
    * than the shared project checkout (session_isolation_target, workspace.c). */
   git_ops_register_session_isolation(session_isolation_target);

   /* Fail closed on an undeclared tool BEFORE serving anything. The
    * externalization gate consults the egress declaration registry, so a
    * built-in tool with no declaration would be an ungated egress path. Refusing
    * to start is the whole point: the alternative is a silent bypass that looks
    * healthy. Cheap (a few dozen string compares) and has no config dependency,
    * so it runs first. */
   {
      char egress_err[256] = "";
      if (agent_tools_validate_egress_table(egress_err, sizeof(egress_err)) != 0)
      {
         startup_notify(notify_fd, "error: tool egress declaration invariant violated\n");
         aimee_log(LOG_ERROR, "tools", "server startup rejected: %s", egress_err);
         audit_log_close();
         return 1;
      }
   }

   if (!config_present())
   {
      startup_notify(notify_fd, "error: invalid configuration\n");
      aimee_log(LOG_ERROR, "config", "server startup rejected invalid configuration");
      audit_log_close();
      return 1;
   }
   /* An API bearer is a credential and therefore cannot live in the image or
    * config. When an API listener is configured without an
    * operator-supplied first-boot bearer, mint an unexposed random primary and
    * seal it directly into Vault. The trusted local UI/socket can then issue an
    * individual enrollment bearer; `aimee api enable` can reveal the primary to
    * a local operator when a headless deployment explicitly needs it. */
   if ((config_server_api_http_port() > 0 || config_server_api_tls_port() > 0) &&
       !config_server_api_bearer_token()[0])
   {
      char primary[65] = "";
      if (platform_random_hex(primary, 64) != 0 ||
          vault_runtime_secret_set("AIMEE_API_BEARER_TOKEN", primary) != 0)
      {
         runtime_secret_wipe(primary, sizeof(primary));
         startup_notify(notify_fd, "error: API primary bearer Vault bootstrap failed\n");
         audit_log_close();
         return 1;
      }
      runtime_secret_wipe(primary, sizeof(primary));
      /* No need to write it back here: the seed below reloads, and config_load
       * applies AIMEE_API_BEARER_TOKEN out of Vault, so the snapshot every reader
       * below sees carries the freshly minted primary. */
      aimee_log(LOG_INFO, "vault.env", "minted Vault-only API primary for configured listener");
   }
   /* Seed the live config snapshot (live-config-reload P1b): from here, every config read in
    * the server comes from this snapshot, and config_reload (on config.set / SIGHUP) republishes
    * it so changes take effect immediately instead of on the next mtime-cache miss. */
   (void)config_snapshot_seed();
   kb_client_mtls_pool_register_reload();
   /* NOTE: the autonomy.* env bridge (autonomy_config_to_env) is intentionally NOT called —
    * wfe now reads autonomy.* LIVE from the config snapshot via config_autonomy_lookup (an
    * operator-exported AIMEE_AUTONOMY_* still overrides), so a config.set applies without a
    * restart and without an unsafe cross-thread setenv. */

   /* Surface the active fail-closed economizer mode at startup. */
   aimee_log(LOG_INFO, "economizer", "mode=%s", econ_mode_name(econ_mode_current()));

   /* Remote aimee-kb: when a kb_client_url is configured (this host uses a
    * remote kb rather than a local sidecar), export it into our own env so the
    * env-based kb_client transport (kb_client_v1_base_url / auth_header)
    * reaches the remote kb on every launch path — systemd AND the fork-and-exec
    * fallback. Pre-set env wins, so AIMEE_KB_API_URL still overrides config. */
   if (!(getenv("AIMEE_KB_API_URL") && getenv("AIMEE_KB_API_URL")[0]))
   {
      if (config_kb_client_url()[0])
         platform_setenv("AIMEE_KB_API_URL", config_kb_client_url());
      else
      {
         /* Co-located default: with no remote kb_client_url and no explicit
          * AIMEE_KB_API_URL, point at the local aimee-kb sidecar. The systemd
          * unit, the launchd plist, and the fork-and-exec fallback all serve it
          * on 127.0.0.1:8741 (kb_api_http_port if the operator overrode it).
          * Without this a source install's server has no kb URL at all and every
          * DB2-backed feature (memory/kb/rules) silently reports "unavailable". */
         char local_kb[64];
         int kb_port = config_kb_api_http_port() > 0 ? config_kb_api_http_port() : 8741;
         snprintf(local_kb, sizeof(local_kb), "http://127.0.0.1:%d", kb_port);
         platform_setenv("AIMEE_KB_API_URL", local_kb);
      }
   }
   /* KB bearer credentials are consumed through runtime_secret. Never export a
    * config value back into the process environment. */

   /* Result cache + invalidation subscriber: a short-TTL LRU of kb read results
    * (AIMEE_KB_CACHE_TTL_S; off by default) flushed on the kb's /v1/events push,
    * so caching beats TTL-only staleness. No-op unless enabled + an HTTP kb. */
   kb_cache_configure(-1);
   kb_client_ws_start();

   /* Register the bundled context engine and set the active engine from config
    * (empty = default compactor). */
   context_engine_register_compactor();
   if (config_context_engine()[0])
      context_engine_set_active(config_context_engine());

   /* Clear the cached audit_action/audit_worm gates on config reload so a live
    * config.set / SIGHUP toggles the audit + WORM dual-write without a restart. */
   guardrails_action_audit_register_reload();

   /* DB2 + pgvector startup and supervision are owned by aimee-kb, not
    * aimee-server.  Keep the server on the DB1 side of the service split. */

   if (!socket_path)
      socket_path = cli_default_socket_path();

   if (obs_bus_start() != 0)
   {
      startup_notify(notify_fd, "error: server module bus failed to start\n");
      agent_http_cleanup();
      mcp_client_registry_shutdown();
      audit_log_close();
      return 1;
   }
   server_module_routing_configure();
   server_module_stage_adapters_configure();

   /* Initialize server first — creates the Unix socket so clients can connect
    * (and queue in the listen backlog) while HTTP/SSL initializes. */
   if (server_init(&g_ctx, socket_path) != 0)
   {
      char msg[256];
      int saved_errno = errno;
      if (saved_errno)
         snprintf(msg, sizeof(msg), "error: server initialization failed: %s\n",
                  strerror(saved_errno));
      else
         snprintf(msg, sizeof(msg), "error: server initialization failed\n");
      startup_notify(notify_fd, msg);
      obs_bus_stop();
      return 1;
   }

   mcp_client_registry_boot(CONFIG_MCP_INSTALL_SERVER); /* server-hosted plugins only */
   agent_http_init();
   presence_init(); /* unified-presence registry (attachments, turn locks, event ring) */
   presence_set_delivery_fn(presence_deliver_via_notify, NULL); /* outbound: ntfy/local */
   turn_registry_init(); /* per-turn cancel registry (server-owned turn lifecycle) */

   /* Modules declare their commands over the EVENT BUS when they connect -- a
    * module never links into core and never calls another module. The memory
    * module answers stage 6 (event 5894) with its declaration; see
    * server-go/modules/memory/commands.go. An earlier version of this called a C
    * registration function in-process, which is the arrangement that replaced. */

   /* Wire the inference-backed OpenAI completion handlers before the listener
    * accepts requests (agent_http_init above must run first). */
   openai_chat_register();
   anthropic_http_register(); /* Anthropic Messages API ingress (Claude Code) */
   server_native_register();  /* native /v1 REST providers (e.g. GET /v1/rules) */

   /* Inbound /v1 HTTP API (UDS always; optional localhost TCP + bearer when
    * aimee.api.{http_port,bearer_token} are configured). Best-effort: a bind
    * failure must not block the RPC server. */
   server_http_set_max_event_streams(config_server_api_max_event_streams());
   /* Publish the additionally-accepted bearers BEFORE the listener binds, so a
    * client enrolled in a previous run authorizes on its very first request
    * rather than getting a 401 until something else republished them. */
   {
      /* Copy each bearer out of the accessor's thread-local buffer: extra[] is an
       * array of borrowed pointers handed to server_http_set_bearer_extra, and the
       * next config_server_api_bearer_extra() call would overwrite the one buffer
       * they would all be pointing at. */
      char slots[AIMEE_API_BEARER_EXTRA_MAX][256];
      const char *extra[AIMEE_API_BEARER_EXTRA_MAX];
      int extra_count = config_server_api_bearer_extra_count();
      if (extra_count > AIMEE_API_BEARER_EXTRA_MAX)
         extra_count = AIMEE_API_BEARER_EXTRA_MAX;
      for (int i = 0; i < extra_count; i++)
      {
         snprintf(slots[i], sizeof(slots[i]), "%s", config_server_api_bearer_extra(i));
         extra[i] = slots[i];
      }
      server_http_set_bearer_extra(extra, extra_count);
   }
   cli_session_pty_set_forwarding(config_server_api_cli_session_forwarding());
   int http_start =
       server_http_start(NULL, config_server_api_http_port(), config_server_api_tls_port(),
                         config_server_api_bearer_token(), config_server_api_rate_limit_per_min(),
                         config_server_api_remote_writes());
   if (http_start == SERVER_HTTP_START_MGMT_FATAL)
   {
      char management_error[256];
      snprintf(management_error, sizeof(management_error),
               "error: dedicated management listener failed at %s\n",
               server_http_management_last_error());
      startup_notify(notify_fd, management_error);
      server_http_stop();
      server_shutdown(&g_ctx);
      agent_http_cleanup();
      mcp_client_registry_shutdown();
      obs_bus_stop();
      audit_log_close();
      return 1;
   }
   if (http_start != 0)
      LOG_WARN("server.http", "failed to start inbound /v1 HTTP listener");

   /* Install signal handlers */
   install_signal_handlers();

   g_ctx.running = 1;
   if (server_kb_heartbeat_start() != 0)
      LOG_WARN("server.kb", "could not start the server registry heartbeat worker");
   (void)shutdown_forensics_record_unclean_exits();
   (void)shutdown_forensics_mark_started("server", g_ctx.start_time);
   startup_notify(notify_fd, "ok\n");
   int rc = server_run(&g_ctx);

   server_kb_heartbeat_stop();
   server_http_stop();
   server_shutdown(&g_ctx);
   (void)shutdown_forensics_mark_stopped("server", getpid());
   agent_http_cleanup();
   mcp_client_registry_shutdown();
   obs_bus_stop();
   audit_log_close();

   return rc < 0 ? 1 : 0;
}

int main(int argc, char **argv)
{
   /* The container entrypoint must scrub its own inherited environment after
    * the short-lived Vault bootstrap returns. Emit names only, never values, so
    * it does not need to pipe credential plaintext through env/sed. Reject a
    * non-shell identifier rather than leave a credential the parent cannot
    * safely unset. */
   if (argc >= 2 && strcmp(argv[1], "--list-credential-env-names") == 0)
      return vault_env_print_credential_names() == 0 ? 0 : 1;

   /* Emit the compose env for this backend record, for the container entrypoint
    * to write beside the managed compose file as its `.env`.
    *
    * A ONE-SHOT FLAG RATHER THAN THE /v1 ROUTE, because this runs BEFORE the
    * server is listening: the entrypoint has to produce the file at start, and
    * anything that needed a running server could not. It loads config directly,
    * which is the same source config.deploy_env serves later.
    *
    * WHY THE FILE HAS TO BE DERIVED AT EVERY START. The managed deployment's
    * identity -- which kb image variant, which embedder -- lived ONLY in the
    * running container's Config.Env, put there by whichever shell first ran
    * compose. Rebooting is safe (restart=unless-stopped restarts the same
    * container object, env intact), but RECREATING is not, and recreating is
    * what every image upgrade does. `docker compose up -d` with a different
    * caller environment silently reinterpolates:
    *
    *   EMBEDDER_MODEL   unset -> the kb refuses to serve. Loud, recoverable.
    *   AIMEE_KB_VARIANT unset -> ${AIMEE_KB_VARIANT:+-...} resolves to the
    *                             EMBEDDERLESS aimee-kb image. Silent, and the
    *                             deployment quietly loses its embedder.
    *
    * Compose reads `.env` from the project directory automatically, so writing
    * it at start makes EVERY later `docker compose up -d` correct -- the
    * server's own deploy, an operator's, or a script's -- with nobody having to
    * remember to re-supply anything. Swapping an image becomes what it should
    * have been all along: a restart, not a reconfiguration.
    *
    * /opt/aimee/deploy is image content, not a mount, so the file is rebuilt on
    * every start and can never go stale against a config the operator changed
    * while the container was down. Being ephemeral is the point, not a flaw.
    *
    * No secret is written: config_emit_deploy_env deliberately omits
    * embedder_api_key and synthesis_api_key, and the managed-inference bearer is
    * added to the deploy child's envp only (deploy_apply.c), never to a file. */
   if (argc == 2 && strcmp(argv[1], "--emit-deploy-env") == 0)
   {
      char env[4096];
      config_emit_deploy_env_current(env, sizeof(env));
      fputs(env, stdout);
      return 0;
   }

   /* The co-located root-UDS-attested web service consumes these labelled base64
    * records through a pipe for authentication, signed sessions, and in-memory
    * TLS setup. The helper is intentionally webchat-specific: there is no
    * general CLI for printing server-principal Vault secrets. */
   if (argc == 2 && strcmp(argv[1], "--webchat-vault-export") == 0)
      return vault_env_print_webchat_bootstrap() == 0 ? 0 : 1;

   if (argc == 2 && strcmp(argv[1], "--webchat-vault-check") == 0)
      return vault_env_check_webchat_bootstrap() == 0 ? 0 : 1;

   /* One-time migration/onboarding bridge. The credential travels on stdin,
    * never argv or env, and the closed record-name allowlist is enforced by the
    * Vault module. */
   if (argc == 3 && strcmp(argv[1], "--webchat-vault-seal") == 0)
      return vault_env_seal_webchat_record(argv[2]) == 0 ? 0 : 1;

   /* Container/POD entrypoints use this short-lived process to consume
    * first-boot credential inputs before launching any long-lived process. */
   if (argc >= 2 && strcmp(argv[1], "--bootstrap-vault-env") == 0)
   {
      const char *drop_user = NULL;
      if (vault_bootstrap_parse_args(argc, argv, &drop_user) != 0)
      {
         fprintf(stderr, "aimee-server: invalid Vault bootstrap arguments\n");
         return 1;
      }
      if (vault_bootstrap_run_as(drop_user) != 0)
      {
         fprintf(stderr, "aimee-server: Vault bootstrap privilege drop failed\n");
         return 1;
      }
      if (vault_env_bootstrap_init() < 0 || server_vault_bootstrap_prepare() < 0 ||
          vault_config_bootstrap_init() < 0)
         return 1;
      runtime_secret_clear();
      return 0;
   }

   /* Docker Compose bootstrap helper: credentials arrive on a pipe as
    * NUL-delimited environment records, so no value is retained in container
    * Config.Env. This process is one-shot and exits immediately after sealing. */
   if (argc == 2 && strcmp(argv[1], "--bootstrap-vault-stdin") == 0)
   {
      if (vault_env_bootstrap_init() < 0 || server_vault_bootstrap_prepare() < 0 ||
          vault_env_import_stream(stdin) < 0 || vault_env_bootstrap_init() < 0 ||
          server_vault_bootstrap_prepare() < 0 || vault_config_bootstrap_init() < 0 ||
          vault_env_has_credential_environment() != 0)
      {
         runtime_secret_clear();
         return 1;
      }
      runtime_secret_clear();
      return 0;
   }

   /* unsetenv() removes a credential from future lookups but cannot guarantee
    * that Linux /proc/<pid>/environ stops exposing the original inherited
    * bytes. A normal server invocation that inherited first-boot credentials
    * is therefore a disposable bootstrap process: seal everything, verify the
    * environment is clean, then replace this process image. The re-executed
    * long-lived server inherits no credential variables and loads values back
    * only through Vault's locked runtime cache. Container/Kubernetes startup
    * uses the same process boundary in server-entrypoint.sh. */
   {
      int credential_env = vault_env_has_credential_environment();
      if (credential_env < 0)
      {
         fprintf(stderr, "aimee-server: malformed credential environment name\n");
         return 1;
      }
      if (credential_env > 0)
      {
         if (vault_env_bootstrap_init() < 0 || server_vault_bootstrap_prepare() < 0 ||
             vault_config_bootstrap_init() < 0 || vault_env_has_credential_environment() != 0)
         {
            runtime_secret_clear();
            fprintf(stderr, "aimee-server: first-boot credential Vault bootstrap failed\n");
            return 1;
         }
         runtime_secret_clear();
         execvp(argv[0], argv);
         fprintf(stderr, "aimee-server: clean-environment re-exec failed: %s\n", strerror(errno));
         return 1;
      }
   }

   /* Backwards compat: --run-command was the old server command-dispatch path.
    * Command work now needs typed server/kb RPCs and runs on the server's
    * in-process compute pool when it is long-running. */
   if (argc >= 2 && strcmp(argv[1], "--run-command") == 0)
   {
      fprintf(stderr, "aimee-server: --run-command is retired; add a typed server RPC instead.\n");
      return 1;
   }

   /* Pre-warm the server-hosted OAuth CLIs (claude/codex) so the FIRST
    * `aimee agent setup *-oauth` doesn't wait on (or time out against) a cold
    * `npm i -g`. The deploy entrypoint runs this once at boot, backgrounded, as
    * the server's runtime user. It reuses cli_oauth_install (same pinned
    * versions + probe-first idempotency as the lazy path — no drift) and is
    * best-effort: a registry/network hiccup must NOT fail boot, and the lazy
    * install on first setup still covers it. Exits without starting the server. */
   if (argc >= 2 && strcmp(argv[1], "--prewarm-cli-oauth") == 0)
   {
      const cli_oauth_vendor_t vendors[] = {CLI_OAUTH_CLAUDE, CLI_OAUTH_CODEX};
      for (size_t k = 0; k < sizeof(vendors) / sizeof(vendors[0]); k++)
      {
         char err[256] = "";
         if (cli_oauth_install(vendors[k], err, sizeof(err)) == 0)
            fprintf(stderr, "aimee-server: prewarm %s CLI ready\n",
                    cli_oauth_vendor_name(vendors[k]));
         else
            fprintf(stderr, "aimee-server: prewarm %s CLI skipped: %s\n",
                    cli_oauth_vendor_name(vendors[k]), err);
      }
      return 0; /* best-effort — never fail the boot that backgrounds this */
   }

   /* Offline master-key rotation (D13). Re-wrap every principal's server wrap
    * from the old `.server-master.key` to a freshly minted one — a re-wrap, not a
    * re-encrypt. MUST run with the normal server stopped (the server caches one
    * process-wide server KEK, so a live rotation would leave autonomous decrypts
    * failing mid-rotation). Backs up `.vault/` first and restores it on any
    * failure, then exits without starting the server. */
   if (argc >= 2 && strcmp(argv[1], "--rotate-master-key") == 0)
   {
      /* Refuse to rotate while the server is up (D13 F2): a live server caching
       * the OLD KEK could write a NEW credential under the old wrap during the
       * re-wrap→swap window, permanently orphaning it (the backup cannot recover
       * a credential that was never in it). */
      const char *sock = cli_default_socket_path();
      if (server_is_running(sock))
      {
         fprintf(stderr, "aimee-server: --rotate-master-key requires the server to be STOPPED "
                         "(an instance appears to be running). Stop it and retry.\n");
         return 1;
      }
      int principals = 0, creds = 0;
      char backup[1280] = "", err[256] = "";
      if (vault_server_key_rotate(VAULT_SERVER_PRINCIPAL, &principals, &creds, backup,
                                  sizeof(backup), err, sizeof(err)) != 0)
      {
         fprintf(stderr, "aimee-server: master-key rotation FAILED: %s\n",
                 err[0] ? err : "unknown error");
         return 1;
      }
      if (backup[0])
         fprintf(stderr,
                 "aimee-server: master key rotated — re-wrapped %d credential(s) across %d "
                 "principal(s).\n  Pre-rotation backup: %s\n  Verify delegates authenticate, "
                 "then remove the backup.\n",
                 creds, principals, backup);
      else
         fprintf(stderr, "aimee-server: no master key present yet — nothing to rotate.\n");
      return 0;
   }

   /* Operator record+replay: re-present the governed-action rows recorded to an
    * audit-on-bus capture file, in order, with the stream's terminal status. This
    * is the auditability payoff of putting audit on the bus — an offline,
    * observational replay (nothing re-executed). Lives here because the capture
    * reader is bus code and aimee-server is the only shipping binary that links the
    * bus; it runs and exits without starting the server. */
   if (argc >= 2 && strcmp(argv[1], "--audit-replay") == 0)
   {
      if (argc < 3)
      {
         fprintf(stderr, "usage: aimee-server --audit-replay <capture-file>\n");
         return 2;
      }
      int rc = obs_bus_replay_print(argv[2], stdout);
      return rc == 0 ? 0 : 1;
   }

   const char *socket_path = NULL;
   log_level_t log_level = LOG_INFO;
   int service_mode = 0;

   /* Parse args (before stderr redirect so help/errors print to terminal) */
   for (int i = 1; i < argc; i++)
   {
      if (strcmp(argv[i], "--foreground") == 0 || strcmp(argv[i], "-f") == 0)
      {
         /* Default behavior is foreground; flag accepted for compatibility */
      }
      else if (strcmp(argv[i], "--service") == 0)
      {
         service_mode = 1;
      }
      else if (strncmp(argv[i], "--socket=", 9) == 0)
      {
         socket_path = argv[i] + 9;
      }
      else if (strncmp(argv[i], "--log-level=", 12) == 0)
      {
         if (log_parse_level(argv[i] + 12, &log_level) != 0)
         {
            fprintf(stderr, "aimee-server: invalid log level: %s\n", argv[i] + 12);
            return 1;
         }
      }
      else if (strcmp(argv[i], "--version") == 0 || strcmp(argv[i], "-v") == 0)
      {
         fprintf(stdout, "aimee-server %s (protocol %d)\n", AIMEE_VERSION, SERVER_PROTOCOL_VERSION);
         return 0;
      }
      else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0)
      {
         fprintf(
             stdout,
             "Usage: aimee-server [options]\n"
             "  --socket=PATH        Accepted for compatibility; the NDJSON RPC socket was\n"
             "                       removed. Only the pid file is derived from PATH. The\n"
             "                       server now serves the /v1 HTTP surface (UDS + optional TCP).\n"
             "  --log-level=LEVEL    Log level: error, warn, info, debug (default: info)\n"
             "  --foreground         Run in foreground (default)\n"
             "  --service            Run under the Windows Service Control Manager\n"
             "  --rotate-master-key  Re-wrap the vault under a fresh .server-master.key and exit\n"
             "                       (run with the server STOPPED; backs up .vault first)\n"
             "  --audit-replay FILE  Replay a governed-action audit capture file (the recorded\n"
             "                       bus event stream) to stdout, in order, and exit\n"
             "  --version            Print version\n"
             "  --help               Show this help\n");
         return 0;
      }
      else
      {
         fprintf(stderr, "aimee-server: unknown option: %s\n", argv[i]);
         return 1;
      }
   }

   if (service_mode)
      return platform_server_service_dispatch(socket_path, log_level, run_server);

   return run_server(socket_path, log_level);
}
