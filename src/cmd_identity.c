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
#include "db1_client/db1.h"
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

/* --- working-profile subcommand --- */

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

/* --- diff subcommand --- */

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

/* --- dispatch --- */
