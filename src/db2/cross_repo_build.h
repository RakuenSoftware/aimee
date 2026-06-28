#ifndef AIMEE_CROSS_REPO_BUILD_H
#define AIMEE_CROSS_REPO_BUILD_H

#include <stddef.h>

/* Build-declared cross-repo dependency extraction (recall-recovery R2). The real
 * intra-corpus deps in a CMake/cargo corpus are declared in BUILD files
 * (FetchContent GIT_REPOSITORY, git submodules, Cargo git/path deps), not as source
 * #includes — so they yield no import route and are missed by the symbol resolver.
 * This module reads the indexed manifests (R2a) and records declared deps in the
 * cross_repo_build_dep table; the resolver merges them as a separate evidence class
 * (evidence_type=build_declared) per docs/proposals/pending/cross-repo-recall-recovery.md. */

typedef struct
{
   char ref[512]; /* the declared reference: a git URL or a path dep */
   char kind[16]; /* build_kind: "fetchcontent" | "submodule" | "manifest" */
   int low_conf;  /* 1 if the ref is unresolved (${VAR}/generator-expr) -> parse_confidence=low */
} xrepo_build_dep_t;

/* Pure: extract declared build deps from one manifest's content. `path` selects the
 * parser by trailing component (CMakeLists.txt or .cmake -> GIT_REPOSITORY;
 * .gitmodules -> submodule url; Cargo.toml -> git=/path= deps). Returns the count;
 * sets *overflow if the per-file cap was hit. De-dups within the file. */
int xrepo_extract_build_deps(const char *path, const char *content, xrepo_build_dep_t *out, int max,
                             int *overflow);

/* Pure: extract the repo component of a git URL or path dep — the last path
 * component with any '.git' suffix / trailing slash / userinfo (user:tok@) stripped,
 * lowercased. e.g. "https://u:t@github.com/owner/Foo-Bar.git" -> "foo-bar";
 * "../inputtino" -> "inputtino". Returns 1 + fills out, 0 if none. */
int xrepo_build_ref_repo(const char *ref, char *out, size_t cap);

/* Transactional rebuild of cross_repo_build_dep from indexed (non-vendored) manifest
 * file_contents: extract refs, map each to a corpus repo by repo-component ==
 * projects.name (normalized), insert (caller, definer, build_kind, parse_confidence,
 * evidence). Self-edges + external (unmapped) refs are skipped. Returns row count or
 * -1 on failure (fail-to-last-known-good, like the route rebuild). */
int db2_cross_repo_rebuild_build_deps(void);

#endif /* AIMEE_CROSS_REPO_BUILD_H */
