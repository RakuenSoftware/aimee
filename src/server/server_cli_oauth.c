/* server_cli_oauth.c — see server_cli_oauth.h. Server-hosted OAuth CLI agents.
 *
 * Security contract (roundtable R1): the only untrusted input is the vendor
 * string + (claude) the operator-pasted code; both are validated against closed
 * sets before any process runs. Every process is spawned argv-only via
 * safe_exec_capture_env (no shell, no interpolation). The login runs in a
 * private-socket tmux session; we scrape ONLY the URL + code from the pane and
 * never log raw pane contents. Tokens live in 0700 per-vendor dirs on the home
 * volume. A per-vendor lock serialises setups. */
#include "server_cli_oauth.h"
#include "aimee.h"
#include "util.h" /* safe_exec_capture_env */
#include "log.h"
#include "platform_path.h" /* platform_mkdir_p */

#include <ctype.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h> /* flock */
#include <sys/stat.h>
#include <unistd.h>

/* --- vendor table (the one source of truth) ------------------------------- */

typedef struct
{
   const char *name;       /* canonical vendor name */
   const char *agent;      /* registered agent name */
   const char *npm_pkg;    /* pinned `npm i -g` target */
   const char *npm_ver;    /* exact pinned version */
   const char *exe;        /* installed executable, resolved from the npm prefix */
   const char *url_anchor; /* substring that precedes the verification URL */
} vendor_info_t;

static const vendor_info_t g_vendors[] = {
    [CLI_OAUTH_CLAUDE] = {"claude", "claude", "@anthropic-ai/claude-code", "2.1.177", "claude",
                          "https://claude.com/"},
    [CLI_OAUTH_CODEX] = {"codex", "codex", "@openai/codex", "0.139.0", "codex",
                         "https://auth.openai.com/codex/device"},
};

int cli_oauth_vendor_parse(const char *s, cli_oauth_vendor_t *out)
{
   if (!s || !out)
      return -1;
   if (strcmp(s, "claude") == 0 || strcmp(s, "claude-oauth") == 0)
   {
      *out = CLI_OAUTH_CLAUDE;
      return 0;
   }
   if (strcmp(s, "codex") == 0 || strcmp(s, "codex-oauth") == 0)
   {
      *out = CLI_OAUTH_CODEX;
      return 0;
   }
   return -1;
}

const char *cli_oauth_vendor_name(cli_oauth_vendor_t v)
{
   return (v == CLI_OAUTH_CLAUDE || v == CLI_OAUTH_CODEX) ? g_vendors[v].name : "?";
}
const char *cli_oauth_agent_name(cli_oauth_vendor_t v)
{
   return (v == CLI_OAUTH_CLAUDE || v == CLI_OAUTH_CODEX) ? g_vendors[v].agent : "?";
}

/* --- paths / environment -------------------------------------------------- */

static const char *home_dir(void)
{
   const char *h = getenv("AIMEE_HOME");
   return (h && h[0]) ? h : "/var/lib/aimee";
}

/* Build the explicit child environment for a vendor command: HOME + per-vendor
 * config dirs on the persistent volume, the npm prefix on PATH. Fills `buf`
 * (>= 7 slots) with "KEY=VALUE" strings stored in `store` (a flat char buffer)
 * and NUL-terminates `buf`. */
static void build_env(cli_oauth_vendor_t v, char store[7][512], char *buf[8])
{
   const char *h = home_dir();
   snprintf(store[0], 512, "HOME=%s", h);
   snprintf(store[1], 512, "NPM_CONFIG_PREFIX=%s/.npm-global", h);
   snprintf(store[2], 512, "PATH=%s/.npm-global/bin:/usr/local/bin:/usr/bin:/bin", h);
   snprintf(store[3], 512, "XDG_CONFIG_HOME=%s/.config", h);
   snprintf(store[4], 512, "CODEX_HOME=%s/.codex", h);
   /* A login that wants no pager/editor must not block; keep it headless. */
   snprintf(store[5], 512, "NO_COLOR=1");
   snprintf(store[6], 512, "CI=1");
   (void)v;
   for (int i = 0; i < 7; i++)
      buf[i] = store[i];
   buf[7] = NULL;
}

/* Absolute path to the installed executable in the npm prefix. */
static void exe_path(cli_oauth_vendor_t v, char *out, size_t n)
{
   snprintf(out, n, "%s/.npm-global/bin/%s", home_dir(), g_vendors[v].exe);
}

/* --- install -------------------------------------------------------------- */

int cli_oauth_install(cli_oauth_vendor_t v, char *err, size_t errn)
{
   if (v != CLI_OAUTH_CLAUDE && v != CLI_OAUTH_CODEX)
   {
      snprintf(err, errn, "unsupported vendor");
      return -1;
   }
   char store[7][512];
   char *env[8];
   build_env(v, store, env);

   char exe[600];
   exe_path(v, exe, sizeof(exe));

   /* Idempotent: a real version probe (not mere file existence). */
   const char *probe[] = {exe, "--version", NULL};
   char *out = NULL;
   if (safe_exec_capture_env(probe, env, &out, 256) == 0)
   {
      free(out);
      return 0; /* already installed and runnable */
   }
   free(out);

   char pkgspec[256];
   snprintf(pkgspec, sizeof(pkgspec), "%s@%s", g_vendors[v].npm_pkg, g_vendors[v].npm_ver);
   /* --ignore-scripts: no package lifecycle scripts run with server privileges. */
   const char *install[] = {"npm", "install", "-g", "--ignore-scripts", pkgspec, NULL};
   out = NULL;
   int rc = safe_exec_capture_env(install, env, &out, 64 * 1024);
   free(out); /* never surface raw npm output (may carry registry/token noise) */
   if (rc != 0)
   {
      snprintf(err, errn, "npm install of %s failed (exit %d)", g_vendors[v].npm_pkg, rc);
      return -1;
   }
   /* Confirm it now runs. */
   out = NULL;
   rc = safe_exec_capture_env(probe, env, &out, 256);
   free(out);
   if (rc != 0)
   {
      snprintf(err, errn, "%s installed but not runnable", g_vendors[v].exe);
      return -1;
   }
   return 0;
}

/* --- tmux helpers (argv-only, private socket) ----------------------------- */

static void tmux_paths(cli_oauth_vendor_t v, char *sock, size_t sockn, char *sess, size_t sessn)
{
   snprintf(sock, sockn, "%s/.tmux/cli-oauth-%s.sock", home_dir(), g_vendors[v].name);
   /* One in-flight session per vendor (the lock guarantees it); the socket dir is
    * 0700 so no other local user can attach. */
   snprintf(sess, sessn, "cli-oauth-%s", g_vendors[v].name);
}

static int tmux_capture(cli_oauth_vendor_t v, const char *sock, const char *sess, char **out)
{
   char store[7][512];
   char *env[8];
   build_env(v, store, env);
   const char *argv[] = {"tmux", "-S", sock, "capture-pane", "-p", "-t", sess, NULL};
   *out = NULL;
   return safe_exec_capture_env(argv, env, out, 64 * 1024);
}

static void tmux_kill(cli_oauth_vendor_t v, const char *sock, const char *sess)
{
   char store[7][512];
   char *env[8];
   build_env(v, store, env);
   const char *argv[] = {"tmux", "-S", sock, "kill-session", "-t", sess, NULL};
   char *out = NULL;
   safe_exec_capture_env(argv, env, &out, 256);
   free(out);
}

/* --- scraping (URL + code only; raw pane never leaves this function) ------- */

/* Copy the first https URL at/after `anchor` into out (up to whitespace). */
int cli_oauth_scrape_url(const char *pane, const char *anchor, char *out, size_t n)
{
   const char *p = strstr(pane, anchor);
   if (!p)
      return -1;
   /* anchor may be a prefix label; find the actual https:// from there. */
   const char *u = strstr(p, "https://");
   if (!u)
      u = p; /* anchor already is the URL */
   size_t i = 0;
   while (u[i] && !isspace((unsigned char)u[i]) && i < n - 1)
   {
      out[i] = u[i];
      i++;
   }
   out[i] = '\0';
   return i > 8 ? 0 : -1;
}

/* codex one-time code: a token shaped like XXXX-XXXXX (upper alnum + one dash). */
int cli_oauth_scrape_codex_code(const char *pane, char *out, size_t n)
{
   for (const char *p = pane; *p; p++)
   {
      if (!(isupper((unsigned char)*p) || isdigit((unsigned char)*p)))
         continue;
      const char *s = p;
      int dashes = 0, len = 0;
      while (s[len] && (isupper((unsigned char)s[len]) || isdigit((unsigned char)s[len]) ||
                        (s[len] == '-' && dashes == 0)))
      {
         if (s[len] == '-')
            dashes++;
         len++;
      }
      if (dashes == 1 && len >= 8 && len <= 20 && (size_t)len < n)
      {
         /* require a boundary before/after so we don't clip a longer token */
         char before = (p == pane) ? ' ' : p[-1];
         char after = s[len];
         if (!isalnum((unsigned char)before) && !isalnum((unsigned char)after))
         {
            memcpy(out, s, len);
            out[len] = '\0';
            return 0;
         }
      }
      p = s + len - 1;
   }
   return -1;
}

/* --- per-vendor lock ------------------------------------------------------ */

static int lock_acquire(cli_oauth_vendor_t v)
{
   char dir[512], path[600];
   snprintf(dir, sizeof(dir), "%s/lock", home_dir());
   platform_mkdir_p(dir, 0700);
   snprintf(path, sizeof(path), "%s/cli-oauth-%s.lock", dir, g_vendors[v].name);
   int fd = open(path, O_CREAT | O_RDWR, 0600);
   if (fd < 0)
      return -1;
   if (flock(fd, LOCK_EX | LOCK_NB) != 0)
   {
      close(fd);
      return -1; /* setup already in progress */
   }
   return fd; /* caller closes to release */
}

/* --- start / submit / poll ------------------------------------------------ */

int cli_oauth_start(cli_oauth_vendor_t v, cli_oauth_start_t *out, char *err, size_t errn)
{
   if ((v != CLI_OAUTH_CLAUDE && v != CLI_OAUTH_CODEX) || !out)
   {
      snprintf(err, errn, "unsupported vendor");
      return -1;
   }
   memset(out, 0, sizeof(*out));

   int lock = lock_acquire(v);
   if (lock < 0)
   {
      snprintf(err, errn, "a %s setup is already in progress", g_vendors[v].name);
      return -1;
   }

   /* Per-vendor config + socket dirs, 0700. */
   char cfgdir[512], tmuxdir[512];
   snprintf(cfgdir, sizeof(cfgdir), "%s/%s", home_dir(),
            v == CLI_OAUTH_CODEX ? ".codex" : ".claude");
   platform_mkdir_p(cfgdir, 0700);
   chmod(cfgdir, 0700);
   snprintf(tmuxdir, sizeof(tmuxdir), "%s/.tmux", home_dir());
   platform_mkdir_p(tmuxdir, 0700);
   chmod(tmuxdir, 0700);

   char sock[600], sess[128];
   tmux_paths(v, sock, sizeof(sock), sess, sizeof(sess));
   tmux_kill(v, sock, sess); /* clear any stale session before starting */

   char store[7][512];
   char *env[8];
   build_env(v, store, env);

   /* Launch the login in a detached tmux session, argv-only. The vendor matrix:
    * claude -> `claude setup-token` (paste-back code); codex -> `codex login
    * --device-auth` (device URL+code, polls). Build:
    *   tmux -S sock new-session -d -s sess <exe> <loginargs...>
    * tmux runs argv[7..] as the command, so the exe path makes PATH moot. */
   char exe[600];
   exe_path(v, exe, sizeof(exe));
   const char *argv[16];
   int ai = 0;
   argv[ai++] = "tmux";
   argv[ai++] = "-S";
   argv[ai++] = sock;
   argv[ai++] = "new-session";
   argv[ai++] = "-d";
   argv[ai++] = "-s";
   argv[ai++] = sess;
   argv[ai++] = exe;
   if (v == CLI_OAUTH_CLAUDE)
   {
      argv[ai++] = "setup-token";
   }
   else
   {
      argv[ai++] = "login";
      argv[ai++] = "--device-auth";
   }
   argv[ai] = NULL;

   char *cap = NULL;
   if (safe_exec_capture_env(argv, env, &cap, 256) != 0)
   {
      free(cap);
      close(lock);
      snprintf(err, errn, "failed to launch %s login session", g_vendors[v].name);
      return -1;
   }
   free(cap);

   /* The CLI takes a moment to print the URL/code; poll the pane a few times. */
   int got = -1;
   for (int attempt = 0; attempt < 12 && got != 0; attempt++)
   {
      struct timespec ts = {1, 0};
      nanosleep(&ts, NULL);
      char *pane = NULL;
      if (tmux_capture(v, sock, sess, &pane) != 0 || !pane)
      {
         free(pane);
         continue;
      }
      if (cli_oauth_scrape_url(pane, g_vendors[v].url_anchor, out->url, sizeof(out->url)) == 0)
      {
         if (v == CLI_OAUTH_CODEX)
            got = cli_oauth_scrape_codex_code(pane, out->code, sizeof(out->code));
         else
            got = 0; /* claude: URL is enough; code is pasted back later */
      }
      free(pane); /* raw pane discarded — never logged or persisted */
   }
   if (got != 0)
   {
      tmux_kill(v, sock, sess);
      close(lock);
      snprintf(err, errn, "could not read the %s verification URL/code", g_vendors[v].name);
      return -1;
   }

   snprintf(out->session, sizeof(out->session), "%s", sess);
   out->needs_code_back = (v == CLI_OAUTH_CLAUDE) ? 1 : 0;
   /* Release the lock fd: the session persists; _submit/_poll re-acquire as a
    * best-effort guard. (The single fixed session name already serialises.) */
   close(lock);
   aimee_log(LOG_INFO, "cli.oauth", "%s login started; URL surfaced to operator",
             g_vendors[v].name);
   return 0;
}

/* code charset: the claude paste-back is an opaque token — allow url-safe base64
 * + a few separators, bounded length; reject anything else (no control chars,
 * never interpreted by a shell since send-keys is argv). */
int cli_oauth_code_is_safe(const char *code)
{
   if (!code || !code[0])
      return 0;
   size_t n = strlen(code);
   if (n > 512)
      return 0;
   for (size_t i = 0; i < n; i++)
   {
      char c = code[i];
      if (!(isalnum((unsigned char)c) || c == '-' || c == '_' || c == '.' || c == '/' || c == '+' ||
            c == '=' || c == '#'))
         return 0;
   }
   return 1;
}

int cli_oauth_submit_code(cli_oauth_vendor_t v, const char *session, const char *code, char *err,
                          size_t errn)
{
   if (v != CLI_OAUTH_CLAUDE)
   {
      snprintf(err, errn, "%s does not take a pasted code", cli_oauth_vendor_name(v));
      return -1;
   }
   if (!session || !session[0] || !cli_oauth_code_is_safe(code))
   {
      snprintf(err, errn, "invalid session or code");
      return -1;
   }
   char sock[600], sess[128];
   tmux_paths(v, sock, sizeof(sock), sess, sizeof(sess));
   if (strcmp(session, sess) != 0)
   {
      snprintf(err, errn, "unknown session");
      return -1;
   }
   char store[7][512];
   char *env[8];
   build_env(v, store, env);
   /* send-keys is argv: the code is a literal key string, never shell-parsed. */
   const char *keys[] = {"tmux", "-S", sock, "send-keys", "-t", sess, code, "Enter", NULL};
   char *out = NULL;
   int rc = safe_exec_capture_env(keys, env, &out, 256);
   free(out);
   if (rc != 0)
   {
      snprintf(err, errn, "failed to submit the code");
      return -1;
   }
   return 0;
}

int cli_oauth_poll(cli_oauth_vendor_t v, const char *session, cli_oauth_state_t *state, char *err,
                   size_t errn)
{
   if ((v != CLI_OAUTH_CLAUDE && v != CLI_OAUTH_CODEX) || !state)
   {
      snprintf(err, errn, "unsupported vendor");
      return -1;
   }
   char sock[600], sess[128];
   tmux_paths(v, sock, sizeof(sock), sess, sizeof(sess));
   if (!session || strcmp(session, sess) != 0)
   {
      snprintf(err, errn, "unknown session");
      return -1;
   }
   *state = CLI_OAUTH_PENDING;

   /* Authoritative check: does the vendor now report a valid login? */
   char store[7][512];
   char *env[8];
   build_env(v, store, env);
   char exe[600];
   exe_path(v, exe, sizeof(exe));
   int authed = 0;
   if (v == CLI_OAUTH_CODEX)
   {
      const char *st[] = {exe, "login", "status", NULL};
      char *o = NULL;
      authed = (safe_exec_capture_env(st, env, &o, 4096) == 0);
      free(o);
   }
   else
   {
      /* claude: the token file appears under ~/.claude once setup-token finishes. */
      char tok[600];
      snprintf(tok, sizeof(tok), "%s/.claude/.credentials.json", home_dir());
      struct stat sb;
      authed = (stat(tok, &sb) == 0 && sb.st_size > 0);
      if (!authed)
      {
         char tok2[600];
         snprintf(tok2, sizeof(tok2), "%s/.claude.json", home_dir());
         authed = (stat(tok2, &sb) == 0 && sb.st_size > 0);
      }
   }
   if (authed)
   {
      /* Lock down the token files, then tear the session down. */
      char tokdir[600];
      snprintf(tokdir, sizeof(tokdir), "%s/%s", home_dir(),
               v == CLI_OAUTH_CODEX ? ".codex" : ".claude");
      chmod(tokdir, 0700);
      tmux_kill(v, sock, sess);
      *state = CLI_OAUTH_AUTHENTICATED;
      aimee_log(LOG_INFO, "cli.oauth", "%s login authenticated", g_vendors[v].name);
      return 0;
   }

   /* Still pending unless the session has died (CLI exited without auth). */
   char *pane = NULL;
   if (tmux_capture(v, sock, sess, &pane) != 0)
   {
      free(pane);
      *state = CLI_OAUTH_FAILED;
      snprintf(err, errn, "%s login session ended without authenticating", g_vendors[v].name);
      return 0;
   }
   free(pane);
   return 0;
}
