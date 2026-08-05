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
#include "log.h"
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

   if (cJSON_GetObjectItemCaseSensitive(root, "aggregator"))
      aimee_log(LOG_WARN, "roundtable.preset",
                "ignoring removed 'aggregator' field in roundtable preset; synthesis is "
                "deterministic and chairman is the only optional final agent");
   snprintf(out->chairman, sizeof(out->chairman), "%s", json_str(root, "chairman", ""));
   out->chairman_enabled = json_bool(root, "chairman_enabled", 0);
   out->min_successful = (int)json_num(root, "min_successful", 2);
   out->max_cost_usd = json_num(root, "max_cost_usd", 0.0);

   out->max_rounds = (int)json_num(root, "max_rounds", 0);
   out->converge_threshold = (int)json_num(root, "converge_threshold", 0);
   out->deadline_ms = (int)json_num(root, "deadline_ms", 0);
   out->discussion = json_bool(root, "discussion", 0);
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

   cJSON_AddStringToObject(root, "chairman", p->chairman);
   cJSON_AddBoolToObject(root, "chairman_enabled", p->chairman_enabled ? 1 : 0);
   cJSON_AddNumberToObject(root, "min_successful", p->min_successful);
   cJSON_AddNumberToObject(root, "max_cost_usd", p->max_cost_usd);
   cJSON_AddNumberToObject(root, "max_rounds", p->max_rounds);
   cJSON_AddNumberToObject(root, "converge_threshold", p->converge_threshold);
   cJSON_AddNumberToObject(root, "deadline_ms", p->deadline_ms);
   cJSON_AddBoolToObject(root, "discussion", p->discussion ? 1 : 0);
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
   if (out->chairman_enabled && !out->chairman[0])
   {
      cJSON_Delete(req);
      *errmsg = "chairman_enabled requires a chairman";
      return -1;
   }

   /* CHAIRING, SYNTHESIS AND DISCUSSION ALL REQUIRE SOMEONE TO DISAGREE WITH.
    *
    * A chairman arbitrates between seats; synthesis reconciles their findings;
    * discussion is seats talking to each other. On a panel of one there is no
    * second opinion to arbitrate, reconcile or talk to -- a lone seat would be
    * discussing with itself. Each option can only add a delegate call and a way
    * to fail.
    *
    * Measured on a real one-seat completeness review: the seat returned a correct
    * blocking finding, the chair then died on "unknown persona 'chairman'", and
    * the whole run reported FAILED -- so a caller polling roundtable_status
    * discards findings that were exactly right.
    *
    * Refused at intake rather than normalised silently, so an operator asking for
    * these on one seat is told the request is meaningless instead of having it
    * quietly dropped. */
   if (out->seat_count <= 1 && (out->chairman_enabled || out->chairman[0] || out->discussion))
   {
      cJSON_Delete(req);
      *errmsg = "a roundtable of one has nobody to chair, synthesise or discuss with: "
                "drop chairman/discussion or add seats";
      return -1;
   }
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

   int n = config_ensemble_reference_count();
   if (n > RT_PRESET_MAX_SEATS)
      n = RT_PRESET_MAX_SEATS;
   for (int i = 0; i < n; i++)
   {
      snprintf(out->seats[i].model, sizeof(out->seats[i].model), "%s",
               config_ensemble_reference_models(i));
      if (i < config_ensemble_reference_persona_count())
         snprintf(out->seats[i].persona, sizeof(out->seats[i].persona), "%s",
                  config_ensemble_reference_personas(i));
   }
   out->seat_count = n;
   out->min_successful = config_ensemble_min_successful();
   out->max_cost_usd = config_ensemble_max_cost_usd();
   out->max_rounds = config_roundtable_max_rounds();
   out->converge_threshold = config_roundtable_converge_threshold();
   out->deadline_ms = config_roundtable_deadline_ms();
   snprintf(out->turns, sizeof(out->turns), "%s",
            config_roundtable_turns()[0] ? config_roundtable_turns() : "parallel");
   snprintf(out->pipeline_done_bar, sizeof(out->pipeline_done_bar), "%s",
            config_roundtable_pipeline_done_bar());
   out->pipeline_max_passes = config_roundtable_pipeline_max_passes();
   out->pipeline_max_attempts_per_pass = config_roundtable_pipeline_max_attempts_per_pass();
   out->pipeline_max_cost_usd = config_roundtable_pipeline_max_cost_usd();
   out->pipeline_max_total_cost_usd = config_roundtable_pipeline_max_total_cost_usd();
   out->pipeline_gate_ttl_h = config_roundtable_pipeline_gate_ttl_h();
   out->pipeline_parked_releases_slot = config_roundtable_pipeline_parked_releases_slot();
   out->pipeline_unknown_context_tokens = config_roundtable_pipeline_unknown_context_tokens();
}

/* Translate a preset into the config module's plain apply-struct. config never
 * learns the preset file format; this module never touches a config_t. */
static void preset_to_config_apply(const roundtable_preset_t *p, config_roundtable_preset_t *out)
{
   memset(out, 0, sizeof(*out));
   int n = p->seat_count;
   if (n > CONFIG_RT_PRESET_MAX_SEATS)
      n = CONFIG_RT_PRESET_MAX_SEATS;
   for (int i = 0; i < n; i++)
   {
      snprintf(out->models[i], sizeof(out->models[i]), "%s", p->seats[i].model);
      snprintf(out->personas[i], sizeof(out->personas[i]), "%s", p->seats[i].persona);
   }
   out->seat_count = n;
   /* A roundtable preset no longer owns the separate C compatibility route's
    * aggregator setting. Do not change that setting while applying this preset. */
   out->min_successful = p->min_successful;
   out->max_cost_usd = p->max_cost_usd;
   out->max_rounds = p->max_rounds;
   out->converge_threshold = p->converge_threshold;
   out->deadline_ms = p->deadline_ms;
   if (p->turns[0])
      snprintf(out->turns, sizeof(out->turns), "%s", p->turns);
   if (p->pipeline_done_bar[0])
      snprintf(out->pipeline_done_bar, sizeof(out->pipeline_done_bar), "%s", p->pipeline_done_bar);
   out->pipeline_max_passes = p->pipeline_max_passes;
   out->pipeline_max_attempts_per_pass = p->pipeline_max_attempts_per_pass;
   out->pipeline_max_cost_usd = p->pipeline_max_cost_usd;
   out->pipeline_max_total_cost_usd = p->pipeline_max_total_cost_usd;
   out->pipeline_gate_ttl_h = p->pipeline_gate_ttl_h;
   out->pipeline_parked_releases_slot = p->pipeline_parked_releases_slot;
   out->pipeline_unknown_context_tokens = p->pipeline_unknown_context_tokens;
   snprintf(out->name, sizeof(out->name), "%s", p->name);
}

int roundtable_preset_resolve_runtime(const char *requested, ensemble_panel_t *panel,
                                      char *resolved, size_t resolved_n, char *err, size_t err_n)
{
   if (resolved && resolved_n)
      resolved[0] = '\0';
   if (err && err_n)
      err[0] = '\0';
   if (!panel)
      return -1;

   const int explicit_request = requested && requested[0];
   const int configured_default = !explicit_request && config_roundtable_default()[0];
   const char *name = explicit_request
                          ? requested
                          : (configured_default ? config_roundtable_default() : "default");
   roundtable_preset_t p;
   if (roundtable_preset_load(name, &p) != 0)
   {
      if (explicit_request || configured_default)
      {
         if (err && err_n)
            snprintf(err, err_n, "roundtable preset '%s' does not exist", name);
         return -1;
      }
      return 0;
   }

   /* Runtime overlay only -- nothing is persisted here. */
   int seats = p.seat_count;
   if (seats > ENSEMBLE_PANEL_MAX_SEATS)
      seats = ENSEMBLE_PANEL_MAX_SEATS;
   for (int i = 0; i < seats; i++)
   {
      snprintf(panel->reference_models[i], sizeof(panel->reference_models[i]), "%s",
               p.seats[i].model);
      snprintf(panel->reference_personas[i], sizeof(panel->reference_personas[i]), "%s",
               p.seats[i].persona);
   }
   panel->reference_count = seats;
   panel->reference_persona_count = seats;
   panel->min_successful = p.min_successful;
   panel->max_cost_usd = p.max_cost_usd;
   panel->max_rounds = p.max_rounds;
   panel->converge_threshold = p.converge_threshold;
   panel->deadline_ms = p.deadline_ms;
   if (p.turns[0])
      snprintf(panel->turns, sizeof(panel->turns), "%s", p.turns);
   if (resolved && resolved_n)
      snprintf(resolved, resolved_n, "%s", p.name);
   return 1;
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
   config_roundtable_preset_t apply;
   preset_to_config_apply(&p, &apply);
   if (config_apply_roundtable_preset(&apply) != 0)
   {
      if (err && errn)
         snprintf(err, errn, "could not save configuration");
      return -1;
   }
   return 0;
}
