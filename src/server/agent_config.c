/* agent_config.c: config loading/saving, agent routing, role checking, auth resolution */
#include "aimee.h"
#include "util.h"
#include "agent_config.h"
#include "agent_config_internal.h"
#include "vault_principal.h" /* VAULT_PRINCIPAL_MAX for the per-turn vault principal */
#include "runtime_secret.h"
#include "vault_service.h" /* vault_service_* : the permanent credential store (P4) */
#include "model_registry.h"
#include "models_dev.h"
#include "model_provider.h"
#include "platform_path.h"
#include "provider_cli_adapter.h"
#include "oauth_flow.h" /* oauth_token_store/get : vault-backed auto-refreshing codex token */
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
static _Thread_local char g_request_session_id[128];

/* The roster array. `models` is the current key; `agents` is what every file
 * written before the rename carries. Both are read so an existing deployment
 * loads unchanged; agent_save_config writes the canonical `models`. */
static cJSON *agent_roster_array(cJSON *root)
{
   cJSON *arr = cJSON_GetObjectItemCaseSensitive(root, "models");
   if (cJSON_IsArray(arr))
      return arr;
   return cJSON_GetObjectItemCaseSensitive(root, "agents");
}

/* --- Config path --- */

/* The roster file. `models.json` is the current name -- a roster entry is one
 * (endpoint, model) target, so it is a model, not an agent.
 *
 * `agents.json` is the pre-rename name and every existing deployment has one.
 * Resolution is therefore: use models.json when it exists, else fall back to an
 * existing agents.json, else models.json for a fresh install. Deliberately
 * resolved per call rather than migrated at startup, so a save rewrites the file
 * the operator actually has instead of silently forking the roster into two
 * files -- one of which nothing would read again. */
const char *agent_config_path(void)
{
   static char path[MAX_PATH_LEN];
   const char *dir = config_default_dir();

   snprintf(path, sizeof(path), "%s/models.json", dir);
   if (access(path, F_OK) == 0)
      return path;

   char legacy[MAX_PATH_LEN];
   snprintf(legacy, sizeof(legacy), "%s/agents.json", dir);
   if (access(legacy, F_OK) == 0)
   {
      snprintf(path, sizeof(path), "%s", legacy);
      return path;
   }

   snprintf(path, sizeof(path), "%s/models.json", dir);
   return path;
}

/* --- First-boot environment references ---------------------------------- */

void agent_expand_env(const char *src, char *dst, size_t dst_len)
{
   if (!src || !src[0])
   {
      dst[0] = '\0';
      return;
   }

   if (src[0] == '$')
   {
      /* Credential environment variables are consumed and unset before config
       * loading. The only runtime view is the locked-memory cache hydrated from
       * Vault; dotenv and direct getenv fallbacks are intentionally forbidden. */
      if (runtime_secret_get(src + 1, dst, dst_len))
         return;
      dst[0] = '\0';
      return;
   }

   snprintf(dst, dst_len, "%s", src);
}

static const char *const openrouter_env_vars[] = {"OPENROUTER_API_KEY", NULL};
static const char *const mistral_env_vars[] = {"MISTRAL_API_KEY", NULL};
static const char *const anthropic_env_vars[] = {"ANTHROPIC_API_KEY", NULL};
static const char *const gemini_env_vars[] = {"GEMINI_API_KEY", "GOOGLE_API_KEY", NULL};
static const char *const minimax_env_vars[] = {"MINIMAX_API_KEY", NULL};

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

static int agent_default_inject_respond_tool(const agent_t *ag)
{
   if (!ag || !ag->tools_enabled)
      return 0;
   if (strcmp(ag->provider, "ollama") == 0 || strcmp(ag->provider, "llama_native") == 0 ||
       strcmp(ag->provider, "llama-eval") == 0)
      return 1;
   if (strcmp(ag->auth_type, "none") == 0 && agent_endpoint_is_localish(ag->endpoint))
      return 1;
   return 0;
}

static const char *const *agent_provider_env_vars(const char *provider)
{
   if (!provider || !provider[0])
      return NULL;
   if (strcmp(provider, "openrouter") == 0)
      return openrouter_env_vars;
   if (strcmp(provider, "mistral") == 0)
      return mistral_env_vars;
   if (strcmp(provider, "anthropic") == 0)
      return anthropic_env_vars;
   if (strcmp(provider, "gemini") == 0)
      return gemini_env_vars;
   if (strcmp(provider, "minimax") == 0)
      return minimax_env_vars;
   return NULL;
}

static void agent_normalize_legacy_claude_cli(agent_t *ag)
{
   if (!ag)
      return;
   if (strcmp(ag->backend, AGENT_BACKEND_PROVIDER_CLI) != 0 || strcmp(ag->cli_kind, "claude") != 0)
      return;

   snprintf(ag->backend, sizeof(ag->backend), "%s", AGENT_BACKEND_TMUX_CLI);
   if (!ag->provider[0] || strcmp(ag->provider, "openai") == 0)
      snprintf(ag->provider, sizeof(ag->provider), "%s", "claude");
   if (!ag->auth_type[0] || strcmp(ag->auth_type, "bearer") == 0)
      snprintf(ag->auth_type, sizeof(ag->auth_type), "%s", "none");
   if (!ag->cli_cmd[0] || strcmp(ag->cli_cmd, "claude-p") == 0)
      snprintf(ag->cli_cmd, sizeof(ag->cli_cmd), "%s", "claude");
   ag->cli_kind[0] = '\0';
}

static void agent_derive_catalog_provider(agent_t *ag);

/* Cost tier derived from a model's BASE input price. Provider-general
 * registration expands one operator entry into several model targets, and
 * sol/terra/luna are $5.00/$2.50/$1.00 — they cannot share a tier inherited from
 * the registration, or "cheapest first" would pick arbitrarily among them.
 *
 * Deliberately banded rather than ordering on the raw price: routing directly on
 * a float makes route choice churn every time the catalog refreshes, whereas a
 * band is a stable policy boundary. Thresholds sit between the observed fleet
 * price points ($0.30 / $0.95-1.00 / $2.00-2.50 / $5.00 / $10.00) so each lands
 * in its own tier. */
static int model_price_to_cost_tier(double in_per_mtok)
{
   if (in_per_mtok <= 0.0)
      return -1; /* unknown: caller keeps the registration's declared tier */
   if (in_per_mtok < 0.5)
      return 0;
   if (in_per_mtok < 1.5)
      return 1;
   if (in_per_mtok < 3.5)
      return 2;
   if (in_per_mtok < 7.5)
      return 3;
   return 4;
}

/* Expand a provider-general registration ("models": [...]) into one runtime
 * agent per routable model.
 *
 * A model endpoint is what the runtime actually schedules: health, admission,
 * cost and fallback are all per-model already. Materialising a target per model
 * therefore needs no change to routing, dispatch or admission, whereas making
 * routing return (agent, model) would thread a new compound identity through the
 * highest-risk path. The registration stays the operator-facing object; these
 * are its runtime targets.
 *
 * Targets are named `<registration>:<model>`, matching the canonical ref form
 * model_capability_resolve_ref() already parses, so `--via codex:gpt-5.6-sol`
 * addresses exactly one target and health keys stay distinct per model.
 *
 * Returns the number of targets written, or -1 if they do not fit — the caller
 * must then reject the whole config rather than register a partial fleet, since
 * a silently truncated expansion would drop models an operator declared. */
static int agent_expand_one_model(agent_config_t *cfg, const agent_t *base, const char *model_id);

/* Curated allowlist for "models": "auto". The provider CATALOG is discovery
 * data, not exposure policy: a model appearing in it does not prove it is
 * intended for this product, has complete capability metadata, or is enabled for
 * the account. Returns NULL when the profile publishes no curated set, so "auto"
 * fails loudly rather than exposing whatever the provider happens to list. */
static const char **agent_provider_routable_models(const agent_t *base)
{
   const char *vendor = agent_catalog_provider(base);
   model_provider_t *p = vendor && vendor[0] ? model_provider_get(vendor) : NULL;
   if (!p && base->provider[0])
      p = model_provider_get(base->provider);
   return p ? p->routable_models : NULL;
}

static int agent_expand_provider_models(agent_config_t *cfg, const agent_t *base, cJSON *models)
{
   int written = 0;

   /* "models": "auto" -> the provider profile's curated allowlist, so an
    * operator can register a provider without naming its models at all. */
   if (cJSON_IsString(models))
   {
      if (strcmp(cJSON_GetStringValue(models), "auto") != 0)
         return -1;
      const char **curated = agent_provider_routable_models(base);
      if (!curated || !curated[0])
         return -1;
      for (int i = 0; curated[i]; i++)
      {
         int rc = agent_expand_one_model(cfg, base, curated[i]);
         if (rc < 0)
            return -1;
         written += rc;
      }
      return written;
   }

   cJSON *m;
   cJSON_ArrayForEach(m, models)
   {
      const char *model_id = cJSON_GetStringValue(m);
      /* A non-string or empty entry is a config error. Skipping it would hand
       * back a partial fleet with no indication anything was dropped. */
      if (!model_id || !model_id[0])
         return -1;
      int rc = agent_expand_one_model(cfg, base, model_id);
      if (rc < 0)
         return -1;
      written += rc;
   }
   return written;
}

/* Materialise one runtime target. Returns 1 when written, 0 when skipped as a
 * duplicate, -1 when it does not fit. */
static int agent_expand_one_model(agent_config_t *cfg, const agent_t *base, const char *model_id)
{
   char name[MAX_AGENT_NAME];
   int n = snprintf(name, sizeof(name), "%s:%s", base->name, model_id);
   /* A truncated name would collide with a sibling target and, because health
    * and --via both key on the name, silently conflate two models. Reject the
    * registration instead. */
   if (n < 0 || (size_t)n >= sizeof(name))
      return -1;

   /* A name collision - a repeated model id, or a legacy agent already using the
    * generated name - must REJECT the config, not silently drop a target. Health
    * and --via both key on the name, so two entries sharing one would conflate
    * distinct models, and quietly registering fewer models than the operator
    * declared routes work to a set they never approved. */
   for (int i = 0; i < cfg->agent_count; i++)
   {
      if (strcmp(cfg->agents[i].name, name) == 0)
         return -1;
   }

   if (cfg->agent_count >= MAX_AGENTS)
      return -1;

   agent_t *ag = &cfg->agents[cfg->agent_count];
   *ag = *base;
   snprintf(ag->model, sizeof(ag->model), "%s", model_id);
   snprintf(ag->name, sizeof(ag->name), "%s", name);
   /* Record the generating registration so fallback grouping never has to infer
    * it from the name. */
   snprintf(ag->registration, sizeof(ag->registration), "%s", base->name);
   /* Re-derive per target, since the vendor can depend on the model id — but
    * ONLY when the registration's value was itself derived. An operator who
    * pinned catalog_provider on the registration (a gateway speaking one wire
    * format while serving another vendor's models) means it for every target;
    * discarding it would silently swap the vendor identity that drives capability
    * lookup, price, tier derivation and the canonical model ref. */
   if (!ag->catalog_provider_explicit)
   {
      ag->catalog_provider[0] = '\0';
      agent_derive_catalog_provider(ag);
   }

   model_capability_t cap;
   if (model_capability_get(agent_catalog_provider(ag), ag->model, &cap))
   {
      int tier = model_price_to_cost_tier(cap.cost_in_per_mtok);
      if (tier >= 0)
         ag->cost_tier = tier;
   }
   else
   {
      /* The curated list is compiled in, so a vendor rename or retirement leaves
       * a target the catalog cannot describe: it keeps the registration's tier
       * and fails only when a request is sent. Warn loudly rather than fail the
       * load — a local metadata gap should not take the whole fleet down, and
       * routing's capability gate already declines to select what it cannot
       * describe. */
      LOG_WARN("agent",
               "agent '%s': no catalog entry for %s:%s — tier and capability are unverified; "
               "the model may have been renamed or retired",
               ag->name, agent_catalog_provider(ag), ag->model);
   }

   cfg->agent_count++;
   return 1;
}

static void agent_normalize_builtin_cost_tier(agent_t *ag)
{
   if (!ag)
      return;
   if (strcmp(ag->backend, AGENT_BACKEND_PROVIDER_CLI) == 0 &&
       strcmp(ag->cli_kind, "mistral-plan") == 0 && ag->cost_tier > 0)
      ag->cost_tier = 0;
}

/* Vendor (catalog) identity for capability lookup. `provider` names the WIRE
 * SHAPE; a third-party vendor served over another vendor's wire format needs its
 * own catalog key or every model_capability_get() misses and falls through to
 * the heuristic under the wrong provider branch. */

/* Registrable vendor domains. Matching is on HOST LABELS, never a substring of
 * the whole URL: "https://api.kimi.com.attacker.example/v1" contains
 * "api.kimi.com" but its host is not under kimi.com, and a path or query segment
 * must never select a vendor. */
static const struct
{
   const char *domain;
   const char *vendor;
} g_catalog_vendor_domains[] = {
    {"minimax.io", "minimax"},
    {"minimaxi.com", "minimax"},
    {"minimax.com", "minimax"},
    {"kimi.com", "moonshotai"},
    {"moonshot.cn", "moonshotai"},
    {"moonshot.ai", "moonshotai"},
    {NULL, NULL},
};

/* Wire/CLI provider names that are NOT catalog vendor keys. aimee names some
 * providers after the CLI or product ("claude" for the Claude CLI/OAuth seat,
 * "chatgpt"/"codex" for the Codex seat) while models.dev keys the vendor
 * ("anthropic", "openai"). Without this map the primary agent resolves NO
 * capability flags at all and an 8192 output ceiling. */
static const struct
{
   const char *wire;
   const char *vendor;
} g_catalog_provider_aliases[] = {
    {"claude", "anthropic"},
    {"chatgpt", "openai"},
    {"codex", "openai"},
    {NULL, NULL},
};

/* Vendor namespaces as they appear in gateway/OpenRouter model ids
 * ("moonshotai/kimi-k2.7-code"). */
static const struct
{
   const char *ns;
   const char *vendor;
} g_catalog_vendor_namespaces[] = {
    {"minimax", "minimax"},
    {"moonshotai", "moonshotai"},
    {"moonshot", "moonshotai"},
    {"kimi", "moonshotai"},
    {NULL, NULL},
};

/* Lowercased host of an endpoint URL, without scheme, userinfo, port or path.
 * Returns 0 when no host could be parsed.
 *
 * The scheme must be ANCHORED: searching for "://" anywhere lets a scheme-less
 * endpoint smuggle an authority through a path segment
 * ("gateway.example/relay://api.minimax.io/v1" would otherwise parse as
 * api.minimax.io), which for the legacy wire-provider rewrite would change auth
 * and credential selection. Scheme-relative "//host/path" is accepted. */
static int agent_endpoint_host(const char *endpoint, char *out, size_t out_len)
{
   if (!endpoint || !endpoint[0] || !out || out_len == 0)
      return 0;
   out[0] = '\0';

   const char *p = endpoint;
   if (p[0] == '/' && p[1] == '/')
   {
      p += 2; /* scheme-relative */
   }
   else
   {
      /* scheme = ALPHA *( ALPHA / DIGIT / "+" / "-" / "." ) ":" "//" */
      size_t i = 0;
      if (isalpha((unsigned char)p[0]))
      {
         i = 1;
         while (p[i] && (isalnum((unsigned char)p[i]) || p[i] == '+' || p[i] == '-' || p[i] == '.'))
            i++;
      }
      if (i > 0 && p[i] == ':' && p[i + 1] == '/' && p[i + 2] == '/')
         p += i + 3;
      /* else: no scheme — treat the whole string as an authority + path. */
   }

   /* Userinfo (user:pass@host) must not be mistaken for the host, but only when
    * the '@' precedes the authority's end. */
   const char *authority_end = p + strcspn(p, "/?#");
   const char *at = memchr(p, '@', (size_t)(authority_end - p));
   if (at)
      p = at + 1;

   size_t n;
   if (p[0] == '[')
   {
      /* Bracketed IPv6 literal: the colons inside are not a port separator. */
      const char *close = memchr(p, ']', (size_t)(authority_end - p));
      if (!close)
         return 0;
      n = (size_t)(close - p) + 1;
   }
   else
   {
      n = strcspn(p, ":/?#");
   }
   if (n == 0 || n >= out_len)
      return 0;
   for (size_t i = 0; i < n; i++)
      out[i] = (char)tolower((unsigned char)p[i]);
   out[n] = '\0';

   /* Tolerate a trailing dot (fully-qualified form). */
   if (n > 1 && out[n - 1] == '.')
      out[n - 1] = '\0';
   return out[0] ? 1 : 0;
}

/* 1 when `model` names the given vendor FAMILY: the prefix must be followed by
 * end-of-string or a separator, never more letters. Unanchored matching let
 * "minimaximum-production" be treated as MiniMax — which, in the legacy wire
 * rewrite, would change the agent's auth type and credential env vars. */
static int model_family_is(const char *model, const char *family)
{
   size_t n = strlen(family);
   if (!model || strncasecmp(model, family, n) != 0)
      return 0;
   char c = model[n];
   return c == '\0' || c == '-' || c == '_' || c == '.' || c == '/' || c == ':' || c == ' ';
}

/* 1 when host is `domain` itself or a subdomain of it. Label-anchored, so
 * "kimi.com.evil.example" does NOT match "kimi.com". */
static int host_is_within_domain(const char *host, const char *domain)
{
   size_t hl = strlen(host), dl = strlen(domain);
   if (hl == dl)
      return strcmp(host, domain) == 0;
   if (hl < dl + 1)
      return 0;
   return host[hl - dl - 1] == '.' && strcmp(host + hl - dl, domain) == 0;
}

/* Derive catalog_provider when the operator did not set it explicitly. Host
 * domain wins; a vendor-namespaced or vendor-prefixed model id is the fallback
 * for gateways and CLI/OAuth backends whose endpoint does not name the vendor.
 * Leaves the field empty when the wire provider is already the vendor (or when
 * nothing is recognisable), so agent_catalog_provider() falls back and the
 * caller can tell "derived nothing" from "derived something". */
static void agent_derive_catalog_provider(agent_t *ag)
{
   if (!ag || ag->catalog_provider[0])
      return;

   char host[256];
   if (agent_endpoint_host(ag->endpoint, host, sizeof(host)))
   {
      for (int i = 0; g_catalog_vendor_domains[i].domain; i++)
      {
         if (host_is_within_domain(host, g_catalog_vendor_domains[i].domain))
         {
            snprintf(ag->catalog_provider, sizeof(ag->catalog_provider), "%s",
                     g_catalog_vendor_domains[i].vendor);
            return;
         }
      }
   }

   /* Provider-alias fallback runs even with NO model: a provider-general
    * registration carries only a provider until it is expanded, and its vendor
    * identity is what selects the curated model list. Checked before the
    * model-shaped rules below so an explicit model still wins where present. */
   if (!ag->model[0])
   {
      for (int i = 0; g_catalog_provider_aliases[i].wire; i++)
      {
         if (strcasecmp(ag->provider, g_catalog_provider_aliases[i].wire) == 0)
         {
            snprintf(ag->catalog_provider, sizeof(ag->catalog_provider), "%s",
                     g_catalog_provider_aliases[i].vendor);
            return;
         }
      }
      return;
   }

   /* Namespaced gateway id: "<vendor>/<model>". */
   const char *slash = strchr(ag->model, '/');
   if (slash && slash > ag->model)
   {
      size_t nlen = (size_t)(slash - ag->model);
      for (int i = 0; g_catalog_vendor_namespaces[i].ns; i++)
      {
         const char *ns = g_catalog_vendor_namespaces[i].ns;
         if (strlen(ns) == nlen && strncasecmp(ag->model, ns, nlen) == 0)
         {
            snprintf(ag->catalog_provider, sizeof(ag->catalog_provider), "%s",
                     g_catalog_vendor_namespaces[i].vendor);
            return;
         }
      }
   }

   /* Bare vendor-family model id. */
   if (model_family_is(ag->model, "minimax"))
   {
      snprintf(ag->catalog_provider, sizeof(ag->catalog_provider), "%s", "minimax");
      return;
   }
   if (model_family_is(ag->model, "kimi"))
   {
      snprintf(ag->catalog_provider, sizeof(ag->catalog_provider), "%s", "moonshotai");
      return;
   }

   /* Last: the agent's own provider may be a wire/CLI name rather than a vendor. */
   for (int i = 0; g_catalog_provider_aliases[i].wire; i++)
   {
      if (strcasecmp(ag->provider, g_catalog_provider_aliases[i].wire) == 0)
      {
         snprintf(ag->catalog_provider, sizeof(ag->catalog_provider), "%s",
                  g_catalog_provider_aliases[i].vendor);
         return;
      }
   }
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

static int agent_provider_requires_credentials(const char *provider)
{
   return agent_provider_env_vars(provider) != NULL;
}

static int agent_api_key_literal(const char *api_key)
{
   return api_key && api_key[0] && api_key[0] != '$';
}

static int agent_provider_env_value(const char *provider, char *dst, size_t dst_len)
{
   const char *const *envs = agent_provider_env_vars(provider);
   if (!envs)
      return 0;
   for (int i = 0; envs[i]; i++)
   {
      char value[MAX_API_KEY_LEN];
      if (runtime_secret_get(envs[i], value, sizeof(value)) && value[0])
      {
         if (dst && dst_len > 0)
            snprintf(dst, dst_len, "%s", value);
         runtime_secret_wipe(value, sizeof(value));
         return 1;
      }
   }
   return 0;
}

static int agent_vault_get(const char *agent_name, const char *cred, char *out, size_t out_len);

int agent_has_resolvable_credentials(const agent_t *agent)
{
   if (!agent)
      return 0;
   if (!agent_provider_requires_credentials(agent->provider))
      return 1;
   if (agent->auth_cmd[0] || agent_api_key_literal(agent->api_key))
      return 1;
   /* A credential in the permanent vault (this turn's principal or the server
    * principal) makes the agent usable with no on-disk key — the P4 primary path.
    * For codex we probe the TOKEN, not the account id: a vaulted account without a
    * token is NOT a usable credential (the token is what authenticates). */
   {
      char k[MAX_API_KEY_LEN];
      int is_codex = strcmp(agent->auth_type, "codex-oauth") == 0;
      if (agent_vault_get(agent->name, is_codex ? VAULT_CODEX_TOKEN_CRED : VAULT_API_KEY_CRED, k,
                          sizeof(k)))
         return 1;
   }
   for (int i = 0; i < agent->credential_count; i++)
   {
      char value[MAX_API_KEY_LEN];
      if (runtime_secret_get(agent->credentials[i].api_key_env, value, sizeof(value)) && value[0])
      {
         runtime_secret_wipe(value, sizeof(value));
         return 1;
      }
   }
   return agent_provider_env_value(agent->provider, NULL, 0);
}

/* Per-turn Codex OAuth creds supplied by the thin client (see agent_config.h).
 * Thread-local: each chat/delegate turn runs on its own worker thread. */
static _Thread_local char g_request_codex_token[MAX_API_KEY_LEN];
static _Thread_local char g_request_codex_account_id[128];

/* Explicit, actionable reason the current turn's auth resolution failed — set by
 * agent_resolve_auth on a known failure (e.g. codex REAUTH_REQUIRED) so the
 * delegate/chat error path can surface it instead of a generic provider 401 (D6).
 * Thread-local + cleared at the start of each resolve. */
static _Thread_local char g_request_auth_error[256];

const char *agent_request_auth_error(void)
{
   return g_request_auth_error[0] ? g_request_auth_error : NULL;
}

void agent_set_request_session(const char *session_id)
{
   if (session_id && session_id[0])
      snprintf(g_request_session_id, sizeof(g_request_session_id), "%s", session_id);
   else
      g_request_session_id[0] = '\0';
}

/* WP-C.2c(3): the attested vault principal for the in-flight chat turn, so a
 * chat-spawned delegate (dispatched through the conn-decoupled agent loop) can
 * reach the originating user's vault. */
static _Thread_local char g_request_vault_principal[VAULT_PRINCIPAL_MAX];

void agent_set_request_vault_principal(const char *principal)
{
   if (principal && principal[0])
      snprintf(g_request_vault_principal, sizeof(g_request_vault_principal), "%s", principal);
   else
      g_request_vault_principal[0] = '\0';
}

const char *agent_get_request_vault_principal(void)
{
   return g_request_vault_principal;
}

/* Per-turn cancellation flag (server-owned turn lifecycle). The chat worker
 * binds a pointer to its turn-registry cancel flag around the in-process agent
 * loop; the loop polls agent_request_cancelled() at safe points to abort a
 * detached turn promptly. Thread-local; NULL clears it. */
static _Thread_local atomic_int *g_request_cancel;

void agent_set_request_cancel(atomic_int *flag)
{
   g_request_cancel = flag;
}

int agent_request_cancelled(void)
{
   return g_request_cancel ? atomic_load(g_request_cancel) : 0;
}

/* Read a credential for the one server environment. The in-flight principal is
 * actor attribution only and must never select a model/provider credential. */
static int agent_vault_get(const char *agent_name, const char *cred, char *out, size_t out_len)
{
   if (!agent_name || !agent_name[0] || !out || out_len == 0)
      return 0;
   out[0] = '\0';
   if (vault_service_get_server_principal(agent_name, cred, out, out_len) == VAULT_OK && out[0])
      return 1;
   out[0] = '\0';
   return 0;
}

void agent_set_request_codex_creds(const char *token, const char *account_id)
{
   if (token && token[0])
      snprintf(g_request_codex_token, sizeof(g_request_codex_token), "%s", token);
   else
      g_request_codex_token[0] = '\0';
   if (account_id && account_id[0])
      snprintf(g_request_codex_account_id, sizeof(g_request_codex_account_id), "%s", account_id);
   else
      g_request_codex_account_id[0] = '\0';
}

int agent_request_codex_token_present(void)
{
   return g_request_codex_token[0] != '\0';
}

void agent_request_creds_snapshot(agent_request_creds_t *out)
{
   if (!out)
      return;
   snprintf(out->session_id, sizeof(out->session_id), "%s", g_request_session_id);
   snprintf(out->codex_token, sizeof(out->codex_token), "%s", g_request_codex_token);
   snprintf(out->codex_account_id, sizeof(out->codex_account_id), "%s", g_request_codex_account_id);
   snprintf(out->vault_principal, sizeof(out->vault_principal), "%s", g_request_vault_principal);
}

void agent_request_creds_restore(const agent_request_creds_t *creds)
{
   if (!creds)
      return;
   agent_set_request_session(creds->session_id);
   agent_set_request_codex_creds(creds->codex_token, creds->codex_account_id);
   agent_set_request_vault_principal(creds->vault_principal);
}

static void append_header_line(char *buf, size_t buf_len, const char *line)
{
   if (!buf || buf_len == 0 || !line || !line[0])
      return;
   size_t used = strlen(buf);
   if (used > 0 && buf[used - 1] != '\n' && used + 1 < buf_len)
   {
      buf[used++] = '\n';
      buf[used] = '\0';
   }
   if (used < buf_len - 1)
      snprintf(buf + used, buf_len - used, "%s", line);
}

void agent_build_extra_headers(const agent_t *agent, char *buf, size_t buf_len)
{
   if (!buf || buf_len == 0)
      return;
   buf[0] = '\0';
   if (!agent)
      return;

   if (agent->extra_headers[0])
      snprintf(buf, buf_len, "%s", agent->extra_headers);

   if (strcmp(agent->provider, "anthropic") == 0 && !strstr(buf, "anthropic-version:") &&
       !strstr(buf, "Anthropic-Version:"))
      append_header_line(buf, buf_len, "anthropic-version: 2023-06-01");

   if (strcmp(agent->provider, "openrouter") == 0)
   {
      if (!strstr(buf, "HTTP-Referer:"))
         append_header_line(buf, buf_len, "HTTP-Referer: https://github.com/JBailes/aimee");
      if (!strstr(buf, "X-Title:"))
         append_header_line(buf, buf_len, "X-Title: aimee");
   }

   /* Codex (ChatGPT OAuth): inject the headers the codex backend requires from
    * the client-supplied account id (per-turn push, else the session keyring),
    * unless the agent's stored headers already carry it. Keyed on the
    * codex-oauth auth type so it fires regardless of the provider label (the
    * codex adapter's provider is "chatgpt"). Lets a codex agent be configured
    * without server-held creds. */
   if (strcmp(agent->auth_type, "codex-oauth") == 0 && !strstr(buf, "ChatGPT-Account-ID:"))
   {
      char acct[128];
      acct[0] = '\0';
      if (g_request_codex_account_id[0])
         snprintf(acct, sizeof(acct), "%s", g_request_codex_account_id);
      else
         /* miss leaves acct empty (helper guarantees) -> header is skipped below. */
         (void)agent_vault_get(agent->name, VAULT_CODEX_ACCOUNT_CRED, acct, sizeof(acct));
      if (acct[0])
      {
         if (!strstr(buf, "originator:"))
            append_header_line(buf, buf_len, "originator: codex_cli_rs");
         char line[160];
         snprintf(line, sizeof(line), "ChatGPT-Account-ID: %s", acct);
         append_header_line(buf, buf_len, line);
      }
   }
}

/* --- Load/Save config (with mtime cache) --- */

/* The cache is read/written from many threads at once — every delegate
 * dispatch calls agent_load_config, and the parallel autonomy scheduler plus
 * the panel seats dispatch concurrently. An unguarded memcpy of this
 * multi-KB struct while a reloading thread rewrites it is a torn read (and
 * TSan-class UB), so all cache access holds g_agent_config_cache_lock. The
 * lock is NOT held across file I/O parsing — a reloader parses into the
 * caller's buffer first and only then publishes under the lock. */
static agent_config_t g_agent_config_cache;
static struct timespec g_agent_config_mtime;
/* mtime alone is not a safe cache key. It is not monotonic and not always
 * distinct: a rewritten agents.json can land with a timestamp equal to (or
 * older than) the cached one, and the cache then serves stale content forever.
 * Observed live on the tiered appliance filesystem, where a freshly installed
 * agents.json arrived with an mtime ~9h in the past and /v1/agents kept failing
 * until the file was touched. Size and inode are free from the same stat() and
 * make an in-place rewrite detectable. */
static off_t g_agent_config_size;
static ino_t g_agent_config_ino;
static int g_agent_config_cached;
static pthread_mutex_t g_agent_config_cache_lock = PTHREAD_MUTEX_INITIALIZER;

static struct timespec agent_config_stat_mtime(const struct stat *st)
{
   struct timespec ts;
#if defined(__APPLE__)
   ts = st->st_mtimespec;
#elif defined(_WIN32) || defined(_WIN64)
   ts.tv_sec = st->st_mtime;
   ts.tv_nsec = 0;
#elif defined(__linux__)
   ts = st->st_mtim;
#else
   ts.tv_sec = st->st_mtime;
   ts.tv_nsec = 0;
#endif
   return ts;
}

static int agent_config_mtime_eq(const struct timespec *a, const struct timespec *b)
{
   return a->tv_sec == b->tv_sec && a->tv_nsec == b->tv_nsec;
}

/* A cache hit requires the file to look identical on every cheap axis stat()
 * gives us: same mtime, same size, same inode. mtime alone is spoofable by a
 * same-timestamp rewrite (see g_agent_config_size). */
static int agent_config_stat_eq(const struct stat *st)
{
   struct timespec mt = agent_config_stat_mtime(st);
   return agent_config_mtime_eq(&mt, &g_agent_config_mtime) && st->st_size == g_agent_config_size &&
          st->st_ino == g_agent_config_ino;
}

/* Record the identity of the file whose parsed contents are now cached. */
static void agent_config_stat_remember(const struct stat *st)
{
   g_agent_config_mtime = agent_config_stat_mtime(st);
   g_agent_config_size = st->st_size;
   g_agent_config_ino = st->st_ino;
}

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
static void agent_migrate_declare_from_catalog(agent_config_t *cfg)
{
   if (!cfg)
      return;
   for (int i = 0; i < cfg->agent_count; i++)
   {
      agent_t *ag = &cfg->agents[i];
      if (!ag->model[0])
         continue; /* a CLI seat with no model id has nothing to look up */

      /* The CATALOG only -- deliberately not model_capability_get(), which falls
       * through to a heuristic that infers a window for any unknown name and
       * always succeeds. Adopting that would stamp a guess into the operator's
       * config as though they had chosen it, which is worse than leaving the
       * value unknown: a guess nobody can trace is exactly what this whole
       * change is removing. Only a figure a provider actually published is
       * worth writing down. */
      model_capability_t cap;
      memset(&cap, 0, sizeof cap);
      if (!models_dev_cache_lookup(agent_catalog_provider(ag), ag->model, &cap))
         continue;

      if (ag->middleware.context_window <= 0 && cap.context_window > 0)
      {
         ag->middleware.context_window = cap.context_window;
         ag->declared |= AGENT_DECL_CONTEXT_WINDOW;
         aimee_log(LOG_INFO, "agent_config", "%s: adopting context_window %d from the catalog",
                   ag->name, cap.context_window);
      }
      if (ag->max_output <= 0 && cap.max_output > 0)
      {
         ag->max_output = cap.max_output;
         ag->declared |= AGENT_DECL_MAX_OUTPUT;
         aimee_log(LOG_INFO, "agent_config", "%s: adopting max_output %d from the catalog",
                   ag->name, cap.max_output);
      }
      /* PRICES ARE DELIBERATELY NOT ADOPTED. Copying a catalog price into the
       * agent marks it declared, which is what price_overridden and the Providers
       * UI read as "the operator chose this" -- so a vendor list price would
       * start presenting itself as the operator's own decision. That is the exact
       * confusion this work exists to remove, and it would also silently change
       * what the tier lint treats as an operator exemption. Prices stay resolved
       * through the existing path until someone decides how they should be
       * carried; only the LIMITS, which are plain facts about the model, migrate.
       */
   }
}

int agent_load_config(agent_config_t *cfg)
{
   memset(cfg, 0, sizeof(*cfg));

   const char *path = agent_config_path();

   /* Return cached config if mtime unchanged and caching enabled */
   if (!getenv("AIMEE_NO_CACHE"))
   {
      struct stat st;
      if (stat(path, &st) == 0)
      {
         pthread_mutex_lock(&g_agent_config_cache_lock);
         int hit = g_agent_config_cached && agent_config_stat_eq(&st);
         if (hit)
            memcpy(cfg, &g_agent_config_cache, sizeof(*cfg));
         pthread_mutex_unlock(&g_agent_config_cache_lock);
         if (hit)
            return 0;
      }
   }

   FILE *f = fopen(path, "r");
   if (!f)
      return -1;

   fseek(f, 0, SEEK_END);
   long sz = ftell(f);
   fseek(f, 0, SEEK_SET);
   if (sz <= 0 || sz > 1024 * 1024)
   {
      fclose(f);
      return -1;
   }

   char *data = malloc((size_t)sz + 1);
   if (!data)
   {
      fclose(f);
      return -1;
   }
   size_t nread = fread(data, 1, (size_t)sz, f);
   data[nread] = '\0';
   fclose(f);

   cJSON *root = cJSON_Parse(data);
   free(data);
   if (!root)
      return -1;

   /* Default agent */
   cJSON *def = cJSON_GetObjectItem(root, "default_agent");
   if (def && cJSON_IsString(def))
      snprintf(cfg->default_agent, MAX_AGENT_NAME, "%s", def->valuestring);

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
            /* Keep the verbatim on-disk form ($VAR ref) for re-save, and resolve
             * a separate runtime copy. Without this, a save would write the
             * expanded secret back to agents.json as plaintext. */
            snprintf(ag->api_key_disk, MAX_API_KEY_LEN, "%s", v->valuestring);
            agent_expand_env(v->valuestring, ag->api_key, MAX_API_KEY_LEN);
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
         /* Legacy OpenAI-wire MiniMax normalisation. This rewrites the WIRE
          * provider (so it changes auth and credential env-var selection), which
          * makes a false positive worse than a catalog misderivation — it used
          * substring matching over the whole endpoint, so a gateway with
          * "api.minimax." anywhere in its path or userinfo was rewritten. Match
          * the host's domain labels, and a model FAMILY rather than the literal
          * "MiniMax-M2" (which stopped matching the moment M3 shipped). */
         if (strcmp(ag->provider, "openai") == 0)
         {
            char lhost[256];
            int minimax_host = agent_endpoint_host(ag->endpoint, lhost, sizeof(lhost)) &&
                               (host_is_within_domain(lhost, "minimax.io") ||
                                host_is_within_domain(lhost, "minimaxi.com") ||
                                host_is_within_domain(lhost, "minimax.com"));
            if (minimax_host || model_family_is(ag->model, "minimax"))
               snprintf(ag->provider, sizeof(ag->provider), "%s", "minimax");
         }

         /* Catalog identity is SEPARATE from the wire provider above: rewriting
          * `provider` for an Anthropic-wire third party would drop the
          * anthropic-version header, the x-api-key auth coercion, and the
          * credential env-var set. An explicit operator value always wins. */
         v = cJSON_GetObjectItem(a, "catalog_provider");
         if (v && cJSON_IsString(v) && v->valuestring[0])
         {
            snprintf(ag->catalog_provider, sizeof(ag->catalog_provider), "%s", v->valuestring);
            ag->catalog_provider_explicit = 1;
         }
         agent_derive_catalog_provider(ag);

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
         if (v && cJSON_IsNumber(v))
         {
            ag->timeout_ms = v->valueint; /* operator's explicit value wins */
         }
         else
         {
            /* No operator timeout: give a reasoning-capable model a higher per-call
             * default so its slow (multi-minute) completions aren't cut off and
             * retried as spurious read failures. Capability lookup is total +
             * offline (same guarantees as the tools_enabled derivation below);
             * an unknown/non-reasoning model keeps the standard default. */
            model_capability_t tmc;
            int reasoning = ag->model[0] &&
                            model_capability_get(agent_catalog_provider(ag), ag->model, &tmc) &&
                            (tmc.flags & MODEL_CAP_REASONING);
            ag->timeout_ms = reasoning ? AGENT_REASONING_TIMEOUT_MS : AGENT_DEFAULT_TIMEOUT_MS;
         }

         /* "enabled" accepts a boolean or a 0/1 number: hand-edited rosters
          * write `"enabled": 0`, and treating a non-bool as "enabled" silently
          * re-armed agents the operator had switched off (observed live: a
          * disabled agent with no valid key kept winning role routing and
          * 401-looped every implement step). Absent key stays enabled. */
         v = cJSON_GetObjectItem(a, "enabled");
         if (!v)
            ag->enabled = 1;
         else if (cJSON_IsBool(v))
            ag->enabled = cJSON_IsTrue(v);
         else if (cJSON_IsNumber(v))
            ag->enabled = (v->valueint != 0);
         else
            ag->enabled = 1;

         v = cJSON_GetObjectItem(a, "tools_enabled");
         if (v && cJSON_IsBool(v))
         {
            /* Explicit operator setting always wins (force on or off). */
            ag->tools_enabled = cJSON_IsTrue(v);
         }
         else
         {
            /* Absent key: default tools ON (opt-out, not opt-in). An agent is
             * tool-capable unless the operator EXPLICITLY disables it, or unless
             * the backing model is KNOWN to lack tool support. Defaulting an
             * unknown model to OFF silently made new/unregistered agents (e.g.
             * codex/gpt-5.5, provider=chatgpt — absent from the capability table)
             * look tool-INCAPABLE to the routing filter
             * (delegate_filter_route_capabilities), so tool-requiring roles
             * (`code`/`review`/…) rejected them ("no configured model supports
             * required capabilities (caps=tools)") even though the model runs
             * tools fine.
             *
             * Invariants:
             *  - Operator intent is never overridden: an explicit
             *    "tools_enabled": true|false is honoured verbatim (branch above).
             *    This only sets a value the operator left UNSPECIFIED.
             *  - The lookup is total and offline: model_capability_get resolves
             *    via on-disk overrides/caches and a pure in-code heuristic, never
             *    the network. We only force OFF when it POSITIVELY resolves a
             *    model that lacks MODEL_CAP_TOOLS; an unknown/empty model defaults
             *    ON. So embedders/rerankers registered with a known non-tool model
             *    stay off, while any real chat/code delegate is capable by default. */
            model_capability_t mc;
            int known =
                ag->model[0] && model_capability_get(agent_catalog_provider(ag), ag->model, &mc);
            ag->tools_enabled = (known && !(mc.flags & MODEL_CAP_TOOLS)) ? 0 : 1;
         }

         v = cJSON_GetObjectItem(a, "recommended_sampling");
         ag->recommended_sampling = (v && cJSON_IsBool(v)) ? cJSON_IsTrue(v) : 0;

         v = cJSON_GetObjectItem(a, "inject_respond_tool");
         ag->inject_respond_tool =
             (v && cJSON_IsBool(v)) ? cJSON_IsTrue(v) : agent_default_inject_respond_tool(ag);

         /* Per-agent delegate turn cap. Declared value wins verbatim:
          *   0  = unlimited (frontier agents, e.g. MiniMax-M3),
          *   >0 = explicit cap,
          *   absent -> -1 = inherit the per-role floor (see delegate_role.c).
          * The role floor never clamps a declared cap down. */
         v = cJSON_GetObjectItem(a, "max_turns");
         ag->max_turns = (v && cJSON_IsNumber(v)) ? v->valueint : -1;

         v = cJSON_GetObjectItem(a, "max_parallel");
         if (v && cJSON_IsNumber(v))
            ag->max_parallel = v->valueint;
         else if (strcmp(ag->provider, "mistral") == 0)
            ag->max_parallel = 2; /* mistral rate limits fire under concurrent load */
         else
            ag->max_parallel = AGENT_DEFAULT_MAX_PARALLEL;

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
         /* An explicit primary_only wins; remember whether the key was present so
          * an ABSENT key (a legacy agents.json written before this field existed)
          * can be migrated below rather than defaulting everything to
          * delegate-eligible. */
         v = cJSON_GetObjectItem(a, "primary_only");
         int primary_only_present = (v && cJSON_IsBool(v));
         if (primary_only_present)
            ag->primary_only = cJSON_IsTrue(v);

         agent_normalize_legacy_claude_cli(ag);
         agent_normalize_builtin_cost_tier(ag);

         /* Migration for a legacy file: default an ABSENT primary_only to the
          * pre-change semantics. Before this field, a claude-CLI subscription was
          * primary-only by default (gated behind the removed global
          * claude_cli_delegate_enabled, default off) and every other agent was
          * delegate-eligible. The removed gate tested agent_is_claude_cli on the
          * loaded+NORMALIZED agent, so evaluating the SAME predicate here — AFTER
          * agent_normalize_legacy_claude_cli — reproduces exactly the set that was
          * primary-only before, with no behavior change on upgrade. An explicit
          * value always wins, and agent_save_config always writes the key, so this
          * only fires for a genuinely legacy file; once resaved the stored value
          * (including an explicit false) is authoritative and unchecking Primary
          * Agent Only persists. */
         if (!primary_only_present)
            ag->primary_only = agent_is_claude_cli(ag) ? 1 : 0;

         /* Provider-general registration: an explicit "models" array expands
          * into one runtime target per model. Strictly OPT-IN — a legacy entry
          * naming a single `model` keeps exactly its previous meaning, including
          * its name for `--via`. Inferring provider-general mode from anything
          * else would change cost, health and attribution on upgrade. */
         cJSON *models_arr = cJSON_GetObjectItem(a, "models");
         if (models_arr)
         {
            /* Reject an ill-formed registration rather than degrading it into
             * something the operator did not write. An empty or non-array/string
             * `models` would otherwise fall through and register the bare
             * registration as a routable agent with NO model - routing would
             * then dispatch whatever the catalog happens to default to. */
            int models_ok = (cJSON_IsArray(models_arr) && cJSON_GetArraySize(models_arr) > 0) ||
                            cJSON_IsString(models_arr);
            if (!models_ok)
            {
               LOG_ERROR("agent",
                         "agents.json: registration '%s' has an empty or malformed \"models\"; "
                         "use a non-empty array of model ids or \"auto\"",
                         ag->name);
               cJSON_Delete(root);
               return -1;
            }
            /* `model` and `models` together are ambiguous: the single value would
             * be silently discarded by expansion. Make the operator choose. */
            if (ag->model[0])
            {
               LOG_ERROR("agent",
                         "agents.json: registration '%s' sets both \"model\" and \"models\"; "
                         "use one or the other",
                         ag->name);
               cJSON_Delete(root);
               return -1;
            }
         }
         if (models_arr)
         {
            agent_t base = *ag;
            int n = agent_expand_provider_models(cfg, &base, models_arr);
            if (n < 0)
            {
               LOG_ERROR("agent",
                         "agents.json: registration '%s' expands past the %d-target limit; "
                         "refusing to load a partial fleet",
                         base.name, MAX_AGENTS);
               /* `data` was already freed immediately after cJSON_Parse. */
               cJSON_Delete(root);
               return -1;
            }
            /* The registration itself is not a route target. */
            continue;
         }
         cfg->agent_count++;
      }
   }

   /* Q5 mitigation: a fleet where EVERY agent declares a ceiling below whole_task
    * cannot serve a default-scope packet at all, and the failure would otherwise
    * appear only at dispatch as a confusing "no route". Warn at load instead.
    * Deliberately not fatal: an operator may legitimately run only bounded work. */
   if (cfg->agent_count > 0)
   {
      int unbounded = 0;
      for (int i = 0; i < cfg->agent_count; i++)
      {
         if (cfg->agents[i].enabled && (cfg->agents[i].max_scope == AGENT_SCOPE_UNSET ||
                                        cfg->agents[i].max_scope >= AGENT_SCOPE_WHOLE_TASK))
         {
            unbounded = 1;
            break;
         }
      }
      if (!unbounded)
         LOG_WARN("agent",
                  "no enabled agent can serve whole_task work (every agent declares a lower "
                  "max_scope); packets without an explicit scope will fail to route. Remove a "
                  "ceiling or add a more capable agent.");
   }

   /* Whole-config name uniqueness. Checking only against agents committed SO FAR
    * is order-dependent: an expansion generating `codex:gpt-5.6-sol` is caught
    * when a legacy agent of that name was loaded first, but not when it is
    * declared after. Both health and --via key on the name, so two entries
    * sharing one conflate distinct models — and they may differ in endpoint,
    * credentials, roles and model. Validate the final set once, so the check is
    * independent of declaration order. */
   for (int i = 0; i < cfg->agent_count; i++)
   {
      for (int j = i + 1; j < cfg->agent_count; j++)
      {
         if (strcmp(cfg->agents[i].name, cfg->agents[j].name) != 0)
            continue;
         LOG_ERROR("agent",
                   "agents.json: two agents are both named '%s' (one may be generated by a "
                   "provider-general registration); names must be unique because health and "
                   "--via key on them",
                   cfg->agents[i].name);
         cJSON_Delete(root);
         return -1;
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

   /* Before the cache snapshot, so every reader of the cached config sees the
    * migrated values rather than only the first caller. */
   agent_migrate_declare_from_catalog(cfg);

   /* Update mtime cache */
   {
      struct stat st;
      if (stat(path, &st) == 0)
      {
         pthread_mutex_lock(&g_agent_config_cache_lock);
         memcpy(&g_agent_config_cache, cfg, sizeof(g_agent_config_cache));
         agent_config_stat_remember(&st);
         g_agent_config_cached = 1;
         pthread_mutex_unlock(&g_agent_config_cache_lock);
      }
   }

   return 0;
}

/* Count the agents in the on-disk agents.json WITHOUT the cache — the cache can be
 * stale or empty, and this guards against destroying the real file. Returns the
 * count, or -1 if the file is absent or unparseable (i.e. "nothing to protect"). */
static int agent_config_existing_agent_count(void)
{
   FILE *f = fopen(agent_config_path(), "r");
   if (!f)
      return -1;
   if (fseek(f, 0, SEEK_END) != 0)
   {
      fclose(f);
      return -1;
   }
   long sz = ftell(f);
   if (sz <= 0 || sz > 8 * 1024 * 1024)
   {
      fclose(f);
      return -1;
   }
   rewind(f);
   char *buf = malloc((size_t)sz + 1);
   if (!buf)
   {
      fclose(f);
      return -1;
   }
   size_t rd = fread(buf, 1, (size_t)sz, f);
   fclose(f);
   buf[rd] = '\0';
   cJSON *root = cJSON_Parse(buf);
   free(buf);
   if (!root)
      return -1;
   cJSON *arr = agent_roster_array(root);
   int n = cJSON_IsArray(arr) ? cJSON_GetArraySize(arr) : -1;
   cJSON_Delete(root);
   return n;
}

static int agent_save_config_impl(const agent_config_t *cfg, int emptied_by_removal)
{
   cJSON *root = cJSON_CreateObject();

   if (cfg->default_agent[0])
      JSON_ADD_STR(root, "default_agent", cfg->default_agent);

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

   char *json = cJSON_Print(root);
   cJSON_Delete(root);
   if (!json)
      return -1;

   /* Never overwrite a populated agents.json with an empty registry. A zero-agent
    * save is legitimate ONLY on a fresh install (no existing file, or an existing
    * file that already has none) — never as a silent wipe of configured agents.
    * This is the agents.json-deletion guard: whatever the trigger (a load that
    * came back empty, a caller with a zeroed cfg), the destruction stops here, and
    * it stops LOUDLY so the next occurrence is diagnosable rather than invisible.
    *
    * Removing the LAST delegate reaches zero legitimately, and looks identical
    * from here — so that one caller declares itself instead. Without the
    * exemption the guard refused the removal and the delegate could never be
    * deleted. */
   if (cfg->agent_count == 0 && !emptied_by_removal)
   {
      int existing = agent_config_existing_agent_count();
      if (existing > 0)
      {
         aimee_log(LOG_ERROR, "agent-config",
                   "REFUSING to overwrite %d configured agent(s) in %s with an empty registry. "
                   "An empty save is only valid on a fresh install; this is the deletion guard. "
                   "If a wipe is truly intended, remove the file first.",
                   existing, agent_config_path());
         free(json);
         return -1;
      }
   }

   /* Ensure directory exists */
   char dir[MAX_PATH_LEN];
   snprintf(dir, sizeof(dir), "%s", config_default_dir());
   platform_mkdir_p(dir, 0700);

   /* Write atomically: a temp file, fsync, then rename over the target. rename(2)
    * is atomic on a POSIX filesystem, so an interrupted or failed write can NEVER
    * leave a truncated or empty agents.json — the previous file survives untouched.
    * The old fopen(path, "w") truncated the real file to zero the instant it
    * opened, before a single byte was written; any hiccup then destroyed it, which
    * on a tiered/networked home is exactly how it went missing. */
   char tmp[MAX_PATH_LEN];
   if ((size_t)snprintf(tmp, sizeof(tmp), "%s.tmp.%d", agent_config_path(), (int)getpid()) >=
       sizeof(tmp))
   {
      free(json);
      return -1;
   }
   int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
   if (fd < 0)
   {
      free(json);
      return -1;
   }
   size_t jlen = strlen(json);
   int ok = (write(fd, json, jlen) == (ssize_t)jlen) && (write(fd, "\n", 1) == 1);
   /* fsync before rename: a rename can otherwise be durable while the data it
    * points at is not, leaving an empty file after a crash — the very failure this
    * guards against. */
   if (ok && fsync(fd) != 0)
      ok = 0;
   if (close(fd) != 0)
      ok = 0;
   if (!ok || rename(tmp, agent_config_path()) != 0)
   {
      unlink(tmp); /* leave the existing agents.json intact */
      aimee_log(LOG_ERROR, "agent-config", "atomic write of %s failed; existing file left intact",
                agent_config_path());
      free(json);
      return -1;
   }
   chmod(agent_config_path(), 0600);
   /* INVALIDATE, never populate. A caller-built config has not been through
    * agent_load_config()'s normalisation — provider defaulting, the MiniMax wire
    * rewrite, agent_derive_catalog_provider() and the cost-tier/capability pass
    * all run there, not here. Caching |cfg| verbatim while stamping it with the
    * file's fresh stat made every later load a cache HIT on that unnormalised
    * struct, so the derived fields stayed empty for the whole process lifetime
    * and only a restart recovered them. That is what left a wizard-added agent
    * with an empty catalog_provider: it silently lost its required-temperature
    * row (sending temperature=0.3 to a model that accepts only 1) and its
    * catalog capability lookup (so it advertised no tools). Dropping the entry
    * costs one re-parse of a small file on the next load, which then caches the
    * fully normalised result itself. */
   pthread_mutex_lock(&g_agent_config_cache_lock);
   g_agent_config_cached = 0;
   pthread_mutex_unlock(&g_agent_config_cache_lock);
   free(json);
   return 0;
}

int agent_save_config(const agent_config_t *cfg)
{
   return agent_save_config_impl(cfg, 0);
}

int agent_save_config_after_removal(const agent_config_t *cfg)
{
   return agent_save_config_impl(cfg, 1);
}

/* --- Auth resolution --- */

/* The codex CLI's public OAuth client + token endpoint, as carried verbatim in
 * the codex access token's own `client_id`/`iss` claims (stable public values).
 * Used to auto-refresh the codex bearer through aimee's vault-backed oauth store. */
#define CODEX_OAUTH_STORE          "codex" /* aimee oauth-store client name (vault key) */
#define CODEX_OAUTH_CLIENT_ID      "app_EMoamEEZ73f0CkXaXp7hrann"
#define CODEX_OAUTH_TOKEN_ENDPOINT "https://auth.openai.com/oauth/token"

/* Refresh the codex token this many seconds before its JWT `exp`. Overridable via
 * AIMEE_CODEX_REFRESH_SKEW (set huge to force a refresh — used to live-verify). */
static int codex_oauth_refresh_skew(void)
{
   const char *e = getenv("AIMEE_CODEX_REFRESH_SKEW");
   if (e && e[0])
   {
      long v = atol(e);
      if (v >= 0)
         return (int)v;
   }
   return 3600; /* 1h */
}

/* Decode a JWT's `exp` (Unix seconds); 0 if unparseable. Base64url-decodes the
 * payload (middle) segment and reads the exp number — no signature check (we only
 * need expiry to schedule a refresh). */
static long codex_jwt_exp(const char *jwt)
{
   if (!jwt || !jwt[0])
      return 0;
   const char *p1 = strchr(jwt, '.');
   if (!p1)
      return 0;
   const char *seg = p1 + 1;
   const char *p2 = strchr(seg, '.');
   if (!p2 || p2 == seg)
      return 0;
   size_t seglen = (size_t)(p2 - seg);
   if (seglen > 8192)
      return 0;
   /* base64url -> base64 + pad */
   char b64[8200];
   size_t j = 0;
   for (size_t i = 0; i < seglen && j + 1 < sizeof(b64); i++)
   {
      char c = seg[i];
      b64[j++] = (c == '-') ? '+' : (c == '_') ? '/' : c;
   }
   while ((j % 4) != 0 && j + 1 < sizeof(b64))
      b64[j++] = '=';
   b64[j] = '\0';
   unsigned char dec[8200];
   size_t dn = aimee_base64_decode(b64, dec, sizeof(dec) - 1);
   if (dn == 0)
      return 0;
   dec[dn] = '\0';
   cJSON *root = cJSON_Parse((const char *)dec);
   long exp = 0;
   if (root)
   {
      cJSON *e = cJSON_GetObjectItemCaseSensitive(root, "exp");
      if (cJSON_IsNumber(e))
         exp = (long)e->valuedouble;
      cJSON_Delete(root);
   }
   return exp;
}

/* Parse access_token + refresh_token from codex auth JSON (top-level keys or
 * `tokens.{}`). Returns 0 only when both are present: a refresh token is
 * required to manage the credential in AIMEE's Vault-backed OAuth store. */
static int codex_parse_oauth_pair(const char *data, char *access, size_t an, char *refresh,
                                  size_t rn)
{
   if (access && an)
      access[0] = '\0';
   if (refresh && rn)
      refresh[0] = '\0';
   cJSON *root = cJSON_Parse(data);
   if (!root)
      return -1;
   cJSON *at = cJSON_GetObjectItemCaseSensitive(root, "access_token");
   cJSON *rt = cJSON_GetObjectItemCaseSensitive(root, "refresh_token");
   cJSON *tokens = cJSON_GetObjectItemCaseSensitive(root, "tokens");
   if (cJSON_IsObject(tokens))
   {
      if (!cJSON_IsString(at))
         at = cJSON_GetObjectItemCaseSensitive(tokens, "access_token");
      if (!cJSON_IsString(rt))
         rt = cJSON_GetObjectItemCaseSensitive(tokens, "refresh_token");
   }
   int ok = 0;
   if (cJSON_IsString(at) && at->valuestring[0] && cJSON_IsString(rt) && rt->valuestring[0])
   {
      snprintf(access, an, "%s", at->valuestring);
      snprintf(refresh, rn, "%s", rt->valuestring);
      ok = 1;
   }
   cJSON_Delete(root);
   return ok ? 0 : -1;
}

/* Vault-backed, auto-refreshing codex bearer. On first use, bootstrap aimee's
 * oauth store from the server-sealed, opaque codex OAuth document; thereafter
 * oauth_token_get() returns a token, refreshing via the stored refresh_token
 * against OpenAI's token endpoint whenever it is within the skew of expiry —
 * without reading or rewriting a persistent auth.json. Returns 0 + a bearer in
 * |buf|; -1 when Vault has no usable OAuth document. */
static int codex_oauth_vault_token(char *buf, size_t len)
{
   for (int tries = 0; tries < 2; tries++)
   {
      if (oauth_token_get(CODEX_OAUTH_STORE, CODEX_OAUTH_CLIENT_ID, CODEX_OAUTH_TOKEN_ENDPOINT,
                          codex_oauth_refresh_skew(), buf, len) == 0 &&
          buf[0])
         return 0;
      if (tries > 0)
         break; /* bootstrapped already and still no token -> give up */
      /* Bootstrap (or re-bootstrap after a rejected refresh) from the opaque
       * vendor document stored in Vault by the login/legacy-migration path. */
      char access[MAX_API_KEY_LEN] = "", refresh[1024] = "", document[64 * 1024];
      vault_status_t vst =
          vault_service_get_server_principal("codex", "oauth", document, sizeof(document));
      int got = vst == VAULT_OK && codex_parse_oauth_pair(document, access, sizeof(access), refresh,
                                                          sizeof(refresh)) == 0;
      OPENSSL_cleanse(document, sizeof(document));
      if (!got)
         return -1;
      oauth_token_response_t resp;
      memset(&resp, 0, sizeof(resp));
      snprintf(resp.access_token, sizeof(resp.access_token), "%s", access);
      snprintf(resp.refresh_token, sizeof(resp.refresh_token), "%s", refresh);
      resp.expires_at = codex_jwt_exp(access); /* 0 = unknown (no proactive refresh) */
      if (oauth_token_store(CODEX_OAUTH_STORE, &resp) != 0)
      {
         OPENSSL_cleanse(access, sizeof(access));
         OPENSSL_cleanse(refresh, sizeof(refresh));
         return -1;
      }
      OPENSSL_cleanse(access, sizeof(access));
      OPENSSL_cleanse(refresh, sizeof(refresh));
      /* loop: oauth_token_get now serves the vaulted token (refreshing if stale). */
   }
   return -1;
}

int agent_resolve_auth(const agent_t *agent, char *buf, size_t buf_len)
{
   buf[0] = '\0';
   g_request_auth_error[0] = '\0'; /* reset the per-turn explicit-error channel (D6) */
   const char *auth_type = agent->auth_type;
   if (strcmp(agent->provider, "anthropic") == 0 &&
       (!auth_type[0] || strcmp(auth_type, "bearer") == 0 || strcmp(auth_type, "api_key") == 0))
      auth_type = "x-api-key";

   if (strcmp(auth_type, "none") == 0)
      return 0;

   if (strcmp(auth_type, "codex-oauth") == 0)
   {
      /* Codex OAuth token, in precedence: per-turn token (set by the vault delegate
       * path) > the permanent Vault (turn principal, then server principal). */
      if (g_request_codex_token[0])
      {
         snprintf(buf, buf_len, "Authorization: Bearer %s", g_request_codex_token);
         return 0;
      }
      char token[MAX_API_KEY_LEN];
      if (agent_vault_get(agent->name, VAULT_CODEX_TOKEN_CRED, token, sizeof(token)))
      {
         snprintf(buf, buf_len, "Authorization: Bearer %s", token);
         return 0;
      }
      /* Vault-backed, auto-refreshing token bootstrapped from the opaque codex
       * OAuth document already ingested by login or boot migration. */
      if (codex_oauth_vault_token(token, sizeof(token)) == 0)
      {
         snprintf(buf, buf_len, "Authorization: Bearer %s", token);
         return 0;
      }
      /* No usable codex token. If a prior refresh was rejected by the IdP, the
       * refresh token is dead and the server cannot recover on its own — give
       * the operator the exact remedy instead of a generic provider 401 (D6). */
      if (oauth_token_reauth_required(CODEX_OAUTH_STORE))
         snprintf(g_request_auth_error, sizeof(g_request_auth_error),
                  "codex re-auth required: the stored OAuth refresh token was rejected — run "
                  "`aimee codex reauth` to re-authenticate");
      return -1;
   }

   if (strcmp(auth_type, "oauth") == 0 && agent->auth_cmd[0])
   {
      /* Run auth_cmd via safe_exec_capture (no shell injection) */
      char *auth_tokens[32];
      int auth_tc = shlex_split(agent->auth_cmd, auth_tokens, 32);
      if (auth_tc <= 0)
         return -1;
      const char *auth_argv[33];
      for (int ai = 0; ai < auth_tc && ai < 32; ai++)
         auth_argv[ai] = auth_tokens[ai];
      auth_argv[auth_tc] = NULL;
      char *output = NULL;
      int status = safe_exec_capture(auth_argv, &output, MAX_API_KEY_LEN);
      for (int ai = 0; ai < auth_tc; ai++)
         free(auth_tokens[ai]);
      if (status != 0 || !output || !output[0])
      {
         free(output);
         return -1;
      }
      char token[MAX_API_KEY_LEN];
      snprintf(token, sizeof(token), "%s", output);
      free(output);
      /* Strip trailing newline */
      size_t len = strlen(token);
      while (len > 0 && (token[len - 1] == '\n' || token[len - 1] == '\r'))
         token[--len] = '\0';
      if (!token[0])
         return -1;

      snprintf(buf, buf_len, "Authorization: Bearer %s", token);
      return 0;
   }

   /* Credential precedence for the in-flight turn (P4): the permanent VAULT wins
    * — resolved for the turn's attested principal with autonomous server-principal
    * fallback, so EVERY connection (primary chat, webchat, in-model tool, delegate)
    * is served from the one store; then the agent's stored api_key / env below.
    * vault_service_inject_api_key handles the dual-wrap (server-wrap -> user-KEK ->
    * server principal) AND is robust to an empty principal (it just resolves the
    * server principal); a locked/miss result falls through here, while the delegate
    * path keeps its own D15 hard-fail-on-locked. Computed only for the key-bearing
    * auth types below (not codex-oauth / auth_cmd), to avoid a wasted decrypt on
    * turns that never consult it. */
   char primary_key[MAX_API_KEY_LEN];
   int have_primary_key =
       (vault_service_inject_api_key(agent_get_request_vault_principal(), agent->name, primary_key,
                                     sizeof(primary_key), time(NULL)) == VAULT_OK &&
        primary_key[0]);

   /* x-api-key auth (Anthropic): resolve via the vault, auth_cmd or api_key */
   if (strcmp(auth_type, "x-api-key") == 0)
   {
      if (have_primary_key)
      {
         snprintf(buf, buf_len, "x-api-key: %s", primary_key);
         return 0;
      }
      if (agent->auth_cmd[0])
      {
         char *auth_tokens[32];
         int auth_tc = shlex_split(agent->auth_cmd, auth_tokens, 32);
         if (auth_tc <= 0)
            return -1;
         const char *auth_argv[33];
         for (int ai = 0; ai < auth_tc && ai < 32; ai++)
            auth_argv[ai] = auth_tokens[ai];
         auth_argv[auth_tc] = NULL;
         char *output = NULL;
         int status = safe_exec_capture(auth_argv, &output, MAX_API_KEY_LEN);
         for (int ai = 0; ai < auth_tc; ai++)
            free(auth_tokens[ai]);
         if (status != 0 || !output || !output[0])
         {
            free(output);
            return -1;
         }
         char token[MAX_API_KEY_LEN];
         snprintf(token, sizeof(token), "%s", output);
         free(output);
         size_t len = strlen(token);
         while (len > 0 && (token[len - 1] == '\n' || token[len - 1] == '\r'))
            token[--len] = '\0';
         if (!token[0])
            return -1;
         snprintf(buf, buf_len, "x-api-key: %s", token);
         return 0;
      }
      if (agent_api_key_literal(agent->api_key))
      {
         snprintf(buf, buf_len, "x-api-key: %s", agent->api_key);
         return 0;
      }
      char token[MAX_API_KEY_LEN];
      if (agent_provider_env_value(agent->provider, token, sizeof(token)))
      {
         snprintf(buf, buf_len, "x-api-key: %s", token);
         return 0;
      }
   }

   if (strcmp(agent->provider, "gemini") == 0 &&
       (!auth_type[0] || strcmp(auth_type, "api_key") == 0))
   {
      if (have_primary_key)
      {
         snprintf(buf, buf_len, "x-goog-api-key: %s", primary_key);
         return 0;
      }
      if (agent_api_key_literal(agent->api_key))
      {
         snprintf(buf, buf_len, "x-goog-api-key: %s", agent->api_key);
         return 0;
      }
      char token[MAX_API_KEY_LEN];
      if (agent_provider_env_value(agent->provider, token, sizeof(token)))
      {
         snprintf(buf, buf_len, "x-goog-api-key: %s", token);
         return 0;
      }
   }

   /* Default: bearer token — session keyring first, then stored api_key. */
   if (have_primary_key)
   {
      snprintf(buf, buf_len, "Authorization: Bearer %s", primary_key);
      return 0;
   }
   if (agent_api_key_literal(agent->api_key))
   {
      snprintf(buf, buf_len, "Authorization: Bearer %s", agent->api_key);
      return 0;
   }
   {
      char token[MAX_API_KEY_LEN];
      if (agent_provider_env_value(agent->provider, token, sizeof(token)))
      {
         snprintf(buf, buf_len, "Authorization: Bearer %s", token);
         return 0;
      }
   }

   return 0; /* no auth needed (e.g., local Ollama) */
}
