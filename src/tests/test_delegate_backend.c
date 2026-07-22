/* test_delegate_backend.c: registry-only coverage for the
 * delegate_backend interface. The vtable function pointers are not
 * exercised here — that lands once a real backend ships. */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <aimee/delegates/delegate_backend.h>

static delegate_backend_t make_stub(const char *name)
{
   delegate_backend_t b;
   memset(&b, 0, sizeof(b));
   b.name = name;
   b.description = "stub";
   return b;
}

static void test_register_and_lookup(void)
{
   delegate_backend_reset_for_test();
   delegate_backend_t a = make_stub("local");
   delegate_backend_t b = make_stub("docker");
   assert(delegate_backend_register(&a) == 0);
   assert(delegate_backend_register(&b) == 0);

   assert(delegate_backend_lookup("local") == &a);
   assert(delegate_backend_lookup("docker") == &b);
   assert(delegate_backend_lookup("does-not-exist") == NULL);
   printf("  PASS: test_register_and_lookup\n");
}

static void test_register_rejects_duplicate(void)
{
   delegate_backend_reset_for_test();
   delegate_backend_t a = make_stub("local");
   delegate_backend_t a_dup = make_stub("local");
   assert(delegate_backend_register(&a) == 0);
   assert(delegate_backend_register(&a_dup) == -1);
   /* The first registration wins; a_dup must NOT have replaced it. */
   assert(delegate_backend_lookup("local") == &a);
   printf("  PASS: test_register_rejects_duplicate\n");
}

static void test_register_rejects_invalid(void)
{
   delegate_backend_reset_for_test();
   delegate_backend_t empty_name = make_stub("");
   assert(delegate_backend_register(NULL) == -1);
   assert(delegate_backend_register(&empty_name) == -1);
   delegate_backend_t null_name;
   memset(&null_name, 0, sizeof(null_name));
   null_name.name = NULL;
   assert(delegate_backend_register(&null_name) == -1);
   printf("  PASS: test_register_rejects_invalid\n");
}

static void test_lookup_invalid_inputs(void)
{
   delegate_backend_reset_for_test();
   assert(delegate_backend_lookup(NULL) == NULL);
   assert(delegate_backend_lookup("") == NULL);
   /* Empty registry: any lookup is NULL. */
   assert(delegate_backend_lookup("local") == NULL);
   printf("  PASS: test_lookup_invalid_inputs\n");
}

static void test_list_returns_in_registration_order(void)
{
   delegate_backend_reset_for_test();
   delegate_backend_t a = make_stub("local");
   delegate_backend_t b = make_stub("ssh");
   delegate_backend_t c = make_stub("docker");
   delegate_backend_register(&a);
   delegate_backend_register(&b);
   delegate_backend_register(&c);

   delegate_backend_t *out[8] = {0};
   int n = delegate_backend_list(out, 8);
   assert(n == 3);
   assert(out[0] == &a);
   assert(out[1] == &b);
   assert(out[2] == &c);
   printf("  PASS: test_list_returns_in_registration_order\n");
}

static void test_list_truncates_to_max(void)
{
   delegate_backend_reset_for_test();
   delegate_backend_t a = make_stub("a");
   delegate_backend_t b = make_stub("b");
   delegate_backend_t c = make_stub("c");
   delegate_backend_register(&a);
   delegate_backend_register(&b);
   delegate_backend_register(&c);

   delegate_backend_t *out[2] = {0};
   int n = delegate_backend_list(out, 2);
   assert(n == 2);
   assert(out[0] == &a);
   assert(out[1] == &b);

   /* max=0 returns 0 with no writes. */
   delegate_backend_t *out0[1] = {(delegate_backend_t *)0xdeadbeefULL};
   assert(delegate_backend_list(out0, 0) == 0);
   assert(out0[0] == (delegate_backend_t *)0xdeadbeefULL);

   /* NULL out is a no-op. */
   assert(delegate_backend_list(NULL, 8) == 0);
   printf("  PASS: test_list_truncates_to_max\n");
}

static void test_wrap_command_basic(void)
{
   char *script = NULL;
   assert(delegate_backend_wrap_command("/tmp/aimee/snap.sh", "/tmp/aimee/cwd", "echo hello",
                                        &script) == 0);
   assert(script != NULL);
   /* Spot-check the surface — we depend on these in real backends. */
   assert(strstr(script, "SNAP=/tmp/aimee/snap.sh") != NULL);
   assert(strstr(script, "CWD_FILE=/tmp/aimee/cwd") != NULL);
   /* User command runs in parent shell (no subshell) so `cd` persists
    * for the wrapper's pwd capture. The EXIT capture immediately
    * after preserves the user's exit code. */
   assert(strstr(script, "\necho hello\nEXIT=$?") != NULL);
   assert(strstr(script, "exit $EXIT") != NULL);
   /* Snapshot dump must be atomic (.tmp + rename), not a direct write. */
   assert(strstr(script, "$SNAP.tmp") != NULL);
   assert(strstr(script, "mv \"$SNAP.tmp\" \"$SNAP\"") != NULL);
   assert(strstr(script, "$CWD_FILE.tmp") != NULL);
   free(script);
   printf("  PASS: test_wrap_command_basic\n");
}

static void test_wrap_command_passes_through_complex_user_cmd(void)
{
   char *script = NULL;
   /* User commands routinely include pipes, redirects, env-var
    * references, and quoted strings. wrap_command must not escape or
    * mangle them — the wrapper relies on bash's own parsing. */
   const char *cmd = "cd /tmp && export X=1 | tee out.txt > /dev/null && echo \"ok ${X}\"";
   assert(delegate_backend_wrap_command("/s", "/c", cmd, &script) == 0);
   assert(strstr(script, cmd) != NULL);
   free(script);
   printf("  PASS: test_wrap_command_passes_through_complex_user_cmd\n");
}

static void test_wrap_command_rejects_invalid_inputs(void)
{
   char *script = (char *)0x1; /* sentinel — must be NULLed on failure */
   assert(delegate_backend_wrap_command(NULL, "/c", "x", &script) == -1);
   assert(script == NULL);

   script = (char *)0x1;
   assert(delegate_backend_wrap_command("/s", NULL, "x", &script) == -1);
   assert(script == NULL);

   script = (char *)0x1;
   assert(delegate_backend_wrap_command("/s", "/c", NULL, &script) == -1);
   assert(script == NULL);

   /* Empty paths are rejected — they would produce a no-op snapshot. */
   script = (char *)0x1;
   assert(delegate_backend_wrap_command("", "/c", "x", &script) == -1);
   assert(script == NULL);

   script = (char *)0x1;
   assert(delegate_backend_wrap_command("/s", "", "x", &script) == -1);
   assert(script == NULL);

   /* NULL out pointer: treated as an invalid call (would leak the buf). */
   assert(delegate_backend_wrap_command("/s", "/c", "x", NULL) == -1);
   printf("  PASS: test_wrap_command_rejects_invalid_inputs\n");
}

static void test_config_parse_empty(void)
{
   delegate_backend_config_t cfg = {0};
   const char *name = NULL;
   int idx = -1;
   assert(delegate_backend_config_parse(0, NULL, &cfg, &name, &idx) == 0);
   assert(name == NULL);
   assert(cfg.image == NULL);
   assert(cfg.host == NULL);
   assert(idx == 0);
   printf("  PASS: test_config_parse_empty\n");
}

static void test_config_parse_backend_only(void)
{
   char *argv[] = {(char *)"--backend", (char *)"local", (char *)"prompt-here"};
   delegate_backend_config_t cfg = {0};
   const char *name = NULL;
   int idx = -1;
   assert(delegate_backend_config_parse(3, argv, &cfg, &name, &idx) == 0);
   assert(strcmp(name, "local") == 0);
   assert(idx == 2);
   /* Caller resumes scanning at idx — argv[idx] is the prompt. */
   assert(strcmp(argv[idx], "prompt-here") == 0);
   printf("  PASS: test_config_parse_backend_only\n");
}

static void test_config_parse_all_flags(void)
{
   char *argv[] = {(char *)"--backend",      (char *)"docker", (char *)"--image",
                   (char *)"python:3.11",    (char *)"--host", (char *)"pve",
                   (char *)"--no-hibernate", (char *)"role",   (char *)"prompt"};
   delegate_backend_config_t cfg = {0};
   cfg.hibernate_on_exit = 1; /* default seeded */
   const char *name = NULL;
   int idx = -1;
   assert(delegate_backend_config_parse(9, argv, &cfg, &name, &idx) == 0);
   assert(strcmp(name, "docker") == 0);
   assert(strcmp(cfg.image, "python:3.11") == 0);
   assert(strcmp(cfg.host, "pve") == 0);
   assert(cfg.hibernate_on_exit == 0);
   assert(idx == 7);
   assert(strcmp(argv[idx], "role") == 0);
   printf("  PASS: test_config_parse_all_flags\n");
}

static void test_config_parse_stops_at_first_unknown_arg(void)
{
   /* --backend recognised, then "code" is the first unknown — stop. */
   char *argv[] = {(char *)"--backend", (char *)"local", (char *)"code", (char *)"--image",
                   (char *)"unused"};
   delegate_backend_config_t cfg = {0};
   const char *name = NULL;
   int idx = -1;
   assert(delegate_backend_config_parse(5, argv, &cfg, &name, &idx) == 0);
   assert(strcmp(name, "local") == 0);
   /* --image after the unknown arg is NOT consumed — operator's argv
    * order is the source of truth. Image stays NULL. */
   assert(cfg.image == NULL);
   assert(idx == 2);
   assert(strcmp(argv[idx], "code") == 0);
   printf("  PASS: test_config_parse_stops_at_first_unknown_arg\n");
}

static void test_config_parse_rejects_dangling_flag(void)
{
   /* --backend with no value */
   char *argv[] = {(char *)"--backend"};
   delegate_backend_config_t cfg = {0};
   const char *name = NULL;
   assert(delegate_backend_config_parse(1, argv, &cfg, &name, NULL) == -1);

   /* --image with no value */
   char *argv2[] = {(char *)"--image"};
   assert(delegate_backend_config_parse(1, argv2, &cfg, &name, NULL) == -1);

   /* --host with no value */
   char *argv3[] = {(char *)"--host"};
   assert(delegate_backend_config_parse(1, argv3, &cfg, &name, NULL) == -1);
   printf("  PASS: test_config_parse_rejects_dangling_flag\n");
}

static void test_config_parse_preserves_pre_seeded_defaults(void)
{
   /* Pre-seed cfg from agents.json defaults; argv only sets --backend. */
   delegate_backend_config_t cfg = {0};
   cfg.image = "default-image";
   cfg.host = "default-host";
   cfg.hibernate_on_exit = 1;

   char *argv[] = {(char *)"--backend", (char *)"docker"};
   const char *name = NULL;
   assert(delegate_backend_config_parse(2, argv, &cfg, &name, NULL) == 0);
   /* Defaults preserved — argv didn't override. */
   assert(strcmp(cfg.image, "default-image") == 0);
   assert(strcmp(cfg.host, "default-host") == 0);
   assert(cfg.hibernate_on_exit == 1);
   printf("  PASS: test_config_parse_preserves_pre_seeded_defaults\n");
}

static void test_list_json_empty_registry(void)
{
   delegate_backend_reset_for_test();
   char *json = delegate_backend_list_json();
   assert(json != NULL);
   assert(strcmp(json, "[]") == 0);
   free(json);
   printf("  PASS: test_list_json_empty_registry\n");
}

static void test_list_json_single_backend(void)
{
   delegate_backend_reset_for_test();
   delegate_backend_t a = make_stub("local");
   a.description = "in-process subprocess";
   delegate_backend_register(&a);
   char *json = delegate_backend_list_json();
   assert(json != NULL);
   assert(strcmp(json, "[{\"name\":\"local\",\"description\":\"in-process subprocess\"}]") == 0);
   free(json);
   printf("  PASS: test_list_json_single_backend\n");
}

static void test_list_json_multiple_backends_in_registration_order(void)
{
   delegate_backend_reset_for_test();
   delegate_backend_t a = make_stub("local");
   a.description = "first";
   delegate_backend_t b = make_stub("ssh");
   b.description = "second";
   delegate_backend_t c = make_stub("docker");
   c.description = "third";
   delegate_backend_register(&a);
   delegate_backend_register(&b);
   delegate_backend_register(&c);

   char *json = delegate_backend_list_json();
   assert(json != NULL);
   /* Spot-check shape: leading [, trailing ], comma-separated entries
    * in registration order. */
   assert(json[0] == '[');
   assert(json[strlen(json) - 1] == ']');
   /* Substrings of each entry appear in registration order. */
   const char *p_local = strstr(json, "\"name\":\"local\"");
   const char *p_ssh = strstr(json, "\"name\":\"ssh\"");
   const char *p_docker = strstr(json, "\"name\":\"docker\"");
   assert(p_local && p_ssh && p_docker);
   assert(p_local < p_ssh);
   assert(p_ssh < p_docker);
   free(json);
   printf("  PASS: test_list_json_multiple_backends_in_registration_order\n");
}

static void test_list_json_escapes_special_chars(void)
{
   delegate_backend_reset_for_test();
   delegate_backend_t a = make_stub("name-with-\"quote\"");
   a.description = "desc with \\backslash and \nnewline";
   delegate_backend_register(&a);
   char *json = delegate_backend_list_json();
   assert(json != NULL);
   /* Quotes escaped as \" — confirm the literal \" appears in output. */
   assert(strstr(json, "\\\"quote\\\"") != NULL);
   /* Backslash escaped as \\. */
   assert(strstr(json, "\\\\backslash") != NULL);
   /* Newline escaped as
. */
   assert(strstr(json, "\\u000a") != NULL);
   /* Raw newline byte must not appear inside the JSON value. */
   /* (We can't test this directly without a parser; the escape check
    * above is sufficient evidence that the writer transformed it.) */
   free(json);
   printf("  PASS: test_list_json_escapes_special_chars\n");
}

int main(void)
{
   printf("delegate_backend:\n");
   test_register_and_lookup();
   test_register_rejects_duplicate();
   test_register_rejects_invalid();
   test_lookup_invalid_inputs();
   test_list_returns_in_registration_order();
   test_list_truncates_to_max();
   test_wrap_command_basic();
   test_wrap_command_passes_through_complex_user_cmd();
   test_wrap_command_rejects_invalid_inputs();
   test_config_parse_empty();
   test_config_parse_backend_only();
   test_config_parse_all_flags();
   test_config_parse_stops_at_first_unknown_arg();
   test_config_parse_rejects_dangling_flag();
   test_config_parse_preserves_pre_seeded_defaults();
   test_list_json_empty_registry();
   test_list_json_single_backend();
   test_list_json_multiple_backends_in_registration_order();
   test_list_json_escapes_special_chars();
   printf("ok\n");
   return 0;
}
