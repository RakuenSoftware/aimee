#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include "cli_client.h"

/* client_launch_exec returns before these collaborators in the usage test;
 * stubs keep this focused unit independent of git/process platform objects. */
int platform_random_bytes(void *buf, size_t len)
{
   memset(buf, 0x5a, len);
   return 0;
}
int platform_getppid(void)
{
   return 42;
}
typedef struct
{
   char name[64];
   char value[512];
} captured_env_t;
static captured_env_t g_env[10];
static int g_env_count;
static char g_worktree_sid[128];
static char g_chdir_path[512], g_exec_file[256];
static int g_remote_active = 1;
static int g_worktree_result = -1, g_order, g_chdir_order, g_exec_order;

int platform_setenv(const char *name, const char *value)
{
   assert(g_env_count < (int)(sizeof g_env / sizeof g_env[0]));
   snprintf(g_env[g_env_count].name, sizeof g_env[g_env_count].name, "%s", name);
   snprintf(g_env[g_env_count].value, sizeof g_env[g_env_count].value, "%s", value);
   g_env_count++;
   return 0;
}
int aimee_client_remote_active(char *out, unsigned long cap)
{
   if (!g_remote_active)
      return 0;
   if (out && cap)
      snprintf(out, cap, "%s", "gateway.example:8910");
   return 1;
}
const char *aimee_client_transport_label(void)
{
   return "https://gateway.example:8910/v1/";
}
int aimee_client_remote_token(char *out, unsigned long cap)
{
   snprintf(out, cap, "%s", "test-token");
   return 1;
}
int client_session_worktree_ensure(const char *sid, char *out, size_t cap)
{
   snprintf(g_worktree_sid, sizeof g_worktree_sid, "%s", sid);
   if (out && cap)
      snprintf(out, cap, "%s", "/aimee-test-fixture/owned-worktree");
   return g_worktree_result;
}
int chdir(const char *path)
{
   snprintf(g_chdir_path, sizeof g_chdir_path, "%s", path);
   g_chdir_order = ++g_order;
   return 0;
}
int execvp(const char *file, char *const argv[])
{
   (void)argv;
   snprintf(g_exec_file, sizeof g_exec_file, "%s", file);
   g_exec_order = ++g_order;
   errno = ENOENT;
   return -1;
}

static void test_parse_basic_provider(void)
{
   launch_meta_t meta;
   const char *output = "session info\n__LAUNCH__{\"provider\":\"claude\",\"builtin\":false}\n";
   int rc = parse_launch_meta(output, &meta);
   assert(rc == 1);
   assert(strcmp(meta.provider, "claude") == 0);
   assert(meta.builtin == 0);
   assert(meta.worktree_cwd[0] == '\0');
   assert(meta.context_len == 13); /* "session info\n" */
}

static void test_parse_custom_provider(void)
{
   launch_meta_t meta;
   const char *output = "__LAUNCH__{\"provider\":\"cursor\",\"builtin\":false}\n";
   int rc = parse_launch_meta(output, &meta);
   assert(rc == 1);
   assert(strcmp(meta.provider, "cursor") == 0);
   assert(meta.context_len == 0);
}

static void test_parse_builtin(void)
{
   launch_meta_t meta;
   const char *output = "__LAUNCH__{\"provider\":\"claude\",\"builtin\":true}\n";
   int rc = parse_launch_meta(output, &meta);
   assert(rc == 1);
   assert(meta.builtin == 1);
}

static void test_parse_worktree_cwd(void)
{
   launch_meta_t meta;
   const char *output = "__LAUNCH__{\"provider\":\"claude\",\"builtin\":false,"
                        "\"worktree_cwd\":\"/tmp/wt/branch\"}\n";
   int rc = parse_launch_meta(output, &meta);
   assert(rc == 1);
   assert(strcmp(meta.worktree_cwd, "/tmp/wt/branch") == 0);
}

static void test_parse_model(void)
{
   launch_meta_t meta;
   const char *output = "__LAUNCH__{\"provider\":\"claude\",\"model\":\"opus-4\","
                        "\"builtin\":false}\n";
   int rc = parse_launch_meta(output, &meta);
   assert(rc == 1);
   assert(strcmp(meta.model, "opus-4") == 0);
}

static void test_parse_missing_provider_defaults_claude(void)
{
   launch_meta_t meta;
   const char *output = "__LAUNCH__{\"builtin\":false}\n";
   int rc = parse_launch_meta(output, &meta);
   assert(rc == 1);
   assert(strcmp(meta.provider, "claude") == 0);
}

static void test_parse_no_launch_marker(void)
{
   launch_meta_t meta;
   const char *output = "just some regular output, no marker here\n";
   int rc = parse_launch_meta(output, &meta);
   assert(rc == 0);
}

static void test_parse_empty_output(void)
{
   launch_meta_t meta;
   int rc = parse_launch_meta("", &meta);
   assert(rc == 0);
}

static void test_parse_invalid_json(void)
{
   launch_meta_t meta;
   const char *output = "__LAUNCH__not-json-at-all\n";
   int rc = parse_launch_meta(output, &meta);
   assert(rc == 0);
}

static void test_parse_no_trailing_newline(void)
{
   launch_meta_t meta;
   const char *output = "__LAUNCH__{\"provider\":\"aider\",\"builtin\":false}";
   int rc = parse_launch_meta(output, &meta);
   assert(rc == 1);
   assert(strcmp(meta.provider, "aider") == 0);
}

static void test_parse_long_provider_truncated(void)
{
   launch_meta_t meta;
   /* provider field is 64 bytes; a 100-char name should be truncated, not overflow */
   char output[256];
   snprintf(output, sizeof(output),
            "__LAUNCH__{\"provider\":\""
            "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
            "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"
            "\",\"builtin\":false}\n");
   int rc = parse_launch_meta(output, &meta);
   assert(rc == 1);
   assert(strlen(meta.provider) < 64);
}

static void test_parse_context_preserved(void)
{
   launch_meta_t meta;
   const char *output = "line1\nline2\nline3\n__LAUNCH__{\"provider\":\"claude\"}\ntrailing\n";
   int rc = parse_launch_meta(output, &meta);
   assert(rc == 1);
   assert(meta.context_len == 18); /* "line1\nline2\nline3\n" */
   /* Verify context_len points exactly to the marker */
   assert(strncmp(output + meta.context_len, "__LAUNCH__", 10) == 0);
}

static void test_parse_autonomous_true(void)
{
   launch_meta_t meta;
   const char *output =
       "__LAUNCH__{\"provider\":\"claude\",\"builtin\":false,\"autonomous\":true}\n";
   int rc = parse_launch_meta(output, &meta);
   assert(rc == 1);
   assert(strcmp(meta.provider, "claude") == 0);
   assert(meta.autonomous == 1);
}

static void test_parse_autonomous_false(void)
{
   launch_meta_t meta;
   const char *output =
       "__LAUNCH__{\"provider\":\"claude\",\"builtin\":false,\"autonomous\":false}\n";
   int rc = parse_launch_meta(output, &meta);
   assert(rc == 1);
   assert(meta.autonomous == 0);
}

static void test_parse_autonomous_absent_defaults_off(void)
{
   launch_meta_t meta;
   const char *output = "__LAUNCH__{\"provider\":\"claude\",\"builtin\":false}\n";
   int rc = parse_launch_meta(output, &meta);
   assert(rc == 1);
   assert(meta.autonomous == 0);
}

static void test_parse_autonomous_codex(void)
{
   launch_meta_t meta;
   const char *output =
       "__LAUNCH__{\"provider\":\"codex\",\"builtin\":false,\"autonomous\":true}\n";
   int rc = parse_launch_meta(output, &meta);
   assert(rc == 1);
   assert(strcmp(meta.provider, "codex") == 0);
   assert(meta.autonomous == 1);
}

static void test_launch_requires_a_client_command(void)
{
   assert(client_launch_exec(0, NULL) == 2);
   char *separator_only[] = {(char *)"--", NULL};
   assert(client_launch_exec(1, separator_only) == 2);
}

static void test_launch_owns_session_worktree_and_cwd_before_exec(void)
{
   memset(g_env, 0, sizeof g_env);
   g_env_count = 0;
   memset(g_worktree_sid, 0, sizeof g_worktree_sid);
   memset(g_chdir_path, 0, sizeof g_chdir_path);
   memset(g_exec_file, 0, sizeof g_exec_file);
   g_order = g_chdir_order = g_exec_order = 0;
   g_worktree_result = 0;
   char *argv[] = {(char *)"--", (char *)"example-client", (char *)"--flag", NULL};
   assert(client_launch_exec(3, argv) == 127); /* exec stub deliberately fails */
   assert(g_env_count == 1);
   assert(strcmp(g_env[0].name, "AIMEE_SESSION_ID") == 0);
   assert(strlen(g_env[0].value) == 32);
   assert(strcmp(g_env[0].value, g_worktree_sid) == 0);
   assert(strcmp(g_chdir_path, "/aimee-test-fixture/owned-worktree") == 0);
   assert(strcmp(g_exec_file, "example-client") == 0);
   assert(g_chdir_order > 0 && g_chdir_order < g_exec_order);
   g_worktree_result = -1;
}

static void test_launch_gateway_is_explicit_and_fail_closed(void)
{
   g_worktree_result = 0;
   g_env_count = 0;
   char *gateway[] = {(char *)"--gateway", (char *)"--", (char *)"example-client", NULL};
   assert(client_launch_exec(3, gateway) == 127);
   assert(g_env_count == 6);
   assert(strcmp(g_env[1].name, "AIMEE_CONVERSATION_GATEWAY") == 0);
   assert(strcmp(g_env[1].value, "https://gateway.example:8910/v1") == 0);

   g_remote_active = 0;
   g_env_count = 0;
   g_exec_file[0] = '\0';
   assert(client_launch_exec(3, gateway) == 1);
   assert(g_exec_file[0] == '\0');
   g_remote_active = 1;
   g_worktree_result = -1;
}

int main(void)
{
   test_parse_basic_provider();
   test_parse_custom_provider();
   test_parse_builtin();
   test_parse_worktree_cwd();
   test_parse_model();
   test_parse_missing_provider_defaults_claude();
   test_parse_no_launch_marker();
   test_parse_empty_output();
   test_parse_invalid_json();
   test_parse_no_trailing_newline();
   test_parse_long_provider_truncated();
   test_parse_context_preserved();
   test_parse_autonomous_true();
   test_parse_autonomous_false();
   test_parse_autonomous_absent_defaults_off();
   test_parse_autonomous_codex();
   test_launch_requires_a_client_command();
   test_launch_owns_session_worktree_and_cwd_before_exec();
   test_launch_gateway_is_explicit_and_fail_closed();
   printf("cli_launch: all tests passed\n");
   return 0;
}
