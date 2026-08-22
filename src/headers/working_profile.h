#ifndef DEC_WORKING_PROFILE_H
#define DEC_WORKING_PROFILE_H 1

/* Working profile: learned, revisable model of how the user wants to
 * collaborate. Backed by DB1 `working_profile_*_local`, and paired
 * with the operator-authored charter in `legacy_config_record`. */

#define WORKING_PROFILE_FIELD_LEN 64
#define WORKING_PROFILE_VALUE_LEN 256

/* Canonical working-profile fields. These are not enforced by the
 * schema so future proposals can add categories without a migration,
 * but the helpers here use the canonical list when introspecting. */
#define WORKING_PROFILE_FIELD_COMMUNICATION_STYLE "communication_style"
#define WORKING_PROFILE_FIELD_VERBOSITY           "verbosity"
#define WORKING_PROFILE_FIELD_WORKING_HABITS      "working_habits"
#define WORKING_PROFILE_FIELD_TRUST_CALIBRATION   "trust_calibration"
#define WORKING_PROFILE_FIELD_RELATIONSHIP_NOTES  "relationship_notes"
#define WORKING_PROFILE_FIELD_PROJECT_ROLE        "project_role"

/* Default threshold for committing an observation to the local working
 * profile state: N matching observations for (field, value) before the
 * modal value becomes the committed one. Exposed for tests. */
#define WORKING_PROFILE_DEFAULT_COMMIT_THRESHOLD 3

/* Return 1 if `field` is one of the canonical six names, 0 otherwise. */
int working_profile_field_is_canonical(const char *field);

/* Scan free-form feedback text for well-known phrases and emit
 * working-profile observations when they match. Confidence is capped at
 * 0.7 so a single noisy turn can't unseat a strong existing commit
 * via working_profile_observe's average-confidence gate. Returns the
 * number of observations recorded (0 when nothing matched). Safe to
 * call on arbitrary user input. Public for tests + direct operator
 * use; the live-turn hook is a separate, intentionally deferred
 * integration. */
int working_profile_autoobserve_from_feedback(const char *text);

#endif /* DEC_WORKING_PROFILE_H */
