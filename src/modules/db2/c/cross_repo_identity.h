#ifndef AIMEE_CROSS_REPO_IDENTITY_H
#define AIMEE_CROSS_REPO_IDENTITY_H

#include <stddef.h>

/* H0c: repo-identity layer (precision-hardening §1.5). Each repo PROVIDES a set
 * of identities — the package / module / build-target names a cross-repo
 * import/link directive resolves against. Built at index time from the repo's
 * indexed manifest files into the cross_repo_identity table; the structural-edge
 * resolver (H1) joins a directive's package/module name against it. */

typedef struct
{
   char kind[24];   /* crate|gomod|npm|pypi|cmake_project|cmake_target|pkgconfig */
   char value[256]; /* the provided identity name */
} xrepo_identity_t;

/* Pure, DB-free: extract the identities a single manifest file declares. `basename`
 * selects the format: go.mod / Cargo.toml / package.json / pyproject.toml (one id
 * each, via xrepo_parse_module_id); CMakeLists.txt (project() + add_library/
 * add_executable targets, possibly several); a *.pc file (the pkg-config name =
 * the file basename minus ".pc"). Writes up to `max` identities into out and
 * returns the count; *overflow (optional) is set to 1 if `max` was hit (the
 * manifest declares more identities than were captured). Unit-tested directly. */
int xrepo_extract_identities(const char *basename, const char *content, xrepo_identity_t *out,
                             int max, int *overflow);

/* Rebuild cross_repo_identity for all registered repos from their indexed manifest
 * file_contents (broadens the load_descs manifest query to CMakeLists.txt + *.pc).
 * Transactional: replaces each project's rows. Returns rows written, -1 on error. */
int db2_cross_repo_rebuild_identities(void);

#endif /* AIMEE_CROSS_REPO_IDENTITY_H */
