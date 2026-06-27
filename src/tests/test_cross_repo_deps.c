/* test_cross_repo_deps.c: unit tests for the pure cross-repo resolver core
 * (S2a: import resolution + distinctiveness). DB-free -- exercises
 * src/db2/cross_repo_resolver.c directly. Acceptance #1 (mechanical) for the
 * S2a portion; S2b adds the tier-pipeline tests. See
 * docs/proposals/pending/cross-repo-dependency-graph.md. */

#include "cross_repo_resolver.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

/* ---- language classification + utf8 -------------------------------------- */

static void test_lang_from_path(void)
{
   printf("test_lang_from_path... ");
   assert(xrepo_lang_from_path("src/a.c") == XREPO_LANG_C);
   assert(xrepo_lang_from_path("inc/a.h") == XREPO_LANG_C);
   assert(xrepo_lang_from_path("src/a.cpp") == XREPO_LANG_CPP);
   assert(xrepo_lang_from_path("src/a.hpp") == XREPO_LANG_CPP);
   assert(xrepo_lang_from_path("src/a.cc") == XREPO_LANG_CPP);
   assert(xrepo_lang_from_path("m/lib.rs") == XREPO_LANG_RUST);
   assert(xrepo_lang_from_path("m/main.go") == XREPO_LANG_GO);
   assert(xrepo_lang_from_path("ui/app.ts") == XREPO_LANG_TS);
   assert(xrepo_lang_from_path("ui/app.tsx") == XREPO_LANG_TS);
   assert(xrepo_lang_from_path("ui/app.js") == XREPO_LANG_JS);
   assert(xrepo_lang_from_path("ui/app.mjs") == XREPO_LANG_JS);
   assert(xrepo_lang_from_path("svc/app.py") == XREPO_LANG_PYTHON);
   assert(xrepo_lang_from_path("README.md") == XREPO_LANG_UNKNOWN);
   assert(xrepo_lang_from_path("Main.java") == XREPO_LANG_UNKNOWN); /* deferred lang */
   assert(xrepo_lang_from_path(NULL) == XREPO_LANG_UNKNOWN);
   assert(strcmp(xrepo_lang_name(XREPO_LANG_PYTHON), "python") == 0);
   assert(strcmp(xrepo_lang_name(XREPO_LANG_UNKNOWN), "unknown") == 0);
   printf("ok\n");
}

static void test_utf8_len(void)
{
   printf("test_utf8_len... ");
   assert(xrepo_utf8_len("abcd") == 4);
   assert(xrepo_utf8_len("") == 0);
   assert(xrepo_utf8_len(NULL) == 0);
   assert(xrepo_utf8_len("héllo") == 5); /* é is 2 bytes, 1 code point */
   assert(xrepo_utf8_len("LiStartConnection") == 17);
   printf("ok\n");
}

/* ---- distinctiveness (§3.3) ---------------------------------------------- */

static void test_distinctiveness(void)
{
   printf("test_distinctiveness... ");
   xrepo_distinct_cfg_t cfg = {.k = 5, .m = 8, .p_pct = 25, .len_min = 4};

   /* Distinctive: rare, long, low caller-file ratio. */
   xrepo_distinct_stats_t good = {
       .callee_repo_count = 1, .definer_repo_count = 1, .caller_file_pct = 2};
   assert(xrepo_name_distinctive("LiStartConnection", &good, &cfg) == 1);

   /* Too short (len < 4). */
   assert(xrepo_name_distinctive("get", &good, &cfg) == 0);

   /* Used as a callee in >= K trusted repos -> not distinctive (common method). */
   xrepo_distinct_stats_t common = {
       .callee_repo_count = 9, .definer_repo_count = 1, .caller_file_pct = 2};
   assert(xrepo_name_distinctive("render", &common, &cfg) == 0);

   /* Defined in >= M repos -> not distinctive (vendored/ubiquitous). */
   xrepo_distinct_stats_t manydefs = {
       .callee_repo_count = 1, .definer_repo_count = 8, .caller_file_pct = 2};
   assert(xrepo_name_distinctive("hashmap_new", &manydefs, &cfg) == 0);

   /* Heavy local use (>= P% of caller's files) -> not distinctive (local method). */
   xrepo_distinct_stats_t local = {
       .callee_repo_count = 1, .definer_repo_count = 1, .caller_file_pct = 40};
   assert(xrepo_name_distinctive("update_state", &local, &cfg) == 0);

   /* Boundary: exactly at the floor is NOT distinctive (>= is the cut). */
   xrepo_distinct_stats_t at_k = {
       .callee_repo_count = 5, .definer_repo_count = 1, .caller_file_pct = 0};
   assert(xrepo_name_distinctive("connect_pipe", &at_k, &cfg) == 0);
   printf("ok\n");
}

/* ---- import resolution (§3.7): seed fixture ------------------------------ *
 * A minimal checked-in corpus exercising the four acceptance shapes:
 *   - moonlight-common-c : import-resolvable positive (C header + Go/etc module)
 *   - vendored-copy-a/-b : the same header in two repos -> AMBIGUOUS (MANY)
 *   - untrusted-lib      : an untrusted definer (resolution still finds it; the
 *                          trust cap is applied downstream in S2b)
 * S8 grows this into the full stratified ground-truth corpus. */

static const char *const moonlight_headers[] = {"include/Limelight.h", "src/connection.c"};
static const char *const vendored_a_headers[] = {"third_party/zlib/zlib.h"};
static const char *const vendored_b_headers[] = {"vendor/zlib/zlib.h"};

static const xrepo_repo_desc_t SEED[] = {
    {"moonlight-common-c", 1, "github.com/moonlight-stream/moonlight-common-c", moonlight_headers,
     2},
    {"moonlight-qt", 1, "github.com/moonlight-stream/moonlight-qt", NULL, 0},
    {"vendored-copy-a", 1, "", vendored_a_headers, 1},
    {"vendored-copy-b", 1, "", vendored_b_headers, 1},
    {"untrusted-lib", 0, "sketchy-pkg", NULL, 0},
    {"crate-host", 1, "serde_helpers", NULL, 0},
    {"go-mono", 1, "example.com/mono", NULL, 0},
    {"ts-host", 1, "@scope/widgets", NULL, 0},
    {"py-host", 1, "requests_ext", NULL, 0},
};
static const size_t SEED_N = sizeof(SEED) / sizeof(SEED[0]);

static int seed_index(const char *name)
{
   for (size_t i = 0; i < SEED_N; i++)
      if (strcmp(SEED[i].name, name) == 0)
         return (int)i;
   return -1;
}

static void test_resolve_c(void)
{
   printf("test_resolve_c... ");
   /* Positive: a quoted include resolving to exactly one trusted repo's header. */
   xrepo_resolve_result_t r = xrepo_resolve_import_to_repo(
       "Limelight.h", XREPO_LANG_C, "moonlight-qt", XREPO_IMPORT_STATIC, SEED, SEED_N);
   assert(r.cardinality == XREPO_RESOLVE_ONE);
   assert(r.repo_index == seed_index("moonlight-common-c"));
   assert(r.system_header == 0);

   /* Path-suffix match on a multi-component include. */
   r = xrepo_resolve_import_to_repo("include/Limelight.h", XREPO_LANG_C, "moonlight-qt",
                                    XREPO_IMPORT_STATIC, SEED, SEED_N);
   assert(r.cardinality == XREPO_RESOLVE_ONE);

   /* System header -> rejected, never an edge. */
   r = xrepo_resolve_import_to_repo("stdio.h", XREPO_LANG_C, "moonlight-qt", XREPO_IMPORT_STATIC,
                                    SEED, SEED_N);
   assert(r.cardinality == XREPO_RESOLVE_NONE && r.system_header == 1);
   r = xrepo_resolve_import_to_repo("vector", XREPO_LANG_CPP, "moonlight-qt", XREPO_IMPORT_STATIC,
                                    SEED, SEED_N);
   assert(r.cardinality == XREPO_RESOLVE_NONE && r.system_header == 1);

   /* Vendored copy in two repos -> AMBIGUOUS (MANY), never guessed; the colliders
    * are enumerated for the review queue. */
   r = xrepo_resolve_import_to_repo("zlib.h", XREPO_LANG_C, "moonlight-qt", XREPO_IMPORT_STATIC,
                                    SEED, SEED_N);
   assert(r.cardinality == XREPO_RESOLVE_MANY && r.repo_index == -1);
   assert(r.collision_count == 2);
   {
      int a = r.collisions[0], b = r.collisions[1];
      int va = seed_index("vendored-copy-a"), vb = seed_index("vendored-copy-b");
      assert((a == va && b == vb) || (a == vb && b == va));
   }

   /* Unknown include -> NONE. */
   r = xrepo_resolve_import_to_repo("nope_nowhere.h", XREPO_LANG_C, "moonlight-qt",
                                    XREPO_IMPORT_STATIC, SEED, SEED_N);
   assert(r.cardinality == XREPO_RESOLVE_NONE);
   printf("ok\n");
}

static void test_resolve_rust_go_py_ts(void)
{
   printf("test_resolve_rust_go_py_ts... ");
   xrepo_resolve_result_t r;

   /* Rust: crate token matches; crate::/super::/self:: are intra-repo. */
   r = xrepo_resolve_import_to_repo("serde_helpers::ser", XREPO_LANG_RUST, "app",
                                    XREPO_IMPORT_STATIC, SEED, SEED_N);
   assert(r.cardinality == XREPO_RESOLVE_ONE && r.repo_index == seed_index("crate-host"));
   r = xrepo_resolve_import_to_repo("crate::util", XREPO_LANG_RUST, "app", XREPO_IMPORT_STATIC,
                                    SEED, SEED_N);
   assert(r.cardinality == XREPO_RESOLVE_NONE);
   /* Rust 2018+ absolute path "::crate_token::..." resolves the crate. */
   r = xrepo_resolve_import_to_repo("::serde_helpers::de", XREPO_LANG_RUST, "app",
                                    XREPO_IMPORT_STATIC, SEED, SEED_N);
   assert(r.cardinality == XREPO_RESOLVE_ONE && r.repo_index == seed_index("crate-host"));

   /* Go: exact module + monorepo sub-package both resolve to the module repo. */
   r = xrepo_resolve_import_to_repo("example.com/mono", XREPO_LANG_GO, "app", XREPO_IMPORT_STATIC,
                                    SEED, SEED_N);
   assert(r.cardinality == XREPO_RESOLVE_ONE && r.repo_index == seed_index("go-mono"));
   r = xrepo_resolve_import_to_repo("example.com/mono/internal/util", XREPO_LANG_GO, "app",
                                    XREPO_IMPORT_STATIC, SEED, SEED_N);
   assert(r.cardinality == XREPO_RESOLVE_ONE && r.repo_index == seed_index("go-mono"));

   /* Python: top-level package; relative imports are intra-package. */
   r = xrepo_resolve_import_to_repo("requests_ext.session", XREPO_LANG_PYTHON, "app",
                                    XREPO_IMPORT_STATIC, SEED, SEED_N);
   assert(r.cardinality == XREPO_RESOLVE_ONE && r.repo_index == seed_index("py-host"));
   r = xrepo_resolve_import_to_repo(".relative", XREPO_LANG_PYTHON, "app", XREPO_IMPORT_STATIC,
                                    SEED, SEED_N);
   assert(r.cardinality == XREPO_RESOLVE_NONE);

   /* TS: scoped package "@scope/widgets[/sub]"; relative specifiers are intra-repo. */
   r = xrepo_resolve_import_to_repo("@scope/widgets/button", XREPO_LANG_TS, "app",
                                    XREPO_IMPORT_STATIC, SEED, SEED_N);
   assert(r.cardinality == XREPO_RESOLVE_ONE && r.repo_index == seed_index("ts-host"));
   r = xrepo_resolve_import_to_repo("./local", XREPO_LANG_JS, "app", XREPO_IMPORT_STATIC, SEED,
                                    SEED_N);
   assert(r.cardinality == XREPO_RESOLVE_NONE);
   printf("ok\n");
}

static void test_resolve_edge_cases(void)
{
   printf("test_resolve_edge_cases... ");
   /* Self-match collapses to NONE (no self-dependency edge): a repo importing
    * its own module id. */
   xrepo_resolve_result_t r = xrepo_resolve_import_to_repo(
       "serde_helpers::x", XREPO_LANG_RUST, "crate-host", XREPO_IMPORT_STATIC, SEED, SEED_N);
   assert(r.cardinality == XREPO_RESOLVE_NONE);

   /* Caller is excluded from candidates: vendored-copy-a importing zlib.h sees
    * only vendored-copy-b -> ONE (not MANY, and not a self-edge). */
   r = xrepo_resolve_import_to_repo("zlib.h", XREPO_LANG_C, "vendored-copy-a", XREPO_IMPORT_STATIC,
                                    SEED, SEED_N);
   assert(r.cardinality == XREPO_RESOLVE_ONE && r.repo_index == seed_index("vendored-copy-b"));

   /* Untrusted definer is still resolved (the trust cap is applied downstream). */
   r = xrepo_resolve_import_to_repo("sketchy-pkg", XREPO_LANG_PYTHON, "app", XREPO_IMPORT_STATIC,
                                    SEED, SEED_N);
   assert(r.cardinality == XREPO_RESOLVE_ONE && r.repo_index == seed_index("untrusted-lib"));

   /* Modality is echoed through for the pipeline's import-modality cap. */
   r = xrepo_resolve_import_to_repo("requests_ext", XREPO_LANG_PYTHON, "app",
                                    XREPO_IMPORT_CONDITIONAL, SEED, SEED_N);
   assert(r.cardinality == XREPO_RESOLVE_ONE && r.modality == XREPO_IMPORT_CONDITIONAL);

   /* Unknown language -> import route unavailable. */
   r = xrepo_resolve_import_to_repo("anything", XREPO_LANG_UNKNOWN, "app", XREPO_IMPORT_STATIC,
                                    SEED, SEED_N);
   assert(r.cardinality == XREPO_RESOLVE_NONE);

   /* Empty / NULL inputs are safe. */
   r = xrepo_resolve_import_to_repo("", XREPO_LANG_GO, "app", XREPO_IMPORT_STATIC, SEED, SEED_N);
   assert(r.cardinality == XREPO_RESOLVE_NONE);
   r = xrepo_resolve_import_to_repo(NULL, XREPO_LANG_GO, "app", XREPO_IMPORT_STATIC, SEED, SEED_N);
   assert(r.cardinality == XREPO_RESOLVE_NONE);
   printf("ok\n");
}

int main(void)
{
   test_lang_from_path();
   test_utf8_len();
   test_distinctiveness();
   test_resolve_c();
   test_resolve_rust_go_py_ts();
   test_resolve_edge_cases();
   printf("cross_repo_deps: all tests passed\n");
   return 0;
}
