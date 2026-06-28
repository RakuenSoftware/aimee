/* test_cross_repo_deps.c: unit tests for the pure cross-repo resolver core
 * (S2a: import resolution + distinctiveness). DB-free -- exercises
 * src/db2/cross_repo_resolver.c directly. Acceptance #1 (mechanical) for the
 * S2a portion; S2b adds the tier-pipeline tests. See
 * docs/proposals/pending/cross-repo-dependency-graph.md. */

#include "cross_repo_classify.h"
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

static void test_path_is_vendored(void)
{
   printf("test_path_is_vendored... ");
   /* whole-segment match anywhere in the path */
   assert(xrepo_path_is_vendored("vendor/foo/bar.c") == 1);
   assert(xrepo_path_is_vendored("src/third_party/x.h") == 1);
   assert(xrepo_path_is_vendored("app/extern/lib/y.cpp") == 1);
   assert(xrepo_path_is_vendored("web/node_modules/pkg/index.js") == 1);
   assert(xrepo_path_is_vendored("build/subprojects/dep/d.c") == 1);
   assert(xrepo_path_is_vendored("deps/zlib/zlib.h") == 1);
   assert(xrepo_path_is_vendored("build/_deps/fmt-src/fmt.h") == 1); /* CMake FetchContent */
   assert(xrepo_path_is_vendored(".venv/lib/site-packages/x.py") == 1);
   /* first-party paths are NOT vendored; substring-but-not-segment must not match */
   assert(xrepo_path_is_vendored("src/main.c") == 0);
   assert(xrepo_path_is_vendored("src/vendored_thing/x.c") == 0); /* "vendored_thing" != "vendor" */
   assert(xrepo_path_is_vendored("lib/dependency.c") == 0);       /* "dependency" != "deps" */
   assert(xrepo_path_is_vendored("") == 0);
   assert(xrepo_path_is_vendored(NULL) == 0);
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

/* ---- S2b: multiplicity (§3.5) -------------------------------------------- */

static void test_multiplicity(void)
{
   printf("test_multiplicity... ");
   xrepo_mult_cfg_t cfg = {.dom_share_pct = 90, .runnerup_share_pct = 5, .runnerup_abs = 2};

   /* Single definer repo. */
   xrepo_def_t one[] = {{"repo-b", XREPO_LANG_C, "", 2, "int,int", 0}};
   int oc[] = {3};
   int oe[] = {1};
   xrepo_mult_t m = xrepo_classify_multiplicity(one, oc, oe, 1, &cfg);
   assert(m.kind == XREPO_MULT_SINGLE && m.dominant_index == 0);

   /* Dominant definer: 95 defs vs 1, runner-up not an exporter -> DOMINANT. */
   xrepo_def_t dom[] = {{"repo-b", XREPO_LANG_C, "", 1, "int", 0},
                        {"repo-c", XREPO_LANG_C, "", 1, "int", 0}};
   int dc[] = {95, 1};
   int de[] = {1, 0};
   m = xrepo_classify_multiplicity(dom, dc, de, 2, &cfg);
   assert(m.kind == XREPO_MULT_DOMINANT && m.dominant_index == 0);

   /* Even split across two repos -> name-clash (AMBIGUOUS). */
   int sc[] = {5, 5};
   m = xrepo_classify_multiplicity(dom, sc, de, 2, &cfg);
   assert(m.kind == XREPO_MULT_NAMECLASH);

   /* Dominant by count but the runner-up is a distinctive exporter -> name-clash. */
   int dc2[] = {95, 1};
   int de2[] = {1, 1};
   m = xrepo_classify_multiplicity(dom, dc2, de2, 2, &cfg);
   assert(m.kind == XREPO_MULT_NAMECLASH);

   /* Provably-unrelated signatures (differing arity) -> name-clash even if a
    * count majority exists (polymorphic rescue does not apply). */
   xrepo_def_t unrel[] = {{"repo-b", XREPO_LANG_CPP, "", 1, "", 0},
                          {"repo-c", XREPO_LANG_CPP, "", 3, "", 0}};
   int uc[] = {95, 1};
   int ue[] = {1, 0};
   m = xrepo_classify_multiplicity(unrel, uc, ue, 2, &cfg);
   assert(m.kind == XREPO_MULT_NAMECLASH);

   /* Unknown/variadic signatures (arity -1, "" params) are NOT provably unrelated,
    * so a clear count majority still resolves to DOMINANT (overloads not punished). */
   xrepo_def_t vararg[] = {{"repo-b", XREPO_LANG_CPP, "", -1, "", 0},
                           {"repo-c", XREPO_LANG_CPP, "", 2, "", 0}};
   m = xrepo_classify_multiplicity(vararg, dc, de, 2, &cfg); /* dc={95,1}, de={1,0} */
   assert(m.kind == XREPO_MULT_DOMINANT);

   /* Non-positive def_counts are clamped (no negative shares); total 0 -> name-clash. */
   int zc[] = {0, 0};
   m = xrepo_classify_multiplicity(dom, zc, de, 2, &cfg);
   assert(m.kind == XREPO_MULT_NAMECLASH);
   printf("ok\n");
}

/* ---- S2b: the deterministic pipeline (§3.10) ----------------------------- */

static xrepo_candidate_t base_candidate(void)
{
   xrepo_candidate_t c;
   memset(&c, 0, sizeof(c));
   c.lang = XREPO_LANG_C;
   c.distinctive = 1;
   c.caller_trusted = 1;
   c.definer_trusted = 1;
   c.mult.kind = XREPO_MULT_SINGLE;
   c.corroboration = XREPO_CORROB_NONE;
   c.modality = XREPO_IMPORT_STATIC;
   return c;
}

static void test_classify_pipeline(void)
{
   printf("test_classify_pipeline... ");

   /* Import-corroborated, distinctive, trusted -> HIGH. */
   xrepo_candidate_t c = base_candidate();
   c.corroboration = XREPO_CORROB_IMPORT;
   assert(xrepo_classify(&c).tier == XREPO_TIER_HIGH);

   /* Trusted-export route -> HIGH. */
   c = base_candidate();
   c.corroboration = XREPO_CORROB_TRUSTED_EXPORT;
   assert(xrepo_classify(&c).tier == XREPO_TIER_HIGH);

   /* Dominant definer only -> MEDIUM. */
   c = base_candidate();
   c.corroboration = XREPO_CORROB_DOMINANT;
   c.mult.kind = XREPO_MULT_DOMINANT;
   assert(xrepo_classify(&c).tier == XREPO_TIER_MEDIUM);

   /* No corroboration -> LOW. */
   c = base_candidate();
   assert(xrepo_classify(&c).tier == XREPO_TIER_LOW);

   /* Invariant: S originates in caller -> NONE. */
   c = base_candidate();
   c.corroboration = XREPO_CORROB_IMPORT;
   c.originated_in_caller = 1;
   assert(xrepo_classify(&c).tier == XREPO_TIER_NONE);

   /* Not distinctive + multi-definer -> AMBIGUOUS (review); single -> NONE. */
   c = base_candidate();
   c.distinctive = 0;
   c.mult.kind = XREPO_MULT_NAMECLASH;
   xrepo_classification_t r = xrepo_classify(&c);
   assert(r.tier == XREPO_TIER_AMBIGUOUS && r.routed_to_review == 1);
   c.mult.kind = XREPO_MULT_SINGLE;
   assert(xrepo_classify(&c).tier == XREPO_TIER_NONE);

   /* Name-clash without corroboration -> AMBIGUOUS; with import -> resolved HIGH. */
   c = base_candidate();
   c.mult.kind = XREPO_MULT_NAMECLASH;
   assert(xrepo_classify(&c).tier == XREPO_TIER_AMBIGUOUS);
   c.corroboration = XREPO_CORROB_IMPORT;
   assert(xrepo_classify(&c).tier == XREPO_TIER_HIGH);

   /* Caller-collision caps one tier: HIGH -> MEDIUM. */
   c = base_candidate();
   c.corroboration = XREPO_CORROB_IMPORT;
   c.caller_collision = 1;
   r = xrepo_classify(&c);
   assert(r.tier == XREPO_TIER_MEDIUM && r.caller_collision_applied == 1);

   /* Conditional import caps one tier: HIGH -> MEDIUM. */
   c = base_candidate();
   c.corroboration = XREPO_CORROB_IMPORT;
   c.modality = XREPO_IMPORT_CONDITIONAL;
   assert(xrepo_classify(&c).tier == XREPO_TIER_MEDIUM);

   /* Dynamic import -> review (AMBIGUOUS). */
   c = base_candidate();
   c.corroboration = XREPO_CORROB_IMPORT;
   c.modality = XREPO_IMPORT_DYNAMIC;
   r = xrepo_classify(&c);
   assert(r.tier == XREPO_TIER_AMBIGUOUS && r.routed_to_review == 1);

   /* Untrusted caller caps the import route at MEDIUM (§0). */
   c = base_candidate();
   c.corroboration = XREPO_CORROB_IMPORT;
   c.caller_trusted = 0;
   r = xrepo_classify(&c);
   assert(r.tier == XREPO_TIER_MEDIUM && r.trust_cap_applied == 1);

   /* Untrusted DEFINER caps the export route at MEDIUM (§0): an untrusted
    * definer can never lend HIGH export. Caller trust alone does not save it. */
   c = base_candidate();
   c.corroboration = XREPO_CORROB_TRUSTED_EXPORT;
   c.definer_trusted = 0;
   r = xrepo_classify(&c);
   assert(r.tier == XREPO_TIER_MEDIUM && r.trust_cap_applied == 1);
   /* A trusted caller importing from an untrusted definer is unaffected on the
    * import route (the export-route cap is route-specific). */
   c = base_candidate();
   c.corroboration = XREPO_CORROB_IMPORT;
   c.definer_trusted = 0;
   assert(xrepo_classify(&c).tier == XREPO_TIER_HIGH);

   /* Structural invariant (§3.10): caps only lower -- final tier <= base tier. */
   c = base_candidate();
   c.corroboration = XREPO_CORROB_IMPORT;
   c.caller_collision = 1;
   c.modality = XREPO_IMPORT_CONDITIONAL;
   r = xrepo_classify(&c);
   assert(r.tier <= r.base_tier);

   /* Combined caps floor at LOW: import + collision + conditional. */
   c = base_candidate();
   c.corroboration = XREPO_CORROB_IMPORT;
   c.caller_collision = 1;
   c.modality = XREPO_IMPORT_CONDITIONAL;
   assert(xrepo_classify(&c).tier == XREPO_TIER_LOW);

   /* Unknown language -> UNIMPLEMENTED. */
   c = base_candidate();
   c.lang = XREPO_LANG_UNKNOWN;
   c.corroboration = XREPO_CORROB_IMPORT;
   assert(xrepo_classify(&c).tier == XREPO_TIER_UNIMPLEMENTED);

   /* Determinism: the same candidate classifies identically. */
   c = base_candidate();
   c.corroboration = XREPO_CORROB_DOMINANT;
   assert(xrepo_classify(&c).tier == xrepo_classify(&c).tier);
   printf("ok\n");
}

static void test_evidence_score(void)
{
   printf("test_evidence_score... ");
   /* More corroboration routes outweigh more call sites (eviction keeps the
    * better-evidenced rows). */
   assert(xrepo_evidence_score(0, 2, 1) > xrepo_evidence_score(99, 1, 0));
   /* Ties broken toward more call sites, then distinctiveness. */
   assert(xrepo_evidence_score(0, 3, 0) > xrepo_evidence_score(0, 2, 0));
   assert(xrepo_evidence_score(5, 2, 0) > xrepo_evidence_score(1, 2, 0));
   printf("ok\n");
}

int main(void)
{
   test_lang_from_path();
   test_path_is_vendored();
   test_utf8_len();
   test_distinctiveness();
   test_resolve_c();
   test_resolve_rust_go_py_ts();
   test_resolve_edge_cases();
   test_multiplicity();
   test_classify_pipeline();
   test_evidence_score();
   printf("cross_repo_deps: all tests passed\n");
   return 0;
}
