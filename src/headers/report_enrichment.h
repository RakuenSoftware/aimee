#ifndef DEC_REPORT_ENRICHMENT_H
#define DEC_REPORT_ENRICHMENT_H 1

#include <stddef.h>

struct cJSON;

#define REPORT_SUBJECT_TYPE_GIT_REPO  "git_repo"
#define REPORT_SUBJECT_TYPE_GIT_ORG   "git_org"
#define REPORT_SUBJECT_TYPE_AGGREGATE "aggregate"

typedef struct report_subject
{
   char type[32];
   char id[512];
   char display[256];
} report_subject_t;

/* Resolve a git remote URL into the canonical report-enrichment subject
 * for a repository. Returns 0 on success, -1 on invalid input. */
int report_subject_from_git_remote(const char *remote_url, report_subject_t *out);

/* Resolve a local project root to a git_repo subject. The resolver reads
 * `origin` when available; local-only repositories fall back to a
 * local:<repo-root> subject. project_root may be NULL to use ".". */
int report_subject_from_project_root(const char *project_root, report_subject_t *out);

/* Resolve an explicit provider organization / namespace URL into a
 * report-enrichment subject. This does not infer org boundaries from
 * nested repo URLs; callers should pass the org/namespace URL they want
 * reports to target. Returns 0 on success, -1 on invalid input. */
int report_subject_from_git_org_url(const char *org_url, report_subject_t *out);

/* Build a local-only repository subject. The stable_id is caller-owned
 * and should be durable for the local repo. It may be passed with or
 * without the "local:" prefix. Local subjects are not org-promotable. */
int report_subject_from_local_repo_id(const char *stable_id, report_subject_t *out);

/* Returns 1 when the subject is local-only and therefore has no provider
 * organization parent, 0 otherwise. */
int report_subject_is_local(const report_subject_t *subject);

/* Add {"subject": {"type", "id", "display"}} to obj, replacing any
 * existing subject field. Returns 0 on success, -1 on invalid input. */
int report_subject_add_json(struct cJSON *obj, const report_subject_t *subject);

/* Add an aggregate subject with child subjects to obj. Aggregate subjects
 * are report-local wrappers; children are the durable enrichment targets. */
int report_subject_add_aggregate_json(struct cJSON *obj, const char *aggregate_id,
                                      const report_subject_t *children, int child_count);

#endif /* DEC_REPORT_ENRICHMENT_H */
