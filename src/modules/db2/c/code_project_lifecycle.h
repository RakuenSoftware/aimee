/* code_project_lifecycle.h: stable code-project detach/purge/gc contract. */
#ifndef AIMEE_DB2_CODE_PROJECT_LIFECYCLE_H
#define AIMEE_DB2_CODE_PROJECT_LIFECYCLE_H 1

#include <stddef.h>
#include <stdint.h>

#define CODE_PROJECT_MANIFEST_MAX_TARGETS 40

typedef struct
{
   char table[64];
   long rows;
   char fingerprint[72];
} code_project_target_t;

typedef struct
{
   char operation[16];
   char project[256];
   int64_t generation;
   char mode[16];
   char criteria[128];
   code_project_target_t targets[CODE_PROJECT_MANIFEST_MAX_TARGETS];
   int target_count;
   long total_rows;
   char manifest_hash[72];
} code_project_manifest_t;

enum
{
   CODE_PROJECT_LIFECYCLE_ERROR = -1,
   CODE_PROJECT_LIFECYCLE_NOT_FOUND = -2,
   CODE_PROJECT_LIFECYCLE_HASH_MISMATCH = -3,
   CODE_PROJECT_LIFECYCLE_AUDIT_FAILED = -4
};

/* Mark the current generation detached. Indexed rows remain recoverable. The
 * verified principal and state transition commit with one WORM audit event. */
int db2_code_project_detach(const char *project, const char *principal, int64_t *generation_out);

/* Read-only exact target manifests. GC retention is measured from last_seen /
 * detached_at. */
int db2_code_project_purge_manifest(const char *project, code_project_manifest_t *out);
int db2_code_project_gc_manifest(const char *project, int retention_days,
                                 code_project_manifest_t *out);

/* Confirmed mutations. The expected hash must equal a newly computed manifest;
 * audit and deletion commit in one transaction. */
int db2_code_project_purge_confirm(const char *project, const char *expected_hash,
                                   const char *principal, const char *reason,
                                   code_project_manifest_t *out);
int db2_code_project_gc_confirm(const char *project, int retention_days, const char *expected_hash,
                                const char *principal, const char *reason,
                                code_project_manifest_t *out);

#endif
