/* workspace_manifest.h — declarative workspace bootstrap manifest (aimee.workspace.yaml).
 *
 * The manifest describes repositories to clone, language-specific dependency
 * install commands, and credential requirements that must be satisfied before
 * the workspace can be used.  Both `workspace add --repo` and `quickstart`
 * consume it when it is present at the workspace root.
 */
#ifndef DEC_WORKSPACE_MANIFEST_H
#define DEC_WORKSPACE_MANIFEST_H 1

#include "aimee.h"

#define WORKSPACE_MANIFEST_FILENAME "aimee.workspace.yaml"

#define MANIFEST_MAX_REPOS    32
#define MANIFEST_MAX_DEP_CMDS 16
#define MANIFEST_MAX_SECRETS  16

/* A single repository entry declared in the manifest. */
typedef struct
{
   char url[512];
   char path[MAX_PATH_LEN]; /* relative clone destination; empty = derive from URL */
} manifest_repo_t;

/* A resolved dependency install command (one per language / package manager). */
typedef struct
{
   char cmd[512]; /* e.g. "apt-get install -y libssl-dev cmake" */
} manifest_dep_cmd_t;

/* A credential requirement the user must satisfy. */
typedef struct
{
   char name[128];
   char description[512];
} manifest_secret_t;

typedef struct
{
   manifest_repo_t repos[MANIFEST_MAX_REPOS];
   int repo_count;

   manifest_dep_cmd_t dep_cmds[MANIFEST_MAX_DEP_CMDS];
   int dep_cmd_count;

   manifest_secret_t secrets[MANIFEST_MAX_SECRETS];
   int secret_count;

   /* quickstart knobs (both default to 1 when absent) */
   int index;          /* 1 = index discovered projects */
   int generate_rules; /* 1 = write .aimee-rules from detected stacks */
} workspace_manifest_t;

/* Load and parse aimee.workspace.yaml from dir_or_path.
 * If dir_or_path is a directory, WORKSPACE_MANIFEST_FILENAME is appended.
 * Returns  0 on success,
 *         -1 if the file does not exist,
 *         -2 on YAML/parse error. */
int workspace_manifest_load(const char *dir_or_path, workspace_manifest_t *out);

#endif /* DEC_WORKSPACE_MANIFEST_H */
