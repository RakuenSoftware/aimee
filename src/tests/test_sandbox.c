/* test_sandbox.c: unit tests for sandbox config parsing, container detection,
 * availability checks, and the degraded-isolation audit hook. */
#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "sandbox.h"

/* -------------------------------------------------------------------------
 * sandbox_mode_from_string / sandbox_mode_to_string
 * ---------------------------------------------------------------------- */

static void test_mode_from_string(void)
{
   assert(sandbox_mode_from_string("off") == SANDBOX_MODE_OFF);
   assert(sandbox_mode_from_string("workspace_only") == SANDBOX_MODE_WORKSPACE_ONLY);
   assert(sandbox_mode_from_string("allowlist") == SANDBOX_MODE_ALLOWLIST);
   /* Unknown input falls back to off */
   assert(sandbox_mode_from_string("unknown") == SANDBOX_MODE_OFF);
   assert(sandbox_mode_from_string(NULL) == SANDBOX_MODE_OFF);
   assert(sandbox_mode_from_string("") == SANDBOX_MODE_OFF);
}

static void test_mode_to_string(void)
{
   assert(strcmp(sandbox_mode_to_string(SANDBOX_MODE_OFF), "off") == 0);
   assert(strcmp(sandbox_mode_to_string(SANDBOX_MODE_WORKSPACE_ONLY), "workspace_only") == 0);
   assert(strcmp(sandbox_mode_to_string(SANDBOX_MODE_ALLOWLIST), "allowlist") == 0);
}

static void test_mode_roundtrip(void)
{
   const char *modes[] = {"off", "workspace_only", "allowlist", NULL};
   for (int i = 0; modes[i]; i++)
   {
      sandbox_mode_t m = sandbox_mode_from_string(modes[i]);
      assert(strcmp(sandbox_mode_to_string(m), modes[i]) == 0);
   }
}

/* -------------------------------------------------------------------------
 * sandbox_detect_container
 * ---------------------------------------------------------------------- */

static void test_detect_container_returns_int(void)
{
   /* Just verify it returns a valid boolean (0 or 1) without crashing */
   int r = sandbox_detect_container();
   assert(r == 0 || r == 1);
}

/* -------------------------------------------------------------------------
 * sandbox_available
 * ---------------------------------------------------------------------- */

static void test_available_returns_int_with_reason(void)
{
   const char *reason = NULL;
   int avail = sandbox_available(&reason);
   assert(avail == 0 || avail == 1);
   /* When unavailable, reason must be set */
   if (!avail)
      assert(reason != NULL && reason[0] != '\0');
}

static void test_available_null_reason_ok(void)
{
   /* Passing NULL for reason should not crash */
   int avail = sandbox_available(NULL);
   assert(avail == 0 || avail == 1);
}

/* -------------------------------------------------------------------------
 * sandbox_config_t defaults
 * ---------------------------------------------------------------------- */

static void test_config_defaults(void)
{
   sandbox_config_t cfg;
   memset(&cfg, 0, sizeof(cfg));
   assert(cfg.mode == SANDBOX_MODE_OFF);
   assert(cfg.network_isolated == 0);
   assert(cfg.allow_path_count == 0);
}

/* -------------------------------------------------------------------------
 * sandbox_command_program — NON-SECRET program extraction
 * ---------------------------------------------------------------------- */

static void test_command_program(void)
{
   char out[64];

   sandbox_command_program("npm install left-pad", out, sizeof out);
   assert(strcmp(out, "npm") == 0);

   sandbox_command_program("/usr/bin/python3 -c 'print(1)'", out, sizeof out);
   assert(strcmp(out, "python3") == 0);

   /* Leading env assignments (which may carry secrets) are skipped, and their
    * values NEVER appear in the extracted program name. */
   sandbox_command_program("TOKEN=sk-secret-abc curl https://x/api?key=sk-secret-abc", out,
                           sizeof out);
   assert(strcmp(out, "curl") == 0);
   assert(strstr(out, "sk-secret") == NULL);

   sandbox_command_program("  FOO=1 BAR=2 /bin/ls -la", out, sizeof out);
   assert(strcmp(out, "ls") == 0);

   /* A value with no shell-special byte (even one containing '=') is a simple
    * assignment and is skipped whole. */
   sandbox_command_program("URL=http://x/a-b_c /usr/bin/wget", out, sizeof out);
   assert(strcmp(out, "wget") == 0);

   /* The first program of a pipeline is named; the rest are arguments (no leak). */
   sandbox_command_program("npm run build | tee out.log", out, sizeof out);
   assert(strcmp(out, "npm") == 0);

   /* --- adversarial forms that MUST NOT leak: each yields the "unparsed" marker,
    * never the secret-bearing bytes. These pin the safe-by-construction parser. */
   const char *leaky[] = {
       "(TOKEN=sk-secret-123 npm publish)",         /* subshell prefix */
       "PASS='hunter2 secretword' /usr/bin/deploy", /* quoted value spanning ws */
       "TOKEN=$(cat /run/secrets/api_key) curl x",  /* command substitution */
       "'TOKEN=sk-secret' cmd",                     /* leading quote */
       "\"KEY=sk-secret\" cmd",                     /* leading dquote */
       "FOO=a\\ b realcmd",                         /* backslash-escaped space */
       "FOO=x;curl evil",                           /* metachar in value */
       "$SECRETVAR",                                /* leading '$' */
       "`id`",                                      /* backtick */
       NULL,
   };
   for (int i = 0; leaky[i]; i++)
   {
      sandbox_command_program(leaky[i], out, sizeof out);
      assert(strcmp(out, "unparsed") == 0);
      assert(strstr(out, "secret") == NULL && strstr(out, "hunter2") == NULL &&
             strstr(out, "api_key") == NULL);
   }

   sandbox_command_program("", out, sizeof out);
   assert(out[0] == '\0');
   out[0] = 'x';
   sandbox_command_program(NULL, out, sizeof out);
   assert(out[0] == '\0');

   printf("  PASS: test_command_program\n");
}

/* -------------------------------------------------------------------------
 * degraded-isolation audit hook
 * ---------------------------------------------------------------------- */

static struct
{
   int calls;
   char program[64];
   sandbox_mode_t mode;
   int network_isolated;
   char verdict[32];
   char reason[64];
} g_last;

static void capture_hook(const char *program, sandbox_mode_t mode, int network_isolated,
                         const char *verdict, const char *reason)
{
   g_last.calls++;
   snprintf(g_last.program, sizeof g_last.program, "%s", program);
   g_last.mode = mode;
   g_last.network_isolated = network_isolated;
   snprintf(g_last.verdict, sizeof g_last.verdict, "%s", verdict);
   snprintf(g_last.reason, sizeof g_last.reason, "%s", reason);
}

/* The override forces the sandbox "unavailable" so the degraded paths are
 * deterministically reached regardless of the host kernel. */
static int avail_unavailable(const char **reason)
{
   if (reason)
      *reason = "forced-unavailable (test)";
   return 0;
}

/* Forces "available" so the real sandboxed path is taken (which must NOT audit). */
static int avail_available(const char **reason)
{
   (void)reason;
   return 1;
}

static void test_audit_hook_degraded(void)
{
   int devnull = open("/dev/null", O_WRONLY);
   assert(devnull >= 0);

   sandbox_set_available_override_for_test(avail_unavailable);
   sandbox_set_audit_hook(capture_hook);

   sandbox_config_t cfg;
   memset(&cfg, 0, sizeof cfg);
   cfg.mode = SANDBOX_MODE_WORKSPACE_ONLY;
   cfg.network_isolated = 1;

   /* A non-require-isolation guarded exec falls back to running UNSANDBOXED when
    * the sandbox is unavailable — audited, and the command's secret-bearing env
    * assignment must not leak (only the program name crosses). */
   memset(&g_last, 0, sizeof g_last);
   pid_t pid = sandbox_exec(&cfg, "TOKEN=sk-leak-9z /usr/bin/true", devnull, devnull, NULL);
   assert(pid > 0);
   waitpid(pid, NULL, 0);
   assert(g_last.calls == 1);
   assert(strcmp(g_last.program, "true") == 0);
   assert(strstr(g_last.program, "sk-leak") == NULL);
   assert(g_last.mode == SANDBOX_MODE_WORKSPACE_ONLY);
   assert(g_last.network_isolated == 1);
   assert(strcmp(g_last.verdict, "unsandboxed_fallback") == 0);
   assert(g_last.reason[0] != '\0');

   /* A require-isolation exec is REFUSED (-1) rather than downgraded — audited. */
   memset(&g_last, 0, sizeof g_last);
   pid_t rc =
       sandbox_exec_with_readonly(&cfg, "curl https://x", devnull, devnull, NULL, NULL, NULL);
   assert(rc == -1);
   assert(g_last.calls == 1);
   assert(strcmp(g_last.program, "curl") == 0);
   assert(strcmp(g_last.verdict, "refused") == 0);

   /* mode==off is NOT a degradation: it runs unsandboxed by policy, no audit. */
   memset(&g_last, 0, sizeof g_last);
   sandbox_config_t off;
   memset(&off, 0, sizeof off);
   off.mode = SANDBOX_MODE_OFF;
   pid = sandbox_exec(&off, "/usr/bin/true", devnull, devnull, NULL);
   assert(pid > 0);
   waitpid(pid, NULL, 0);
   assert(g_last.calls == 0);

   /* When the sandbox IS available, the exec takes the real isolated path and the
    * degradation hook must stay SILENT — only exceptions are recorded, never a
    * routine sandboxed run. (The child may still degrade post-fork on a kernel
    * without userns, but that non-require path is out of parent-audit scope.) */
   sandbox_set_audit_hook(capture_hook);
   sandbox_set_available_override_for_test(avail_available);
   memset(&g_last, 0, sizeof g_last);
   pid = sandbox_exec(&cfg, "/usr/bin/true", devnull, devnull, NULL);
   if (pid > 0)
      waitpid(pid, NULL, 0);
   assert(g_last.calls == 0);

   /* A NULL hook records nothing (no crash). */
   sandbox_set_available_override_for_test(avail_unavailable);
   sandbox_set_audit_hook(NULL);
   memset(&g_last, 0, sizeof g_last);
   pid = sandbox_exec(&cfg, "/usr/bin/true", devnull, devnull, NULL);
   assert(pid > 0);
   waitpid(pid, NULL, 0);
   assert(g_last.calls == 0);

   sandbox_set_available_override_for_test(NULL);
   close(devnull);
   printf("  PASS: test_audit_hook_degraded\n");
}

/* -------------------------------------------------------------------------
 * main
 * ---------------------------------------------------------------------- */

int main(void)
{
   test_mode_from_string();
   test_mode_to_string();
   test_mode_roundtrip();
   test_detect_container_returns_int();
   test_available_returns_int_with_reason();
   test_available_null_reason_ok();
   test_config_defaults();
   test_command_program();
   test_audit_hook_degraded();

   printf("sandbox: all tests passed\n");
   return 0;
}
