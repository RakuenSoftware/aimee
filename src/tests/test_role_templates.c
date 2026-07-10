/* test_role_templates.c: unit tests for role_templates.c */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../headers/role_templates.h"
#include "../headers/config.h" /* config_default_dir */

/* --- Helpers --- */

static int file_exists(const char *path)
{
   struct stat st;
   return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

/* Create a temp directory; returns malloc'd path, caller must rmdir+free. */
static char *make_tmpdir(void)
{
   char *tmp = malloc(64);
   assert(tmp);
   snprintf(tmp, 64, "/tmp/test_role_templates_XXXXXX");
   assert(mkdtemp(tmp) != NULL);
   return tmp;
}

static void rm_tmpdir(const char *dir)
{
   /* Remove all .md files, then rmdir */
   char cmd[512];
   snprintf(cmd, sizeof(cmd), "rm -f %s/*.md && rmdir %s 2>/dev/null || true", dir, dir);
   (void)system(cmd);
}

/* Write a file with given content. */
static void write_file(const char *path, const char *content)
{
   FILE *f = fopen(path, "w");
   assert(f);
   fputs(content, f);
   fclose(f);
}

/* --- Tests --- */

static void test_build_builtin_review(void)
{
   char *result = role_template_build(NULL, "review", "Review the auth module", NULL);
   assert(result != NULL);
   assert(strstr(result, "code reviewer") != NULL);
   /* review now trusts the index for current repo state and reviews the pushed diff */
   assert(strstr(result, "Trust aimee's index and code graph") != NULL);
   assert(strstr(result, "CHANGE UNDER REVIEW") != NULL);
   assert(strstr(result, "find_symbol") != NULL);
   assert(strstr(result, "Do not use Aimee memory, docs, index, search") == NULL);
   assert(strstr(result, "no filesystem or shell access") != NULL);
   assert(strstr(result, "Review the auth module") != NULL);
   assert(strstr(result, "{{TASK}}") == NULL); /* placeholder substituted */
   free(result);
}

static void test_build_builtin_code(void)
{
   char *result = role_template_build(NULL, "code", "Add input validation", "main.c:42");
   assert(result != NULL);
   assert(strstr(result, "coding delegate") != NULL);
   assert(strstr(result, "Use aimee's indexed tools before broad shell search") != NULL);
   assert(strstr(result, "Use index hits to choose exact files and lines") != NULL);
   assert(strstr(result, "run `rg` in the exact worktree") != NULL);
   assert(strstr(result, "Add input validation") != NULL);
   assert(strstr(result, "main.c:42") != NULL);
   assert(strstr(result, "{{TASK}}") == NULL);
   assert(strstr(result, "{{CONTEXT}}") == NULL);
   free(result);
}

static void test_build_builtin_validate(void)
{
   char *result = role_template_build(NULL, "validate", "Find git verify", "worktree_path: /repo");
   assert(result != NULL);
   assert(strstr(result, "validation delegate") != NULL);
   assert(strstr(result, "Inspect the repository files before naming commands") != NULL);
   assert(strstr(result, "Use aimee's indexed tools before broad shell search") != NULL);
   assert(strstr(result, "Use index hits to choose exact files and lines") != NULL);
   assert(strstr(result, "run `rg` in the exact worktree") != NULL);
   assert(strstr(result, "exact output") != NULL);
   assert(strstr(result, "worktree_path: /repo") != NULL);
   assert(strstr(result, "{{TASK}}") == NULL);
   assert(strstr(result, "{{CONTEXT}}") == NULL);
   free(result);
}

static void test_build_builtin_diagnose(void)
{
   char *result = role_template_build(NULL, "diagnose", "Explain delegate output leak", NULL);
   assert(result != NULL);
   assert(strstr(result, "diagnostic delegate") != NULL);
   assert(strstr(result, "Do not reveal private chain-of-thought") != NULL);
   assert(strstr(result, "cannot inspect that exact artifact") != NULL);
   assert(strstr(result, "Use only current-checkout evidence") != NULL);
   assert(strstr(result, "Do not use Aimee memory, docs, index, search") != NULL);
   assert(strstr(result, "run `rg` in the exact worktree") != NULL);
   assert(strstr(result, "inspected artifacts") != NULL);
   assert(strstr(result, "exact output") != NULL);
   assert(strstr(result, "Explain delegate output leak") != NULL);
   assert(strstr(result, "{{TASK}}") == NULL);
   free(result);
}

static void test_build_novel_roles(void)
{
   /* continuity: read-only, treats memory/graph as canon (NOT the code-review
    * evidence boundary that forbids memory). */
   char *cont = role_template_build(NULL, "continuity", "Check chapter 3", NULL);
   assert(cont != NULL);
   assert(strstr(cont, "continuity editor") != NULL);
   assert(strstr(cont, "READ-ONLY") != NULL);
   assert(strstr(cont, "aimee:search_graph") != NULL);
   assert(strstr(cont, "Do not use Aimee memory") == NULL); /* inverted vs review */
   assert(strstr(cont, "Check chapter 3") != NULL);
   assert(strstr(cont, "{{TASK}}") == NULL);
   free(cont);

   /* prose: write role, voice-aware. */
   char *prose = role_template_build(NULL, "prose", "Draft the duel", NULL);
   assert(prose != NULL);
   assert(strstr(prose, "prose-drafting delegate") != NULL);
   assert(strstr(prose, "voice") != NULL);
   free(prose);

   /* line-edit: must not change events/canon. */
   char *le = role_template_build(NULL, "line-edit", "Polish the opening", NULL);
   assert(le != NULL);
   assert(strstr(le, "line-editing delegate") != NULL);
   assert(strstr(le, "Do NOT change what happens") != NULL);
   free(le);

   /* beat-check: read-only structure check. */
   char *bc = role_template_build(NULL, "beat-check", "Check pacing", NULL);
   assert(bc != NULL);
   assert(strstr(bc, "structure delegate") != NULL);
   free(bc);
}

static void test_build_songwriter_roles(void)
{
   char *lyric = role_template_build(NULL, "lyric", "Write verse 2", NULL);
   assert(lyric && strstr(lyric, "lyric-writing delegate") != NULL);
   assert(strstr(lyric, "meter") != NULL);
   free(lyric);

   char *hook = role_template_build(NULL, "hook", "Sharpen the chorus", NULL);
   assert(hook && strstr(hook, "hook delegate") != NULL);
   free(hook);

   /* prosody: read-only, emits a PROSODY verdict for the done-gate. */
   char *pros = role_template_build(NULL, "prosody", "Check verse 1", NULL);
   assert(pros && strstr(pros, "prosody editor") != NULL);
   assert(strstr(pros, "READ-ONLY") != NULL);
   assert(strstr(pros, "PROSODY: FAIL") != NULL);
   free(pros);

   char *sf = role_template_build(NULL, "songform", "Check structure", NULL);
   assert(sf && strstr(sf, "structure delegate") != NULL);
   free(sf);
}

static void test_build_all_builtin_roles(void)
{
   static const char *roles[] = {
       "review",    "validate",   "diagnose", "code",   "refactor", "explain",    "draft",
       "execute",   "summarize",  "format",   "search", "reason",   "continuity", "prose",
       "line-edit", "beat-check", "lyric",    "hook",   "prosody",  "songform",   NULL,
   };
   for (int i = 0; roles[i]; i++)
   {
      char *result = role_template_build(NULL, roles[i], "test task", NULL);
      assert(result != NULL);
      assert(strlen(result) > 20);
      assert(strstr(result, "{{TASK}}") == NULL);
      free(result);
   }
}

static void test_build_unknown_role_returns_null(void)
{
   char *result = role_template_build(NULL, "xyzzy_nonexistent_role", "task", NULL);
   assert(result == NULL);
}

static void test_build_null_role_returns_null(void)
{
   char *result = role_template_build(NULL, NULL, "task", NULL);
   assert(result == NULL);
}

static void test_build_null_task_uses_placeholder(void)
{
   char *result = role_template_build(NULL, "review", NULL, NULL);
   assert(result != NULL);
   assert(strstr(result, "{{TASK}}") == NULL);
   assert(strstr(result, "(see context)") != NULL);
   free(result);
}

static void test_build_file_overrides_builtin(void)
{
   char *dir = make_tmpdir();

   /* Set up project structure: <dir>/.aimee/role_templates/ */
   char sub[256];
   snprintf(sub, sizeof(sub), "%s/.aimee", dir);
   mkdir(sub, 0755);
   snprintf(sub, sizeof(sub), "%s/.aimee/role_templates", dir);
   mkdir(sub, 0755);

   char path[256];
   snprintf(path, sizeof(path), "%s/.aimee/role_templates/review.md", dir);
   write_file(path, "Custom review template: {{TASK}} - context: {{CONTEXT}}");

   char *result = role_template_build(dir, "review", "my task", "some context");
   assert(result != NULL);
   assert(strstr(result, "Custom review template") != NULL);
   assert(strstr(result, "my task") != NULL);
   assert(strstr(result, "some context") != NULL);
   assert(strstr(result, "{{TASK}}") == NULL);
   assert(strstr(result, "{{CONTEXT}}") == NULL);
   free(result);

   rm_tmpdir(sub);
   snprintf(sub, sizeof(sub), "%s/.aimee", dir);
   rmdir(sub);
   rm_tmpdir(dir);
   free(dir);
}

static void test_path_project_found(void)
{
   char *dir = make_tmpdir();

   char sub[256];
   snprintf(sub, sizeof(sub), "%s/.aimee", dir);
   mkdir(sub, 0755);
   snprintf(sub, sizeof(sub), "%s/.aimee/role_templates", dir);
   mkdir(sub, 0755);

   char path[256];
   snprintf(path, sizeof(path), "%s/.aimee/role_templates/code.md", dir);
   write_file(path, "project code template");

   char found[512];
   int rc = role_template_path(dir, "code", found, sizeof(found));
   assert(rc == 0);
   assert(strstr(found, "code.md") != NULL);
   assert(file_exists(found));

   rm_tmpdir(sub);
   snprintf(sub, sizeof(sub), "%s/.aimee", dir);
   rmdir(sub);
   rm_tmpdir(dir);
   free(dir);
}

static void test_path_not_found(void)
{
   char found[512];
   int rc = role_template_path("/tmp/nonexistent_project_dir_12345", "unknownrole", found,
                               sizeof(found));
   assert(rc == -1);
   assert(found[0] == '\0');
}

static void test_list_includes_builtins(void)
{
   char names[ROLE_TEMPLATE_MAX_ROLES][ROLE_TEMPLATE_NAME_MAX];
   int n = role_template_list(NULL, names, ROLE_TEMPLATE_MAX_ROLES);
   assert(n >= 11); /* at least the 11 built-in roles */

   /* Check that review and code are present */
   int found_review = 0, found_code = 0, found_validate = 0;
   for (int i = 0; i < n; i++)
   {
      if (strcmp(names[i], "review") == 0)
         found_review = 1;
      if (strcmp(names[i], "code") == 0)
         found_code = 1;
      if (strcmp(names[i], "validate") == 0)
         found_validate = 1;
   }
   assert(found_review);
   assert(found_code);
   assert(found_validate);
}

static void test_list_no_duplicates(void)
{
   char names[ROLE_TEMPLATE_MAX_ROLES][ROLE_TEMPLATE_NAME_MAX];
   int n = role_template_list(NULL, names, ROLE_TEMPLATE_MAX_ROLES);
   for (int i = 0; i < n; i++)
      for (int j = i + 1; j < n; j++)
         assert(strcmp(names[i], names[j]) != 0);
}

static void test_install_defaults(void)
{
   char *dir = make_tmpdir();

   int n = role_template_install_defaults(dir);
   assert(n == 20); /* 12 code roles + 4 novel + 4 songwriter roles */

   /* Verify files were written */
   char path[256];
   snprintf(path, sizeof(path), "%s/review.md", dir);
   assert(file_exists(path));
   snprintf(path, sizeof(path), "%s/code.md", dir);
   assert(file_exists(path));
   snprintf(path, sizeof(path), "%s/validate.md", dir);
   assert(file_exists(path));

   /* Second call should skip existing files */
   int n2 = role_template_install_defaults(dir);
   assert(n2 == 0);

   rm_tmpdir(dir);
   free(dir);
}

static void test_install_defaults_null_dir(void)
{
   int rc = role_template_install_defaults(NULL);
   assert(rc == -1);
}

/* name validation + write/read_raw/delete round-trip against config_default_dir
 * (isolated via AIMEE_HOME, set in main before any test runs). */
static void test_write_read_delete(void)
{
   assert(role_template_name_valid("code"));
   assert(role_template_name_valid("my-role_2"));
   assert(!role_template_name_valid(""));
   assert(!role_template_name_valid("../x"));
   assert(!role_template_name_valid("a/b"));
   assert(!role_template_name_valid(".dot"));

   /* write a new custom role, read it back raw (placeholders intact) */
   assert(role_template_write("triage", "# Triage\n{{TASK}}\n{{CONTEXT}}\n") == 0);
   char *raw = role_template_read_raw(NULL, "triage");
   assert(raw && strstr(raw, "# Triage") != NULL);
   assert(strstr(raw, "{{TASK}}") != NULL); /* raw: not substituted */
   free(raw);

   /* editing a built-in role: the file overrides the code default */
   assert(role_template_write("review", "OVERRIDDEN REVIEW BODY\n") == 0);
   raw = role_template_read_raw(NULL, "review");
   assert(raw && strstr(raw, "OVERRIDDEN REVIEW BODY") != NULL);
   free(raw);
   char *built = role_template_build(NULL, "review", "T", "C");
   assert(built && strstr(built, "OVERRIDDEN REVIEW BODY") != NULL);
   free(built);

   /* delete reverts to the code default */
   assert(role_template_delete("review") == 0);
   assert(role_template_delete("review") == -1); /* gone */
   built = role_template_build(NULL, "review", "T", "C");
   assert(built && strstr(built, "code reviewer") != NULL); /* code default again */
   free(built);

   /* bad inputs */
   assert(role_template_write("../escape", "x") == -1);
   assert(role_template_delete("triage") == 0);
}

static void test_role_max_turns_frontmatter(void)
{
   char dir[512];
   snprintf(dir, sizeof(dir), "%s/role_templates", config_default_dir());
   char mkcmd[600];
   snprintf(mkcmd, sizeof(mkcmd), "mkdir -p '%s'", dir);
   assert(system(mkcmd) == 0);

   /* A max_turns frontmatter is read, and stripped from the built prompt. */
   char path[600];
   snprintf(path, sizeof(path), "%s/capped.md", dir);
   write_file(path, "---\nmax_turns: 7\n---\n\nYou are a capped delegate. {{TASK}}\n");
   assert(role_template_max_turns("capped") == 7);
   char *built = role_template_build(NULL, "capped", "do it", NULL);
   assert(built);
   assert(strstr(built, "max_turns") == NULL); /* frontmatter stripped */
   assert(strstr(built, "---") == NULL);
   assert(strstr(built, "You are a capped delegate. do it") != NULL);
   free(built);

   /* No frontmatter => -1 (INFINITE, the default). */
   snprintf(path, sizeof(path), "%s/plain.md", dir);
   write_file(path, "You are a plain delegate. {{TASK}}\n");
   assert(role_template_max_turns("plain") == -1);

   /* Unknown role => -1. */
   assert(role_template_max_turns("nonexistent_role_zzz") == -1);
   printf("  test_role_max_turns_frontmatter: ok\n");
}

int main(void)
{
   /* Isolate config_default_dir() so write/delete and user-dir scans do not
    * touch the developer's real ~/.config/aimee. */
   char home[256];
   snprintf(home, sizeof(home), "/tmp/test_role_templates_home_XXXXXX");
   assert(mkdtemp(home) != NULL);
   setenv("AIMEE_HOME", home, 1);

   test_build_builtin_review();
   test_build_builtin_code();
   test_build_builtin_validate();
   test_build_builtin_diagnose();
   test_build_novel_roles();
   test_build_songwriter_roles();
   test_build_all_builtin_roles();
   test_build_unknown_role_returns_null();
   test_build_null_role_returns_null();
   test_build_null_task_uses_placeholder();
   test_build_file_overrides_builtin();
   test_path_project_found();
   test_path_not_found();
   test_list_includes_builtins();
   test_list_no_duplicates();
   test_install_defaults();
   test_install_defaults_null_dir();
   test_write_read_delete();
   test_role_max_turns_frontmatter();
   printf("role_templates: all tests passed\n");
   return 0;
}
