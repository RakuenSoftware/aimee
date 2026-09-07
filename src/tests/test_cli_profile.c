/* test_cli_profile.c: -p / --profile flag extraction. */

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "cli_profile.h"

static void test_no_flag_no_change(void)
{
   char *argv[] = {(char *)"aimee", (char *)"status"};
   int argc = 2;
   char out[64] = "untouched";
   assert(cli_extract_profile(&argc, argv, out, sizeof(out)) == 0);
   assert(argc == 2);
   assert(strcmp(out, "untouched") == 0);
   printf("  PASS: test_no_flag_no_change\n");
}

static void test_short_flag_at_position_1(void)
{
   char *argv[] = {(char *)"aimee", (char *)"-p", (char *)"coder", (char *)"status"};
   int argc = 4;
   char out[64] = {0};
   assert(cli_extract_profile(&argc, argv, out, sizeof(out)) == 1);
   assert(strcmp(out, "coder") == 0);
   /* argv shifted: ["aimee", "status"] */
   assert(argc == 2);
   assert(strcmp(argv[0], "aimee") == 0);
   assert(strcmp(argv[1], "status") == 0);
   printf("  PASS: test_short_flag_at_position_1\n");
}

static void test_long_flag_with_separate_value(void)
{
   char *argv[] = {(char *)"aimee", (char *)"--profile", (char *)"client-x", (char *)"memory",
                   (char *)"list"};
   int argc = 5;
   char out[64] = {0};
   assert(cli_extract_profile(&argc, argv, out, sizeof(out)) == 1);
   assert(strcmp(out, "client-x") == 0);
   assert(argc == 3);
   assert(strcmp(argv[1], "memory") == 0);
   assert(strcmp(argv[2], "list") == 0);
   printf("  PASS: test_long_flag_with_separate_value\n");
}

static void test_long_flag_equals_form(void)
{
   char *argv[] = {(char *)"aimee", (char *)"--profile=homelab", (char *)"hud"};
   int argc = 3;
   char out[64] = {0};
   assert(cli_extract_profile(&argc, argv, out, sizeof(out)) == 1);
   assert(strcmp(out, "homelab") == 0);
   /* Only one slot consumed. */
   assert(argc == 2);
   assert(strcmp(argv[1], "hud") == 0);
   printf("  PASS: test_long_flag_equals_form\n");
}

static void test_short_flag_in_middle_of_argv(void)
{
   char *argv[] = {(char *)"aimee", (char *)"memory", (char *)"-p", (char *)"coder",
                   (char *)"list"};
   int argc = 5;
   char out[64] = {0};
   assert(cli_extract_profile(&argc, argv, out, sizeof(out)) == 1);
   assert(strcmp(out, "coder") == 0);
   /* argv now: ["aimee", "memory", "list"] */
   assert(argc == 3);
   assert(strcmp(argv[0], "aimee") == 0);
   assert(strcmp(argv[1], "memory") == 0);
   assert(strcmp(argv[2], "list") == 0);
   printf("  PASS: test_short_flag_in_middle_of_argv\n");
}

static void test_short_flag_no_value_returns_zero(void)
{
   char *argv[] = {(char *)"aimee", (char *)"-p"};
   int argc = 2;
   char out[64] = "untouched";
   assert(cli_extract_profile(&argc, argv, out, sizeof(out)) == 0);
   /* argv unchanged on no-value. */
   assert(argc == 2);
   assert(strcmp(out, "untouched") == 0);
   printf("  PASS: test_short_flag_no_value_returns_zero\n");
}

static void test_empty_value_rejected(void)
{
   /* '--profile=' (empty value) returns 0; argv unchanged. */
   char *argv[] = {(char *)"aimee", (char *)"--profile=", (char *)"status"};
   int argc = 3;
   char out[64] = "untouched";
   assert(cli_extract_profile(&argc, argv, out, sizeof(out)) == 0);
   assert(argc == 3);
   assert(strcmp(out, "untouched") == 0);
   printf("  PASS: test_empty_value_rejected\n");
}

static void test_invalid_inputs(void)
{
   char *argv[] = {(char *)"aimee", (char *)"-p", (char *)"x"};
   int argc = 3;
   char out[64];
   /* NULL out / argv / argc rejected. */
   assert(cli_extract_profile(NULL, argv, out, sizeof(out)) == 0);
   assert(cli_extract_profile(&argc, NULL, out, sizeof(out)) == 0);
   assert(cli_extract_profile(&argc, argv, NULL, sizeof(out)) == 0);
   assert(cli_extract_profile(&argc, argv, out, 0) == 0);
   /* Empty argv (just program name) → no scan needed. */
   int argc1 = 1;
   char *argv1[] = {(char *)"aimee"};
   assert(cli_extract_profile(&argc1, argv1, out, sizeof(out)) == 0);
   printf("  PASS: test_invalid_inputs\n");
}

static void test_client_profile_after_terminator_is_preserved(void)
{
   const char *flags[] = {"-p", "--profile", "--profile=codex-profile"};
   for (size_t i = 0; i < sizeof(flags) / sizeof(flags[0]); i++)
   {
      char *argv[] = {"aimee", "launch",         "--gateway",     "--",
                      "codex", (char *)flags[i], "codex-profile", NULL};
      int argc = 7;
      char out[64] = "untouched";
      assert(cli_extract_profile(&argc, argv, out, sizeof(out)) == 0);
      assert(argc == 7 && strcmp(out, "untouched") == 0);
      assert(strcmp(argv[5], flags[i]) == 0);
   }
   printf("  PASS: client profiles after -- are preserved\n");
}

int main(void)
{
   printf("cli_profile:\n");
   test_no_flag_no_change();
   test_short_flag_at_position_1();
   test_long_flag_with_separate_value();
   test_long_flag_equals_form();
   test_short_flag_in_middle_of_argv();
   test_short_flag_no_value_returns_zero();
   test_empty_value_rejected();
   test_invalid_inputs();
   test_client_profile_after_terminator_is_preserved();
   printf("ok\n");
   return 0;
}
