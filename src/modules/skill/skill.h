#ifndef DEC_SKILL_H
#define DEC_SKILL_H 1

#include <stdint.h>
#include <stddef.h>

#define SKILL_MAX_SIZE         (100 * 1024) /* max bytes per skill file */
#define SKILL_MAX_SKILLS       64           /* max skills returned by list */
#define SKILL_NAME_MAX         64           /* max skill name length */
#define SKILL_PATH_MAX         512          /* max path length */
#define SKILL_SUPPORT_MAX_SIZE (1024 * 1024)
#define SKILL_LINT_MAX_WORDS   1200
#define SKILL_EVAL_MAX_CASES   32

typedef struct
{
   int use_count;
   int view_count;
   int patch_count;
   int pinned;
   char created_at[32];
   char last_used_at[32];
   char last_patched_at[32];
   char state[16];
   char created_by[16];
} skill_usage_t;

typedef struct
{
   int considered;
   int stale_marked;
   int archived;
   int skipped_pinned;
   int errors;
} skill_lifecycle_result_t;

typedef struct
{
   int scenarios;
   int baseline_violations;
   int baseline_compliances;
   int treatment_compliances;
   int passed;
   double compliance_delta;
   char first_failure[256];
} skill_eval_result_t;

typedef struct
{
   int scanned;
   int existing;
   int proposed;
   int skipped;
   int errors;
   char first_proposal[SKILL_NAME_MAX];
   char first_change_path[SKILL_PATH_MAX];
} skill_capability_autostub_result_t;

/* Resolve the path for a named skill.
 * Looks in: 1) <project_root>/.aimee/skills/<name>.md
 *           2) ~/.config/aimee/skills/<name>.md
 * Returns 0 and writes path to buf on success; -1 if not found. */
int skill_path(const char *project_root, const char *name, char *buf, size_t bufsz);
const char *skill_source(const char *project_root, const char *name);

/* Load a skill's content by name.
 * Returns a malloc'd string (caller must free), or NULL if not found.
 * Content is capped at SKILL_MAX_SIZE bytes. */
char *skill_load(const char *project_root, const char *name);

/* List available skill names from project and user directories.
 * Writes up to max_names names into names_out (no duplicates).
 * Returns count written. */
int skill_list(const char *project_root, char names_out[][SKILL_NAME_MAX], int max_names);
int skill_description(const char *project_root, const char *name, char *out, size_t out_len);
int skill_trigger_matches_content(const char *content, const char *tool_name, const char *subject);
int skill_trigger_matches(const char *project_root, const char *name, const char *tool_name,
                          const char *subject);

/* Read usage telemetry for a skill. Missing sidecars return zero counters
 * with state=active and created_by=user. */
int skill_usage_get(const char *project_root, const char *name, skill_usage_t *out);

/* Best-effort telemetry bumps. Return 0 when the sidecar was updated, -1
 * when the skill cannot be resolved or the sidecar cannot be written. */
int skill_record_activation(const char *project_root, const char *name);
void skill_metrics(int64_t *self_activations_total);
int skill_record_view(const char *project_root, const char *name);
int skill_name_is_valid(const char *name);
int skill_manage_create(const char *project_root, const char *name, const char *content,
                        const char *created_by, char *errbuf, size_t errbuf_len);
int skill_manage_patch(const char *project_root, const char *name, const char *old_string,
                       const char *new_string, int replace_all, const char *updated_by,
                       char *errbuf, size_t errbuf_len);
int skill_manage_edit(const char *project_root, const char *name, const char *content,
                      const char *updated_by, char *errbuf, size_t errbuf_len);
int skill_manage_archive(const char *project_root, const char *name, const char *absorbed_into,
                         char *errbuf, size_t errbuf_len);
int skill_lint_content(const char *name, const char *content, char *report, size_t report_len);
int skill_lint(const char *project_root, const char *name, char *report, size_t report_len);
int skill_eval_run(const char *project_root, const char *name, skill_eval_result_t *out,
                   char *errbuf, size_t errbuf_len);
int skill_change_eval_gate_allows(const char *payload_json, double threshold, char *reason,
                                  size_t reason_len);
int skill_manage_write_file(const char *project_root, const char *name, const char *file_path,
                            const char *content, const char *updated_by, char *errbuf,
                            size_t errbuf_len);
char *skill_support_file_load(const char *project_root, const char *name, const char *file_path,
                              char *errbuf, size_t errbuf_len);
int skill_import_content(const char *project_root, const char *content, const char *created_by,
                         char *name_out, size_t name_out_len, char *errbuf, size_t errbuf_len);
int skill_set_pinned(const char *project_root, const char *name, int pinned);
int skill_rollback_snapshot(const char *project_root, const char *snapshot_id, char *errbuf,
                            size_t errbuf_len);
int skill_lifecycle_apply(const char *project_root, int stale_after_days, int archive_after_days,
                          skill_lifecycle_result_t *out, char *errbuf, size_t errbuf_len);
int skill_capability_autostub_from_json(const char *project_root, const char *snapshot_json,
                                        skill_capability_autostub_result_t *out, char *errbuf,
                                        size_t errbuf_len);

/* Build an augmented system prompt by appending a skill block.
 * base_prompt is the existing system prompt (may be NULL).
 * skill_name is the skill to inject (looked up via skill_load).
 * Returns a malloc'd string with the combined prompt (caller must free),
 * or a copy of base_prompt if the skill is not found.
 * Returns NULL only if both base_prompt and the skill are unavailable. */
char *skill_inject(const char *project_root, const char *base_prompt, const char *skill_name);

#endif /* DEC_SKILL_H */
