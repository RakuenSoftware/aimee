/* agent_config.c: config loading/saving, agent routing, role checking, auth resolution */
#include "config.h"
#include "aimee.h"
#include "util.h"
#include "agent_config.h"
#include "providers_client.h"

#include "agent_registry.h"
#include "agent_config_internal.h"
#include "model_registry.h"
#include "models_dev.h"
#include "model_provider.h"
#include "platform_path.h"
#include "provider_cli_adapter.h"
#include "cJSON.h"
#include "json_fluent.h"
#include <openssl/crypto.h>
#include <ctype.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <time.h>
#include "log.h"
#include <fcntl.h>
#include <unistd.h>

int agent_name_valid(const char *name)
{
   if (!name || !name[0])
      return 0;
   size_t len = strlen(name);
   if (len > 48)
      return 0;
   if (!isalnum((unsigned char)name[0]))
      return 0;
   for (size_t i = 0; i < len; i++)
   {
      char c = name[i];
      if (!isalnum((unsigned char)c) && c != '.' && c != '_' && c != '-')
         return 0;
   }
   return 1;
}

/* Is `endpoint` plausibly an address rather than a mis-parsed flag?
 *
 * `agent add` takes three POSITIONAL arguments, so a flag typed in the endpoint's
 * place was stored as the endpoint: `agent add x --provider openai --endpoint URL`
 * saved endpoint="--provider", reported the agent ON, and returned success. The only
 * symptom came later, from `agent probe`: "GET --provider/models returned -1".
 *
 * Deliberately narrow. A leading '-' is unambiguous evidence of a mis-parsed flag,
 * because no address starts with one. Requiring a scheme would reject the host:port
 * forms this command has always accepted, which is a guess, not evidence. */
int agent_endpoint_valid(const char *endpoint)
{
   return endpoint && endpoint[0] && endpoint[0] != '-';
}

/* Per-turn session id, set from the request in the chat/delegate workers (setter
 * below) and carried in the creds snapshot so a fan-out worker inherits the
 * originating turn's session identity. (Credentials no longer ride this: the
 * permanent vault is the single source — the legacy session-scoped RAM keyring
 * was retired in P4b.) */

/* Roster array: `models`, or the pre-rename `agents` older files carry. */
static cJSON *agent_roster_array(cJSON *root)
{
   cJSON *a = cJSON_GetObjectItemCaseSensitive(root, "models");
   return cJSON_IsArray(a) ? a : cJSON_GetObjectItemCaseSensitive(root, "agents");
}

/* --- Config path ---
 * models.json, else a pre-rename agents.json. Resolved per call, not migrated, so
 * a save rewrites the file the operator has instead of forking the roster in two. */
const char *agent_config_path(void)
{
   static char path[MAX_PATH_LEN];
   char legacy[MAX_PATH_LEN];
   snprintf(path, sizeof(path), "%s/models.json", config_default_dir());
   snprintf(legacy, sizeof(legacy), "%s/agents.json", config_default_dir());
   if (access(path, F_OK) != 0 && access(legacy, F_OK) == 0)
      snprintf(path, sizeof(path), "%s", legacy);
   return path;
}

int agent_endpoint_is_localish(const char *endpoint)
{
   if (!endpoint || !endpoint[0])
      return 0;
   return strstr(endpoint, "://localhost") || strstr(endpoint, "://127.") ||
          strstr(endpoint, "://0.0.0.0") || strstr(endpoint, "://[::1]") ||
          strstr(endpoint, "://10.") || strstr(endpoint, "://192.168.") ||
          strstr(endpoint, "://172.16.") || strstr(endpoint, "://172.17.") ||
          strstr(endpoint, "://172.18.") || strstr(endpoint, "://172.19.") ||
          strstr(endpoint, "://172.20.") || strstr(endpoint, "://172.21.") ||
          strstr(endpoint, "://172.22.") || strstr(endpoint, "://172.23.") ||
          strstr(endpoint, "://172.24.") || strstr(endpoint, "://172.25.") ||
          strstr(endpoint, "://172.26.") || strstr(endpoint, "://172.27.") ||
          strstr(endpoint, "://172.28.") || strstr(endpoint, "://172.29.") ||
          strstr(endpoint, "://172.30.") || strstr(endpoint, "://172.31.") ||
          strncmp(endpoint, "unix://", 7) == 0 || endpoint[0] == '/';
}

/* Registration prefix of a route-target name: everything before the first ':'.
 * Provider-general registration names targets `<registration>:<model>`, so this
 * identifies siblings that share credentials, endpoint and wire protocol. A
 * legacy single-model agent has no ':' and is its own registration. */
void agent_registration_prefix(const char *name, char *out, size_t out_len)
{
   if (!out || out_len == 0)
      return;
   out[0] = '\0';
   if (!name || !name[0])
      return;
   const char *colon = strchr(name, ':');
   size_t n = colon ? (size_t)(colon - name) : strlen(name);
   if (n >= out_len)
      n = out_len - 1;
   memcpy(out, name, n);
   out[n] = '\0';
}

/* See agent_config.h for why this compares the stored registration rather than
 * parsing the name, and why an unregistered agent has no siblings. */
int agent_same_registration(const agent_t *a, const agent_t *b)
{
   if (!a || !b)
      return 0;
   if (!a->registration[0] || !b->registration[0])
      return 0;
   return strcmp(a->registration, b->registration) == 0;
}

const char *agent_scope_name(agent_scope_t s)
{
   switch (s)
   {
   case AGENT_SCOPE_BOUNDED:
      return "bounded";
   case AGENT_SCOPE_WHOLE_TASK:
      return "whole_task";
   default:
      return "";
   }
}

agent_scope_t agent_scope_from_string(const char *s)
{
   if (!s || !s[0])
      return AGENT_SCOPE_UNSET;
   if (strcmp(s, "bounded") == 0)
      return AGENT_SCOPE_BOUNDED;
   if (strcmp(s, "whole_task") == 0 || strcmp(s, "whole-task") == 0)
      return AGENT_SCOPE_WHOLE_TASK;
   return AGENT_SCOPE_UNSET;
}

const char *agent_catalog_provider(const agent_t *ag)
{
   if (!ag)
      return "";
   return ag->catalog_provider[0] ? ag->catalog_provider : ag->provider;
}

/* Effective per-model limits, in one place.
 *
 * Precedence is DECLARED first, then the model catalog. Declaration is tested
 * by its AGENT_DECL_* bit, not by "> 0": callers used to key on the value, so a
 * deliberately-declared 0 fell through to the catalog and the operator was
 * silently overruled by a number they had explicitly replaced.
 *
 * These exist so the catalog branch is written once. It is being removed --
 * operator declaration and the provider's own reply are becoming the only
 * sources -- and a single call site is the difference between deleting three
 * lines and auditing every consumer again. Returns 0 for "unknown", which every
 * caller already models. */
/* NOTE the asymmetry with prices, which is deliberate. A declared 0 PRICE is a
 * real statement ("this seat costs nothing per token") and must survive. A
 * declared 0 CAPACITY is not a capacity at all -- there is no such thing as a
 * model with a zero-token window -- so here 0 reads as "unknown" and resolution
 * continues to the catalog. The declared BIT still matters: it is what keeps the
 * key in agents.json across a save. Only the resolved MEANING differs. */
int agent_declared_context_window(const agent_t *ag)
{
   if (!ag)
      return 0;
   /* Keyed on the VALUE, not on AGENT_DECL_CONTEXT_WINDOW. The bit records that
    * a value came from config so it round-trips on save; it is not the test for
    * whether one is set. Several paths assign this field directly on an agent_t
    * -- ag_probe_slots' auto-detection, and callers building an agent in memory
    * -- and requiring the bit made every one of those silently ignored. A
    * capacity needs no such bit anyway: 0 is not a capacity, so "> 0" carries
    * exactly the same information. Prices are the opposite and do need theirs,
    * because a declared 0 there is a real statement. */
   /* Loaded agents have already adopted the catalog's figure at load
    * (agent_migrate_declare_from_catalog), so this normally answers from the
    * agent itself. The catalog lookup remains for an agent built in memory
    * rather than read from config -- and it is the single place left to delete
    * when the snapshot finally goes. */
   if (ag->middleware.context_window > 0)
      return ag->middleware.context_window;
   model_capability_t cap;
   if (model_capability_get(agent_catalog_provider(ag), ag->model, &cap) && cap.context_window > 0)
      return cap.context_window;
   return 0;
}

int agent_declared_max_output(const agent_t *ag)
{
   if (!ag)
      return 0;
   if (ag->max_output > 0)
      return ag->max_output;
   return model_max_output(agent_catalog_provider(ag), ag->model);
}

/* --- Load/Save config (with mtime cache) --- */

/* Materialize catalog figures as the agent's OWN values, once, at load.
 *
 * The bundled catalog is going away: it was a third-party snapshot that aged
 * out of date and silently outranked what operators had set. Everything that
 * consumes a per-model limit now reads the agent, so each agent has to carry
 * the numbers it has in fact been running on rather than looking them up.
 *
 * Follows the primary_only migration's shape deliberately: derive in MEMORY at
 * load and let the next agent_save_config persist it, rather than writing the
 * operator's config as a side effect of reading it. Resolution therefore works
 * on this boot, and the file catches up the first time anything saves.
 *
 * Strictly additive. A value the operator has already stated is never touched --
 * the catalog is what we are removing precisely because it is not authoritative.
 * A price is copied only when the catalog actually published one, because
 * writing a 0 would assert "this model is free", which is a different and much
 * worse claim than leaving it unstated. */

/* One agent out of the cached registry, without materialising the registry.
 *
 * The cache and its lock live in the config module (agent_registry.c); this side
 * owns parsing and knows the file path, so it supplies the stat() identity. */
static int agent_registry_pick(agent_t *out, agent_t *(*pick)(agent_config_t *, const void *),
                               const void *arg)
{
   if (!out || !pick)
      return -1;

   const char *path = agent_config_path();
   struct stat st;

   /* FAST PATH: cache current, answer already in memory. Nearly every request. */
   if (!config_cache_disabled() && stat(path, &st) == 0)
   {
      int rc = agent_registry_pick_cached(&st, out, pick, arg);
      if (rc >= 0)
         return rc == 0 ? 0 : -1; /* 1 means "current cache, no such agent" */
   }

   /* SLOW PATH: cold or stale. Load once -- on the HEAP, because 343 KB has no
    * business on a request thread's stack -- which republishes the cache as a
    * side effect, then answer from the fresh copy. */
   agent_config_t *tmp = malloc(sizeof(*tmp));
   if (!tmp)
      return -1;
   int rc = -1;
   if (agent_load_config(tmp) == 0)
   {
      agent_t *found = pick(tmp, arg);
      if (found)
      {
         memcpy(out, found, sizeof(*out));
         rc = 0;
      }
   }
   free(tmp);
   return rc;
}

static agent_t *agent_registry_pick_by_name(agent_config_t *cfg, const void *arg)
{
   return agent_find(cfg, (const char *)arg);
}

static agent_t *agent_registry_pick_default(agent_config_t *cfg, const void *arg)
{
   (void)arg;
   return agent_default_primary(cfg);
}

int agent_registry_find(const char *name, agent_t *out)
{
   if (!name || !name[0])
      return -1;
   return agent_registry_pick(out, agent_registry_pick_by_name, name);
}

int agent_registry_default_primary(agent_t *out)
{
   return agent_registry_pick(out, agent_registry_pick_default, NULL);
}

int agent_registry_resolve_ingress_model(const char *model, agent_t *out)
{
   int rc;

   if (!out)
      return -1;
   if (model && model[0] && strcmp(model, "aimee") != 0)
      rc = agent_registry_find(model, out);
   else
      rc = agent_registry_default_primary(out);
   if (rc != 0 || !out->enabled)
      return -1;
   return 0;
}

int agent_load_config(agent_config_t *cfg)
{
   memset(cfg, 0, sizeof(*cfg));

   cJSON *reply = providers_module_request("snapshot.load", NULL, "server", 0);
   if (!reply)
      return -1;
   cJSON *root = cJSON_DetachItemFromObject(reply, "config");
   cJSON_Delete(reply);
   if (!cJSON_IsObject(root))
   {
      cJSON_Delete(root);
      return -1;
   }

   const char *revision = cJSON_GetStringValue(cJSON_GetObjectItem(root, "revision"));
   if (revision)
      snprintf(cfg->revision, sizeof(cfg->revision), "%s", revision);
   /* Default agent */
   cJSON *def = cJSON_GetObjectItem(root, "default_agent");
   if (def && cJSON_IsString(def))
      snprintf(cfg->default_agent, MAX_AGENT_NAME, "%s", def->valuestring);

   /* Delegate preference is independent from the primary/chat default. */
   cJSON *delegate_def = cJSON_GetObjectItem(root, "default_delegate");
   if (delegate_def && cJSON_IsString(delegate_def))
      snprintf(cfg->default_delegate, MAX_AGENT_NAME, "%s", delegate_def->valuestring);

   /* Fallback chain */
   cJSON *fb = cJSON_GetObjectItem(root, "fallback_chain");
   if (fb && cJSON_IsArray(fb))
   {
      int n = cJSON_GetArraySize(fb);
      if (n > MAX_FALLBACK)
         n = MAX_FALLBACK;
      for (int i = 0; i < n; i++)
      {
         cJSON *item = cJSON_GetArrayItem(fb, i);
         if (cJSON_IsString(item))
            snprintf(cfg->fallback_chain[cfg->fallback_count++], MAX_AGENT_NAME, "%s",
                     item->valuestring);
      }
   }

   /* Roster array (models, or the pre-rename `agents`). */
   cJSON *agents = agent_roster_array(root);
   if (agents && cJSON_IsArray(agents))
   {
      int n = cJSON_GetArraySize(agents);
      if (n > MAX_AGENTS)
         n = MAX_AGENTS;

      for (int i = 0; i < n; i++)
      {
         cJSON *a = cJSON_GetArrayItem(agents, i);
         if (!cJSON_IsObject(a))
            continue;

         agent_t *ag = &cfg->agents[cfg->agent_count];
         memset(ag, 0, sizeof(*ag));

         cJSON *v;
         v = cJSON_GetObjectItem(a, "name");
         if (v && cJSON_IsString(v))
            snprintf(ag->name, MAX_AGENT_NAME, "%s", v->valuestring);

         v = cJSON_GetObjectItem(a, "endpoint");
         if (v && cJSON_IsString(v))
            snprintf(ag->endpoint, MAX_ENDPOINT_LEN, "%s", v->valuestring);

         v = cJSON_GetObjectItem(a, "model");
         if (v && cJSON_IsString(v))
            snprintf(ag->model, MAX_MODEL_LEN, "%s", v->valuestring);

         v = cJSON_GetObjectItem(a, "api_key");
         if (v && cJSON_IsString(v))
         {
            /* VERBATIM, both copies. Config does not resolve credentials: the
             * registry is cached for the process's lifetime and copied per
             * lookup, so a resolved secret here would be a credential living in
             * config and duplicated on every copy. agent_api_key_secret()
             * resolves at the point of use, in the vault module. */
            snprintf(ag->api_key_disk, MAX_API_KEY_LEN, "%s", v->valuestring);
            snprintf(ag->api_key, MAX_API_KEY_LEN, "%s", v->valuestring);
         }

         /* Optional credential pool. Each entry is {name, api_key_env};
          * sibling delegates lease distinct entries via the lease pool in
          * delegate_economics. Skipped when "credentials" is missing —
          * the single-token api_key path stays unchanged. */
         {
            cJSON *creds = cJSON_GetObjectItem(a, "credentials");
            if (creds && cJSON_IsArray(creds))
            {
               cJSON *c;
               cJSON_ArrayForEach(c, creds)
               {
                  if (!cJSON_IsObject(c) || ag->credential_count >= MAX_AGENT_CREDENTIALS)
                     continue;
                  cJSON *cname = cJSON_GetObjectItem(c, "name");
                  cJSON *cenv = cJSON_GetObjectItem(c, "api_key_env");
                  if (!cJSON_IsString(cname) || !cJSON_IsString(cenv))
                     continue;
                  agent_credential_t *slot = &ag->credentials[ag->credential_count];
                  snprintf(slot->name, sizeof(slot->name), "%s", cname->valuestring);
                  snprintf(slot->api_key_env, sizeof(slot->api_key_env), "%s", cenv->valuestring);
                  slot->cooldown_until_ms = 0;
                  ag->credential_count++;
               }
            }
         }

         v = cJSON_GetObjectItem(a, "auth_cmd");
         if (v && cJSON_IsString(v))
            snprintf(ag->auth_cmd, MAX_AUTH_CMD_LEN, "%s", v->valuestring);

         v = cJSON_GetObjectItem(a, "auth_type");
         if (v && cJSON_IsString(v))
            snprintf(ag->auth_type, sizeof(ag->auth_type), "%s", v->valuestring);
         if (!ag->auth_type[0])
            snprintf(ag->auth_type, sizeof(ag->auth_type), "%s", "bearer");

         v = cJSON_GetObjectItem(a, "provider");
         if (v && cJSON_IsString(v))
            snprintf(ag->provider, sizeof(ag->provider), "%s", v->valuestring);
         if (!ag->provider[0])
            snprintf(ag->provider, sizeof(ag->provider), "%s", "openai");
         /* Catalog identity is SEPARATE from the wire provider above: rewriting
          * `provider` for an Anthropic-wire third party would drop the
          * anthropic-version header, the x-api-key auth coercion, and the
          * credential env-var set. An explicit operator value always wins. */
         v = cJSON_GetObjectItem(a, "catalog_provider");
         if (v && cJSON_IsString(v) && v->valuestring[0])
         {
            snprintf(ag->catalog_provider, sizeof(ag->catalog_provider), "%s", v->valuestring);
            ag->catalog_provider_explicit =
                cJSON_IsTrue(cJSON_GetObjectItem(a, "catalog_provider_explicit"));
         }

         v = cJSON_GetObjectItem(a, "cost_tier");
         if (v && cJSON_IsNumber(v))
            ag->cost_tier = v->valueint;

         v = cJSON_GetObjectItem(a, "tier_price_exempt");
         if (v && cJSON_IsString(v))
            snprintf(ag->tier_price_exempt, sizeof(ag->tier_price_exempt), "%s", v->valuestring);

         /* Operator price override ($/Mtok); absent or negative leaves it unset
          * so the catalog answers. */
         /* isfinite() matters: a parser can yield +inf from an overflowing
          * literal, and a non-finite price defeats every ordered comparison in
          * the resolver, making an unset axis look set. */
         /* A PRESENT, valid key is a declaration -- including a declared 0,
          * which is how a free or subscription-priced seat says "genuinely
          * nothing per token" rather than "I did not configure this". An absent
          * key leaves both the value and its bit alone. */
         v = cJSON_GetObjectItem(a, "price_in_per_mtok");
         if (v && cJSON_IsNumber(v) && isfinite(v->valuedouble) && v->valuedouble >= 0.0)
         {
            ag->price_in_per_mtok = v->valuedouble;
            ag->declared |= AGENT_DECL_PRICE_IN;
         }
         v = cJSON_GetObjectItem(a, "price_out_per_mtok");
         if (v && cJSON_IsNumber(v) && isfinite(v->valuedouble) && v->valuedouble >= 0.0)
         {
            ag->price_out_per_mtok = v->valuedouble;
            ag->declared |= AGENT_DECL_PRICE_OUT;
         }
         v = cJSON_GetObjectItem(a, "price_cached_per_mtok");
         if (v && cJSON_IsNumber(v) && isfinite(v->valuedouble) && v->valuedouble >= 0.0)
         {
            ag->price_cached_per_mtok = v->valuedouble;
            ag->declared |= AGENT_DECL_PRICE_CACHED;
         }

         /* Declared per-model limits. context_window has always lived on the
          * middleware config (its "explicit override" field); max_output is new.
          * Both are the operator stating what the model does, which is the
          * authoritative source once the bundled catalog is gone. */
         v = cJSON_GetObjectItem(a, "context_window");
         if (v && cJSON_IsNumber(v) && isfinite(v->valuedouble) && v->valuedouble >= 0.0)
         {
            ag->middleware.context_window = (int)v->valuedouble;
            ag->declared |= AGENT_DECL_CONTEXT_WINDOW;
         }
         v = cJSON_GetObjectItem(a, "max_output");
         if (v && cJSON_IsNumber(v) && isfinite(v->valuedouble) && v->valuedouble >= 0.0)
         {
            ag->max_output = (int)v->valuedouble;
            ag->declared |= AGENT_DECL_MAX_OUTPUT;
         }

         /* An unrecognised value must not silently mean "no ceiling": that would
          * hand the hardest work to the seat the operator was trying to limit. */
         /* Persisted so same-registration fallback grouping survives a save. The
          * expanded targets are what get written (the registration entry itself
          * is not a route target), so without this the grouping silently
          * disappeared the first time anything called agent_save_config. */
         v = cJSON_GetObjectItem(a, "registration");
         if (v && cJSON_IsString(v))
            snprintf(ag->registration, sizeof(ag->registration), "%s", v->valuestring);

         v = cJSON_GetObjectItem(a, "max_scope");
         if (v && cJSON_IsString(v) && v->valuestring[0])
         {
            ag->max_scope = agent_scope_from_string(v->valuestring);
            if (ag->max_scope == AGENT_SCOPE_UNSET)
            {
               LOG_ERROR("agent",
                         "agents.json: agent '%s' has unknown max_scope '%s'; "
                         "use \"bounded\" or \"whole_task\"",
                         ag->name, v->valuestring);
               cJSON_Delete(root);
               return -1;
            }
         }

         v = cJSON_GetObjectItem(a, "max_tokens");
         ag->max_tokens = (v && cJSON_IsNumber(v)) ? v->valueint : AGENT_DEFAULT_MAX_TOKENS;

         v = cJSON_GetObjectItem(a, "timeout_ms");
         ag->timeout_ms = cJSON_IsNumber(v) ? v->valueint : 0;
         ag->enabled = cJSON_IsTrue(cJSON_GetObjectItem(a, "enabled"));
         ag->tools_enabled = cJSON_IsTrue(cJSON_GetObjectItem(a, "tools_enabled"));

         v = cJSON_GetObjectItem(a, "recommended_sampling");
         ag->recommended_sampling = (v && cJSON_IsBool(v)) ? cJSON_IsTrue(v) : 0;

         v = cJSON_GetObjectItem(a, "inject_respond_tool");
         ag->inject_respond_tool = (v && cJSON_IsBool(v)) ? cJSON_IsTrue(v) : 0;

         /* Per-agent delegate turn cap. Declared value wins verbatim:
          *   0  = unlimited (frontier agents, e.g. MiniMax-M3),
          *   >0 = explicit cap,
          *   absent -> -1 = inherit the per-role floor (see delegate_role.c).
          * The role floor never clamps a declared cap down. */
         v = cJSON_GetObjectItem(a, "max_turns");
         ag->max_turns = (v && cJSON_IsNumber(v)) ? v->valueint : -1;

         v = cJSON_GetObjectItem(a, "max_parallel");
         ag->max_parallel = cJSON_IsNumber(v) ? v->valueint : 0;

         v = cJSON_GetObjectItem(a, "exec_system_prompt");
         if (v && cJSON_IsString(v))
            snprintf(ag->exec_system_prompt, MAX_EXEC_PROMPT_LEN, "%s", v->valuestring);

         v = cJSON_GetObjectItem(a, "extra_headers");
         if (v && cJSON_IsString(v))
            snprintf(ag->extra_headers, sizeof(ag->extra_headers), "%s", v->valuestring);

         v = cJSON_GetObjectItem(a, "fallback_model");
         if (v && cJSON_IsString(v))
            snprintf(ag->fallback_model, sizeof(ag->fallback_model), "%s", v->valuestring);

         cJSON *exec_roles = cJSON_GetObjectItem(a, "exec_roles");
         if (exec_roles && cJSON_IsArray(exec_roles))
         {
            int ern = cJSON_GetArraySize(exec_roles);
            if (ern > MAX_EXEC_ROLES)
               ern = MAX_EXEC_ROLES;
            for (int j = 0; j < ern; j++)
            {
               cJSON *er = cJSON_GetArrayItem(exec_roles, j);
               if (cJSON_IsString(er))
                  snprintf(ag->exec_roles[ag->exec_role_count++], 32, "%s", er->valuestring);
            }
         }

         cJSON *roles = cJSON_GetObjectItem(a, "roles");
         if (roles && cJSON_IsArray(roles))
         {
            int rn = cJSON_GetArraySize(roles);
            if (rn > MAX_AGENT_ROLES)
               rn = MAX_AGENT_ROLES;
            for (int j = 0; j < rn; j++)
            {
               cJSON *r = cJSON_GetArrayItem(roles, j);
               if (cJSON_IsString(r))
                  snprintf(ag->roles[ag->role_count++], 32, "%s", r->valuestring);
            }
         }

         /* Personas this agent may be dispatched AS (delegate identities). An
          * absent/empty list means "all" (agent_supports_persona), so existing
          * agents.json without this key keep serving every persona. */
         cJSON *personas = cJSON_GetObjectItem(a, "personas");
         if (personas && cJSON_IsArray(personas))
         {
            int pn = cJSON_GetArraySize(personas);
            if (pn > MAX_AGENT_PERSONAS)
               pn = MAX_AGENT_PERSONAS;
            for (int j = 0; j < pn; j++)
            {
               cJSON *p = cJSON_GetArrayItem(personas, j);
               if (cJSON_IsString(p))
                  snprintf(ag->personas[ag->persona_count++], 32, "%s", p->valuestring);
            }
         }

         /* Middleware configuration (optional per-agent overrides) */
         cJSON *mw = cJSON_GetObjectItem(a, "middleware");
         if (mw && cJSON_IsObject(mw))
         {
            v = cJSON_GetObjectItem(mw, "cost_limit");
            if (v && cJSON_IsNumber(v))
               ag->middleware.cost_limit = v->valueint;
            v = cJSON_GetObjectItem(mw, "context_warn_pct");
            if (v && cJSON_IsNumber(v))
               ag->middleware.context_warn_pct = v->valueint;
            v = cJSON_GetObjectItem(mw, "auto_compact_pct");
            if (v && cJSON_IsNumber(v))
               ag->middleware.auto_compact_pct = v->valueint;
            v = cJSON_GetObjectItem(mw, "stall_threshold");
            if (v && cJSON_IsNumber(v))
               ag->middleware.stall_threshold = v->valueint;
            /* LEGACY LOCATION. context_window used to live only inside this
             * middleware block; it is now a declared property of the model at
             * the top level. Read the old spelling so existing configs keep
             * working, but let an explicit top-level value win and do not
             * re-write this one on save -- that is what migrates a config
             * forward the first time it is persisted. */
            v = cJSON_GetObjectItem(mw, "context_window");
            if (v && cJSON_IsNumber(v) && !(ag->declared & AGENT_DECL_CONTEXT_WINDOW))
            {
               ag->middleware.context_window = v->valueint;
               ag->declared |= AGENT_DECL_CONTEXT_WINDOW;
            }
         }

         /* CLI backend config */
         v = cJSON_GetObjectItem(a, "backend");
         if (v && cJSON_IsString(v))
         {
            if (strcmp(v->valuestring, AGENT_BACKEND_CLI_STDIO) == 0)
               snprintf(ag->backend, sizeof(ag->backend), "%s", AGENT_BACKEND_PROVIDER_CLI);
            else
               snprintf(ag->backend, sizeof(ag->backend), "%s", v->valuestring);
         }

         v = cJSON_GetObjectItem(a, "cli_cmd");
         if (v && cJSON_IsString(v))
            snprintf(ag->cli_cmd, sizeof(ag->cli_cmd), "%s", v->valuestring);

         v = cJSON_GetObjectItem(a, "cli_idle_timeout_ms");
         if (v && cJSON_IsNumber(v))
            ag->cli_idle_timeout_ms = v->valueint;

         v = cJSON_GetObjectItem(a, "session_reuse");
         ag->session_reuse = (v && cJSON_IsBool(v)) ? cJSON_IsTrue(v) : 0;

         /* Per-CLI adapter selector for provider-cli backend. Empty when
          * the backend is HTTP or tmux-cli. */
         v = cJSON_GetObjectItem(a, "cli_kind");
         if (v && cJSON_IsString(v))
            snprintf(ag->cli_kind, sizeof(ag->cli_kind), "%s", v->valuestring);
         v = cJSON_GetObjectItem(a, "is_server_hosted");
         if (v && cJSON_IsBool(v))
            ag->is_server_hosted = cJSON_IsTrue(v);
         ag->primary_only = cJSON_IsTrue(cJSON_GetObjectItem(a, "primary_only"));
         cfg->agent_count++;
      }
   }

   /* Network config */
   cJSON *net = cJSON_GetObjectItem(root, "network");
   if (net && cJSON_IsObject(net))
   {
      agent_network_t *nw = &cfg->network;
      memset(nw, 0, sizeof(*nw));

      cJSON *v;
      v = cJSON_GetObjectItem(net, "ssh_entry");
      if (v && cJSON_IsString(v))
         snprintf(nw->ssh_entry, sizeof(nw->ssh_entry), "%s", v->valuestring);
      v = cJSON_GetObjectItem(net, "ssh_key");
      if (v && cJSON_IsString(v))
         snprintf(nw->ssh_key, sizeof(nw->ssh_key), "%s", v->valuestring);

      cJSON *hosts = cJSON_GetObjectItem(net, "hosts");
      if (hosts && cJSON_IsArray(hosts))
      {
         int hn = cJSON_GetArraySize(hosts);
         if (hn > AGENT_MAX_NET_HOSTS)
            hn = AGENT_MAX_NET_HOSTS;
         for (int i = 0; i < hn; i++)
         {
            cJSON *h = cJSON_GetArrayItem(hosts, i);
            if (!cJSON_IsObject(h))
               continue;
            agent_net_host_t *host = &nw->hosts[nw->host_count];
            v = cJSON_GetObjectItem(h, "name");
            if (v && cJSON_IsString(v))
               snprintf(host->name, sizeof(host->name), "%s", v->valuestring);
            v = cJSON_GetObjectItem(h, "ip");
            if (v && cJSON_IsString(v))
               snprintf(host->ip, sizeof(host->ip), "%s", v->valuestring);
            v = cJSON_GetObjectItem(h, "user");
            if (v && cJSON_IsString(v))
               snprintf(host->user, sizeof(host->user), "%s", v->valuestring);
            v = cJSON_GetObjectItem(h, "port");
            host->port = (v && cJSON_IsNumber(v)) ? v->valueint : 0;
            v = cJSON_GetObjectItem(h, "desc");
            if (v && cJSON_IsString(v))
               snprintf(host->desc, sizeof(host->desc), "%s", v->valuestring);
            v = cJSON_GetObjectItem(h, "tunnel");
            if (v && cJSON_IsString(v))
               snprintf(host->tunnel, sizeof(host->tunnel), "%s", v->valuestring);
            nw->host_count++;
         }
      }

      cJSON *networks = cJSON_GetObjectItem(net, "networks");
      if (networks && cJSON_IsArray(networks))
      {
         int nn = cJSON_GetArraySize(networks);
         if (nn > AGENT_MAX_NETWORKS)
            nn = AGENT_MAX_NETWORKS;
         for (int i = 0; i < nn; i++)
         {
            cJSON *n = cJSON_GetArrayItem(networks, i);
            if (!cJSON_IsObject(n))
               continue;
            agent_net_def_t *nd = &nw->networks[nw->network_count];
            v = cJSON_GetObjectItem(n, "name");
            if (v && cJSON_IsString(v))
               snprintf(nd->name, sizeof(nd->name), "%s", v->valuestring);
            v = cJSON_GetObjectItem(n, "cidr");
            if (v && cJSON_IsString(v))
               snprintf(nd->cidr, sizeof(nd->cidr), "%s", v->valuestring);
            v = cJSON_GetObjectItem(n, "desc");
            if (v && cJSON_IsString(v))
               snprintf(nd->desc, sizeof(nd->desc), "%s", v->valuestring);
            nw->network_count++;
         }
      }

      /* Tunnel config */
      cJSON *tunnels = cJSON_GetObjectItem(net, "tunnels");
      if (tunnels && cJSON_IsArray(tunnels))
      {
         int tn = cJSON_GetArraySize(tunnels);
         if (tn > AGENT_MAX_TUNNELS)
            tn = AGENT_MAX_TUNNELS;
         agent_tunnel_mgr_t *tmgr = &cfg->tunnel_mgr;
         for (int i = 0; i < tn; i++)
         {
            cJSON *t = cJSON_GetArrayItem(tunnels, i);
            if (!cJSON_IsObject(t))
               continue;
            agent_tunnel_t *tun = &tmgr->tunnels[tmgr->tunnel_count];
            memset(tun, 0, sizeof(*tun));
            v = cJSON_GetObjectItem(t, "name");
            if (v && cJSON_IsString(v))
               snprintf(tun->name, sizeof(tun->name), "%s", v->valuestring);
            v = cJSON_GetObjectItem(t, "relay_ssh");
            if (v && cJSON_IsString(v))
               snprintf(tun->relay_ssh, sizeof(tun->relay_ssh), "%s", v->valuestring);
            v = cJSON_GetObjectItem(t, "relay_key");
            if (v && cJSON_IsString(v))
               snprintf(tun->relay_key, sizeof(tun->relay_key), "%s", v->valuestring);
            v = cJSON_GetObjectItem(t, "target_host");
            if (v && cJSON_IsString(v))
               snprintf(tun->target_host, sizeof(tun->target_host), "%s", v->valuestring);
            v = cJSON_GetObjectItem(t, "target_port");
            tun->target_port = (v && cJSON_IsNumber(v)) ? v->valueint : 22;
            v = cJSON_GetObjectItem(t, "reconnect_delay");
            tun->reconnect_delay_s = (v && cJSON_IsNumber(v)) ? v->valueint : 5;
            v = cJSON_GetObjectItem(t, "max_reconnects");
            tun->max_reconnects = (v && cJSON_IsNumber(v)) ? v->valueint : 0;
            tmgr->tunnel_count++;
         }
         if (tmgr->tunnel_count > 0)
            nw->tunnel_mgr = tmgr;
      }
   }

   cJSON_Delete(root);

   return 0;
}

/* Count the agents in the on-disk agents.json WITHOUT the cache — the cache can be
 * stale or empty, and this guards against destroying the real file. Returns the
 * count, or -1 if the file is absent or unparseable (i.e. "nothing to protect"). */

static int agent_save_config_impl(agent_config_t *cfg, int emptied_by_removal)
{
   cJSON *root = cJSON_CreateObject();

   if (cfg->default_agent[0])
      JSON_ADD_STR(root, "default_agent", cfg->default_agent);
   if (cfg->default_delegate[0])
      JSON_ADD_STR(root, "default_delegate", cfg->default_delegate);

   cJSON *fb = cJSON_CreateArray();
   for (int i = 0; i < cfg->fallback_count; i++)
      cJSON_AddItemToArray(fb, cJSON_CreateString(cfg->fallback_chain[i]));
   cJSON_AddItemToObject(root, "fallback_chain", fb);

   cJSON *agents = cJSON_CreateArray();
   for (int i = 0; i < cfg->agent_count; i++)
   {
      const agent_t *ag = &cfg->agents[i];
      cJSON *a = cJSON_CreateObject();

      JSON_ADD_STR(a, "name", ag->name);
      JSON_ADD_STR(a, "endpoint", ag->endpoint);
      JSON_ADD_STR(a, "model", ag->model);
      /* Persist references only. A literal API key is a credential and therefore
       * belongs in Vault, never agents.json. In-memory callers may still put a
       * $VAR reference in api_key; first boot resolves that from the locked
       * Vault-backed runtime cache and the server migration canonicalizes it to
       * the per-agent Vault slot. */
      {
         const char *disk_key = ag->api_key_disk[0] ? ag->api_key_disk : ag->api_key;
         if (disk_key[0] == '$' && disk_key[1])
            JSON_ADD_STR(a, "api_key", disk_key);
      }
      if (ag->auth_cmd[0])
         JSON_ADD_STR(a, "auth_cmd", ag->auth_cmd);
      if (strcmp(ag->auth_type, "bearer") != 0)
         JSON_ADD_STR(a, "auth_type", ag->auth_type);
      if (strcmp(ag->provider, "openai") != 0)
         JSON_ADD_STR(a, "provider", ag->provider);
      /* Only an operator-supplied catalog_provider round-trips; a derived value
       * is recomputed at load so the derivation rules stay authoritative. */
      if (ag->catalog_provider_explicit && ag->catalog_provider[0])
         JSON_ADD_STR(a, "catalog_provider", ag->catalog_provider);
      if (ag->tier_price_exempt[0])
         JSON_ADD_STR(a, "tier_price_exempt", ag->tier_price_exempt);
      /* Driven off the declared bits, not off "> 0". Writing only positive
       * values silently dropped a declared 0 on every save, so a free seat
       * reverted to "unset" the first time anything persisted the config. */
      if (ag->declared & AGENT_DECL_PRICE_IN)
         cJSON_AddNumberToObject(a, "price_in_per_mtok", ag->price_in_per_mtok);
      if (ag->declared & AGENT_DECL_PRICE_OUT)
         cJSON_AddNumberToObject(a, "price_out_per_mtok", ag->price_out_per_mtok);
      if (ag->declared & AGENT_DECL_PRICE_CACHED)
         cJSON_AddNumberToObject(a, "price_cached_per_mtok", ag->price_cached_per_mtok);
      /* Capacities serialize on value: an explicit 0 carries no information a
       * reader could act on, and writing one would only make configs noisier. */
      if (ag->middleware.context_window > 0)
         cJSON_AddNumberToObject(a, "context_window", ag->middleware.context_window);
      if (ag->max_output > 0)
         cJSON_AddNumberToObject(a, "max_output", ag->max_output);
      if (ag->max_scope != AGENT_SCOPE_UNSET)
         JSON_ADD_STR(a, "max_scope", agent_scope_name(ag->max_scope));
      if (ag->registration[0])
         JSON_ADD_STR(a, "registration", ag->registration);

      cJSON *roles = cJSON_CreateArray();
      for (int j = 0; j < ag->role_count; j++)
         cJSON_AddItemToArray(roles, cJSON_CreateString(ag->roles[j]));
      cJSON_AddItemToObject(a, "roles", roles);

      /* Only serialize personas when explicitly set (empty = "all" implicitly). */
      if (ag->persona_count > 0)
      {
         cJSON *personas = cJSON_CreateArray();
         for (int j = 0; j < ag->persona_count; j++)
            cJSON_AddItemToArray(personas, cJSON_CreateString(ag->personas[j]));
         cJSON_AddItemToObject(a, "personas", personas);
      }

      cJSON_AddNumberToObject(a, "cost_tier", ag->cost_tier);
      cJSON_AddNumberToObject(a, "max_tokens", ag->max_tokens);
      cJSON_AddNumberToObject(a, "timeout_ms", ag->timeout_ms);
      cJSON_AddBoolToObject(a, "enabled", ag->enabled);
      if (ag->recommended_sampling)
         cJSON_AddBoolToObject(a, "recommended_sampling", ag->recommended_sampling);
      if (ag->max_parallel != AGENT_DEFAULT_MAX_PARALLEL)
         cJSON_AddNumberToObject(a, "max_parallel", ag->max_parallel);

      if (ag->tools_enabled)
      {
         cJSON_AddBoolToObject(a, "tools_enabled", ag->tools_enabled);
         if (ag->inject_respond_tool)
            cJSON_AddBoolToObject(a, "inject_respond_tool", ag->inject_respond_tool);
         cJSON_AddNumberToObject(a, "max_turns", ag->max_turns);
         if (ag->exec_system_prompt[0])
            JSON_ADD_STR(a, "exec_system_prompt", ag->exec_system_prompt);
      }
      if (ag->exec_role_count > 0)
      {
         cJSON *er = cJSON_CreateArray();
         for (int j = 0; j < ag->exec_role_count; j++)
            cJSON_AddItemToArray(er, cJSON_CreateString(ag->exec_roles[j]));
         cJSON_AddItemToObject(a, "exec_roles", er);
      }

      if (ag->extra_headers[0])
         JSON_ADD_STR(a, "extra_headers", ag->extra_headers);
      if (ag->fallback_model[0])
         JSON_ADD_STR(a, "fallback_model", ag->fallback_model);
      if (ag->backend[0])
         JSON_ADD_STR(a, "backend", ag->backend);
      if (ag->cli_cmd[0])
         JSON_ADD_STR(a, "cli_cmd", ag->cli_cmd);
      if (ag->cli_idle_timeout_ms > 0)
         cJSON_AddNumberToObject(a, "cli_idle_timeout_ms", ag->cli_idle_timeout_ms);
      if (ag->session_reuse)
         cJSON_AddBoolToObject(a, "session_reuse", ag->session_reuse);
      if (ag->cli_kind[0])
         JSON_ADD_STR(a, "cli_kind", ag->cli_kind);
      if (ag->is_server_hosted)
         cJSON_AddBoolToObject(a, "is_server_hosted", 1);
      /* Always written (both true AND false), unlike is_server_hosted: an absent
       * key triggers the legacy migration on load (see agent_load_config), so
       * persisting false explicitly is what makes unchecking Primary Agent Only
       * on a claude-CLI agent stick instead of re-defaulting to true. */
      cJSON_AddBoolToObject(a, "primary_only", ag->primary_only);

      /* Middleware config: only write if any non-zero field is set */
      {
         const agent_middleware_cfg_t *mwc = &ag->middleware;
         /* context_window is excluded: it now serializes at the top level, so
          * including it here would emit an otherwise-empty middleware object for
          * an agent whose only setting has moved out of this block. */
         if (mwc->cost_limit || mwc->context_warn_pct || mwc->auto_compact_pct ||
             mwc->stall_threshold)
         {
            cJSON *mw = cJSON_CreateObject();
            if (mwc->cost_limit)
               cJSON_AddNumberToObject(mw, "cost_limit", mwc->cost_limit);
            if (mwc->context_warn_pct)
               cJSON_AddNumberToObject(mw, "context_warn_pct", mwc->context_warn_pct);
            if (mwc->auto_compact_pct)
               cJSON_AddNumberToObject(mw, "auto_compact_pct", mwc->auto_compact_pct);
            if (mwc->stall_threshold)
               cJSON_AddNumberToObject(mw, "stall_threshold", mwc->stall_threshold);
            /* context_window is deliberately NOT written here any more -- it is
             * serialized once, at the top level, off AGENT_DECL_CONTEXT_WINDOW.
             * Writing both would leave two spellings of one value free to drift. */
            cJSON_AddItemToObject(a, "middleware", mw);
         }
      }

      cJSON_AddItemToArray(agents, a);
   }
   cJSON_AddItemToObject(root, "models", agents);

   /* Network config (only write if ssh_entry is set) */
   if (cfg->network.ssh_entry[0])
   {
      const agent_network_t *nw = &cfg->network;
      cJSON *net = cJSON_CreateObject();
      JSON_ADD_STR(net, "ssh_entry", nw->ssh_entry);
      if (nw->ssh_key[0])
         JSON_ADD_STR(net, "ssh_key", nw->ssh_key);

      if (nw->host_count > 0)
      {
         cJSON *hosts = cJSON_CreateArray();
         for (int i = 0; i < nw->host_count; i++)
         {
            const agent_net_host_t *h = &nw->hosts[i];
            cJSON *hobj = cJSON_CreateObject();
            JSON_ADD_STR(hobj, "name", h->name);
            JSON_ADD_STR(hobj, "ip", h->ip);
            if (h->user[0])
               JSON_ADD_STR(hobj, "user", h->user);
            if (h->port > 0)
               cJSON_AddNumberToObject(hobj, "port", h->port);
            if (h->desc[0])
               JSON_ADD_STR(hobj, "desc", h->desc);
            if (h->tunnel[0])
               JSON_ADD_STR(hobj, "tunnel", h->tunnel);
            cJSON_AddItemToArray(hosts, hobj);
         }
         cJSON_AddItemToObject(net, "hosts", hosts);
      }

      if (nw->network_count > 0)
      {
         cJSON *nets = cJSON_CreateArray();
         for (int i = 0; i < nw->network_count; i++)
         {
            const agent_net_def_t *nd = &nw->networks[i];
            cJSON *nobj = cJSON_CreateObject();
            JSON_ADD_STR(nobj, "name", nd->name);
            JSON_ADD_STR(nobj, "cidr", nd->cidr);
            if (nd->desc[0])
               JSON_ADD_STR(nobj, "desc", nd->desc);
            cJSON_AddItemToArray(nets, nobj);
         }
         cJSON_AddItemToObject(net, "networks", nets);
      }

      /* Tunnel config */
      if (cfg->tunnel_mgr.tunnel_count > 0)
      {
         cJSON *tarr = cJSON_CreateArray();
         for (int i = 0; i < cfg->tunnel_mgr.tunnel_count; i++)
         {
            const agent_tunnel_t *tun = &cfg->tunnel_mgr.tunnels[i];
            cJSON *tobj = cJSON_CreateObject();
            JSON_ADD_STR(tobj, "name", tun->name);
            JSON_ADD_STR(tobj, "relay_ssh", tun->relay_ssh);
            if (tun->relay_key[0])
               JSON_ADD_STR(tobj, "relay_key", tun->relay_key);
            JSON_ADD_STR(tobj, "target_host", tun->target_host);
            cJSON_AddNumberToObject(tobj, "target_port", tun->target_port);
            if (tun->reconnect_delay_s != 5)
               cJSON_AddNumberToObject(tobj, "reconnect_delay", tun->reconnect_delay_s);
            if (tun->max_reconnects > 0)
               cJSON_AddNumberToObject(tobj, "max_reconnects", tun->max_reconnects);
            cJSON_AddItemToArray(tarr, tobj);
         }
         cJSON_AddItemToObject(net, "tunnels", tarr);
      }

      cJSON_AddItemToObject(root, "network", net);
   }

   cJSON *args = cJSON_CreateObject();
   cJSON_AddItemToObject(args, "config", root);
   cJSON_AddBoolToObject(args, "allow_empty", emptied_by_removal);
   cJSON_AddStringToObject(args, "expected_revision", cfg->revision);
   cJSON *reply = providers_module_request("snapshot.save", args, "server", 0);
   cJSON_Delete(args);
   const char *status = reply ? cJSON_GetStringValue(cJSON_GetObjectItem(reply, "status")) : NULL;
   int ok = status && strcmp(status, "ok") == 0;
   const char *revision = cJSON_GetStringValue(cJSON_GetObjectItem(reply, "revision"));
   if (ok && revision)
      snprintf(cfg->revision, sizeof(cfg->revision), "%s", revision);
   cJSON_Delete(reply);
   if (ok)
      agent_registry_cache_invalidate();
   return ok ? 0 : -1;
}

int agent_save_config(agent_config_t *cfg)
{
   return agent_save_config_impl(cfg, 0);
}

int agent_save_config_after_removal(agent_config_t *cfg)
{
   return agent_save_config_impl(cfg, 1);
}
