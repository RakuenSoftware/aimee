/* model_registry.c: model alias resolution and provider autodetection */
#include "aimee.h"
#include "aimee_home.h"
#include "model_registry.h"
#include "cJSON.h"
#include "models_dev.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

/* --- Alias table ---
 * Short aliases → canonical provider + model ID.
 * Keep in sync with assistant knowledge cutoff: August 2025.
 */
typedef struct
{
   const char *alias;
   const char *provider;
   const char *model_id;
} alias_entry_t;

static const alias_entry_t g_aliases[] = {
    /* Anthropic Claude */
    {"opus", "anthropic", "claude-opus-4-6"},
    {"opus4", "anthropic", "claude-opus-4-6"},
    {"claude-opus", "anthropic", "claude-opus-4-6"},
    {"sonnet", "anthropic", "claude-sonnet-4-6"},
    {"sonnet4", "anthropic", "claude-sonnet-4-6"},
    {"claude-sonnet", "anthropic", "claude-sonnet-4-6"},
    {"haiku", "anthropic", "claude-haiku-4-5-20251001"},
    {"haiku4", "anthropic", "claude-haiku-4-5-20251001"},
    {"claude-haiku", "anthropic", "claude-haiku-4-5-20251001"},
    {"claude", "anthropic", "claude-sonnet-4-6"},

    /* OpenAI */
    {"gpt4o", "openai", "gpt-4o"},
    {"gpt-4o", "openai", "gpt-4o"},
    {"gpt4", "openai", "gpt-4-turbo"},
    {"gpt-4", "openai", "gpt-4-turbo"},
    {"gpt4-turbo", "openai", "gpt-4-turbo"},
    {"gpt35", "openai", "gpt-3.5-turbo"},
    {"gpt-35", "openai", "gpt-3.5-turbo"},
    {"gpt3", "openai", "gpt-3.5-turbo"},
    {"o1", "openai", "o1"},
    {"o1-mini", "openai", "o1-mini"},
    {"o3", "openai", "o3"},
    {"o3-mini", "openai", "o3-mini"},

    /* Google Gemini */
    {"gemini", "gemini", "gemini-1.5-pro"},
    {"gemini-pro", "gemini", "gemini-1.5-pro"},
    {"gemini-flash", "gemini", "gemini-1.5-flash"},
    {"gemini-2", "gemini", "gemini-2.0-flash"},
    {"gemini-25", "gemini", "gemini-2.5-pro"},

    /* Mistral */
    {"mistral", "mistral", "mistral-large-latest"},
    {"mistral-large", "mistral", "mistral-large-latest"},
    {"mistral-small", "mistral", "mistral-small-latest"},
    {"codestral", "mistral", "codestral-latest"},

    /* Sentinel */
    {NULL, NULL, NULL},
};

int model_alias_resolve(const char *alias, model_info_t *out)
{
   if (!alias || !alias[0] || !out)
      return 0;

   for (int i = 0; g_aliases[i].alias != NULL; i++)
   {
      if (strcasecmp(g_aliases[i].alias, alias) == 0)
      {
         snprintf(out->provider, sizeof(out->provider), "%s", g_aliases[i].provider);
         snprintf(out->model_id, sizeof(out->model_id), "%s", g_aliases[i].model_id);
         return 1;
      }
   }
   return 0;
}

/* --- Provider detection table ---
 * Map model ID prefixes/substrings to provider names.
 */
typedef struct
{
   const char *prefix; /* substring to match (case-insensitive) */
   const char *provider;
} detect_entry_t;

static const detect_entry_t g_detect[] = {
    {"claude", "anthropic"}, {"gpt-", "openai"},         {"gpt4", "openai"},     {"o1", "openai"},
    {"o3", "openai"},        {"text-davinci", "openai"}, {"gemini", "gemini"},   {"palm", "gemini"},
    {"mistral", "mistral"},  {"codestral", "mistral"},   {"minimax", "minimax"}, {NULL, NULL},
};

const char *model_detect_provider(const char *model_id)
{
   if (!model_id || !model_id[0])
      return NULL;

   for (int i = 0; g_detect[i].prefix != NULL; i++)
   {
      /* Case-insensitive substring match from start */
      size_t plen = strlen(g_detect[i].prefix);
      if (strncasecmp(model_id, g_detect[i].prefix, plen) == 0)
         return g_detect[i].provider;
   }

   /* Check for substring match anywhere (e.g. "mistral-claude-...") */
   for (int i = 0; g_detect[i].prefix != NULL; i++)
   {
      /* Only do substring for longer prefixes to avoid false positives */
      if (strlen(g_detect[i].prefix) >= 4)
      {
         /* Case-insensitive search using manual scan */
         const char *hay = model_id;
         const char *needle = g_detect[i].prefix;
         size_t nlen = strlen(needle);
         while (*hay)
         {
            if (strncasecmp(hay, needle, nlen) == 0)
               return g_detect[i].provider;
            hay++;
         }
      }
   }

   /* Unknown: treat as OpenAI-compatible */
   return "openai";
}

/* --- Context window table ---
 * Map model ID prefixes to context window sizes (in tokens).
 * Order matters: longer/more-specific prefixes first for correct matching.
 */
typedef struct
{
   const char *prefix;
   int context_window;
} ctx_window_entry_t;

static const ctx_window_entry_t g_ctx_windows[] = {
    /* Anthropic Claude */
    {"claude-opus-4", 200000},
    {"claude-sonnet-4", 200000},
    {"claude-haiku-4", 200000},
    {"claude-3-5-sonnet", 200000},
    {"claude-3-5-haiku", 200000},
    {"claude-3-opus", 200000},
    {"claude-3-sonnet", 200000},
    {"claude-3-haiku", 200000},
    {"claude-2", 100000},
    {"claude", 200000}, /* fallback for any claude model */

    /* OpenAI */
    {"gpt-5.5", 272000},
    {"gpt-5.4", 272000},
    {"gpt-5.3", 272000},
    {"gpt-5.2", 272000},
    {"gpt-5", 272000},
    {"gpt-4o", 128000},
    {"gpt-4-turbo", 128000},
    {"gpt-4-0125", 128000},
    {"gpt-4-1106", 128000},
    {"gpt-4-32k", 32768},
    {"gpt-4", 8192},
    {"gpt-3.5-turbo-16k", 16384},
    {"gpt-3.5-turbo", 16384},
    {"o1", 200000},
    {"o3", 200000},

    /* Google Gemini */
    {"gemini-2.5", 1000000},
    {"gemini-2.0", 1000000},
    {"gemini-1.5-pro", 1000000},
    {"gemini-1.5-flash", 1000000},
    {"gemini-1.0", 32768},
    {"gemini", 1000000}, /* fallback */

    /* Mistral */
    {"codestral", 256000},
    {"mistral-large", 128000},
    {"mistral-small", 128000},
    {"mistral", 128000}, /* fallback */

    /* MiniMax */
    {"MiniMax-M2", 200000},
    {"minimax-m2", 200000},
    {"minimax", 200000}, /* fallback */

    /* Sentinel */
    {NULL, 0},
};

int model_context_window(const char *model_id)
{
   if (!model_id || !model_id[0])
      return 0;

   for (int i = 0; g_ctx_windows[i].prefix != NULL; i++)
   {
      size_t plen = strlen(g_ctx_windows[i].prefix);
      if (strncasecmp(model_id, g_ctx_windows[i].prefix, plen) == 0)
         return g_ctx_windows[i].context_window;
   }
   return 0;
}

static int model_has_prefix(const char *model_id, const char *prefix)
{
   if (!model_id || !prefix)
      return 0;
   size_t plen = strlen(prefix);
   return strncasecmp(model_id, prefix, plen) == 0;
}

static int model_contains_ci(const char *model_id, const char *needle)
{
   if (!model_id || !needle || !needle[0])
      return 0;
   size_t nlen = strlen(needle);
   for (const char *p = model_id; *p; p++)
      if (strncasecmp(p, needle, nlen) == 0)
         return 1;
   return 0;
}

static void capability_set_modalities(model_capability_t *cap)
{
   if (cap->flags & MODEL_CAP_AUDIO)
      snprintf(cap->modalities, sizeof(cap->modalities), "text,image,audio");
   else if (cap->flags & MODEL_CAP_VISION)
      snprintf(cap->modalities, sizeof(cap->modalities), "text,image");
   else
      snprintf(cap->modalities, sizeof(cap->modalities), "text");
}

static const char *model_semantic_id(const char *model_id)
{
   const char *slash = model_id ? strrchr(model_id, '/') : NULL;
   return (slash && slash[1]) ? slash + 1 : model_id;
}

static int model_capability_get_heuristic(const char *provider, const char *model_id,
                                          model_capability_t *out)
{
   if (!model_id || !model_id[0] || !out)
      return 0;
   memset(out, 0, sizeof(*out));
   const char *semantic_id = model_semantic_id(model_id);
   const char *effective_provider =
       provider && provider[0] ? provider : model_detect_provider(model_id);
   /* OpenRouter ids include the upstream provider prefix; keep the caller's
    * provider identity outside this function, but infer capabilities from the
    * upstream model family. */
   if (effective_provider && strcasecmp(effective_provider, "openrouter") == 0)
      effective_provider = model_detect_provider(semantic_id);

   snprintf(out->provider, sizeof(out->provider), "%s",
            provider && provider[0] ? provider : (effective_provider ? effective_provider : ""));
   snprintf(out->model_id, sizeof(out->model_id), "%s", model_id);
   out->context_window = model_context_window(model_id);
   if (out->context_window <= 0 && semantic_id && semantic_id != model_id)
      out->context_window = model_context_window(semantic_id);
   out->max_output = 0;

   if (effective_provider && strcasecmp(effective_provider, "anthropic") == 0)
   {
      out->flags = MODEL_CAP_TOOLS | MODEL_CAP_STREAMING;
      if (model_has_prefix(semantic_id, "claude-3") ||
          model_has_prefix(semantic_id, "claude-opus-4") ||
          model_has_prefix(semantic_id, "claude-sonnet-4") ||
          model_has_prefix(semantic_id, "claude-haiku-4"))
         out->flags |= MODEL_CAP_VISION | MODEL_CAP_PDF;
      if (model_contains_ci(semantic_id, "opus") || model_contains_ci(semantic_id, "sonnet"))
         out->flags |= MODEL_CAP_REASONING;
   }
   else if (effective_provider && strcasecmp(effective_provider, "openai") == 0)
   {
      out->flags = MODEL_CAP_TOOLS | MODEL_CAP_STREAMING;
      if (model_has_prefix(semantic_id, "gpt-4o") || model_has_prefix(semantic_id, "gpt-5"))
         out->flags |= MODEL_CAP_VISION;
      if (model_has_prefix(semantic_id, "gpt-5") || model_has_prefix(semantic_id, "o1") ||
          model_has_prefix(semantic_id, "o3"))
         out->flags |= MODEL_CAP_REASONING;
   }
   else if (effective_provider && strcasecmp(effective_provider, "gemini") == 0)
   {
      out->flags = MODEL_CAP_TOOLS | MODEL_CAP_STREAMING | MODEL_CAP_VISION | MODEL_CAP_PDF;
      if (model_has_prefix(semantic_id, "gemini-2.5"))
         out->flags |= MODEL_CAP_REASONING;
   }
   else if (effective_provider && strcasecmp(effective_provider, "mistral") == 0)
   {
      out->flags = MODEL_CAP_STREAMING | MODEL_CAP_TOOLS;
   }
   else if (effective_provider && strcasecmp(effective_provider, "minimax") == 0)
   {
      out->flags = MODEL_CAP_REASONING | MODEL_CAP_TOOLS | MODEL_CAP_STREAMING;
   }

   /* Output-token ceiling inferred when the model isn't in the static table.
    * Reasoning models must budget for a (sometimes large) hidden reasoning trace
    * plus the visible answer, so they get a higher ceiling than plain chat
    * models; both are clamped to the context window. This is the model-specified
    * cap the request builders use instead of any hardcoded default. */
   if (out->max_output <= 0)
      out->max_output = (out->flags & MODEL_CAP_REASONING) ? 32768 : 8192;
   if (out->context_window > 0 && out->max_output > out->context_window)
      out->max_output = out->context_window;

   out->deprecated = model_contains_ci(model_id, "deprecated") ? 1 : 0;
   capability_set_modalities(out);
   return 1;
}

/* The model's output-token ceiling, the single source of truth for a request's
 * max_tokens when the caller/agent didn't pin one explicitly. Resolves the
 * registry's per-model max_output (static table or inferred); falls back to a
 * conservative ceiling only for a genuinely unknown model. Never returns 0, so
 * callers can always emit a concrete, model-appropriate cap. */
int model_max_output(const char *provider, const char *model_id)
{
   model_capability_t cap;
   if (model_id && model_id[0] && model_capability_get(provider, model_id, &cap) &&
       cap.max_output > 0)
      return cap.max_output;
   return 8192;
}

unsigned model_capability_flag_from_name(const char *name)
{
   if (!name || !name[0])
      return 0;
   if (strcasecmp(name, "reasoning") == 0)
      return MODEL_CAP_REASONING;
   if (strcasecmp(name, "tools") == 0 || strcasecmp(name, "tool") == 0)
      return MODEL_CAP_TOOLS;
   if (strcasecmp(name, "vision") == 0 || strcasecmp(name, "image") == 0)
      return MODEL_CAP_VISION;
   if (strcasecmp(name, "pdf") == 0)
      return MODEL_CAP_PDF;
   if (strcasecmp(name, "audio") == 0)
      return MODEL_CAP_AUDIO;
   if (strcasecmp(name, "streaming") == 0 || strcasecmp(name, "stream") == 0)
      return MODEL_CAP_STREAMING;
   return 0;
}

void model_capability_format_flags(unsigned flags, char *buf, size_t buf_len)
{
   if (!buf || buf_len == 0)
      return;
   buf[0] = '\0';
   struct
   {
      unsigned flag;
      const char *name;
   } rows[] = {{MODEL_CAP_REASONING, "reasoning"},
               {MODEL_CAP_TOOLS, "tools"},
               {MODEL_CAP_VISION, "vision"},
               {MODEL_CAP_PDF, "pdf"},
               {MODEL_CAP_AUDIO, "audio"},
               {MODEL_CAP_STREAMING, "streaming"},
               {0, NULL}};
   for (int i = 0; rows[i].name; i++)
   {
      if (!(flags & rows[i].flag))
         continue;
      size_t used = strlen(buf);
      if (used >= buf_len)
         break;
      snprintf(buf + used, buf_len - used, "%s%s", used ? "," : "", rows[i].name);
   }
   if (!buf[0])
      snprintf(buf, buf_len, "-");
}

int model_capability_resolve_ref(const char *ref, char *provider, size_t provider_cap,
                                 char *model_id, size_t model_id_cap, model_capability_t *out)
{
   if (!ref || !ref[0] || !provider || provider_cap == 0 || !model_id || model_id_cap == 0 || !out)
      return 0;

   model_info_t info;
   if (!strchr(ref, ':') && model_alias_resolve(ref, &info))
   {
      int pn = snprintf(provider, provider_cap, "%s", info.provider);
      int mn = snprintf(model_id, model_id_cap, "%s", info.model_id);
      if (pn < 0 || (size_t)pn >= provider_cap || mn < 0 || (size_t)mn >= model_id_cap)
         return 0;
      return model_capability_get(provider, model_id, out);
   }

   const char *colon = strchr(ref, ':');
   if (colon && colon > ref && colon[1])
   {
      size_t plen = (size_t)(colon - ref);
      if (plen >= provider_cap)
         return 0;
      memcpy(provider, ref, plen);
      provider[plen] = '\0';
      int mn = snprintf(model_id, model_id_cap, "%s", colon + 1);
      if (mn < 0 || (size_t)mn >= model_id_cap)
         return 0;
   }
   else if (colon)
      return 0;
   else
   {
      int mn = snprintf(model_id, model_id_cap, "%s", ref);
      if (mn < 0 || (size_t)mn >= model_id_cap)
         return 0;
      const char *detected = model_detect_provider(model_id);
      int pn = snprintf(provider, provider_cap, "%s", detected ? detected : "");
      if (pn < 0 || (size_t)pn >= provider_cap)
         return 0;
   }
   return model_capability_get(provider, model_id, out);
}

int model_alias_list(model_info_t *out, int max)
{
   int count = 0;
   for (int i = 0; g_aliases[i].alias != NULL; i++)
      count++;

   if (!out || max <= 0)
      return count;

   int n = count < max ? count : max;
   for (int i = 0; i < n; i++)
   {
      snprintf(out[i].provider, sizeof(out[i].provider), "%s", g_aliases[i].provider);
      snprintf(out[i].model_id, sizeof(out[i].model_id), "%s:%s", g_aliases[i].alias,
               g_aliases[i].model_id);
   }
   return count;
}

typedef struct
{
   const char *provider;
   const char *model_id;
   int context_window;
   int max_output;
   double cost_in_per_mtok;
   double cost_out_per_mtok;
   unsigned flags;
   const char *modalities;
   const char *knowledge_cutoff;
   int open_weights;
   int deprecated;
} capability_entry_t;

static const capability_entry_t g_capabilities[] = {
    {"openai", "gpt-4o", 128000, 16384, 2.50, 10.00,
     MODEL_CAP_REASONING | MODEL_CAP_TOOLS | MODEL_CAP_VISION | MODEL_CAP_PDF | MODEL_CAP_AUDIO |
         MODEL_CAP_STREAMING,
     "text,image,audio", "2025-06", 0, 0},
    {"openai", "gpt-4-turbo", 128000, 4096, 10.00, 30.00,
     MODEL_CAP_REASONING | MODEL_CAP_TOOLS | MODEL_CAP_VISION | MODEL_CAP_PDF | MODEL_CAP_STREAMING,
     "text,image", "2024-04", 0, 1},
    {"anthropic", "claude-3-5-sonnet", 200000, 8192, 3.00, 15.00,
     MODEL_CAP_REASONING | MODEL_CAP_TOOLS | MODEL_CAP_VISION | MODEL_CAP_PDF | MODEL_CAP_STREAMING,
     "text,image", "2025-04", 0, 0},
    {"anthropic", "claude-sonnet-4-6", 200000, 8192, 3.00, 15.00,
     MODEL_CAP_REASONING | MODEL_CAP_TOOLS | MODEL_CAP_VISION | MODEL_CAP_PDF | MODEL_CAP_STREAMING,
     "text,image", "2025-10", 0, 0},
    {"anthropic", "claude-opus-4-6", 200000, 8192, 15.00, 75.00,
     MODEL_CAP_REASONING | MODEL_CAP_TOOLS | MODEL_CAP_VISION | MODEL_CAP_PDF | MODEL_CAP_STREAMING,
     "text,image", "2025-10", 0, 0},
    {"gemini", "gemini-1.5-pro", 1000000, 8192, 1.25, 5.00,
     MODEL_CAP_REASONING | MODEL_CAP_TOOLS | MODEL_CAP_VISION | MODEL_CAP_AUDIO |
         MODEL_CAP_STREAMING,
     "text,image,audio", "2025-08", 0, 0},
    {"google", "gemini-1.5-pro", 1000000, 8192, 1.25, 5.00,
     MODEL_CAP_REASONING | MODEL_CAP_TOOLS | MODEL_CAP_VISION | MODEL_CAP_AUDIO |
         MODEL_CAP_STREAMING,
     "text,image,audio", "2025-08", 0, 0},
    {"groq", "llama-3-70b-instruct", 8192, 4096, 0.59, 0.79,
     MODEL_CAP_REASONING | MODEL_CAP_TOOLS | MODEL_CAP_STREAMING, "text", "2023-12", 1, 0},
    {"minimax", "MiniMax-M2.7", 200000, 8192, 0.0, 0.0,
     MODEL_CAP_REASONING | MODEL_CAP_TOOLS | MODEL_CAP_STREAMING, "text", "2026-01", 0, 0},
    {NULL, NULL, 0, 0, 0.0, 0.0, 0, NULL, NULL, 0, 0},
};

#define MODEL_CAP_DYNAMIC_MAX 1024
static model_capability_t g_dynamic_caps[MODEL_CAP_DYNAMIC_MAX];
static int g_dynamic_count = -1; /* -1 means not loaded yet */

static char *read_text_file_all(const char *path, size_t *len_out)
{
   if (!path || !path[0])
      return NULL;
   FILE *fp = fopen(path, "rb");
   if (!fp)
      return NULL;
   if (fseek(fp, 0, SEEK_END) != 0)
   {
      fclose(fp);
      return NULL;
   }
   long sz = ftell(fp);
   if (sz < 0 || sz > 16L * 1024L * 1024L)
   {
      fclose(fp);
      return NULL;
   }
   if (fseek(fp, 0, SEEK_SET) != 0)
   {
      fclose(fp);
      return NULL;
   }
   char *buf = malloc((size_t)sz + 1);
   if (!buf)
   {
      fclose(fp);
      return NULL;
   }
   size_t n = fread(buf, 1, (size_t)sz, fp);
   fclose(fp);
   buf[n] = '\0';
   if (len_out)
      *len_out = n;
   return buf;
}

static int mkdir_p_local(const char *path)
{
   if (!path || !path[0])
      return -1;
   char tmp[MAX_PATH_LEN];
   snprintf(tmp, sizeof(tmp), "%s", path);
   for (char *p = tmp + 1; *p; p++)
   {
      if (*p == '/')
      {
         *p = '\0';
         if (mkdir(tmp, 0700) != 0 && errno != EEXIST)
            return -1;
         *p = '/';
      }
   }
   if (mkdir(tmp, 0700) != 0 && errno != EEXIST)
      return -1;
   return 0;
}

static int write_text_file_atomic(const char *path, const char *text, size_t nbytes)
{
   if (!path || !path[0] || !text)
      return -1;
   char tmp[MAX_PATH_LEN];
   snprintf(tmp, sizeof(tmp), "%s.tmp", path);
   int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
   if (fd < 0)
      return -1;
   ssize_t wn = write(fd, text, nbytes);
   if (wn != (ssize_t)nbytes)
   {
      close(fd);
      unlink(tmp);
      return -1;
   }
   if (close(fd) != 0)
   {
      unlink(tmp);
      return -1;
   }
   if (rename(tmp, path) != 0)
   {
      unlink(tmp);
      return -1;
   }
   return 0;
}

static int model_cache_path(char *out, size_t out_len)
{
   if (!out || out_len == 0)
      return -1;
   const char *xdg = getenv("XDG_CACHE_HOME");
   if (xdg && xdg[0])
   {
      snprintf(out, out_len, "%s/aimee/models_dev.json", xdg);
      return 0;
   }
   const char *home = getenv("HOME");
   if (!home || !home[0])
      return -1;
   snprintf(out, out_len, "%s/.cache/aimee/models_dev.json", home);
   return 0;
}

static int model_override_path(char *out, size_t out_len)
{
   if (!out || out_len == 0)
      return -1;
   const char *env = getenv("AIMEE_MODEL_CAPABILITY_OVERRIDES");
   if (env && env[0])
   {
      snprintf(out, out_len, "%s", env);
      return 0;
   }
   const char *home = aimee_home();
   if (!home || !home[0])
      return -1;
   snprintf(out, out_len, "%s/model_capability_overrides.json", home);
   return 0;
}

static int model_snapshot_path(char *out, size_t out_len)
{
   if (!out || out_len == 0)
      return -1;
   const char *env = getenv("AIMEE_MODELS_DEV_SNAPSHOT");
   if (env && env[0])
   {
      snprintf(out, out_len, "%s", env);
      return 0;
   }
   const char *candidates[] = {"data/models_dev_snapshot.json", "../data/models_dev_snapshot.json",
                               NULL};
   for (int i = 0; candidates[i]; i++)
   {
      if (access(candidates[i], R_OK) == 0)
      {
         snprintf(out, out_len, "%s", candidates[i]);
         return 0;
      }
   }
   return -1;
}

static int capability_copy_from_json(cJSON *obj, model_capability_t *out)
{
   if (!cJSON_IsObject(obj) || !out)
      return 0;
   cJSON *jprovider = cJSON_GetObjectItemCaseSensitive(obj, "provider");
   cJSON *jmodel = cJSON_GetObjectItemCaseSensitive(obj, "model");
   cJSON *jid = cJSON_GetObjectItemCaseSensitive(obj, "id");
   const char *provider = cJSON_GetStringValue(jprovider);
   const char *model = cJSON_GetStringValue(jmodel);
   const char *id = cJSON_GetStringValue(jid);
   char provider_buf[MODEL_PROVIDER_MAX] = "";
   char model_buf[MODEL_ID_MAX] = "";

   if ((!provider || !provider[0]) && id && id[0])
   {
      const char *slash = strchr(id, '/');
      if (slash && slash > id && slash[1])
      {
         size_t plen = (size_t)(slash - id);
         if (plen >= sizeof(provider_buf))
            plen = sizeof(provider_buf) - 1;
         memcpy(provider_buf, id, plen);
         provider_buf[plen] = '\0';
         provider = provider_buf;
         snprintf(model_buf, sizeof(model_buf), "%s", slash + 1);
         model = model_buf;
      }
   }
   if (!provider || !provider[0] || !model || !model[0])
      return 0;

   memset(out, 0, sizeof(*out));
   snprintf(out->provider, sizeof(out->provider), "%s", provider);
   snprintf(out->model_id, sizeof(out->model_id), "%s", model);

   cJSON *jctx = cJSON_GetObjectItemCaseSensitive(obj, "context_window");
   cJSON *jmax = cJSON_GetObjectItemCaseSensitive(obj, "max_output");
   cJSON *jin = cJSON_GetObjectItemCaseSensitive(obj, "cost_in_per_mtok");
   cJSON *jout = cJSON_GetObjectItemCaseSensitive(obj, "cost_out_per_mtok");
   cJSON *jflags = cJSON_GetObjectItemCaseSensitive(obj, "flags");
   cJSON *jcutoff = cJSON_GetObjectItemCaseSensitive(obj, "knowledge_cutoff");
   cJSON *jopen = cJSON_GetObjectItemCaseSensitive(obj, "open_weights");
   cJSON *jdep = cJSON_GetObjectItemCaseSensitive(obj, "deprecated");

   out->context_window =
       cJSON_IsNumber(jctx) ? (int)jctx->valuedouble : model_context_window(model);
   out->max_output = cJSON_IsNumber(jmax) ? (int)jmax->valuedouble : 0;
   out->cost_in_per_mtok = cJSON_IsNumber(jin) ? jin->valuedouble : 0.0;
   out->cost_out_per_mtok = cJSON_IsNumber(jout) ? jout->valuedouble : 0.0;
   out->flags = cJSON_IsNumber(jflags) ? (unsigned)jflags->valuedouble : 0;
   if (out->flags && !(out->flags & MODEL_CAP_STREAMING))
      out->flags |= MODEL_CAP_STREAMING;
   out->open_weights = cJSON_IsTrue(jopen) || (cJSON_IsNumber(jopen) && jopen->valueint != 0);
   out->deprecated = cJSON_IsTrue(jdep) || (cJSON_IsNumber(jdep) && jdep->valueint != 0);
   if (cJSON_IsString(jcutoff) && jcutoff->valuestring)
      snprintf(out->knowledge_cutoff, sizeof(out->knowledge_cutoff), "%s", jcutoff->valuestring);
   capability_set_modalities(out);
   return 1;
}

static int capability_load_from_text(const char *json_text, model_capability_t *out, int max)
{
   if (!json_text || !out || max <= 0)
      return 0;
   cJSON *root = cJSON_Parse(json_text);
   if (!root)
      return 0;
   cJSON *arr = cJSON_GetObjectItemCaseSensitive(root, "models");
   if (!cJSON_IsArray(arr) && cJSON_IsArray(root))
      arr = root;
   if (!cJSON_IsArray(arr))
   {
      cJSON_Delete(root);
      return 0;
   }

   int written = 0;
   cJSON *item;
   cJSON_ArrayForEach(item, arr)
   {
      if (written >= max)
         break;
      if (capability_copy_from_json(item, &out[written]))
         written++;
   }
   cJSON_Delete(root);
   return written;
}

static int capability_load_from_file(const char *path, model_capability_t *out, int max)
{
   size_t n = 0;
   char *text = read_text_file_all(path, &n);
   if (!text)
      return 0;
   int count = capability_load_from_text(text, out, max);
   free(text);
   return count;
}

static void capability_apply_overrides(model_capability_t *caps, int *count_io, int max_caps)
{
   if (!caps || !count_io || *count_io < 0)
      return;
   char path[MAX_PATH_LEN];
   if (model_override_path(path, sizeof(path)) != 0 || access(path, R_OK) != 0)
      return;

   model_capability_t overrides[256];
   int n = capability_load_from_file(path, overrides, 256);
   for (int i = 0; i < n; i++)
   {
      int replaced = 0;
      for (int j = 0; j < *count_io; j++)
      {
         if (strcasecmp(caps[j].provider, overrides[i].provider) == 0 &&
             strcasecmp(caps[j].model_id, overrides[i].model_id) == 0)
         {
            caps[j] = overrides[i];
            replaced = 1;
            break;
         }
      }
      if (!replaced && *count_io < max_caps)
         caps[(*count_io)++] = overrides[i];
   }
}

static void capability_dynamic_load_once(void)
{
   if (g_dynamic_count >= 0)
      return;

   int count = 0;
   char path[MAX_PATH_LEN];
   if (model_cache_path(path, sizeof(path)) == 0 && access(path, R_OK) == 0)
      count = capability_load_from_file(path, g_dynamic_caps, MODEL_CAP_DYNAMIC_MAX);
   if (count <= 0 && model_snapshot_path(path, sizeof(path)) == 0)
      count = capability_load_from_file(path, g_dynamic_caps, MODEL_CAP_DYNAMIC_MAX);
   if (count > 0)
      capability_apply_overrides(g_dynamic_caps, &count, MODEL_CAP_DYNAMIC_MAX);
   g_dynamic_count = count > 0 ? count : 0;
}

int model_capability_refresh(char *msg, size_t msg_len)
{
   char snapshot_path[MAX_PATH_LEN];
   char cache_path[MAX_PATH_LEN];
   int copied_cache = 0;
   int loaded = 0;

   if (model_snapshot_path(snapshot_path, sizeof(snapshot_path)) == 0)
   {
      size_t nbytes = 0;
      char *text = read_text_file_all(snapshot_path, &nbytes);
      if (text)
      {
         model_capability_t tmp[MODEL_CAP_DYNAMIC_MAX];
         loaded = capability_load_from_text(text, tmp, MODEL_CAP_DYNAMIC_MAX);
         if (loaded > 0 && model_cache_path(cache_path, sizeof(cache_path)) == 0)
         {
            char dir[MAX_PATH_LEN];
            snprintf(dir, sizeof(dir), "%s", cache_path);
            char *slash = strrchr(dir, '/');
            if (slash)
            {
               *slash = '\0';
               if (mkdir_p_local(dir) == 0 && write_text_file_atomic(cache_path, text, nbytes) == 0)
                  copied_cache = 1;
            }
         }
         free(text);
      }
   }

   g_dynamic_count = -1;
   capability_dynamic_load_once();
   if (g_dynamic_count > 0)
      loaded = g_dynamic_count;

   if (msg && msg_len > 0)
   {
      if (loaded > 0)
      {
         snprintf(msg, msg_len, "model metadata refreshed (%d entries%s)", loaded,
                  copied_cache ? ", cache updated" : "");
      }
      else
         snprintf(msg, msg_len, "model metadata refresh unavailable; using built-in fallback");
   }
   return loaded;
}

static void capability_entry_copy(const capability_entry_t *entry, model_capability_t *out)
{
   memset(out, 0, sizeof(*out));
   snprintf(out->provider, sizeof(out->provider), "%s", entry->provider);
   snprintf(out->model_id, sizeof(out->model_id), "%s", entry->model_id);
   out->context_window = entry->context_window;
   out->max_output = entry->max_output;
   out->cost_in_per_mtok = entry->cost_in_per_mtok;
   out->cost_out_per_mtok = entry->cost_out_per_mtok;
   out->flags = entry->flags;
   snprintf(out->modalities, sizeof(out->modalities), "%s", entry->modalities);
   snprintf(out->knowledge_cutoff, sizeof(out->knowledge_cutoff), "%s", entry->knowledge_cutoff);
   out->open_weights = entry->open_weights;
   out->deprecated = entry->deprecated;
}

int model_capability_get(const char *provider, const char *model_id, model_capability_t *out)
{
   if (!model_id || !model_id[0] || !out)
      return 0;

   model_info_t alias;
   const char *lookup_model = model_id;
   const char *lookup_provider = provider;
   if ((!provider || !provider[0]) && model_alias_resolve(model_id, &alias))
   {
      lookup_provider = alias.provider;
      lookup_model = alias.model_id;
   }
   if (!lookup_provider || !lookup_provider[0])
      lookup_provider = model_detect_provider(lookup_model);

   /* Operator overrides win over all other sources (including static table). */
   if (models_dev_override_lookup(lookup_provider, lookup_model, out))
      return 1;

   capability_dynamic_load_once();
   for (int i = 0; i < g_dynamic_count; i++)
   {
      model_capability_t *cap = &g_dynamic_caps[i];
      if (lookup_provider && lookup_provider[0] && strcasecmp(cap->provider, lookup_provider) != 0)
         continue;
      if (strcasecmp(cap->model_id, lookup_model) == 0)
      {
         *out = *cap;
         return 1;
      }
   }

   for (int i = 0; g_capabilities[i].provider != NULL; i++)
   {
      const capability_entry_t *entry = &g_capabilities[i];
      if (lookup_provider && strcasecmp(entry->provider, lookup_provider) != 0)
         continue;
      if (strcasecmp(entry->model_id, lookup_model) == 0)
      {
         capability_entry_copy(entry, out);
         return 1;
      }
   }

   /* models.dev cache (disk cache → bundled snapshot). */
   if (models_dev_cache_lookup(lookup_provider, lookup_model, out))
      return 1;

   return model_capability_get_heuristic(lookup_provider, lookup_model, out);
}

int model_capability_list(model_capability_t *out, int max, unsigned required_flags,
                          int open_weights_only)
{
   capability_dynamic_load_once();
   if (g_dynamic_count > 0)
   {
      int count = 0;
      int written = 0;
      for (int i = 0; i < g_dynamic_count; i++)
      {
         model_capability_t *entry = &g_dynamic_caps[i];
         if (required_flags && (entry->flags & required_flags) != required_flags)
            continue;
         if (open_weights_only && !entry->open_weights)
            continue;
         if (out && written < max)
            out[written++] = *entry;
         count++;
      }
      return count;
   }

   int count = 0;
   int written = 0;
   for (int i = 0; g_capabilities[i].provider != NULL; i++)
   {
      const capability_entry_t *entry = &g_capabilities[i];
      if (required_flags && (entry->flags & required_flags) != required_flags)
         continue;
      if (open_weights_only && !entry->open_weights)
         continue;
      if (out && written < max)
         capability_entry_copy(entry, &out[written++]);
      count++;
   }

   /* Augment with models.dev cache entries not already in the static table. */
   int cache_slots = (out && max > written) ? max - written : 0;
   model_capability_t *cache_buf = (out && cache_slots > 0) ? out + written : NULL;
   int cache_count =
       models_dev_cache_list(cache_buf, cache_slots, required_flags, open_weights_only);
   count += cache_count;
   return count;
}

void model_capability_flags_string(unsigned flags, char *out, size_t out_len)
{
   model_capability_format_flags(flags, out, out_len);
}
