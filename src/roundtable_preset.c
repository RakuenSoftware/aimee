/* roundtable_preset.c: named roundtable preset registry (see roundtable_preset.h).
 *
 * Server-side only. Presets are single JSON files under
 * <config_default_dir()>/roundtables/<name>.json. This mirrors the persona
 * registry (persona.c) for path/list/validate idioms, but stores structured JSON
 * (a preset is pure config, not prose). Selecting a preset "active" overlays it
 * onto the live config_t via config_save/config_reload; the roundtable runtime
 * (delegate_ensemble.c) is untouched and keeps reading config_t. */
#include "roundtable_preset.h"
#include "config.h" /* config_default_dir, config_t, config_load_file, config_save, config_reload */
#include "platform_path.h" /* platform_mkdir_p */
#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* A preset JSON file is small config; cap the read defensively. */
#define RT_PRESET_FILE_MAX_SIZE (256 * 1024)

static char *rt_read_file(const char *path)
{
   FILE *f = fopen(path, "rb");
   if (!f)
      return NULL;
   if (fseek(f, 0, SEEK_END) != 0)
   {
      fclose(f);
      return NULL;
   }
   long len = ftell(f);
   if (len < 0 || len > RT_PRESET_FILE_MAX_SIZE)
   {
      fclose(f);
      return NULL;
   }
   rewind(f);
   char *buf = malloc((size_t)len + 1);
   if (!buf)
   {
      fclose(f);
      return NULL;
   }
   size_t n = fread(buf, 1, (size_t)len, f);
   fclose(f);
   buf[n] = '\0';
   return buf;
}

/* --- name validation (same rules as persona_name_valid) ------------------ */

int roundtable_preset_name_valid(const char *name)
{
   if (!name || !name[0])
      return 0;
   size_t n = strlen(name);
   if (n >= RT_PRESET_NAME_MAX)
      return 0;
   for (size_t i = 0; i < n; i++)
   {
      unsigned char c = (unsigned char)name[i];
      if (!(isalnum(c) || c == '.' || c == '_' || c == '-'))
         return 0;
   }
   if (name[0] == '.') /* reject "." / ".." and dotfiles */
      return 0;
   return 1;
}

/* --- path resolution ----------------------------------------------------- */

static void preset_dir(char *buf, size_t bufsz)
{
   snprintf(buf, bufsz, "%s/roundtables", config_default_dir());
}

static void preset_path(const char *name, char *buf, size_t bufsz)
{
   snprintf(buf, bufsz, "%s/roundtables/%s.json", config_default_dir(), name);
}

/* --- list ---------------------------------------------------------------- */

int roundtable_preset_list(char names_out[][RT_PRESET_NAME_MAX], int max_names)
{
   if (!names_out || max_names <= 0)
      return 0;
   int count = 0;
   char dir[RT_PRESET_PATH_MAX];
   preset_dir(dir, sizeof(dir));
   DIR *d = opendir(dir);
   if (!d)
      return 0;
   struct dirent *e;
   while ((e = readdir(d)) != NULL && count < max_names)
   {
      size_t n = strlen(e->d_name);
      if (n <= 5 || strcmp(e->d_name + n - 5, ".json") != 0)
         continue;
      char base[RT_PRESET_NAME_MAX];
      size_t bn = n - 5;
      if (bn >= sizeof(base))
         continue;
      memcpy(base, e->d_name, bn);
      base[bn] = '\0';
      int dup = 0;
      for (int i = 0; i < count; i++)
         if (strcmp(names_out[i], base) == 0)
         {
            dup = 1;
            break;
         }
      if (!dup)
         snprintf(names_out[count++], RT_PRESET_NAME_MAX, "%s", base);
   }
   closedir(d);
   return count;
}

/* --- (de)serialization --------------------------------------------------- */

static const char *json_str(const cJSON *o, const char *key, const char *dflt)
{
   const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
   return (cJSON_IsString(v) && v->valuestring) ? v->valuestring : dflt;
}

static double json_num(const cJSON *o, const char *key, double dflt)
{
   const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
   return cJSON_IsNumber(v) ? v->valuedouble : dflt;
}

static int json_bool(const cJSON *o, const char *key, int dflt)
{
   const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
   if (cJSON_IsBool(v))
      return cJSON_IsTrue(v) ? 1 : 0;
   if (cJSON_IsNumber(v))
      return v->valuedouble != 0.0 ? 1 : 0;
   return dflt;
}

/* Parse the shared body fields (everything except name) from a JSON object into
 * *out, which the caller has already zeroed and named. */
static void preset_fill_from_object(const cJSON *root, roundtable_preset_t *out)
{
   snprintf(out->description, sizeof(out->description), "%s", json_str(root, "description", ""));

   const cJSON *seats = cJSON_GetObjectItemCaseSensitive(root, "seats");
   if (cJSON_IsArray(seats))
   {
      const cJSON *s = NULL;
      cJSON_ArrayForEach(s, seats)
      {
         if (out->seat_count >= RT_PRESET_MAX_SEATS)
            break;
         if (!cJSON_IsObject(s))
            continue;
         const char *model = json_str(s, "model", "");
         if (!model[0])
            continue; /* a seat with no model is meaningless — skip it */
         rt_preset_seat_t *seat = &out->seats[out->seat_count++];
         snprintf(seat->model, sizeof(seat->model), "%s", model);
         snprintf(seat->persona, sizeof(seat->persona), "%s", json_str(s, "persona", ""));
      }
   }

   snprintf(out->aggregator, sizeof(out->aggregator), "%s", json_str(root, "aggregator", ""));
   out->min_successful = (int)json_num(root, "min_successful", 2);
   out->max_cost_usd = json_num(root, "max_cost_usd", 0.0);

   out->max_rounds = (int)json_num(root, "max_rounds", 0);
   out->converge_threshold = (int)json_num(root, "converge_threshold", 0);
   out->deadline_ms = (int)json_num(root, "deadline_ms", 0);
   snprintf(out->turns, sizeof(out->turns), "%s", json_str(root, "turns", "parallel"));

   const cJSON *pl = cJSON_GetObjectItemCaseSensitive(root, "pipeline");
   if (cJSON_IsObject(pl))
   {
      snprintf(out->pipeline_done_bar, sizeof(out->pipeline_done_bar), "%s",
               json_str(pl, "done_bar", ""));
      out->pipeline_max_passes = (int)json_num(pl, "max_passes", 0);
      out->pipeline_max_attempts_per_pass = (int)json_num(pl, "max_attempts_per_pass", 2);
      out->pipeline_max_cost_usd = json_num(pl, "max_cost_usd", 0.0);
      out->pipeline_max_total_cost_usd = json_num(pl, "max_total_cost_usd", 0.0);
      out->pipeline_gate_ttl_h = (int)json_num(pl, "gate_ttl_h", 0);
      out->pipeline_parked_releases_slot = json_bool(pl, "parked_releases_slot", 1);
      out->pipeline_unknown_context_tokens = (int)json_num(pl, "unknown_context_tokens", 0);
   }
   else
   {
      out->pipeline_max_attempts_per_pass = 2;
      out->pipeline_parked_releases_slot = 1;
   }
}

cJSON *roundtable_preset_to_json(const roundtable_preset_t *p)
{
   cJSON *root = cJSON_CreateObject();
   if (!root || !p)
      return root;
   cJSON_AddStringToObject(root, "name", p->name);
   cJSON_AddStringToObject(root, "description", p->description);

   cJSON *seats = cJSON_AddArrayToObject(root, "seats");
   for (int i = 0; seats && i < p->seat_count; i++)
   {
      cJSON *s = cJSON_CreateObject();
      cJSON_AddStringToObject(s, "model", p->seats[i].model);
      cJSON_AddStringToObject(s, "persona", p->seats[i].persona);
      cJSON_AddItemToArray(seats, s);
   }

   cJSON_AddStringToObject(root, "aggregator", p->aggregator);
   cJSON_AddNumberToObject(root, "min_successful", p->min_successful);
   cJSON_AddNumberToObject(root, "max_cost_usd", p->max_cost_usd);
   cJSON_AddNumberToObject(root, "max_rounds", p->max_rounds);
   cJSON_AddNumberToObject(root, "converge_threshold", p->converge_threshold);
   cJSON_AddNumberToObject(root, "deadline_ms", p->deadline_ms);
   cJSON_AddStringToObject(root, "turns", p->turns);

   cJSON *pl = cJSON_AddObjectToObject(root, "pipeline");
   if (pl)
   {
      cJSON_AddStringToObject(pl, "done_bar", p->pipeline_done_bar);
      cJSON_AddNumberToObject(pl, "max_passes", p->pipeline_max_passes);
      cJSON_AddNumberToObject(pl, "max_attempts_per_pass", p->pipeline_max_attempts_per_pass);
      cJSON_AddNumberToObject(pl, "max_cost_usd", p->pipeline_max_cost_usd);
      cJSON_AddNumberToObject(pl, "max_total_cost_usd", p->pipeline_max_total_cost_usd);
      cJSON_AddNumberToObject(pl, "gate_ttl_h", p->pipeline_gate_ttl_h);
      cJSON_AddBoolToObject(pl, "parked_releases_slot", p->pipeline_parked_releases_slot ? 1 : 0);
      cJSON_AddNumberToObject(pl, "unknown_context_tokens", p->pipeline_unknown_context_tokens);
   }
   return root;
}

int roundtable_preset_from_json(const char *body, const char *url_name, roundtable_preset_t *out,
                                const char **errmsg)
{
   const char *dummy = NULL;
   if (!errmsg)
      errmsg = &dummy;
   memset(out, 0, sizeof(*out));
   cJSON *req = body ? cJSON_Parse(body) : NULL;
   if (!req)
   {
      *errmsg = "invalid JSON body";
      return -1;
   }
   const char *name = (url_name && url_name[0]) ? url_name : json_str(req, "name", NULL);
   if (!name || !roundtable_preset_name_valid(name))
   {
      cJSON_Delete(req);
      *errmsg = "missing or invalid preset name";
      return -1;
   }
   snprintf(out->name, sizeof(out->name), "%s", name);
   preset_fill_from_object(req, out);
   cJSON_Delete(req);
   return 0;
}

/* --- load / save / delete ------------------------------------------------ */

int roundtable_preset_load(const char *name, roundtable_preset_t *out)
{
   if (!name || !out || !roundtable_preset_name_valid(name))
      return -1;
   char path[RT_PRESET_PATH_MAX];
   preset_path(name, path, sizeof(path));
   char *raw = rt_read_file(path);
   if (!raw)
      return -1;
   cJSON *root = cJSON_Parse(raw);
   free(raw);
   if (!root)
      return -1;
   memset(out, 0, sizeof(*out));
   snprintf(out->name, sizeof(out->name), "%s", name);
   preset_fill_from_object(root, out);
   cJSON_Delete(root);
   return 0;
}

int roundtable_preset_save(const roundtable_preset_t *p)
{
   if (!p || !roundtable_preset_name_valid(p->name))
      return -1;
   char dir[RT_PRESET_PATH_MAX];
   preset_dir(dir, sizeof(dir));
   struct stat st;
   if (stat(dir, &st) != 0)
      platform_mkdir_p(dir, 0755);
   char path[RT_PRESET_PATH_MAX];
   preset_path(p->name, path, sizeof(path));
   cJSON *json = roundtable_preset_to_json(p);
   char *text = json ? cJSON_Print(json) : NULL;
   cJSON_Delete(json);
   if (!text)
      return -1;
   FILE *f = fopen(path, "w");
   if (!f)
   {
      free(text);
      return -1;
   }
   int rc = (fputs(text, f) >= 0 && fputc('\n', f) != EOF) ? 0 : -1;
   free(text);
   if (fclose(f) != 0)
      rc = -1;
   return rc;
}

int roundtable_preset_delete(const char *name)
{
   if (!roundtable_preset_name_valid(name))
      return -1;
   char path[RT_PRESET_PATH_MAX];
   preset_path(name, path, sizeof(path));
   struct stat st;
   if (stat(path, &st) != 0 || !S_ISREG(st.st_mode))
      return -1;
   return unlink(path) == 0 ? 0 : -1;
}

/* --- current-config synthesis + apply ------------------------------------ */

void roundtable_preset_from_current_config(const char *name, roundtable_preset_t *out)
{
   memset(out, 0, sizeof(*out));
   snprintf(out->name, sizeof(out->name), "%s", name ? name : "current");
   config_t cfg;
   if (config_load(&cfg) != 0)
      return;
   int n = cfg.ensemble_reference_count;
   if (n > RT_PRESET_MAX_SEATS)
      n = RT_PRESET_MAX_SEATS;
   for (int i = 0; i < n; i++)
   {
      snprintf(out->seats[i].model, sizeof(out->seats[i].model), "%s",
               cfg.ensemble_reference_models[i]);
      if (i < cfg.ensemble_reference_persona_count)
         snprintf(out->seats[i].persona, sizeof(out->seats[i].persona), "%s",
                  cfg.ensemble_reference_personas[i]);
   }
   out->seat_count = n;
   snprintf(out->aggregator, sizeof(out->aggregator), "%s", cfg.ensemble_aggregator);
   out->min_successful = cfg.ensemble_min_successful;
   out->max_cost_usd = cfg.ensemble_max_cost_usd;
   out->max_rounds = cfg.roundtable_max_rounds;
   out->converge_threshold = cfg.roundtable_converge_threshold;
   out->deadline_ms = cfg.roundtable_deadline_ms;
   snprintf(out->turns, sizeof(out->turns), "%s",
            cfg.roundtable_turns[0] ? cfg.roundtable_turns : "parallel");
   snprintf(out->pipeline_done_bar, sizeof(out->pipeline_done_bar), "%s",
            cfg.roundtable_pipeline_done_bar);
   out->pipeline_max_passes = cfg.roundtable_pipeline_max_passes;
   out->pipeline_max_attempts_per_pass = cfg.roundtable_pipeline_max_attempts_per_pass;
   out->pipeline_max_cost_usd = cfg.roundtable_pipeline_max_cost_usd;
   out->pipeline_max_total_cost_usd = cfg.roundtable_pipeline_max_total_cost_usd;
   out->pipeline_gate_ttl_h = cfg.roundtable_pipeline_gate_ttl_h;
   out->pipeline_parked_releases_slot = cfg.roundtable_pipeline_parked_releases_slot;
   out->pipeline_unknown_context_tokens = cfg.roundtable_pipeline_unknown_context_tokens;
}

/* Overlay a preset's fields onto a config_t (in memory). */
static void preset_overlay_config(const roundtable_preset_t *p, config_t *cfg)
{
   int n = p->seat_count;
   if (n > RT_PRESET_MAX_SEATS)
      n = RT_PRESET_MAX_SEATS;
   for (int i = 0; i < n; i++)
   {
      snprintf(cfg->ensemble_reference_models[i], sizeof(cfg->ensemble_reference_models[i]), "%s",
               p->seats[i].model);
      snprintf(cfg->ensemble_reference_personas[i], sizeof(cfg->ensemble_reference_personas[i]),
               "%s", p->seats[i].persona);
   }
   cfg->ensemble_reference_count = n;
   cfg->ensemble_reference_persona_count = n;
   snprintf(cfg->ensemble_aggregator, sizeof(cfg->ensemble_aggregator), "%s", p->aggregator);
   cfg->ensemble_min_successful = p->min_successful;
   cfg->ensemble_max_cost_usd = p->max_cost_usd;
   cfg->roundtable_max_rounds = p->max_rounds;
   cfg->roundtable_converge_threshold = p->converge_threshold;
   cfg->roundtable_deadline_ms = p->deadline_ms;
   if (p->turns[0])
      snprintf(cfg->roundtable_turns, sizeof(cfg->roundtable_turns), "%s", p->turns);
   if (p->pipeline_done_bar[0])
      snprintf(cfg->roundtable_pipeline_done_bar, sizeof(cfg->roundtable_pipeline_done_bar), "%s",
               p->pipeline_done_bar);
   cfg->roundtable_pipeline_max_passes = p->pipeline_max_passes;
   cfg->roundtable_pipeline_max_attempts_per_pass = p->pipeline_max_attempts_per_pass;
   cfg->roundtable_pipeline_max_cost_usd = p->pipeline_max_cost_usd;
   cfg->roundtable_pipeline_max_total_cost_usd = p->pipeline_max_total_cost_usd;
   cfg->roundtable_pipeline_gate_ttl_h = p->pipeline_gate_ttl_h;
   cfg->roundtable_pipeline_parked_releases_slot = p->pipeline_parked_releases_slot;
   cfg->roundtable_pipeline_unknown_context_tokens = p->pipeline_unknown_context_tokens;
   snprintf(cfg->roundtable_default, sizeof(cfg->roundtable_default), "%s", p->name);
}

int roundtable_preset_apply_to_config(const char *name, char *err, size_t errn)
{
   roundtable_preset_t p;
   if (roundtable_preset_load(name, &p) != 0)
   {
      if (err && errn)
         snprintf(err, errn, "no such roundtable preset");
      return -1;
   }
   /* Read from DISK for the read-modify-save so we never clobber an external edit
    * made since the last reload (matches handle_config_set). */
   config_t cfg;
   if (config_load_file(&cfg) != 0)
   {
      if (err && errn)
         snprintf(err, errn, "could not load configuration");
      return -1;
   }
   preset_overlay_config(&p, &cfg);
   if (config_save(&cfg) != 0)
   {
      if (err && errn)
         snprintf(err, errn, "could not save configuration");
      return -1;
   }
   (void)config_reload();
   return 0;
}
