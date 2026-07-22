#ifndef WORKSPACE_HANDLE_H
#define WORKSPACE_HANDLE_H 1

/* workspace_handle.h — the /v1 workspace handle manifest
 * (workspace-resource-plane proposal, §1).
 *
 * A workspace is addressed by a handle; its manifest describes which resource
 * provider backs it (`shared` today), where its root is, and the VCS state a
 * detached server would need to reconstruct it. Phase 1 exposes this read-only
 * via the `workspace.get` RPC over the existing co-located registry. */

struct cJSON;

/* Build the workspace manifest object (caller owns the returned cJSON and must
 * cJSON_Delete it). NULL string args are emitted as "". vcs_* are empty when
 * the root is not a git repo. Shape:
 *   { "root": ..., "provider": ..., "exists": bool, "is_dir": bool,
 *     "vcs": { "remote": ..., "head": ..., "branch": ... } } */
struct cJSON *workspace_manifest_json(const char *root, const char *provider, int exists,
                                      int is_dir, const char *vcs_remote, const char *vcs_head,
                                      const char *vcs_branch);

#endif /* WORKSPACE_HANDLE_H */
