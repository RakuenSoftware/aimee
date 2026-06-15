/* test_server_cli_oauth.c: the security-critical pure helpers of the OAuth-CLI
 * module — vendor allowlist, URL/code scraping, and paste-back-code validation.
 * Real pane samples come from the 2026-06-15 spike. */
#include "server_cli_oauth.h"
#include <assert.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

/* --- stubs for the linked-but-unexercised deps of server_cli_oauth.o --- */
int safe_exec_capture_env(const char *const argv[], char *const envp[], char **out_buf,
                          size_t max_out)
{
   (void)argv;
   (void)envp;
   (void)max_out;
   if (out_buf)
      *out_buf = NULL;
   return -1;
}
int platform_mkdir_p(const char *path, int mode)
{
   (void)path;
   (void)mode;
   return 0;
}
void aimee_log(int level, const char *module, const char *fmt, ...)
{
   (void)level;
   (void)module;
   (void)fmt;
}

static const char *CLAUDE_PANE =
    "Browser didn't open? Use the url below to sign in (c to copy)\n\n"
    "https://claude.com/cai/oauth/authorize?code=true&client_id=9d1c250a-e61b-44d9-88ed"
    "&response_type=code&scope=user%3Ainference\n\n Paste code here if prompted >";

static const char *CODEX_PANE =
    "Welcome to Codex\nFollow these steps to sign in with ChatGPT using device code "
    "authorization:\n"
    "1. Open this link in your browser and sign in to your account\n"
    "   https://auth.openai.com/codex/device\n"
    "2. Enter this one-time code (expires in 15 minutes)\n"
    "   OHLP-GPWP9\nDevice codes are a common phishing target. Never share this code.\n";

static void test_vendor_parse(void)
{
   cli_oauth_vendor_t v;
   assert(cli_oauth_vendor_parse("claude", &v) == 0 && v == CLI_OAUTH_CLAUDE);
   assert(cli_oauth_vendor_parse("claude-oauth", &v) == 0 && v == CLI_OAUTH_CLAUDE);
   assert(cli_oauth_vendor_parse("codex", &v) == 0 && v == CLI_OAUTH_CODEX);
   assert(cli_oauth_vendor_parse("codex-oauth", &v) == 0 && v == CLI_OAUTH_CODEX);
   /* anything outside the closed set is rejected before any process runs */
   assert(cli_oauth_vendor_parse("claude; rm -rf /", &v) == -1);
   assert(cli_oauth_vendor_parse("../evil", &v) == -1);
   assert(cli_oauth_vendor_parse("", &v) == -1);
   assert(cli_oauth_vendor_parse(NULL, &v) == -1);
   assert(strcmp(cli_oauth_vendor_name(CLI_OAUTH_CLAUDE), "claude") == 0);
   assert(strcmp(cli_oauth_vendor_name(CLI_OAUTH_CODEX), "codex") == 0);
   printf("  test_vendor_parse: ok\n");
}

static void test_scrape_url(void)
{
   char url[1024];
   assert(cli_oauth_scrape_url(CLAUDE_PANE, "https://claude.com/", url, sizeof(url)) == 0);
   assert(strncmp(url, "https://claude.com/cai/oauth/authorize?code=true", 47) == 0);
   assert(strchr(url, ' ') == NULL && strchr(url, '\n') == NULL); /* stops at whitespace */

   assert(cli_oauth_scrape_url(CODEX_PANE, "https://auth.openai.com/codex/device", url,
                               sizeof(url)) == 0);
   assert(strcmp(url, "https://auth.openai.com/codex/device") == 0);

   /* anchor absent -> no URL */
   assert(cli_oauth_scrape_url("no url here", "https://claude.com/", url, sizeof(url)) == -1);
   printf("  test_scrape_url: ok\n");
}

static void test_scrape_codex_code(void)
{
   char code[64];
   assert(cli_oauth_scrape_codex_code(CODEX_PANE, code, sizeof(code)) == 0);
   assert(strcmp(code, "OHLP-GPWP9") == 0);
   /* no device-code-shaped token -> not found */
   assert(cli_oauth_scrape_codex_code("just some prose without a code", code, sizeof(code)) == -1);
   printf("  test_scrape_codex_code: ok\n");
}

static void test_code_is_safe(void)
{
   assert(cli_oauth_code_is_safe("sk-ant-oat01_AbC-123.xyz/Q+w=="));
   assert(cli_oauth_code_is_safe("OHLP-GPWP9"));
   assert(!cli_oauth_code_is_safe("code; rm -rf /")); /* space + ; rejected */
   assert(!cli_oauth_code_is_safe("with space"));
   assert(!cli_oauth_code_is_safe("back`tick`"));
   assert(!cli_oauth_code_is_safe("new\nline"));
   assert(!cli_oauth_code_is_safe(""));
   assert(!cli_oauth_code_is_safe(NULL));
   printf("  test_code_is_safe: ok\n");
}

int main(void)
{
   printf("server_cli_oauth tests\n");
   test_vendor_parse();
   test_scrape_url();
   test_scrape_codex_code();
   test_code_is_safe();
   printf("all tests passed\n");
   return 0;
}
