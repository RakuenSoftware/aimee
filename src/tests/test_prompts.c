/* test_prompts.c: unit tests for tiered system prompt builder */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "aimee.h"
#include "db1.h"
#include "working_profile.h"
#include "prompts.h"
#include "platform_test_util.h"

/* dstr stubs required by prompts.c */
#include "dstr.h"

/* Point config at a temp dir this test owns and write the keys the case needs.
 * prompt_apply_* read live config now instead of taking a config_t, so a case
 * that does not write its own precondition would inherit the developer's real
 * aimee.yaml and quietly stop testing what it names. Per-pid path rather than
 * mkdtemp() on a static buffer, which fails on a second call. */
static void write_test_config(const char *yaml)
{
   char dir[256], path[320];
   snprintf(dir, sizeof(dir), "/tmp/aimee-prompts-cfg-%d", (int)getpid());
   mkdir(dir, 0755);
   setenv("AIMEE_HOME", dir, 1);
   setenv("AIMEE_NO_CACHE", "1", 1);
   snprintf(path, sizeof(path), "%s/aimee.yaml", dir);
   FILE *f = fopen(path, "w");
   assert(f);
   fputs(yaml, f);
   fclose(f);
}

static void write_file(const char *path, const char *content)
{
   FILE *fp = fopen(path, "w");
   assert(fp);
   fputs(content, fp);
   fclose(fp);
}

typedef struct
{
   char tmpdir[PATH_MAX];
   char db_path[PATH_MAX];
} working_profile_test_db_t;

static working_profile_test_db_t working_profile_test_db_open(void)
{
   working_profile_test_db_t ctx;

   memset(&ctx, 0, sizeof(ctx));
   snprintf(ctx.tmpdir, sizeof(ctx.tmpdir), "%s/aimee-prompt-wp-XXXXXX", platform_tmpdir());
   assert(platform_mkdtemp(ctx.tmpdir) != NULL);
   snprintf(ctx.db_path, sizeof(ctx.db_path), "%s/aimee.db", ctx.tmpdir);
   assert(db1_init(ctx.db_path) == 0);
   return ctx;
}

static void working_profile_test_db_close(working_profile_test_db_t *ctx)
{
   if (!ctx)
      return;
   db1_shutdown();
   if (ctx->db_path[0])
      platform_test_remove_sqlite(ctx->db_path);
   if (ctx->tmpdir[0])
      platform_test_rmrf(ctx->tmpdir);
   memset(ctx, 0, sizeof(*ctx));
}

int main(void)
{
   printf("prompts: ");

   /* --- prompt_tier_from_string --- */
   {
      assert(prompt_tier_from_string("MINIMAL") == PROMPT_MINIMAL);
      assert(prompt_tier_from_string("minimal") == PROMPT_MINIMAL);
      assert(prompt_tier_from_string("STANDARD") == PROMPT_STANDARD);
      assert(prompt_tier_from_string("standard") == PROMPT_STANDARD);
      assert(prompt_tier_from_string("EXTENDED") == PROMPT_EXTENDED);
      assert(prompt_tier_from_string("extended") == PROMPT_EXTENDED);
      /* Unknown values fall back to STANDARD */
      assert(prompt_tier_from_string("UNKNOWN") == PROMPT_STANDARD);
      assert(prompt_tier_from_string("") == PROMPT_STANDARD);
      assert(prompt_tier_from_string(NULL) == PROMPT_STANDARD);
   }

   /* --- prompt_turn_registers_text: gate and wording ---
    * The register grammar is parsed by the Go economizer, and
    * session_compact's record path reads it for Key Decisions) while nothing ever ASKED
    * an agent to emit one — so real transcripts carry no tags and that extraction is
    * empty in practice. This is the request half, and it must stay off by default. */
   {
      assert(prompt_turn_registers_text(0) == NULL); /* default-off changes nobody */

      const char *t = prompt_turn_registers_text(1);
      assert(t != NULL);
      /* Names every tag the parser recognises as load-bearing; asking for a tag the
       * record path ignores would train agents to emit noise. */
      assert(strstr(t, "[verdict]") != NULL);
      assert(strstr(t, "[hazard]") != NULL);
      assert(strstr(t, "[blocked]") != NULL);
      /* Biases AGAINST tagging. A mis-tagged [verdict] survives compaction as a settled
       * fact while its reasoning is discarded, so it is worse than no tag at all — the
       * text has to say that, not merely list the tags. */
      assert(strstr(t, "Do NOT tag speculation") != NULL);
      assert(strstr(t, "safe default") != NULL);
   }

   /* --- prompt_tier_to_string --- */
   {
      assert(strcmp(prompt_tier_to_string(PROMPT_MINIMAL), "MINIMAL") == 0);
      assert(strcmp(prompt_tier_to_string(PROMPT_STANDARD), "STANDARD") == 0);
      assert(strcmp(prompt_tier_to_string(PROMPT_EXTENDED), "EXTENDED") == 0);
   }

   /* --- prompt_build: each tier produces distinct, non-empty content --- */
   {
      char *minimal = prompt_build(PROMPT_MINIMAL, "/tmp", NULL);
      char *standard = prompt_build(PROMPT_STANDARD, "/tmp", NULL);
      char *extended = prompt_build(PROMPT_EXTENDED, "/tmp", NULL);

      assert(minimal && minimal[0]);
      assert(standard && standard[0]);
      assert(extended && extended[0]);

      /* Tiers are distinct */
      assert(strcmp(minimal, standard) != 0);
      assert(strcmp(standard, extended) != 0);
      assert(strcmp(minimal, extended) != 0);

      /* EXTENDED is longer than STANDARD which is longer than MINIMAL */
      assert(strlen(extended) > strlen(standard));
      assert(strlen(standard) > strlen(minimal));

      /* THE PROMPT MUST NOT INSTRUCT A COMMAND THAT DOES NOT EXIST.
       *
       * Every tier carried a "## Work Queue" section telling the agent to
       * coordinate with other sessions via `aimee work claim` / `complete` /
       * `fail` / `list`. None of those exist: there is no `work` row in the
       * command table, no /v1 route, and no work.* dispatch handler. The only
       * surviving references are a capability-registry line and an entry in the
       * write-tier op list, neither of which can make a command run.
       *
       * Live on a deployment: `aimee work list` answers "command 'work' has no
       * /v1 route". This costs every session the turns it takes to discover
       * that, and it is worse than a wasted turn -- an agent told to coordinate
       * through a shared queue can believe coordination happened when nothing
       * was ever queued or claimed.
       *
       * If the work queue is implemented, this assertion is the reminder to
       * restore the instructions WITH it, not before it. */
      assert(strstr(minimal, "aimee work ") == NULL);
      assert(strstr(standard, "aimee work ") == NULL);
      assert(strstr(extended, "aimee work ") == NULL);
      assert(strstr(standard, "Work Queue") == NULL);
      assert(strstr(extended, "Work Queue") == NULL);

      free(minimal);
      free(standard);
      free(extended);
   }

   /* --- shared code principles: prepended exactly once --- */
   {
      const char *principles = prompt_code_principles_text();
      assert(principles != NULL);
      assert(strstr(principles, "# Code Principles") == principles);
      assert(strstr(principles, "Read files before editing") != NULL);
      assert(strstr(principles, "Prefer composition over inheritance") != NULL);
      assert(strstr(principles, "Use structured APIs or parsers") != NULL);
      assert(strstr(principles, "Add abstractions only when") != NULL);

      char *p = prompt_prepend_code_principles("BASE_SYSTEM");
      assert(p);
      assert(strstr(p, principles) == p);
      assert(strstr(p, "BASE_SYSTEM") != NULL);

      char *again = prompt_prepend_code_principles(p);
      assert(again);
      assert(strcmp(again, p) == 0);
      assert(strstr(again + strlen(principles), "# Code Principles") == NULL);
      free(p);
      free(again);
   }

   /* --- prompt_build: cwd is embedded in output --- */
   {
      char *p = prompt_build(PROMPT_STANDARD, "/my/special/cwd", NULL);
      assert(p);
      assert(strstr(p, "/my/special/cwd") != NULL);
      free(p);
   }

   /* --- prompt_build: project override appended when .aimee/prompt.md exists --- */
   {
      char tmpdir[512];
      snprintf(tmpdir, sizeof(tmpdir), "%s/aimee-test-prompts-XXXXXX", platform_tmpdir());
      assert(platform_mkdtemp(tmpdir) != NULL);

      /* Create .aimee/prompt.md */
      char aimee_dir[640];
      snprintf(aimee_dir, sizeof(aimee_dir), "%s/.aimee", tmpdir);
      assert(platform_test_mkdir(aimee_dir, 0755) == 0);

      char override_path[768];
      snprintf(override_path, sizeof(override_path), "%s/prompt.md", aimee_dir);
      write_file(override_path, "## Project Override\nCustom project guidance here.\n");

      char *p = prompt_build(PROMPT_STANDARD, tmpdir, NULL);
      assert(p);
      assert(strstr(p, "Project Override") != NULL);
      assert(strstr(p, "Custom project guidance here.") != NULL);
      free(p);

      /* Without .aimee/prompt.md (different dir), no override */
      char *p2 = prompt_build(PROMPT_STANDARD, "/tmp", NULL);
      assert(p2);
      assert(strstr(p2, "Project Override") == NULL);
      free(p2);

      platform_test_rmrf(tmpdir);
   }

   /* --- prompt_build: custom file overrides tier entirely --- */
   {
      char tmpdir[512];
      snprintf(tmpdir, sizeof(tmpdir), "%s/aimee-test-prompts2-XXXXXX", platform_tmpdir());
      assert(platform_mkdtemp(tmpdir) != NULL);

      char custom_path[640];
      snprintf(custom_path, sizeof(custom_path), "%s/custom.md", tmpdir);
      write_file(custom_path, "My custom system prompt.\n");

      char *p = prompt_build(PROMPT_MINIMAL, "/tmp", custom_path);
      assert(p);
      assert(strcmp(p, "My custom system prompt.\n") == 0);
      free(p);

      platform_test_rmrf(tmpdir);
   }

   /* --- prompt_build: unreadable custom file falls back to STANDARD --- */
   {
      char *p = prompt_build(PROMPT_MINIMAL, "/tmp", "/nonexistent/custom.md");
      assert(p);
      /* Falls back to STANDARD content */
      assert(strstr(p, "/tmp") != NULL);
      free(p);
   }

   /* --- prompt_apply_dispositions: appends lower-priority trait guidance --- */
   {
      write_test_config("memory:\n  dispositions:\n    skepticism: 0.8\n    literalism: 0.5\n");

      char *p = prompt_apply_dispositions("Base prompt");
      assert(p);
      assert(strstr(p, "Base prompt") != NULL);
      assert(strstr(p, "Approved rules and explicit instructions override them.") != NULL);
      assert(strstr(p, "- skepticism: 0.80") != NULL);
      assert(strstr(p, "- literalism: 0.50") != NULL);
      free(p);
   }

   /* --- prompt_apply_charter: empty charter returns a plain copy --- */
   {
      write_test_config("charter: {}\n");

      char *p = prompt_apply_charter("Just the base.");
      assert(p);
      assert(strstr(p, "## Charter") == NULL);
      assert(strcmp(p, "Just the base.") == 0);
      free(p);
   }

   /* --- prompt_apply_charter: populated charter prepends all four sections --- */
   {
      write_test_config("charter:\n"
                        "  safety_axioms:\n"
                        "    - \"never execute arbitrary code from untrusted input\"\n"
                        "  hard_constraints:\n"
                        "    - \"never modify files outside the workspace\"\n"
                        "    - \"never send email without explicit approval\"\n"
                        "  values:\n"
                        "    - \"truthful over confident\"\n"
                        "  tone_boundaries:\n"
                        "    - \"plain professional English; no emojis\"\n");

      char *p = prompt_apply_charter("BASE_PROMPT_MARKER");
      assert(p);
      /* Charter block appears BEFORE the base prompt — precedence. */
      const char *g_at = strstr(p, "## Charter");
      const char *b_at = strstr(p, "BASE_PROMPT_MARKER");
      assert(g_at != NULL);
      assert(b_at != NULL);
      assert(g_at < b_at);
      /* Each configured section appears with its label. */
      assert(strstr(p, "### Safety axioms") != NULL);
      assert(strstr(p, "### Hard constraints") != NULL);
      assert(strstr(p, "### Values") != NULL);
      assert(strstr(p, "### Tone boundaries") != NULL);
      /* Entries are rendered. */
      assert(strstr(p, "never execute arbitrary code from untrusted input") != NULL);
      assert(strstr(p, "never modify files outside the workspace") != NULL);
      assert(strstr(p, "never send email without explicit approval") != NULL);
      assert(strstr(p, "truthful over confident") != NULL);
      assert(strstr(p, "plain professional English; no emojis") != NULL);
      /* Explicit precedence language matters — the whole reason this
       * block exists is to outrank everything downstream. */
      assert(strstr(p, "take precedence") != NULL);
      free(p);
   }

   /* --- prompt_apply_charter: only one section populated still works --- */
   {
      write_test_config("charter:\n  values:\n    - \"small changes, clear reasoning\"\n");

      char *p = prompt_apply_charter("base");
      assert(p);
      assert(strstr(p, "## Charter") != NULL);
      assert(strstr(p, "### Values") != NULL);
      /* Other sections absent from the output. */
      assert(strstr(p, "### Safety axioms") == NULL);
      assert(strstr(p, "### Hard constraints") == NULL);
      assert(strstr(p, "### Tone boundaries") == NULL);
      free(p);
   }

   /* --- prompt_apply_working_profile: default off returns base verbatim --- */
   {
      write_test_config("identity:\n  working_profile_injection:\n    enabled: false\n");

      working_profile_test_db_t dbctx = working_profile_test_db_open();
      db1_working_profile_local_observe(WORKING_PROFILE_FIELD_VERBOSITY, "terse", 0.9, session_id(),
                                        3);
      db1_working_profile_local_observe(WORKING_PROFILE_FIELD_VERBOSITY, "terse", 0.9, session_id(),
                                        3);
      db1_working_profile_local_observe(WORKING_PROFILE_FIELD_VERBOSITY, "terse", 0.9, session_id(),
                                        3);

      char *p = prompt_apply_working_profile("just the base");
      assert(p);
      assert(strcmp(p, "just the base") == 0);
      free(p);
      working_profile_test_db_close(&dbctx);
   }

   /* --- enabled with empty allow list: all committed canonical fields
    *     get injected below the base prompt with soft framing --- */
   {
      write_test_config("identity:\n  working_profile_injection:\n    enabled: true\n");

      working_profile_test_db_t dbctx = working_profile_test_db_open();
      for (int i = 0; i < 3; i++)
         db1_working_profile_local_observe(WORKING_PROFILE_FIELD_VERBOSITY, "terse", 0.9,
                                           session_id(), 3);
      for (int i = 0; i < 3; i++)
         db1_working_profile_local_observe(WORKING_PROFILE_FIELD_COMMUNICATION_STYLE, "direct",
                                           0.85, session_id(), 3);

      char *p = prompt_apply_working_profile("SYSTEM_BASE");
      assert(p);
      /* Block comes AFTER the base prompt (soft constraints follow
       * the hard system prompt). */
      const char *base_at = strstr(p, "SYSTEM_BASE");
      const char *header_at = strstr(p, "## Working Profile");
      assert(base_at != NULL);
      assert(header_at != NULL);
      assert(base_at < header_at);
      /* Soft framing present and both fields rendered. */
      assert(strstr(p, "learned preferences") != NULL);
      assert(strstr(p, "override") != NULL);
      assert(strstr(p, "verbosity") != NULL);
      assert(strstr(p, "terse") != NULL);
      assert(strstr(p, "communication_style") != NULL);
      assert(strstr(p, "direct") != NULL);
      free(p);
      working_profile_test_db_close(&dbctx);
   }

   /* --- allow list restricts the block to listed fields only --- */
   {
      write_test_config("identity:\n"
                        "  working_profile_injection:\n"
                        "    enabled: true\n"
                        "    fields:\n      - \"" WORKING_PROFILE_FIELD_VERBOSITY "\"\n");

      working_profile_test_db_t dbctx = working_profile_test_db_open();
      for (int i = 0; i < 3; i++)
         db1_working_profile_local_observe(WORKING_PROFILE_FIELD_VERBOSITY, "terse", 0.9,
                                           session_id(), 3);
      for (int i = 0; i < 3; i++)
         db1_working_profile_local_observe(WORKING_PROFILE_FIELD_COMMUNICATION_STYLE, "direct",
                                           0.85, session_id(), 3);

      char *p = prompt_apply_working_profile("BASE");
      assert(p);
      assert(strstr(p, "## Working Profile") != NULL);
      assert(strstr(p, "verbosity") != NULL);
      /* Communication style is committed but NOT in the allow list. */
      assert(strstr(p, "communication_style") == NULL);
      assert(strstr(p, "direct") == NULL);
      free(p);
      working_profile_test_db_close(&dbctx);
   }

   /* --- enabled but no committed rows match allow list: no block --- */
   {
      write_test_config("identity:\n"
                        "  working_profile_injection:\n"
                        "    enabled: true\n"
                        "    fields:\n      - \"" WORKING_PROFILE_FIELD_PROJECT_ROLE "\"\n");

      working_profile_test_db_t dbctx = working_profile_test_db_open();
      for (int i = 0; i < 3; i++)
         db1_working_profile_local_observe(WORKING_PROFILE_FIELD_VERBOSITY, "terse", 0.9,
                                           session_id(), 3);

      char *p = prompt_apply_working_profile("BASE_ONLY");
      assert(p);
      assert(strcmp(p, "BASE_ONLY") == 0);
      free(p);
      working_profile_test_db_close(&dbctx);
   }

   /* --- working_profile_autoobserve_from_feedback: well-known phrases
    *     trip the matching observations; unknown phrases are ignored;
    *     three "be terse" hits commit the verbosity field --- */
   {
      working_profile_test_db_t dbctx = working_profile_test_db_open();

      /* Unknown text records nothing. */
      assert(working_profile_autoobserve_from_feedback("this is unrelated text") == 0);

      /* One hit → one observation, no commit yet. */
      int obs = working_profile_autoobserve_from_feedback("please be terse from now on");
      assert(obs == 1);
      db1_working_profile_local_state_t got;
      assert(db1_working_profile_local_get(WORKING_PROFILE_FIELD_VERBOSITY, &got) == 1);

      /* Three hits total → commit. */
      working_profile_autoobserve_from_feedback("prefer terse output, thanks");
      working_profile_autoobserve_from_feedback("keep it short please");
      /* "keep it short" maps to verbosity=terse with confidence 0.6 —
       * verbosity=terse now has three matching observations total. */
      assert(db1_working_profile_local_get(WORKING_PROFILE_FIELD_VERBOSITY, &got) == 0);
      assert(strcmp(got.value, "terse") == 0);

      /* Multi-rule phrase records multiple signals in one call. */
      int multi = working_profile_autoobserve_from_feedback("be terse and be direct");
      assert(multi >= 2);

      working_profile_test_db_close(&dbctx);
   }

   /* --- aimee_mode_from_string / to_string --- */
   {
      assert(aimee_mode_from_string("novel") == AIMEE_MODE_NOVEL);
      assert(aimee_mode_from_string("NOVEL") == AIMEE_MODE_NOVEL);
      assert(aimee_mode_from_string("engineer") == AIMEE_MODE_ENGINEER);
      assert(aimee_mode_from_string("") == AIMEE_MODE_ENGINEER);
      assert(aimee_mode_from_string(NULL) == AIMEE_MODE_ENGINEER);
      assert(aimee_mode_from_string("nonsense") == AIMEE_MODE_ENGINEER);
      assert(aimee_mode_from_string("songwriter") == AIMEE_MODE_SONGWRITER);
      assert(aimee_mode_from_string("SONGWRITER") == AIMEE_MODE_SONGWRITER);
      assert(strcmp(aimee_mode_to_string(AIMEE_MODE_NOVEL), "novel") == 0);
      assert(strcmp(aimee_mode_to_string(AIMEE_MODE_SONGWRITER), "songwriter") == 0);
      assert(strcmp(aimee_mode_to_string(AIMEE_MODE_ENGINEER), "engineer") == 0);
      /* reviewer modes round-trip, case-insensitive */
      assert(aimee_mode_from_string("qa") == AIMEE_MODE_QA);
      assert(aimee_mode_from_string("QA") == AIMEE_MODE_QA);
      assert(aimee_mode_from_string("security") == AIMEE_MODE_SECURITY);
      assert(aimee_mode_from_string("reviewer") == AIMEE_MODE_REVIEWER);
      assert(aimee_mode_from_string("architect") == AIMEE_MODE_ARCHITECT);
      assert(strcmp(aimee_mode_to_string(AIMEE_MODE_QA), "qa") == 0);
      assert(strcmp(aimee_mode_to_string(AIMEE_MODE_SECURITY), "security") == 0);
      assert(strcmp(aimee_mode_to_string(AIMEE_MODE_REVIEWER), "reviewer") == 0);
      assert(strcmp(aimee_mode_to_string(AIMEE_MODE_ARCHITECT), "architect") == 0);
   }

   /* --- mode-aware principles select the right block --- */
   {
      const char *eng = prompt_principles_text(AIMEE_MODE_ENGINEER);
      const char *nov = prompt_principles_text(AIMEE_MODE_NOVEL);
      assert(strstr(eng, "# Code Principles") == eng);
      assert(strstr(nov, "# Craft Principles") == nov);
      /* Engineer form matches the back-compat accessor exactly. */
      assert(strcmp(eng, prompt_code_principles_text()) == 0);

      char *p = prompt_prepend_principles(AIMEE_MODE_NOVEL, "BASE");
      assert(p && strstr(p, nov) == p && strstr(p, "BASE") != NULL);
      /* Idempotent. */
      char *again = prompt_prepend_principles(AIMEE_MODE_NOVEL, p);
      assert(again && strcmp(again, p) == 0);
      free(p);
      free(again);
   }

   /* --- prompt_build_mode: engineer is byte-identical to prompt_build --- */
   {
      prompt_tier_t tiers[3] = {PROMPT_MINIMAL, PROMPT_STANDARD, PROMPT_EXTENDED};
      for (int t = 0; t < 3; t++)
      {
         char *legacy = prompt_build(tiers[t], "/tmp/x", NULL);
         char *eng = prompt_build_mode(AIMEE_MODE_ENGINEER, tiers[t], "/tmp/x", NULL);
         assert(legacy && eng && strcmp(legacy, eng) == 0);
         free(legacy);
         free(eng);
      }
   }

   /* --- prompt_build_mode novel: author persona, distinct from engineer --- */
   {
      char *eng = prompt_build_mode(AIMEE_MODE_ENGINEER, PROMPT_STANDARD, "/tmp/x", NULL);
      char *nov = prompt_build_mode(AIMEE_MODE_NOVEL, PROMPT_STANDARD, "/tmp/x", NULL);
      assert(eng && nov);
      assert(strstr(eng, "autonomous software engineer") != NULL);
      assert(strstr(nov, "fiction author and worldbuilder") != NULL);
      assert(strstr(nov, "autonomous software engineer") == NULL);
      assert(strstr(nov, "/tmp/x") != NULL); /* cwd still embedded */
      assert(strcmp(eng, nov) != 0);
      free(eng);
      free(nov);

      /* MINIMAL is shared across modes. */
      char *me = prompt_build_mode(AIMEE_MODE_ENGINEER, PROMPT_MINIMAL, "/tmp/x", NULL);
      char *mn = prompt_build_mode(AIMEE_MODE_NOVEL, PROMPT_MINIMAL, "/tmp/x", NULL);
      assert(me && mn && strcmp(me, mn) == 0);
      free(me);
      free(mn);
   }

   /* --- prompt_build_mode songwriter: lyricist persona + song principles --- */
   {
      char *song = prompt_build_mode(AIMEE_MODE_SONGWRITER, PROMPT_STANDARD, "/tmp/x", NULL);
      assert(song);
      assert(strstr(song, "songwriter and lyricist") != NULL);
      assert(strstr(song, "autonomous software engineer") == NULL);
      assert(strstr(song, "fiction author and worldbuilder") == NULL);
      free(song);

      const char *sp = prompt_principles_text(AIMEE_MODE_SONGWRITER);
      assert(strstr(sp, "# Craft Principles") == sp);
      assert(strstr(sp, "careful songwriter") != NULL);
   }

   /* --- prompt_build_mode reviewers: each gets its own persona, shared
    *     # Review Principles, and STANDARD == EXTENDED prose --- */
   {
      struct
      {
         aimee_mode_t mode;
         const char *needle;
      } cases[] = {
          {AIMEE_MODE_QA, "senior QA engineer"},
          {AIMEE_MODE_SECURITY, "application-security reviewer"},
          {AIMEE_MODE_REVIEWER, "contrarian code reviewer"},
          {AIMEE_MODE_ARCHITECT, "senior software architect"},
      };
      for (int i = 0; i < 4; i++)
      {
         char *std = prompt_build_mode(cases[i].mode, PROMPT_STANDARD, "/tmp/x", NULL);
         char *ext = prompt_build_mode(cases[i].mode, PROMPT_EXTENDED, "/tmp/x", NULL);
         assert(std && ext);
         assert(strstr(std, cases[i].needle) != NULL);
         assert(strstr(std, "autonomous software engineer") == NULL);
         assert(strstr(std, "/tmp/x") != NULL); /* cwd embedded */
         assert(strcmp(std, ext) == 0);         /* shared prose per tier */
         const char *pr = prompt_principles_text(cases[i].mode);
         assert(strstr(pr, "# Review Principles") == pr); /* shared principles */
         free(std);
         free(ext);
      }
   }

   /* --- Manager block: every lever actually withholds text ---
    *
    * Asserted through the pure composer so no config file is needed. Each lever
    * is checked to REMOVE text rather than merely to be settable: a flag that
    * parses but changes no output is the failure mode worth testing, and it is
    * invisible to a test that only inspects the default prompt. */
   {
      const char *HEADER = "## Your role: manage the work";
      const char *DELEGATION = "ALWAYS delegate multi-file changes";
      const char *REVIEW = "roundtable review";

      char *b = prompt_manager_block(1, 1, 1);
      assert(b);
      assert(strstr(b, HEADER) && strstr(b, DELEGATION) && strstr(b, REVIEW));
      free(b);

      /* Review off: manager framing and delegation stay, the round trip goes. */
      b = prompt_manager_block(1, 1, 0);
      assert(b);
      assert(strstr(b, HEADER) && strstr(b, DELEGATION));
      assert(strstr(b, REVIEW) == NULL);
      free(b);

      /* Block off, and delegates off, each withhold the whole thing. */
      assert(prompt_manager_block(0, 1, 1) == NULL);
      assert(prompt_manager_block(0, 0, 0) == NULL);
      assert(prompt_manager_block(1, 0, 1) == NULL);
   }

   /* --- The block appears exactly ONCE, on both tiers ---
    *
    * It used to be pasted verbatim into both the STANDARD and EXTENDED literals.
    * Asserting a single occurrence is what stops a future edit from reintroducing
    * the second copy, which is how the two drifted apart before. Runs at whatever
    * the ambient config says, so it asserts only the count -- not presence, which
    * the levers legitimately control. */
   {
      const char *HEADER = "## Your role: manage the work";
      prompt_tier_t tiers[] = {PROMPT_STANDARD, PROMPT_EXTENDED};
      for (int i = 0; i < 2; i++)
      {
         char *p = prompt_build_mode(AIMEE_MODE_ENGINEER, tiers[i], "/tmp/x", NULL);
         assert(p);
         const char *first = strstr(p, HEADER);
         if (first)
            assert(strstr(first + 1, HEADER) == NULL);
         assert(strstr(p, "autonomous software engineer") != NULL);
         free(p);
      }

      /* MINIMAL never carries it: a tier chosen for brevity must not gain the
       * longest block in the prompt. */
      char *p = prompt_build_mode(AIMEE_MODE_ENGINEER, PROMPT_MINIMAL, "/tmp/x", NULL);
      assert(p);
      assert(strstr(p, HEADER) == NULL);
      free(p);
   }

   printf("OK\n");
   return 0;
}
