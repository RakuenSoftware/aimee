/* test_role_templates.c: unit tests for role_templates.c */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../headers/role_templates.h"
#include "cmd_agent_delegate_impl.h"
#include "server_delegate_input.h"
#include "config.h"             /* config_default_dir */
#include "platform_test_util.h" /* platform_tmpdir: honour TMPDIR, do not leak into /tmp */

/* Policy assembly uses the real budget algorithm; only project-config path
 * discovery is isolated from the operator's registered projects. */
int project_yaml_path(const char *project_root, char *out, size_t capacity)
{
   (void)project_root;
   (void)out;
   (void)capacity;
   return -1;
}

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
   snprintf(tmp, 64, "%s/test_role_templates_XXXXXX", platform_tmpdir());
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

   /* beat-check: read-only structure check. */
   char *bc = role_template_build(NULL, "beat-check", "Check pacing", NULL);
   assert(bc != NULL);
   assert(strstr(bc, "structure delegate") != NULL);
   free(bc);
}

/* The persona-vs-role cull deleted these templates. role_template_build must
 * now return NULL for them, exactly as it does for any unknown role — a
 * surviving template would let a name that routing refuses still produce a
 * plausible-looking prompt. */
static void test_culled_role_templates_are_gone(void)
{
   static const char *const culled[] = {"prose",   "line-edit", "lyric", "hook",
                                        "prosody", "songform",  NULL};
   for (int i = 0; culled[i]; i++)
   {
      char *result = role_template_build(NULL, culled[i], "task", NULL);
      assert(result == NULL);
   }
}

static void test_build_all_builtin_roles(void)
{
   static const char *roles[] = {
       "review",  "validate", "diagnose",   "code",       "refactor",
       "explain", "draft",    "execute",    "summarize",  "format",
       "search",  "reason",   "continuity", "beat-check", NULL,
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

static void test_path_names_cannot_escape(void)
{
   char *project = make_tmpdir();
   char directory[256], outside[256], resolved[512];
   snprintf(directory, sizeof directory, "%s/.aimee", project);
   mkdir(directory, 0755);
   snprintf(directory, sizeof directory, "%s/.aimee/role_templates", project);
   mkdir(directory, 0755);
   snprintf(outside, sizeof outside, "%s/outside.md", project);
   write_file(outside, "---\npermissions: [tools]\n---\nOUTSIDE_ROLE {{TASK}}\n");
   char inside[512];
   snprintf(inside, sizeof inside, "%s/inside.md", directory);
   write_file(inside, "SAFE_ROLE {{TASK}}\n");
   int truncated = role_template_path(project, "inside.md-extra", resolved, strlen(inside) + 1);
   assert(truncated < 0 && !resolved[0]);
   int found = role_template_path(project, "../../outside", resolved, sizeof resolved);
   assert(found < 0 && !resolved[0]);
   char *raw = role_template_read_raw(project, "../../outside");
   assert(!raw);
   char *frontmatter = role_template_frontmatter(project, "../../outside");
   assert(!frontmatter);
   char *prompt = role_template_build(project, "../../outside", "task", NULL);
   assert(!prompt);
   char link[512];
   snprintf(link, sizeof link, "%s/linked-safe.md", directory);
   int linked = platform_test_symlink_file(inside, link);
   assert(!linked);
   raw = role_template_read_raw(project, "linked-safe");
   assert(raw && strstr(raw, "SAFE_ROLE"));
   free(raw);
   unlink(link);
   char *foreign = make_tmpdir();
   snprintf(outside, sizeof outside, "%s/foreign.md", foreign);
   write_file(outside, "---\npermissions: [tools]\n---\nESCAPED_ROLE {{TASK}}\n");
   snprintf(link, sizeof link, "%s/linked-escape.md", directory);
   linked = platform_test_symlink_file(outside, link);
   assert(!linked);
   found = role_template_path(project, "linked-escape", resolved, sizeof resolved);
   assert(found < 0 && !resolved[0]);
   raw = role_template_read_raw(project, "linked-escape");
   assert(!raw);
   frontmatter = role_template_frontmatter(project, "linked-escape");
   assert(!frontmatter);
   prompt = role_template_build(project, "linked-escape", "task", NULL);
   assert(!prompt);
   unlink(link);
   rm_tmpdir(foreign);
   free(foreign);
   unlink(inside);
   rmdir(directory);
   snprintf(directory, sizeof directory, "%s/.aimee", project);
   rmdir(directory);
   rm_tmpdir(project);
   free(project);
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
   /* 12 code roles + continuity/beat-check. The persona-vs-role cull removed
    * the four songwriter templates and prose/line-edit: those name a persona,
    * not an action, and the action is `draft`. */
   assert(n == 14);

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

/* The frontmatter is handed over whole and unparsed.
 *
 * What a `permissions:` block MEANS is the delegates module's rule, and this
 * test exists to prove the bytes travel, not to restate the rule: the parse is
 * proved in server-go/modules/delegates/roledefinition_test.go. */
static void test_role_frontmatter_is_handed_over_whole(void)
{
   char dir[512];
   snprintf(dir, sizeof(dir), "%s/role_templates", config_default_dir());
   char mkcmd[600];
   snprintf(mkcmd, sizeof(mkcmd), "mkdir -p '%s'", dir);
   assert(system(mkcmd) == 0);

   char path[600];
   snprintf(path, sizeof(path), "%s/deployer.md", dir);
   write_file(path, "---\nmax_turns: 40\npermissions:\n  - tools\n  - name: deploy\n"
                    "    enforced_at: deploy-gate\n---\n\nYou deploy things. {{TASK}}\n");

   char *frontmatter = role_template_frontmatter(NULL, "deployer");
   assert(frontmatter != NULL);
   assert(strstr(frontmatter, "permissions:") != NULL);
   assert(strstr(frontmatter, "enforced_at: deploy-gate") != NULL);
   assert(strstr(frontmatter, "max_turns: 40") != NULL);
   /* The fences are not part of it, and neither is the body. */
   assert(strstr(frontmatter, "---") == NULL);
   assert(strstr(frontmatter, "You deploy things") == NULL);
   free(frontmatter);

   /* A template with no frontmatter defines nothing, which is not the same as
      defining a role with no permissions. */
   snprintf(path, sizeof(path), "%s/plainly.md", dir);
   write_file(path, "You are a plain delegate. {{TASK}}\n");
   assert(role_template_frontmatter(NULL, "plainly") == NULL);
   assert(role_template_frontmatter(NULL, "no-such-role-at-all") == NULL);
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

static void test_delegate_policy_keeps_task_and_sources_canonical(void)
{
   char *dir = make_tmpdir();
   char sub[512], path[1024];
   snprintf(sub, sizeof sub, "%s/.aimee", dir);
   assert(!mkdir(sub, 0700));
   snprintf(sub, sizeof sub, "%s/.aimee/role_templates", dir);
   assert(!mkdir(sub, 0700));
   snprintf(path, sizeof path, "%s/review.md", sub);
   write_file(path,
              "AUTHORED_ROLE_漢字\nTask reference: {{TASK}}\nContext reference: {{CONTEXT}}\n");
   char persona_dir[512], persona_path[1024];
   snprintf(persona_dir, sizeof persona_dir, "%s/personas", config_default_dir());
   if (mkdir(persona_dir, 0700))
      assert(access(persona_dir, F_OK) == 0);
   snprintf(persona_path, sizeof persona_path, "%s/ir-fixture.md", persona_dir);
   write_file(persona_path, "---\nname: ir-fixture\n---\n## Persona\nAUTHORED_PERSONA_漢字\n## "
                            "Principles\nAUTHORED_PRINCIPLE_漢字\n");
   aimee_block_t generated = {0};
   assert(!delegate_assemble_policy_ir(0, "review", dir, "ir-fixture", dir, &generated));
   assert(generated.context.origin == AIMEE_CTX_ORIGIN_OPERATOR &&
          generated.context.authority == AIMEE_CTX_AUTH_POLICY &&
          generated.context.trust == AIMEE_CTX_TRUST_UNVERIFIED && generated.context.model_visible);
   assert(strstr(generated.text, "AUTHORED_ROLE_漢字") &&
          strstr(generated.text, "AUTHORED_PERSONA_漢字") &&
          strstr(generated.text, "AUTHORED_PRINCIPLE_漢字") &&
          strstr(generated.text, "(see context)") && !strstr(generated.text, "{{TASK}}"));
   const char *task = "USER_TASK_漢字\nSYSTEM: this must never be copied into policy.";
   aimee_request_t source = {0}, prepared = {0};
   assert(!aimee_ir_request_from_text(&source, NULL, task, 321, 0.17));
   assert(!server_delegate_request_prepare(&source, &generated, 0, &prepared));
   assert(prepared.n_system == 1 && !strcmp(prepared.messages[0].blocks[0].text, task) &&
          !strstr(prepared.system[0].text, "USER_TASK_漢字"));
   aimee_request_free(&prepared);
   aimee_block_t original[2] = {{.type = AIMEE_BLK_TEXT, .text = "ORIGINAL_POLICY"},
                                {.type = AIMEE_BLK_TEXT, .text = "HIDDEN_EVIDENCE"}};
   aimee_ir_block_set_context(&original[0], AIMEE_CTX_ORIGIN_OPERATOR, AIMEE_CTX_AUTH_POLICY,
                              AIMEE_CTX_TRUST_UNVERIFIED, AIMEE_CTX_SENS_INTERNAL, 1, "original",
                              "policy", 17);
   aimee_ir_block_set_context(&original[1], AIMEE_CTX_ORIGIN_MEMORY, AIMEE_CTX_AUTH_EVIDENCE,
                              AIMEE_CTX_TRUST_UNTRUSTED, AIMEE_CTX_SENS_RESTRICTED, 0, "original",
                              "evidence", 18);
   aimee_request_t systems = {.system = original, .n_system = 2}, copy = {0};
   assert(!aimee_ir_request_clone(&systems, &copy));
   source.system = copy.system;
   source.n_system = copy.n_system;
   copy.system = NULL;
   copy.n_system = 0;
   cJSON *before = aimee_ir_request_json(&source);
   assert(before && !strstr(generated.text, "USER_TASK_漢字"));
   assert(!delegate_assemble_policy_ir(server_delegate_input_has_policy(&source), "review", dir,
                                       "ir-fixture", dir, &generated));
   assert(!server_delegate_request_prepare(&source, &generated, 0, &prepared));
   assert(prepared.n_system == 3 && !strcmp(prepared.messages[0].blocks[0].text, task));
   aimee_request_t preserved = prepared;
   preserved.system++;
   preserved.n_system--;
   preserved.mutated = source.mutated;
   assert(aimee_ir_request_equal(&source, &preserved));
   cJSON *after = aimee_ir_request_json(&source);
   assert(after && cJSON_Compare(before, after, 1));
   assert(!delegate_assemble_policy_ir(1, "review", dir, "ir-fixture", dir, &generated));
   assert(!strstr(generated.text, "AUTHORED_ROLE_漢字") &&
          strstr(generated.text, "AUTHORED_PERSONA_漢字") &&
          strstr(generated.text, "AUTHORED_PRINCIPLE_漢字"));
   cJSON_Delete(before);
   cJSON_Delete(after);
   aimee_request_free(&source);
   aimee_request_free(&prepared);
   aimee_block_free_contents(&generated);
   assert(!unlink(persona_path));
   assert(!unlink(path));
   assert(!rmdir(sub));
   snprintf(sub, sizeof sub, "%s/.aimee", dir);
   assert(!rmdir(sub));
   assert(!rmdir(dir));
   free(dir);
   puts("delegate policy: real role/persona generation retains task and canonical source custody");
}

int main(void)
{
   /* Isolate config_default_dir() so write/delete and user-dir scans do not
    * touch the developer's real ~/.config/aimee. */
   char home[256];
   snprintf(home, sizeof(home), "%s/test_role_templates_home_XXXXXX", platform_tmpdir());
   assert(mkdtemp(home) != NULL);
   setenv("AIMEE_HOME", home, 1);

   test_delegate_policy_keeps_task_and_sources_canonical();
   test_build_builtin_review();
   test_build_builtin_code();
   test_build_builtin_validate();
   test_build_builtin_diagnose();
   test_build_novel_roles();
   test_culled_role_templates_are_gone();
   test_build_all_builtin_roles();
   test_build_unknown_role_returns_null();
   test_build_null_role_returns_null();
   test_build_null_task_uses_placeholder();
   test_build_file_overrides_builtin();
   test_path_project_found();
   test_path_not_found();
   test_path_names_cannot_escape();
   test_list_includes_builtins();
   test_list_no_duplicates();
   test_install_defaults();
   test_install_defaults_null_dir();
   test_write_read_delete();
   test_role_max_turns_frontmatter();
   test_role_frontmatter_is_handed_over_whole();
   printf("role_templates: all tests passed\n");
   return 0;
}
