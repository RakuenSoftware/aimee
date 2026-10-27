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
static int g_proxy_called;

int aimee_client_has_remote(void)
{
   return g_remote_active;
}
int cli_proxy_launch(char *const argv[], const char *sid)
{
   assert(strcmp(sid, g_worktree_sid) == 0);
   assert(g_chdir_order > 0);
   assert(strcmp(argv[0], "example-client") == 0);
   g_proxy_called++;
   return 127;
}

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
   g_proxy_called = 0;
   assert(client_launch_exec(3, gateway) == 127);
   assert(g_proxy_called == 1);
   assert(g_env_count == 1); /* proxy owns its local endpoint and credentials */

   g_remote_active = 0;
   g_env_count = 0;
   g_exec_file[0] = '\0';
   assert(client_launch_exec(3, gateway) == 1);
   assert(g_exec_file[0] == '\0');
   assert(g_proxy_called == 1);
   g_remote_active = 1;
   g_worktree_result = -1;
}

int main(void)
{
   test_launch_requires_a_client_command();
   test_launch_owns_session_worktree_and_cwd_before_exec();
   test_launch_gateway_is_explicit_and_fail_closed();
   printf("cli_launch: all tests passed\n");
   return 0;
}
