/* server_main.c: aimee-server entry point -- socket lifecycle, signal handling */
#include "aimee.h"
#include "cli_client.h"
#include "commands.h"
#include "config.h"
#include "config_sections.h"
#include "forge_app_token.h"
#include "forge_credentials.h"
#include "git_host_cred.h"
#include "git_host_resolve.h"
#include "git_ops.h"
#include "workspace.h"
#include "kb_client_cache.h"
#include "kb_client_ws.h"
#include "db1.h"
#include "mcp_client_registry.h"
#include "server.h"
#include "server_http.h"
#include "cli_session_pty.h"
#include "presence.h"
#include "turn_registry.h"
#include "events.h"
#include "agent_exec.h"
#include "log.h"
#include "audit_action.h"
#include "platform_path.h"
#include "platform_process.h"
#include "shutdown_forensics.h"
#include "headers/plugin_loader.h"
#include "headers/context_engine.h"
#include "headers/server_cli_oauth.h"
#include "vault_server_key.h"
#include "vault_service.h" /* VAULT_SERVER_PRINCIPAL (rotation target) */
#include <signal.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
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

#ifndef _WIN32
static void signal_handler_info(int sig, siginfo_t *info, void *ucontext)
{
   (void)ucontext;
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
      }
   }

   /* Initialize logging */
   log_init(log_level);
   audit_log_open();
   audit_ensure_key(); /* provision the per-action audit key (best-effort) */

   /* Activate the GitHub App installation-token provider for the server's forge
    * identity. Inert unless AIMEE_FORGE_APP_* is set (see forge_app_token.c). */
   forge_cred_register_app_token_provider(forge_app_token_configured, forge_app_token_get);

   /* Wire the per-host git credential vault into the credential resolvers so git
    * clone/fetch/push/PR authenticate with the stored token for the repo's host
    * (git_host_resolve.c). Without this the per-host step is skipped. */
   git_host_resolve_register(git_host_cred_for_url);

   /* Wire the per-session worktree resolver so the webchat git surfaces (git panel
    * + editor) act on the SAME isolated worktree the session's agent edits, rather
    * than the shared project checkout (session_isolation_target, workspace.c). */
   git_ops_register_session_isolation(session_isolation_target);

   config_t cfg;
   config_load(&cfg);

   /* Startup-fatal validation for the live gateway-mutation path: an invalid
    * session-disable TTL (<=0) must refuse to bring up the /v1 server rather than
    * pin/disable the breaker on live client traffic. Scoped to server startup so
    * unrelated CLI callers of config_load are unaffected. */
   {
      char cfg_err[256];
      if (config_reduce_validate(&cfg, cfg_err, sizeof(cfg_err)) != 0)
      {
         fprintf(stderr, "aimee: fatal config error: %s\n", cfg_err);
         return 1;
      }
   }

   /* Remote aimee-kb: when a kb_client_url is configured (this host uses a
    * remote kb rather than a local sidecar), export it into our own env so the
    * env-based kb_client transport (kb_client_v1_base_url / auth_header)
    * reaches the remote kb on every launch path — systemd AND the fork-and-exec
    * fallback. Pre-set env wins, so AIMEE_KB_API_URL still overrides config. */
   if (!(getenv("AIMEE_KB_API_URL") && getenv("AIMEE_KB_API_URL")[0]))
   {
      if (cfg.kb_client_url[0])
         platform_setenv("AIMEE_KB_API_URL", cfg.kb_client_url);
      else
      {
         /* Co-located default: with no remote kb_client_url and no explicit
          * AIMEE_KB_API_URL, point at the local aimee-kb sidecar. The systemd
          * unit, the launchd plist, and the fork-and-exec fallback all serve it
          * on 127.0.0.1:8741 (kb_api_http_port if the operator overrode it).
          * Without this a source install's server has no kb URL at all and every
          * DB2-backed feature (memory/kb/rules) silently reports "unavailable". */
         char local_kb[64];
         int kb_port = cfg.kb_api_http_port > 0 ? cfg.kb_api_http_port : 8741;
         snprintf(local_kb, sizeof(local_kb), "http://127.0.0.1:%d", kb_port);
         platform_setenv("AIMEE_KB_API_URL", local_kb);
      }
   }
   if (cfg.kb_client_bearer_token[0] &&
       !(getenv("AIMEE_KB_API_BEARER_TOKEN") && getenv("AIMEE_KB_API_BEARER_TOKEN")[0]))
      platform_setenv("AIMEE_KB_API_BEARER_TOKEN", cfg.kb_client_bearer_token);

   /* Result cache + invalidation subscriber: a short-TTL LRU of kb read results
    * (AIMEE_KB_CACHE_TTL_S; off by default) flushed on the kb's /v1/events push,
    * so caching beats TTL-only staleness. No-op unless enabled + an HTTP kb. */
   kb_cache_configure(-1);
   kb_client_ws_start();

   /* Parse plugin extension config keys not covered by config_load(). */
   config_load_plugin_extensions(&cfg);

   /* Bridge the autonomy config knobs to their AIMEE_AUTONOMY_* env vars before the wfe
    * engine reads them (an explicitly-set env var still overrides — setenv no-overwrite). */
   autonomy_config_to_env(&cfg);

   /* Register bundled context engine and discover plugins from all sources.
    * Must run after config_load so plugin_loader can read install prefix from env.
    * Set active engine from config (empty = default compactor). */
   context_engine_register_compactor();
   if (cfg.context_engine[0])
      context_engine_set_active(cfg.context_engine);
   {
      char perr[256] = {0};
      plugin_loader_discover_all(perr, sizeof(perr));
      if (perr[0])
         LOG_WARN("server", "plugin discovery: %s", perr);
   }

   /* DB2 + pgvector startup and supervision are owned by aimee-kb, not
    * aimee-server.  Keep the server on the DB1 side of the service split. */

   if (!socket_path)
      socket_path = cli_default_socket_path();

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
      return 1;
   }

   mcp_client_registry_boot(&cfg);
   agent_http_init();
   presence_init(); /* unified-presence registry (attachments, turn locks, event ring) */
   presence_set_delivery_fn(presence_deliver_via_notify, NULL); /* outbound: ntfy/local */
   turn_registry_init(); /* per-turn cancel registry (server-owned turn lifecycle) */

   /* Wire the inference-backed OpenAI completion handlers before the listener
    * accepts requests (agent_http_init above must run first). */
   openai_chat_register();
   anthropic_http_register(); /* Anthropic Messages API ingress (Claude Code) */
   server_native_register();  /* native /v1 REST providers (e.g. GET /v1/rules) */

   /* Inbound /v1 HTTP API (UDS always; optional localhost TCP + bearer when
    * aimee.api.{http_port,bearer_token} are configured). Best-effort: a bind
    * failure must not block the RPC server. */
   server_http_set_max_event_streams(cfg.server_api_max_event_streams);
   cli_session_pty_set_forwarding(cfg.server_api_cli_session_forwarding);
   if (server_http_start(NULL, cfg.server_api_http_port, cfg.server_api_tls_port,
                         cfg.server_api_bearer_token, cfg.server_api_rate_limit_per_min,
                         cfg.server_api_remote_writes) != 0)
      LOG_WARN("server.http", "failed to start inbound /v1 HTTP listener");

   /* Install signal handlers */
   install_signal_handlers();

   g_ctx.running = 1;
   (void)shutdown_forensics_record_unclean_exits();
   (void)shutdown_forensics_mark_started("server", g_ctx.start_time);
   startup_notify(notify_fd, "ok\n");
   int rc = server_run(&g_ctx);

   server_http_stop();
   server_shutdown(&g_ctx);
   (void)shutdown_forensics_mark_stopped("server", getpid());
   agent_http_cleanup();
   mcp_client_registry_shutdown();
   audit_log_close();

   return rc < 0 ? 1 : 0;
}

int main(int argc, char **argv)
{
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
