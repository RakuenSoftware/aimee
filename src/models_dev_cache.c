/* models_dev_cache.c: models.dev JSON cache lookup */
#include "aimee.h"
#include "cJSON.h"
#include "log.h"
#include "models_dev.h"
#include <stdio.h>
#include <stdlib.h>
#include <limits.h>
#include <string.h>
#include <sys/stat.h>

#define MODELS_DEV_MAX_SIZE (8 * 1024 * 1024)

/* The cache is DOWNLOADED and therefore untrusted. A JSON number outside int
 * range converted straight to int is undefined behaviour, so every numeric field
 * goes through this: non-finite, negative, and out-of-range values are rejected
 * rather than truncated into a nonsense capability. */
static int json_int_checked(cJSON *v, int *out)
{
   if (!cJSON_IsNumber(v))
      return 0;
   double d = v->valuedouble;
   if (!(d >= 0.0) || d > 2147483647.0) /* also rejects NaN */
      return 0;
   /* Must be integral: a context window of 199999.9 is malformed data, and
    * silently truncating it would contradict the reject-don't-truncate rule. */
   if (d != (double)(long long)d)
      return 0;
   *out = (int)d;
   return 1;
}

static int json_double_checked(cJSON *v, double *out)
{
   if (!cJSON_IsNumber(v))
      return 0;
   double d = v->valuedouble;
   if (!(d >= 0.0) || d > 1e12) /* also rejects NaN */
      return 0;
   *out = d;
   return 1;
}

/* Defined below: resolves provider/model against the NESTED live api.json shape
 * after the flat key lookup misses. */
static int lookup_in_nested_json(cJSON *root, const char *provider, const char *model_id,
                                 model_capability_t *out);

static void fill_cap_from_json(cJSON *entry, const char *provider, const char *model_id,
                               model_capability_t *out)
{
   memset(out, 0, sizeof(*out));
   snprintf(out->provider, sizeof(out->provider), "%s", provider);
   snprintf(out->model_id, sizeof(out->model_id), "%s", model_id);

   cJSON *tmp;
   tmp = cJSON_GetObjectItemCaseSensitive(entry, "contextWindow");
   (void)json_int_checked(tmp, &out->context_window);
   tmp = cJSON_GetObjectItemCaseSensitive(entry, "maxTokens");
   (void)json_int_checked(tmp, &out->max_output);
   tmp = cJSON_GetObjectItemCaseSensitive(entry, "inputCost");
   (void)json_double_checked(tmp, &out->cost_in_per_mtok);
   tmp = cJSON_GetObjectItemCaseSensitive(entry, "outputCost");
   (void)json_double_checked(tmp, &out->cost_out_per_mtok);
   tmp = cJSON_GetObjectItemCaseSensitive(entry, "cacheReadCost");
   (void)json_double_checked(tmp, &out->cost_cache_read_per_mtok);
   tmp = cJSON_GetObjectItemCaseSensitive(entry, "tools");
   if (cJSON_IsTrue(tmp))
      out->flags |= MODEL_CAP_TOOLS;
   tmp = cJSON_GetObjectItemCaseSensitive(entry, "vision");
   if (cJSON_IsTrue(tmp))
      out->flags |= MODEL_CAP_VISION;
   tmp = cJSON_GetObjectItemCaseSensitive(entry, "pdf");
   if (cJSON_IsTrue(tmp))
      out->flags |= MODEL_CAP_PDF;
   tmp = cJSON_GetObjectItemCaseSensitive(entry, "deprecated");
   if (cJSON_IsTrue(tmp))
      out->deprecated = 1;
}

static cJSON *load_json_from_path(const char *path)
{
   FILE *f = fopen(path, "r");
   if (!f)
      return NULL;
   fseek(f, 0, SEEK_END);
   long sz = ftell(f);
   fseek(f, 0, SEEK_SET);
   if (sz <= 0 || sz > MODELS_DEV_MAX_SIZE)
   {
      fclose(f);
      return NULL;
   }
   char *buf = malloc((size_t)sz + 1);
   if (!buf)
   {
      fclose(f);
      return NULL;
   }
   size_t n = fread(buf, 1, (size_t)sz, f);
   buf[n] = '\0';
   fclose(f);
   cJSON *root = cJSON_Parse(buf);
   free(buf);
   return root;
}

static int lookup_in_json(cJSON *root, const char *provider, const char *model_id,
                          model_capability_t *out)
{
   char key[256];
   snprintf(key, sizeof(key), "%s/%s", provider, model_id);
   /* Flat form wins only when it carries USABLE content. Mere presence must not
    * suppress the nested lookup: a null, a string, or an EMPTY object would
    * otherwise mask a perfectly good nested entry and return zeroed
    * capabilities — which downstream reads as "no context window, no price". */
   cJSON *entry = cJSON_GetObjectItemCaseSensitive(root, key);
   if (cJSON_IsObject(entry))
   {
      model_capability_t flat;
      fill_cap_from_json(entry, provider, model_id, &flat);
      if (flat.context_window > 0 || flat.max_output > 0 || flat.cost_in_per_mtok > 0.0 ||
          flat.cost_out_per_mtok > 0.0 || flat.flags != 0)
      {
         *out = flat;
         return 1;
      }
   }
   return lookup_in_nested_json(root, provider, model_id, out);
}

/* ---- models.dev live api.json (NESTED) ----------------------------------
 *
 * Two on-disk schemas must both resolve:
 *
 *  FLAT   {"provider/model": {"contextWindow":…, "inputCost":…}}
 *         - data/models_dev_snapshot.json (bundled) and model_overrides.json.
 *
 *  NESTED {"provider": {"models": {"model": {"limit":{"context":…},
 *                                            "cost":{"input":…}, …}}}}
 *         - exactly what https://models.dev/api.json serves, which
 *           models_dev_refresh() curls verbatim into the cache with no
 *           transform. Before this reader existed the downloaded cache could
 *           never resolve a single model: the flat key lookup returned NULL
 *           against a nested root, so every capability fell through to the
 *           heuristic and every price stayed 0.
 *
 * Reading both keeps the atomic-rename download and leaves the override format
 * untouched, instead of rewriting bytes we did not author. */
/* Context-band prices from `cost.tiers[]`.
 *
 * Only the structured tier entries are read. The registry also publishes a
 * `context_over_200k` alias, but its KEY DOES NOT ENCODE THE THRESHOLD —
 * gpt-5.6-sol's real boundary is 272000 and MiniMax-M3's is 512000, both under
 * that same key — so trusting the name would price large-context requests at the
 * wrong band. Only `tier.type == "context"` is understood; any other tier type
 * is skipped rather than guessed at. Bands are kept ascending so a lookup can
 * take the last one whose threshold the context exceeds. */
static void fill_price_bands(cJSON *cost, model_capability_t *out)
{
   cJSON *tiers = cJSON_GetObjectItemCaseSensitive(cost, "tiers");
   if (!cJSON_IsArray(tiers))
      return;

   cJSON *t;
   cJSON_ArrayForEach(t, tiers)
   {
      if (!cJSON_IsObject(t))
         continue;
      cJSON *spec = cJSON_GetObjectItemCaseSensitive(t, "tier");
      if (!cJSON_IsObject(spec))
         continue;
      const char *type = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(spec, "type"));
      if (!type || strcmp(type, "context") != 0)
         continue;
      int above = 0;
      if (!json_int_checked(cJSON_GetObjectItemCaseSensitive(spec, "size"), &above) || above <= 0)
         continue;
      /* A threshold of INT_MAX breaks both consumers: `above + 1` overflows
       * (undefined behaviour) when emitting the schedule, and a band applies
       * strictly ABOVE its threshold so INT_MAX could never be selected anyway.
       * Reject rather than store an unusable entry. */
      if (above == INT_MAX)
         continue;

      model_price_band_t band;
      memset(&band, 0, sizeof(band));
      band.above_tokens = above;
      /* A band with no usable input/output price tells us nothing. */
      if (!json_double_checked(cJSON_GetObjectItemCaseSensitive(t, "input"), &band.in_per_mtok) ||
          !json_double_checked(cJSON_GetObjectItemCaseSensitive(t, "output"), &band.out_per_mtok))
         continue;
      (void)json_double_checked(cJSON_GetObjectItemCaseSensitive(t, "cache_read"),
                                &band.cache_read_per_mtok);

      /* Duplicate threshold FIRST: last definition wins and consumes no slot.
       * Checking this before the capacity logic matters — at capacity, an
       * eviction pass would otherwise discard a replacement for the largest
       * threshold, or evict a distinct band to make room for an entry that
       * needed none, losing real data and spuriously marking the schedule
       * truncated. */
      int dup = -1;
      for (int k = 0; k < out->price_band_count; k++)
      {
         if (out->price_bands[k].above_tokens == band.above_tokens)
         {
            dup = k;
            break;
         }
      }
      if (dup >= 0)
      {
         out->price_bands[dup] = band;
         continue;
      }

      /* Insertion sort, ascending. When full, keep the LOWEST thresholds and
       * flag the schedule as truncated rather than dropping whichever entries
       * happened to arrive last: taking the first N in input order silently
       * corrupts pricing (a descending registry would lose the lowest band, so
       * mid-size requests keep the base rate). The flag makes consumers treat
       * an incomplete schedule conservatively instead of trusting it. */
      if (out->price_band_count >= MODEL_PRICE_BANDS_MAX)
      {
         out->price_bands_truncated = 1;
         if (band.above_tokens >= out->price_bands[MODEL_PRICE_BANDS_MAX - 1].above_tokens)
            continue; /* higher than everything kept: drop it */
         out->price_band_count = MODEL_PRICE_BANDS_MAX - 1; /* evict the largest */
      }
      int i = out->price_band_count;
      while (i > 0 && out->price_bands[i - 1].above_tokens > band.above_tokens)
      {
         out->price_bands[i] = out->price_bands[i - 1];
         i--;
      }
      out->price_bands[i] = band;
      out->price_band_count++;
   }
}

static void fill_cap_from_nested(cJSON *entry, const char *provider, const char *model_id,
                                 model_capability_t *out)
{
   memset(out, 0, sizeof(*out));
   snprintf(out->provider, sizeof(out->provider), "%s", provider);
   snprintf(out->model_id, sizeof(out->model_id), "%s", model_id);

   /* models.dev publishes no streaming field, so a catalogued entry would carry
    * MODEL_CAP_STREAMING clear -- which reads as "this model cannot stream"
    * rather than "the source does not say". Every heuristic path in
    * model_registry.c ORs this flag in unconditionally, so streaming is already
    * universal in this codebase's semantics; matching that here keeps a
    * catalogue hit from silently dropping the capability a static row carried. */
   out->flags |= MODEL_CAP_STREAMING;

   cJSON *limit = cJSON_GetObjectItemCaseSensitive(entry, "limit");
   if (cJSON_IsObject(limit))
   {
      (void)json_int_checked(cJSON_GetObjectItemCaseSensitive(limit, "context"),
                             &out->context_window);
      (void)json_int_checked(cJSON_GetObjectItemCaseSensitive(limit, "output"), &out->max_output);
   }

   cJSON *cost = cJSON_GetObjectItemCaseSensitive(entry, "cost");
   if (cJSON_IsObject(cost))
   {
      (void)json_double_checked(cJSON_GetObjectItemCaseSensitive(cost, "input"),
                                &out->cost_in_per_mtok);
      (void)json_double_checked(cJSON_GetObjectItemCaseSensitive(cost, "output"),
                                &out->cost_out_per_mtok);
      (void)json_double_checked(cJSON_GetObjectItemCaseSensitive(cost, "cache_read"),
                                &out->cost_cache_read_per_mtok);
      fill_price_bands(cost, out);
   }

   /* models.dev spells tool use "tool_call". REASONING has no flat-schema
    * equivalent at all, so before this it could only ever come from a
    * per-vendor heuristic branch. */
   if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(entry, "tool_call")))
      out->flags |= MODEL_CAP_TOOLS;
   if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(entry, "reasoning")))
      out->flags |= MODEL_CAP_REASONING;
   if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(entry, "open_weights")))
      out->open_weights = 1;

   cJSON *modalities = cJSON_GetObjectItemCaseSensitive(entry, "modalities");
   if (cJSON_IsObject(modalities))
   {
      cJSON *input = cJSON_GetObjectItemCaseSensitive(modalities, "input");
      cJSON *m;
      cJSON_ArrayForEach(m, input)
      {
         const char *s = cJSON_GetStringValue(m);
         if (!s)
            continue;
         if (strcmp(s, "image") == 0)
            out->flags |= MODEL_CAP_VISION;
         else if (strcmp(s, "pdf") == 0)
            out->flags |= MODEL_CAP_PDF;
         else if (strcmp(s, "audio") == 0)
            out->flags |= MODEL_CAP_AUDIO;
      }
   }

   /* A model the upstream registry still lists is not deprecated unless it says
    * so; absent key means live. */
   if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(entry, "deprecated")))
      out->deprecated = 1;

   const char *name = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(entry, "name"));
   if (name && name[0])
      snprintf(out->display_name, sizeof(out->display_name), "%s", name);
}

/* Resolve provider/model against a NESTED root. Returns 1 on hit. */
static int lookup_in_nested_json(cJSON *root, const char *provider, const char *model_id,
                                 model_capability_t *out)
{
   cJSON *prov = cJSON_GetObjectItemCaseSensitive(root, provider);
   if (!cJSON_IsObject(prov))
      return 0;
   cJSON *models = cJSON_GetObjectItemCaseSensitive(prov, "models");
   if (!cJSON_IsObject(models))
      return 0;
   cJSON *entry = cJSON_GetObjectItemCaseSensitive(models, model_id);
   if (!cJSON_IsObject(entry))
      return 0;
   fill_cap_from_nested(entry, provider, model_id, out);
   return 1;
}

static int list_from_json(cJSON *root, model_capability_t *out, int max, unsigned required_flags,
                          int open_weights_only)
{
   int written = 0;
   cJSON *item;
   cJSON_ArrayForEach(item, root)
   {
      const char *key = item->string;
      if (!key || !cJSON_IsObject(item))
         continue;
      char prov[128], mdl[256];
      const char *slash = strchr(key, '/');
      if (!slash)
      {
         /* NESTED shape: a bare provider key whose "models" object holds one
          * entry per model. lookup_in_json() has always understood this root
          * (via lookup_in_nested_json); this list walker did not, so it skipped
          * every provider key and reported ZERO for the bundled snapshot. The
          * two readers therefore disagreed about the same file: a single-model
          * lookup resolved 1M context, while every consumer that enumerates --
          * model_capability_list(), and the in-memory dynamic capability set --
          * saw an empty catalogue and silently fell through to the stale
          * hand-written table. */
         cJSON *models = cJSON_GetObjectItemCaseSensitive(item, "models");
         if (!cJSON_IsObject(models))
            continue;
         cJSON *entry;
         cJSON_ArrayForEach(entry, models)
         {
            if (!entry->string || !cJSON_IsObject(entry))
               continue;
            model_capability_t ncap;
            memset(&ncap, 0, sizeof ncap);
            fill_cap_from_nested(entry, key, entry->string, &ncap);
            if (required_flags && (ncap.flags & required_flags) != required_flags)
               continue;
            if (open_weights_only && !ncap.open_weights)
               continue;
            if (out && written < max)
               out[written] = ncap;
            written++;
         }
         continue;
      }
      size_t plen = (size_t)(slash - key);
      if (plen >= sizeof(prov))
         continue;
      memcpy(prov, key, plen);
      prov[plen] = '\0';
      snprintf(mdl, sizeof(mdl), "%s", slash + 1);
      model_capability_t cap;
      fill_cap_from_json(item, prov, mdl, &cap);
      if (required_flags && (cap.flags & required_flags) != required_flags)
         continue;
      if (open_weights_only && !cap.open_weights)
         continue;
      if (out && written < max)
         out[written] = cap;
      written++;
   }
   return written;
}

static void get_cache_path(char *buf, size_t len)
{
   const char *home = getenv("HOME");
   if (home && home[0])
      snprintf(buf, len, "%s/.cache/aimee/models_dev.json", home);
   else
      buf[0] = '\0';
}

int models_dev_override_lookup(const char *provider, const char *model_id, model_capability_t *out)
{
   if (!provider || !model_id || !out)
      return 0;
   const char *ov = models_dev_overrides_path();
   if (!ov || !ov[0])
      return 0;
   cJSON *root = load_json_from_path(ov);
   if (!root)
      return 0;
   int found = lookup_in_json(root, provider, model_id, out);
   cJSON_Delete(root);
   return found;
}

int models_dev_cache_lookup(const char *provider, const char *model_id, model_capability_t *out)
{
   if (!provider || !model_id || !out)
      return 0;

   char path[512];
   get_cache_path(path, sizeof(path));
   if (path[0])
   {
      cJSON *root = load_json_from_path(path);
      if (root)
      {
         int found = lookup_in_json(root, provider, model_id, out);
         cJSON_Delete(root);
         if (found)
            return 1;
      }
   }

   const char *snap = models_dev_snapshot_path();
   if (snap && snap[0])
   {
      cJSON *root = load_json_from_path(snap);
      if (root)
      {
         int found = lookup_in_json(root, provider, model_id, out);
         cJSON_Delete(root);
         if (found)
            return 1;
      }
   }

   return 0;
}

int models_dev_cache_list(model_capability_t *out, int max, unsigned required_flags,
                          int open_weights_only)
{
   char path[512];
   get_cache_path(path, sizeof(path));
   if (path[0])
   {
      cJSON *root = load_json_from_path(path);
      if (root)
      {
         int count = list_from_json(root, out, max, required_flags, open_weights_only);
         cJSON_Delete(root);
         return count;
      }
   }

   const char *snap = models_dev_snapshot_path();
   if (snap && snap[0])
   {
      cJSON *root = load_json_from_path(snap);
      if (root)
      {
         int count = list_from_json(root, out, max, required_flags, open_weights_only);
         cJSON_Delete(root);
         return count;
      }
   }

   return 0;
}
