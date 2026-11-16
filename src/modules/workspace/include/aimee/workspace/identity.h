#ifndef AIMEE_WORKSPACE_IDENTITY_H
#define AIMEE_WORKSPACE_IDENTITY_H
#include <stddef.h>

/* Resolve a path-independent project identity for code and memory scoping.
 *
 * For a git repo this yields the canonical remote identity rather than the
 * local checkout path, so two clones of the same repo on different machines
 * resolve to the same scope (the cross-environment recall bug otherwise):
 *   project_out   = canonical repo URL  (https://host/owner/repo) or
 *                   local:<persisted-uuid> for a local-only project.
 *   workspace_out = the repo's org/namespace parent (https://host/owner) when
 *                   it has one, else the repo identity itself.
 * Either out pointer may be NULL. Returns 0 when a repo identity was resolved,
 * For non-git directories, a generated UUID is persisted under .aimee and
 * follows the directory when it moves. Returns -1 only when no durable
 * identity can be read or created. */
int workspace_repo_identity(const char *cwd, char *project_out, size_t project_len,
                            char *workspace_out, size_t workspace_len);

/* Canonical index keys for a discovered repo root: the project name and its
 * workspace, used to key the shared kb_store index (projects.name, kb_documents.project)
 * and the ingest queue. Prefers the repository identity (workspace_repo_identity)
 * so a repo indexed from any machine resolves to one project. Returns -1 and
 * leaves both outputs empty when no durable identity can be read or persisted;
 * callers must skip/refuse indexing rather than substitute a path basename. */
int workspace_repo_index_keys(const char *root, const char *fallback_workspace, char *name_out,
                              size_t name_len, char *ws_out, size_t ws_len);

#endif
