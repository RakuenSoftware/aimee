/* forge_credentials.c — per-workspace short-lived forge-token broker.
 * See forge_credentials.h for the contract and the workspace-resource-plane §4
 * design. Tokens are held in memory only, wiped on revoke, and injected into
 * the git/gh exec environment (never the command line, never disk). */
#include "forge_credentials.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* config_default_dir (config.c): the instance config dir the askpass shim lives
 * in. Forward-declared to keep this TU off the heavyweight config.h include. */
extern const char *config_default_dir(void);

#define FORGE_TOKEN_MAX 4096

/* Optional App installation-token provider, registered by the server at startup
 * (forge_app_token.c). NULL in the thin client and unit tests, where forge
 * identity isn't used — keeping forge_credentials free of any link dependency on
 * the App-token module (and its OpenSSL/HTTP deps). */
static int (*g_app_token_configured)(void) = NULL;
static int (*g_app_token_get)(char *, size_t) = NULL;
static int (*g_static_token_get)(char *, size_t) = NULL;

void forge_cred_register_app_token_provider(int (*configured)(void), int (*get)(char *, size_t))
{
   g_app_token_configured = configured;
   g_app_token_get = get;
}

void forge_cred_register_static_token_provider(int (*get)(char *, size_t))
{
   g_static_token_get = get;
}

int forge_cred_server_identity(char *tok_out, size_t tok_cap, char *scope_out, size_t scope_cap)
{
   if (tok_out && tok_cap)
      tok_out[0] = '\0';
   if (scope_out && scope_cap)
      scope_out[0] = '\0';
   /* App-token layer: when the server has registered an App installation-token
    * provider AND it is configured (AIMEE_FORGE_APP_*), mint/serve an
    * installation token instead of consuming a raw AIMEE_FORGE_TOKEN. The
    * provider is a registered pointer so this core file carries no link
    * dependency on the App-token module. */
   if (g_app_token_configured && g_app_token_configured())
   {
      int rc = g_app_token_get ? g_app_token_get(tok_out, tok_cap) : -1;
      if (rc == 1)
      {
         const char *as = getenv("AIMEE_FORGE_SCOPE");
         if (scope_out && scope_cap)
            snprintf(scope_out, scope_cap, "%s", (as && as[0]) ? as : "workspace");
         return 1;
      }
      /* Configured-but-broken (rc == -1, or the impossible rc == 0): fail closed.
       * Do not silently fall through to a likely-absent raw token; the App layer
       * already logged the mint error. The caller falls back to ambient creds. */
      if (tok_out && tok_cap)
         tok_out[0] = '\0';
      return 0;
   }
   if (!g_static_token_get || g_static_token_get(tok_out, tok_cap) != 1 || !tok_out || !tok_out[0])
      return 0;
   const char *s = getenv("AIMEE_FORGE_SCOPE");
   if (scope_out && scope_cap)
      snprintf(scope_out, scope_cap, "%s", (s && s[0]) ? s : "workspace");
   return 1;
}

/* A user-facing companion to the askpass shim.
 *
 * Removing GH_TOKEN from the editor environment closed a real leak — anything
 * that could see the process could read the secret out of /proc/<pid>/environ —
 * but it also took away the only way a user could reach their own token from the
 * integrated terminal for something other than git (a curl against the forge
 * API, a release script). Telling people to `cat /proc/self/fd/$AIMEE_GIT_TOKEN_FD`
 * is a worse answer than shipping the one line that does it.
 *
 * This prints the token on stdout and nothing else, so `TOKEN=$(aimee-git-token)`
 * works. The secret still lives only in the descriptor and in the memory of
 * whatever asked for it — never in an environment block. Returns the absolute
 * path, or NULL. */
const char *forge_cred_token_helper(void)
{
   static char path[4096];
   static int tried = 0;
   if (tried)
      return path[0] ? path : NULL;
   tried = 1;
   const char *dir = config_default_dir();
   if (!dir || !dir[0])
      return NULL;
   snprintf(path, sizeof(path), "%s/aimee-git-token", dir);
   FILE *f = fopen(path, "w");
   if (!f)
   {
      path[0] = '\0';
      return NULL;
   }
   /* Same all-digits guard as the askpass: the value is used as a /proc/self/fd
    * path component. No descriptor means no token and a non-zero exit, so a
    * script fails loudly instead of silently using an empty string. */
   fputs("#!/bin/sh\n"
         "case \"$AIMEE_GIT_TOKEN_FD\" in\n"
         "  ''|*[!0-9]*)\n"
         "    echo \"aimee-git-token: no credential in this session\" >&2\n"
         "    exit 1 ;;\n"
         "esac\n"
         "cat \"/proc/self/fd/$AIMEE_GIT_TOKEN_FD\" 2>/dev/null || {\n"
         "  echo \"aimee-git-token: credential descriptor is unreadable\" >&2\n"
         "  exit 1\n"
         "}\n",
         f);
   fclose(f);
   chmod(path, 0700);
   return path;
}

const char *forge_cred_askpass_shim(void)
{
   static char path[4096];
   static int tried = 0;
   if (tried)
      return path[0] ? path : NULL;
   tried = 1;
   const char *dir = config_default_dir();
   if (!dir || !dir[0])
      return NULL;
   snprintf(path, sizeof(path), "%s/git-askpass-forge.sh", dir);
   FILE *f = fopen(path, "w");
   if (!f)
   {
      path[0] = '\0';
      return NULL;
   }
   /* The ONLY password source is a token delivered over an inherited file
    * descriptor (AIMEE_GIT_TOKEN_FD), re-readable via /proc/self/fd so the secret
    * never enters this process's environment or /proc/<pid>/environ. There is no
    * GH_TOKEN fallback: an env-borne token is readable by anything that can see
    * the process, which is exactly what the fd path exists to prevent, and a
    * fallback would silently reinstate it whenever the fd was missing.
    *
    * AIMEE_GIT_TOKEN_FD must be all-digits before it is used as a /proc/self/fd
    * path component (defence in depth). Anything else yields no password, and git
    * fails the auth rather than proceeding with a weaker source. cat reopens
    * /proc/self/fd/<n>, so the memfd is read from offset 0 on every invocation —
    * which is what lets one inherited fd serve a long-lived session. */
   fputs("#!/bin/sh\n"
         "case \"$1\" in\n"
         "*[Uu]sername*) echo x-access-token ;;\n"
         "*) case \"$AIMEE_GIT_TOKEN_FD\" in\n"
         "     ''|*[!0-9]*) ;;\n"
         "     *) cat \"/proc/self/fd/$AIMEE_GIT_TOKEN_FD\" 2>/dev/null ;;\n"
         "   esac ;;\n"
         "esac\n",
         f);
   fclose(f);
   chmod(path, 0700);
   return path;
}
