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

#define FORGE_MAX_WS    64
#define FORGE_WS_ID_MAX 1024
#define FORGE_TOKEN_MAX 4096
#define FORGE_SCOPE_MAX 32

typedef struct
{
   int used;
   char workspace_id[FORGE_WS_ID_MAX];
   char *token; /* heap; explicitly zeroed before free */
   char scope[FORGE_SCOPE_MAX];
   long expires_epoch; /* now_epoch + ttl at install */
} forge_slot_t;

static forge_slot_t g_slots[FORGE_MAX_WS];
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

/* Zero `n` bytes of `p` so a compiler cannot optimise the wipe away. */
static void secure_wipe(void *p, size_t n)
{
   volatile unsigned char *v = (volatile unsigned char *)p;
   while (n--)
      *v++ = 0;
}

static void slot_clear(forge_slot_t *s)
{
   if (s->token)
   {
      secure_wipe(s->token, strlen(s->token));
      free(s->token);
      s->token = NULL;
   }
   secure_wipe(s->workspace_id, sizeof(s->workspace_id));
   secure_wipe(s->scope, sizeof(s->scope));
   s->expires_epoch = 0;
   s->used = 0;
}

/* Find the slot for `id`, or NULL. Caller holds g_lock. */
static forge_slot_t *slot_find(const char *id)
{
   for (int i = 0; i < FORGE_MAX_WS; i++)
      if (g_slots[i].used && strcmp(g_slots[i].workspace_id, id) == 0)
         return &g_slots[i];
   return NULL;
}

static int scope_valid(const char *s)
{
   return s && (strcmp(s, "global") == 0 || strcmp(s, "workspace") == 0 ||
                strcmp(s, "project") == 0 || strcmp(s, "user") == 0);
}

/* Lattice rank: global (broadest) = 0 … user (narrowest) = 3. -1 if unknown. */
static int scope_rank(const char *s)
{
   if (!s)
      return -1;
   if (strcmp(s, "global") == 0)
      return 0;
   if (strcmp(s, "workspace") == 0)
      return 1;
   if (strcmp(s, "project") == 0)
      return 2;
   if (strcmp(s, "user") == 0)
      return 3;
   return -1;
}

int forge_cred_install(const char *workspace_id, const char *token, const char *scope,
                       long ttl_seconds, long now_epoch)
{
   if (!workspace_id || !workspace_id[0] || !token || !token[0] || !scope_valid(scope) ||
       ttl_seconds <= 0)
      return -1;
   if (strlen(workspace_id) >= FORGE_WS_ID_MAX || strlen(token) >= FORGE_TOKEN_MAX)
      return -1;

   char *copy = strdup(token);
   if (!copy)
      return -1;

   pthread_mutex_lock(&g_lock);
   forge_slot_t *s = slot_find(workspace_id);
   if (!s)
   {
      for (int i = 0; i < FORGE_MAX_WS; i++)
         if (!g_slots[i].used)
         {
            s = &g_slots[i];
            break;
         }
   }
   if (!s)
   {
      pthread_mutex_unlock(&g_lock);
      secure_wipe(copy, strlen(copy));
      free(copy);
      return -1; /* registry full */
   }
   /* Replace any existing token in this slot, zeroing the old secret first. */
   if (s->token)
   {
      secure_wipe(s->token, strlen(s->token));
      free(s->token);
   }
   s->used = 1;
   snprintf(s->workspace_id, sizeof(s->workspace_id), "%s", workspace_id);
   snprintf(s->scope, sizeof(s->scope), "%s", scope);
   s->token = copy;
   s->expires_epoch = now_epoch + ttl_seconds;
   pthread_mutex_unlock(&g_lock);
   return 0;
}

int forge_cred_get(const char *workspace_id, long now_epoch, char *out, size_t out_cap)
{
   if (out && out_cap)
      out[0] = '\0';
   if (!workspace_id || !out || out_cap == 0)
      return -1;

   int rc = -1;
   pthread_mutex_lock(&g_lock);
   forge_slot_t *s = slot_find(workspace_id);
   if (s && s->token && now_epoch < s->expires_epoch)
   {
      snprintf(out, out_cap, "%s", s->token);
      rc = 0;
   }
   pthread_mutex_unlock(&g_lock);
   return rc;
}

int forge_cred_scope(const char *workspace_id, long now_epoch, char *out, size_t out_cap)
{
   if (out && out_cap)
      out[0] = '\0';
   if (!workspace_id || !out || out_cap == 0)
      return -1;

   int rc = -1;
   pthread_mutex_lock(&g_lock);
   forge_slot_t *s = slot_find(workspace_id);
   if (s && s->token && now_epoch < s->expires_epoch)
   {
      snprintf(out, out_cap, "%s", s->scope);
      rc = 0;
   }
   pthread_mutex_unlock(&g_lock);
   return rc;
}

int forge_cred_scope_allows(const char *cred_scope, const char *required_scope)
{
   int c = scope_rank(cred_scope);
   int r = scope_rank(required_scope);
   if (c < 0 || r < 0)
      return 0; /* unknown either side → deny */
   /* Broader credential (lower rank) satisfies a narrower requirement. */
   return c <= r;
}

void forge_cred_revoke(const char *workspace_id)
{
   if (!workspace_id)
      return;
   pthread_mutex_lock(&g_lock);
   forge_slot_t *s = slot_find(workspace_id);
   if (s)
      slot_clear(s);
   pthread_mutex_unlock(&g_lock);
}

void forge_cred_revoke_all(void)
{
   pthread_mutex_lock(&g_lock);
   for (int i = 0; i < FORGE_MAX_WS; i++)
      if (g_slots[i].used)
         slot_clear(&g_slots[i]);
   pthread_mutex_unlock(&g_lock);
}

int forge_cred_count(void)
{
   int n = 0;
   pthread_mutex_lock(&g_lock);
   for (int i = 0; i < FORGE_MAX_WS; i++)
      if (g_slots[i].used)
         n++;
   pthread_mutex_unlock(&g_lock);
   return n;
}

/* Build an exec envp from an explicit `token`: a copy of `parent_environ` with
 * any inherited GH_TOKEN/GITHUB_TOKEN/GIT_ASKPASS dropped, plus GH_TOKEN and
 * (when `askpass_shim` is set) GIT_ASKPASS + GIT_TERMINAL_PROMPT=0. The token
 * crosses only into the returned env. Returns malloc'd array or NULL on OOM.
 * Caller wipes its own copy of `token`. */
static char **build_env_from_token(const char *token, char *const *parent_environ,
                                   const char *askpass_shim)
{
   int pn = 0;
   if (parent_environ)
      while (parent_environ[pn])
         pn++;
   /* parent entries + GH_TOKEN + (GIT_ASKPASS, GIT_TERMINAL_PROMPT) + NULL */
   int extra = askpass_shim && askpass_shim[0] ? 3 : 1;
   char **envp = calloc((size_t)pn + extra + 1, sizeof(char *));
   if (!envp)
      return NULL;
   int o = 0;
   for (int i = 0; i < pn; i++)
   {
      /* Drop any inherited GH_TOKEN/GIT_ASKPASS so ours win unambiguously. */
      if (strncmp(parent_environ[i], "GH_TOKEN=", 9) == 0 ||
          strncmp(parent_environ[i], "GITHUB_TOKEN=", 13) == 0 ||
          strncmp(parent_environ[i], "GIT_ASKPASS=", 12) == 0)
         continue;
      envp[o] = strdup(parent_environ[i]);
      if (!envp[o])
         goto oom;
      o++;
   }
   {
      size_t need = strlen("GH_TOKEN=") + strlen(token) + 1;
      char *e = malloc(need);
      if (!e)
         goto oom;
      snprintf(e, need, "GH_TOKEN=%s", token);
      envp[o++] = e;
   }
   if (askpass_shim && askpass_shim[0])
   {
      size_t need = strlen("GIT_ASKPASS=") + strlen(askpass_shim) + 1;
      char *e = malloc(need);
      if (!e)
         goto oom;
      snprintf(e, need, "GIT_ASKPASS=%s", askpass_shim);
      envp[o++] = e;
      envp[o++] = strdup("GIT_TERMINAL_PROMPT=0");
      if (!envp[o - 1])
         goto oom;
   }
   envp[o] = NULL;
   return envp;

oom:
   forge_cred_free_env(envp);
   return NULL;
}

char **forge_cred_build_env(const char *workspace_id, long now_epoch, char *const *parent_environ,
                            const char *askpass_shim)
{
   char token[FORGE_TOKEN_MAX];
   if (forge_cred_get(workspace_id, now_epoch, token, sizeof(token)) != 0)
      return NULL; /* no live token → caller falls back to ambient / server identity */
   char **envp = build_env_from_token(token, parent_environ, askpass_shim);
   secure_wipe(token, sizeof(token));
   return envp;
}

/* Optional App installation-token provider, registered by the server at startup
 * (forge_app_token.c). NULL in the thin client and unit tests, where forge
 * identity isn't used — keeping forge_credentials free of any link dependency on
 * the App-token module (and its OpenSSL/HTTP deps). */
static int (*g_app_token_configured)(void) = NULL;
static int (*g_app_token_get)(char *, size_t) = NULL;

void forge_cred_register_app_token_provider(int (*configured)(void), int (*get)(char *, size_t))
{
   g_app_token_configured = configured;
   g_app_token_get = get;
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
   const char *t = getenv("AIMEE_FORGE_TOKEN");
   if (!t || !t[0])
      return 0;
   if (tok_out && tok_cap)
      snprintf(tok_out, tok_cap, "%s", t);
   const char *s = getenv("AIMEE_FORGE_SCOPE");
   if (scope_out && scope_cap)
      snprintf(scope_out, scope_cap, "%s", (s && s[0]) ? s : "workspace");
   return 1;
}

char **forge_cred_build_server_env(char *const *parent_environ, const char *askpass_shim)
{
   char tok[FORGE_TOKEN_MAX];
   if (forge_cred_server_identity(tok, sizeof(tok), NULL, 0) != 1)
      return NULL; /* no server identity configured → caller falls back to ambient */
   char **envp = build_env_from_token(tok, parent_environ, askpass_shim);
   secure_wipe(tok, sizeof(tok));
   return envp;
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
   fputs("#!/bin/sh\n"
         "case \"$1\" in\n"
         "*[Uu]sername*) echo x-access-token ;;\n"
         "*) printf '%s\\n' \"$GH_TOKEN\" ;;\n"
         "esac\n",
         f);
   fclose(f);
   chmod(path, 0700);
   return path;
}

void forge_cred_free_env(char **envp)
{
   if (!envp)
      return;
   for (int i = 0; envp[i]; i++)
   {
      /* Zero the GH_TOKEN entry's secret tail before freeing. */
      if (strncmp(envp[i], "GH_TOKEN=", 9) == 0)
         secure_wipe(envp[i] + 9, strlen(envp[i] + 9));
      free(envp[i]);
   }
   free(envp);
}
