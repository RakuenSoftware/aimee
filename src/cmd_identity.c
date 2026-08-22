/* cmd_identity.c: identity inspection surfaces for the charter /
 * working-profile model.
 *
 *   aimee identity show                          (charter + working profile)
 *   aimee identity working-profile list          (committed working-profile rows)
 *   aimee identity working-profile observe \
 *       --field <name> --value <s> [--confidence 0..1]
 *   aimee identity snapshot [--out DIR]
 *   aimee identity diff A.json B.json [--flip-threshold 0.3]
 *
 * The charter is operator-authored and immutable through this command; the
 * external config module owns its document and mutation policy. The working
 * profile is mutable-but-gated: observations accumulate until a
 * sample-count threshold commits a value, so single-turn reactions
 * can't flip it. */

#include "aimee.h"
#include "commands.h"
#include "cmd_review.h"
#include "db1.h"
#include "working_profile.h"
#include "cJSON.h"
#include "platform_path.h"
#include <errno.h>
#include <sys/stat.h>
#include <time.h>

/* --- shared JSON helpers --- */

/* Takes the section's indexed accessor rather than the array: the entries are no
 * longer a block of memory the caller holds. Each entry is copied into the JSON
 * string immediately, so the accessor's thread-local buffer is consumed before
 * the next index overwrites it. */
static cJSON *charter_array_to_json(const char *(*entry)(int), int count)
{
   cJSON *arr = cJSON_CreateArray();
   if (!arr)
      return NULL;
   for (int i = 0; i < count; i++)
      cJSON_AddItemToArray(arr, cJSON_CreateString(entry(i)));
   return arr;
}

cJSON *identity_charter_json(void)
{
   cJSON *out = cJSON_CreateObject();
   if (!out)
      return NULL;
   cJSON_AddItemToObject(
       out, "safety_axioms",
       charter_array_to_json(config_charter_safety_axioms, config_charter_safety_axioms_count()));
   cJSON_AddItemToObject(out, "hard_constraints",
                         charter_array_to_json(config_charter_hard_constraints,
                                               config_charter_hard_constraints_count()));
   cJSON_AddItemToObject(
       out, "values", charter_array_to_json(config_charter_values, config_charter_values_count()));
   cJSON_AddItemToObject(out, "tone_boundaries",
                         charter_array_to_json(config_charter_tone_boundaries,
                                               config_charter_tone_boundaries_count()));
   cJSON_AddNumberToObject(out, "working_profile_drift_limit",
                           config_charter_working_profile_drift_limit());
   int total = config_charter_safety_axioms_count() + config_charter_hard_constraints_count() +
               config_charter_values_count() + config_charter_tone_boundaries_count();
   cJSON_AddNumberToObject(out, "total_entries", total);
   return out;
}

cJSON *identity_working_profile_json(void)
{
   cJSON *out = cJSON_CreateObject();
   if (!out)
      return NULL;
   cJSON *entries = cJSON_AddArrayToObject(out, "entries");
   db1_working_profile_local_state_t rows[16];
   int n = db1_working_profile_local_list(rows, 16);
   for (int i = 0; i < n; i++)
   {
      cJSON *row = cJSON_CreateObject();
      cJSON_AddStringToObject(row, "field", rows[i].field);
      cJSON_AddStringToObject(row, "value", rows[i].value);
      cJSON_AddNumberToObject(row, "sample_count", rows[i].observation_count);
      cJSON_AddNumberToObject(row, "confidence", rows[i].score);
      cJSON_AddStringToObject(row, "updated_at", rows[i].updated_at);
      cJSON_AddBoolToObject(row, "canonical_field",
                            working_profile_field_is_canonical(rows[i].field));
      cJSON_AddItemToArray(entries, row);
   }
   cJSON_AddNumberToObject(out, "entry_count", n);
   return out;
}

cJSON *identity_local_operator_json(void)
{
   cJSON *out = cJSON_CreateObject();
   if (!out)
      return NULL;

   db1_local_operator_t row;
   if (db1_local_operator_get_active(&row) != 0)
   {
      cJSON_AddNullToObject(out, "active");
      return out;
   }

   cJSON *active = cJSON_AddObjectToObject(out, "active");
   if (!active)
   {
      cJSON_Delete(out);
      return NULL;
   }
   cJSON_AddStringToObject(active, "secret_ref", row.secret_ref);
   cJSON_AddStringToObject(active, "operator_uuid", row.operator_uuid);
   cJSON_AddBoolToObject(active, "active", row.active ? 1 : 0);
   cJSON_AddStringToObject(active, "display_hint", row.display_hint);
   cJSON_AddStringToObject(active, "created_at", row.created_at);
   return out;
}

/* --- show subcommand --- */

/* Same shape as charter_array_to_json: takes the indexed accessor, not the array.
 * Each entry is printed before the next index overwrites the accessor's
 * thread-local buffer. */
static void print_section(const char *label, const char *(*entry)(int), int count)
{
   if (count <= 0)
      return;
   printf("  %s:\n", label);
   for (int i = 0; i < count; i++)
      printf("    - %s\n", entry(i));
}

static void identity_show(app_ctx_t *ctx)
{
   if (!config_present())
      fatal("identity show: could not load config");

   if (ctx->json_output)
   {
      cJSON *obj = cJSON_CreateObject();
      cJSON_AddItemToObject(obj, "charter", identity_charter_json());
      cJSON_AddItemToObject(obj, "local_operator", identity_local_operator_json());
      cJSON_AddItemToObject(obj, "working_profile", identity_working_profile_json());
      emit_json_ctx(obj, ctx->json_fields, ctx->response_profile);
      return;
   }

   int total = config_charter_safety_axioms_count() + config_charter_hard_constraints_count() +
               config_charter_values_count() + config_charter_tone_boundaries_count();
   if (total == 0)
      printf("Charter: (none configured — configure a charter through the config module)\n");
   else
   {
      printf("Charter (immutable, loaded from the config module):\n");
      print_section("Safety axioms", config_charter_safety_axioms,
                    config_charter_safety_axioms_count());
      print_section("Hard constraints", config_charter_hard_constraints,
                    config_charter_hard_constraints_count());
      print_section("Values", config_charter_values, config_charter_values_count());
      print_section("Tone boundaries", config_charter_tone_boundaries,
                    config_charter_tone_boundaries_count());
      if (config_charter_working_profile_drift_limit() > 0)
         printf("  Working-profile drift limit: %d\n",
                config_charter_working_profile_drift_limit());
   }

   printf("\nLocal operator (machine-local credential mapping):\n");
   db1_local_operator_t active_op;
   if (db1_local_operator_get_active(&active_op) != 0)
      printf("  (no active local operator)\n");
   else
   {
      printf("  Operator UUID: %s\n", active_op.operator_uuid);
      printf("  Secret ref: %s\n", active_op.secret_ref);
      if (active_op.display_hint[0])
         printf("  Display hint: %s\n", active_op.display_hint);
      if (active_op.created_at[0])
         printf("  Created at: %s\n", active_op.created_at);
   }

   printf("\nWorking profile (learned, threshold-gated updates):\n");
   db1_working_profile_local_state_t rows[16];
   int n = db1_working_profile_local_list(rows, 16);
   if (n == 0)
   {
      printf("  (no committed values yet)\n");
      return;
   }
   for (int i = 0; i < n; i++)
      printf("  %-22s  %s  (conf %.2f, samples %d)\n", rows[i].field, rows[i].value, rows[i].score,
             rows[i].observation_count);
}

/* --- working-profile subcommand --- */

static void identity_working_profile_list(app_ctx_t *ctx)
{
   db1_working_profile_local_state_t rows[32];
   int n = db1_working_profile_local_list(rows, 32);
   if (ctx->json_output)
   {
      cJSON *obj = identity_working_profile_json();
      emit_json_ctx(obj, ctx->json_fields, ctx->response_profile);
      return;
   }
   if (n == 0)
   {
      printf("No committed working-profile entries.\n");
      return;
   }
   for (int i = 0; i < n; i++)
      printf("%-22s  %s  (conf %.2f, samples %d, updated %s)\n", rows[i].field, rows[i].value,
             rows[i].score, rows[i].observation_count, rows[i].updated_at);
}

static void identity_working_profile_observe(app_ctx_t *ctx, int argc, char **argv)
{
   const char *field = NULL;
   const char *value = NULL;
   double confidence = 0.5;
   int threshold = 0;
   for (int i = 0; i < argc; i++)
   {
      if (strcmp(argv[i], "--field") == 0 && i + 1 < argc)
         field = argv[++i];
      else if (strcmp(argv[i], "--value") == 0 && i + 1 < argc)
         value = argv[++i];
      else if (strcmp(argv[i], "--confidence") == 0 && i + 1 < argc)
         confidence = atof(argv[++i]);
      else if (strcmp(argv[i], "--threshold") == 0 && i + 1 < argc)
         threshold = atoi(argv[++i]);
      else
         fatal("unknown identity working-profile observe flag: %s", argv[i]);
   }
   if (!field || !field[0] || !value || !value[0])
      fatal("identity working-profile observe: --field and --value are required");

   int rc = db1_working_profile_local_observe(field, value, confidence, session_id(), threshold);
   if (rc < 0)
      fatal("identity working-profile observe: write failed");

   if (ctx->json_output)
   {
      cJSON *obj = cJSON_CreateObject();
      cJSON_AddStringToObject(obj, "field", field);
      cJSON_AddStringToObject(obj, "value", value);
      cJSON_AddBoolToObject(obj, "committed", rc == 1 ? 1 : 0);
      emit_json_ctx(obj, ctx->json_fields, ctx->response_profile);
      return;
   }
   if (rc == 1)
      printf("observed and committed: %s = %s\n", field, value);
   else
      printf("observed: %s = %s (not yet at threshold)\n", field, value);
}

static void identity_working_profile(app_ctx_t *ctx, int argc, char **argv)
{
   if (!db1_store_ready())
      fatal("identity working-profile: DB1 store unavailable");

   if (argc < 1)
   {
      identity_working_profile_list(ctx);
      return;
   }
   const char *sub = argv[0];
   argc--;
   argv++;
   if (strcmp(sub, "list") == 0)
      identity_working_profile_list(ctx);
   else if (strcmp(sub, "observe") == 0)
      identity_working_profile_observe(ctx, argc, argv);
   else if (strcmp(sub, "review") == 0)
      cmd_review_surface("working_profile", ctx, argc, argv);
   else
      fatal("unknown identity working-profile subcommand: %s (expected list, observe, review)",
            sub);
}

/* --- snapshot subcommand --- */

/* Default location so the snapshots end up in the repo-relative
 * benchmarks tree the operational proposal expects. */
#define IDENTITY_SNAPSHOT_DEFAULT_DIR "benchmarks/identity"

cJSON *identity_snapshot_build(void)
{
   cJSON *root = cJSON_CreateObject();
   if (!root)
      return NULL;

   char ts[32];
   time_t now = time(NULL);
   struct tm tm_buf;
   gmtime_r(&now, &tm_buf);
   strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%SZ", &tm_buf);
   cJSON_AddStringToObject(root, "snapshot_at", ts);
   cJSON_AddStringToObject(root, "version", AIMEE_VERSION);
   cJSON_AddItemToObject(root, "charter", identity_charter_json());
   cJSON_AddItemToObject(root, "local_operator", identity_local_operator_json());
   cJSON_AddItemToObject(root, "working_profile", identity_working_profile_json());
   return root;
}

static void identity_snapshot(app_ctx_t *ctx, int argc, char **argv)
{
   const char *out_dir = IDENTITY_SNAPSHOT_DEFAULT_DIR;
   for (int i = 0; i < argc; i++)
   {
      if (strcmp(argv[i], "--out") == 0 && i + 1 < argc)
         out_dir = argv[++i];
      else
         fatal("unknown identity snapshot flag: %s", argv[i]);
   }
   if (platform_mkdir_p(out_dir, 0755) != 0)
      fatal("identity snapshot: could not create %s: %s", out_dir, strerror(errno));
   if (!db1_store_ready())
      fatal("identity snapshot: DB1 store unavailable");
   cJSON *snap = identity_snapshot_build();
   if (!snap)
      fatal("identity snapshot: could not build snapshot");

   char stamp[32];
   time_t now = time(NULL);
   struct tm tm_buf;
   gmtime_r(&now, &tm_buf);
   strftime(stamp, sizeof(stamp), "%Y-%m-%d", &tm_buf);

   char path[512];
   snprintf(path, sizeof(path), "%s/%s.json", out_dir, stamp);
   /* If today's file already exists, append an hour suffix so multiple
    * same-day snapshots don't silently overwrite each other. */
   struct stat st;
   if (stat(path, &st) == 0)
   {
      char stamp_hour[32];
      strftime(stamp_hour, sizeof(stamp_hour), "%Y-%m-%dT%H%M", &tm_buf);
      snprintf(path, sizeof(path), "%s/%s.json", out_dir, stamp_hour);
   }

   char *rendered = cJSON_Print(snap);
   cJSON_Delete(snap);
   if (!rendered)
      fatal("identity snapshot: could not render JSON");
   FILE *fp = fopen(path, "w");
   if (!fp)
   {
      free(rendered);
      fatal("identity snapshot: could not open %s: %s", path, strerror(errno));
   }
   fputs(rendered, fp);
   fputc('\n', fp);
   fclose(fp);
   free(rendered);

   if (ctx->json_output)
   {
      cJSON *obj = cJSON_CreateObject();
      cJSON_AddStringToObject(obj, "path", path);
      emit_json_ctx(obj, ctx->json_fields, ctx->response_profile);
   }
   else
      printf("wrote %s\n", path);
}

/* --- diff subcommand --- */

/* Load a snapshot file; caller owns the returned cJSON. */
static cJSON *identity_snapshot_load(const char *path)
{
   FILE *fp = fopen(path, "r");
   if (!fp)
      fatal("identity diff: could not open %s: %s", path, strerror(errno));
   fseek(fp, 0, SEEK_END);
   long n = ftell(fp);
   fseek(fp, 0, SEEK_SET);
   if (n <= 0 || n > 4 * 1024 * 1024)
   {
      fclose(fp);
      fatal("identity diff: %s is empty or too large", path);
   }
   char *buf = malloc((size_t)n + 1);
   if (!buf)
   {
      fclose(fp);
      fatal("identity diff: out of memory reading %s", path);
   }
   size_t rd = fread(buf, 1, (size_t)n, fp);
   fclose(fp);
   buf[rd] = '\0';
   cJSON *j = cJSON_Parse(buf);
   free(buf);
   if (!j)
      fatal("identity diff: %s is not valid JSON", path);
   return j;
}

/* Index working-profile entries by field for quick lookup. Returns a
 * cJSON object mapping field -> entry object (by reference, not copy —
 * caller must keep the parent tree alive). */
static cJSON *working_profile_entries_by_field(cJSON *snapshot)
{
   cJSON *idx = cJSON_CreateObject();
   if (!idx)
      return NULL;
   cJSON *wp = cJSON_GetObjectItemCaseSensitive(snapshot, "working_profile");
   if (!cJSON_IsObject(wp))
      return idx;
   cJSON *entries = cJSON_GetObjectItemCaseSensitive(wp, "entries");
   if (!cJSON_IsArray(entries))
      return idx;
   cJSON *entry;
   cJSON_ArrayForEach(entry, entries)
   {
      cJSON *f = cJSON_GetObjectItemCaseSensitive(entry, "field");
      if (!cJSON_IsString(f) || !f->valuestring[0])
         continue;
      /* ItemReferenceToObject so we don't double-free when the parent
       * snapshot tree is deleted. */
      cJSON_AddItemReferenceToObject(idx, f->valuestring, entry);
   }
   return idx;
}

static const char *obj_str(const cJSON *obj, const char *key)
{
   const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
   return cJSON_IsString(v) ? v->valuestring : "";
}

static double obj_num(const cJSON *obj, const char *key)
{
   const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
   return cJSON_IsNumber(v) ? v->valuedouble : 0.0;
}

/* Pure diff-report builder; no I/O. Caller owns the returned cJSON.
 * Exposed so tests can exercise the classification logic directly. */
cJSON *identity_diff_report(cJSON *snap_a, cJSON *snap_b, double flip_threshold)
{
   cJSON *idx_a = working_profile_entries_by_field(snap_a);
   cJSON *idx_b = working_profile_entries_by_field(snap_b);

   cJSON *report = cJSON_CreateObject();
   cJSON *added = cJSON_AddArrayToObject(report, "added");
   cJSON *removed = cJSON_AddArrayToObject(report, "removed");
   cJSON *changed = cJSON_AddArrayToObject(report, "changed");
   cJSON *flips = cJSON_AddArrayToObject(report, "high_confidence_flips");

   /* Added + changed: walk B, look up in A. */
   cJSON *b_entry;
   cJSON_ArrayForEach(b_entry, idx_b)
   {
      const char *field = b_entry->string;
      cJSON *a_entry = cJSON_GetObjectItemCaseSensitive(idx_a, field);
      if (!a_entry)
      {
         cJSON *row = cJSON_CreateObject();
         cJSON_AddStringToObject(row, "field", field);
         cJSON_AddStringToObject(row, "value", obj_str(b_entry, "value"));
         cJSON_AddNumberToObject(row, "confidence", obj_num(b_entry, "confidence"));
         cJSON_AddItemToArray(added, row);
         continue;
      }
      const char *va = obj_str(a_entry, "value");
      const char *vb = obj_str(b_entry, "value");
      double ca = obj_num(a_entry, "confidence");
      double cb = obj_num(b_entry, "confidence");
      int value_changed = strcmp(va, vb) != 0;
      double conf_delta = cb - ca;
      if (value_changed || conf_delta != 0.0)
      {
         cJSON *row = cJSON_CreateObject();
         cJSON_AddStringToObject(row, "field", field);
         cJSON_AddStringToObject(row, "from_value", va);
         cJSON_AddStringToObject(row, "to_value", vb);
         cJSON_AddNumberToObject(row, "from_confidence", ca);
         cJSON_AddNumberToObject(row, "to_confidence", cb);
         cJSON_AddNumberToObject(row, "confidence_delta", conf_delta);
         cJSON_AddItemToArray(changed, row);
      }
      /* Flip check: value changed AND the new confidence is above
       * threshold, OR confidence swung more than threshold in
       * magnitude. Either is the "spurious high-confidence flip"
       * pattern the proposal warns about. */
      int flipped = (value_changed && cb > flip_threshold) ||
                    (conf_delta > flip_threshold || conf_delta < -flip_threshold);
      if (flipped)
      {
         cJSON *row = cJSON_CreateObject();
         cJSON_AddStringToObject(row, "field", field);
         cJSON_AddStringToObject(row, "from_value", va);
         cJSON_AddStringToObject(row, "to_value", vb);
         cJSON_AddNumberToObject(row, "from_confidence", ca);
         cJSON_AddNumberToObject(row, "to_confidence", cb);
         cJSON_AddNumberToObject(row, "confidence_delta", conf_delta);
         cJSON_AddItemToArray(flips, row);
      }
   }
   /* Removed: walk A, mark any field absent in B. */
   cJSON *a_entry;
   cJSON_ArrayForEach(a_entry, idx_a)
   {
      const char *field = a_entry->string;
      if (!cJSON_GetObjectItemCaseSensitive(idx_b, field))
      {
         cJSON *row = cJSON_CreateObject();
         cJSON_AddStringToObject(row, "field", field);
         cJSON_AddStringToObject(row, "value", obj_str(a_entry, "value"));
         cJSON_AddNumberToObject(row, "confidence", obj_num(a_entry, "confidence"));
         cJSON_AddItemToArray(removed, row);
      }
   }

   cJSON_AddNumberToObject(report, "flip_threshold", flip_threshold);

   cJSON_Delete(idx_a);
   cJSON_Delete(idx_b);
   return report;
}

static void identity_diff(app_ctx_t *ctx, int argc, char **argv)
{
   const char *path_a = NULL;
   const char *path_b = NULL;
   double flip_threshold = 0.3;
   for (int i = 0; i < argc; i++)
   {
      if (strcmp(argv[i], "--flip-threshold") == 0 && i + 1 < argc)
         flip_threshold = atof(argv[++i]);
      else if (argv[i][0] == '-')
         fatal("unknown identity diff flag: %s", argv[i]);
      else if (!path_a)
         path_a = argv[i];
      else if (!path_b)
         path_b = argv[i];
      else
         fatal("identity diff: too many positional arguments");
   }
   if (!path_a || !path_b)
      fatal("identity diff: need two snapshot paths "
            "(usage: aimee identity diff A.json B.json [--flip-threshold 0.3])");

   cJSON *snap_a = identity_snapshot_load(path_a);
   cJSON *snap_b = identity_snapshot_load(path_b);
   cJSON *report = identity_diff_report(snap_a, snap_b, flip_threshold);

   cJSON_AddStringToObject(report, "a", path_a);
   cJSON_AddStringToObject(report, "b", path_b);

   cJSON *flips = cJSON_GetObjectItemCaseSensitive(report, "high_confidence_flips");
   int flip_count = cJSON_IsArray(flips) ? cJSON_GetArraySize(flips) : 0;

   if (ctx->json_output)
   {
      emit_json_ctx(report, ctx->json_fields, ctx->response_profile);
   }
   else
   {
      cJSON *added = cJSON_GetObjectItemCaseSensitive(report, "added");
      cJSON *removed = cJSON_GetObjectItemCaseSensitive(report, "removed");
      cJSON *changed = cJSON_GetObjectItemCaseSensitive(report, "changed");
      printf("identity diff %s → %s (flip threshold %.2f)\n", path_a, path_b, flip_threshold);
      printf("  added:   %d  removed: %d  changed: %d  flips: %d\n", cJSON_GetArraySize(added),
             cJSON_GetArraySize(removed), cJSON_GetArraySize(changed), flip_count);
      cJSON *row;
      cJSON_ArrayForEach(row, changed)
      {
         printf("    %s  %s → %s  (%.2f → %.2f, Δ %+.2f)\n", obj_str(row, "field"),
                obj_str(row, "from_value"), obj_str(row, "to_value"),
                obj_num(row, "from_confidence"), obj_num(row, "to_confidence"),
                obj_num(row, "confidence_delta"));
      }
      if (flip_count > 0)
      {
         printf("  ⚠ spurious-flip candidates:\n");
         cJSON_ArrayForEach(row, flips)
         {
            printf("    %s  %s → %s  Δ %+.2f\n", obj_str(row, "field"), obj_str(row, "from_value"),
                   obj_str(row, "to_value"), obj_num(row, "confidence_delta"));
         }
      }
      cJSON_Delete(report);
   }

   cJSON_Delete(snap_a);
   cJSON_Delete(snap_b);

   /* Exit code mirrors flip count so CI can gate on it. */
   if (flip_count > 0)
      exit(1);
}

/* --- dispatch --- */

static void identity_show_cmd(app_ctx_t *ctx, int argc, char **argv)
{
   if (argc > 0)
      fatal("identity show takes no arguments");
   identity_show(ctx);
}

static const subcmd_t cmd_identity_subs[] = {
    {"show", "show the current identity", identity_show_cmd},
    {"working-profile", "manage the working profile", identity_working_profile},
    {"snapshot", "capture an identity snapshot", identity_snapshot},
    {"diff", "diff identity snapshots", identity_diff},
    {NULL, NULL, NULL},
};

void cmd_identity(app_ctx_t *ctx, int argc, char **argv)
{
   if (argc < 1)
      fatal("identity requires a subcommand: show, working-profile, snapshot, diff");
   const char *sub = argv[0];
   argc--;
   argv++;
   if (subcmd_dispatch(cmd_identity_subs, sub, ctx, argc, argv) != 0)
      fatal("unknown identity subcommand: %s", sub);
}
