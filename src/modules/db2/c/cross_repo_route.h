#ifndef AIMEE_CROSS_REPO_ROUTE_H
#define AIMEE_CROSS_REPO_ROUTE_H

/* H0d: the inter-repo STRUCTURAL-EDGE adjacency (precision-hardening §1). For an
 * ordered pair (caller -> definer) cross_repo_route records that the caller has an
 * import/include directive that resolves into the definer repo. H1 makes the
 * existence of such a route a hard prerequisite for any non-LOW cross-repo edge,
 * replacing P1's "a unique-name match is a dependency" with "a real route is". */

/* Rebuild cross_repo_route for all registered repos from file_imports +
 * cross_repo_identity (H0c) + files (H0b language/vendored). Two route kinds:
 *   import_module (HIGH) — a module/package specifier matched to a definer's
 *     cross_repo_identity value (manifest-authoritative; Go/Rust/npm/Python).
 *   import_header (MEDIUM) — a C/C++ #include matched to a definer file's basename
 *     (the repo-unique HIGH-vs-MEDIUM refinement is applied by H1 §1a).
 * Vendored definers are excluded. Transactional (DELETE + rebuild). Returns rows
 * written, -1 on error. */
int db2_cross_repo_rebuild_routes(void);

#endif /* AIMEE_CROSS_REPO_ROUTE_H */
