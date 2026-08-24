/* test_guardrails_computer_use.c: computer-use guarded capability policy. */
#include "computer_use.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static computer_use_policy_t base_policy(void)
{
   computer_use_policy_t p;
   memset(&p, 0, sizeof(p));
   p.enabled = 1;
   snprintf(p.default_navigation, sizeof(p.default_navigation), "%s", "approve");
   snprintf(p.allowed_domains[0], sizeof(p.allowed_domains[0]), "%s", "localhost");
   snprintf(p.allowed_domains[1], sizeof(p.allowed_domains[1]), "%s", "*.internal.example");
   p.allowed_domain_count = 2;
   p.redact_sensitive_screenshots = 1;
   return p;
}

static void expect_decision(const computer_use_policy_t *p, const char *tool, const char *args,
                            computer_use_decision_t want)
{
   computer_use_decision_t got = COMPUTER_USE_DECISION_ALLOW;
   char reason[256] = "";
   assert(computer_use_classify(p, tool, args, &got, reason, sizeof(reason)) == 1);
   assert(got == want);
   assert(reason[0] != '\0');
}

static void test_disabled_blocks_exposure_escape(void)
{
   computer_use_policy_t p = base_policy();
   p.enabled = 0;
   expect_decision(&p, "computer_use:screenshot", "{}", COMPUTER_USE_DECISION_BLOCK);
   assert(computer_use_is_tool_name("computer-use:navigate") == 1);
   assert(computer_use_is_tool_name("mock:navigate") == 0);
}

static void test_navigation_policy(void)
{
   computer_use_policy_t p = base_policy();
   expect_decision(&p, "computer_use:open_url", "{\"url\":\"http://localhost:3000\"}",
                   COMPUTER_USE_DECISION_ALLOW);
   expect_decision(&p, "computer_use:navigate", "{\"url\":\"https://app.internal.example/x\"}",
                   COMPUTER_USE_DECISION_ALLOW);
   expect_decision(&p, "computer_use:navigate", "{\"url\":\"https://example.com\"}",
                   COMPUTER_USE_DECISION_APPROVE);
   snprintf(p.default_navigation, sizeof(p.default_navigation), "%s", "block");
   expect_decision(&p, "computer_use:navigate", "{\"url\":\"https://example.com\"}",
                   COMPUTER_USE_DECISION_BLOCK);
}

static void test_sensitive_actions_require_approval(void)
{
   computer_use_policy_t p = base_policy();
   expect_decision(&p, "computer_use:type", "{\"field_type\":\"password\",\"text\":\"hunter2\"}",
                   COMPUTER_USE_DECISION_APPROVE);
   expect_decision(&p, "computer_use:click", "{\"label\":\"Pay now\"}",
                   COMPUTER_USE_DECISION_APPROVE);
   expect_decision(&p, "computer_use:screenshot", "{\"page_class\":\"login\"}",
                   COMPUTER_USE_DECISION_APPROVE);
   expect_decision(&p, "computer_use:screenshot", "{\"page_class\":\"dashboard\"}",
                   COMPUTER_USE_DECISION_ALLOW);
}

static void test_redaction(void)
{
   char out[256];
   int n = computer_use_redact_text("token=abc123 sk-testsecret ghp_secret", out, sizeof(out));
   assert(n == 1);
   assert(strstr(out, "[REDACTED]") != NULL);
   assert(strstr(out, "sk-testsecret") == NULL);
   assert(strstr(out, "ghp_secret") == NULL);
}

/* computer_use_policy_from_config reads live config rather than taking a
 * legacy_config_record, so the projection this case checks has to be written to a config
 * file the test owns — a hand-built struct no longer reaches the function. */
static void write_test_config(const char *yaml)
{
   char dir[256], path[320];
   snprintf(dir, sizeof(dir), "/tmp/aimee-computer-use-cfg-%d", (int)getpid());
   mkdir(dir, 0755);
   setenv("AIMEE_HOME", dir, 1);
   setenv("AIMEE_NO_CACHE", "1", 1);
   snprintf(path, sizeof(path), "%s/aimee.yaml", dir);
   FILE *f = fopen(path, "w");
   assert(f);
   fputs(yaml, f);
   fclose(f);
}

static void test_config_projection(void)
{
   write_test_config("computer_use:\n"
                     "  enabled: true\n"
                     "  default_navigation: \"block\"\n"
                     "  redact_sensitive_screenshots: false\n"
                     "  allowed_domains:\n"
                     "    - \"example.test\"\n");

   computer_use_policy_t p;
   computer_use_policy_from_config(&p);
   assert(p.enabled == 1);
   assert(strcmp(p.default_navigation, "block") == 0);
   assert(p.redact_sensitive_screenshots == 0);
   assert(p.allowed_domain_count == 1);
   assert(strcmp(p.allowed_domains[0], "example.test") == 0);
}

int main(void)
{
   test_disabled_blocks_exposure_escape();
   test_navigation_policy();
   test_sensitive_actions_require_approval();
   test_redaction();
   test_config_projection();
   printf("guardrails_computer_use: ok\n");
   return 0;
}
