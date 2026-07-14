#include <assert.h>
#ifndef AIMEE_WINDOWS
#include <regex.h>
#endif
#include <stdio.h>
#include <string.h>
#include "aimee.h"

static void test_normalize_key(void)
{
   char buf[256];
   normalize_key("OpenAI API Key Issue", buf, sizeof(buf));
   assert(strcmp(buf, "openai api key issue") == 0);

   normalize_key("  The  big   problem  ", buf, sizeof(buf));
   assert(strcmp(buf, "big problem") == 0);

   normalize_key("a simple test", buf, sizeof(buf));
   assert(strcmp(buf, "simple test") == 0);

   normalize_key("An Error Was Encountered", buf, sizeof(buf));
   assert(strcmp(buf, "error encountered") == 0);

   normalize_key("[2023-05-25] Release-Window", buf, sizeof(buf));
   assert(strcmp(buf, "2023 05 25 release-window") == 0);

   normalize_key("Before(25-May-2023)", buf, sizeof(buf));
   assert(strcmp(buf, "before 25 may 2023") == 0);

   normalize_key("Melanie's race", buf, sizeof(buf));
   assert(strcmp(buf, "melanie race") == 0);

   normalize_key("boss' laptop", buf, sizeof(buf));
   assert(strcmp(buf, "boss laptop") == 0);

   normalize_key("Um well I think maybe the answer is Seattle", buf, sizeof(buf));
   assert(strcmp(buf, "i answer seattle") == 0);
}

static void test_trigram_similarity(void)
{
   double sim = trigram_similarity("hello world", "hello world");
   assert(sim > 0.99);

   sim = trigram_similarity("hello world", "goodbye moon");
   assert(sim < 0.3);

   sim = trigram_similarity("", "");
   assert(sim < 0.01); /* empty strings have no trigrams */
}

static void test_stem_word(void)
{
   char buf[64];
   stem_word("running", buf, sizeof(buf));
   assert(strcmp(buf, "runn") == 0);

   stem_word("tested", buf, sizeof(buf));
   assert(strcmp(buf, "test") == 0);

   stem_word("go", buf, sizeof(buf));
   assert(strcmp(buf, "go") == 0);
}

static void test_is_likely_path(void)
{
   assert(is_likely_path("/usr/bin/test") == 1);
   assert(is_likely_path("./relative/path") == 1);
   assert(is_likely_path("../parent") == 1);
   assert(is_likely_path("~/home/file") == 1);
   assert(is_likely_path("no") == 0);
   assert(is_likely_path("ls") == 0);
}

static void test_shlex_split(void)
{
   char *tokens[32];
   int n = shlex_split("echo 'hello world' | grep hello", tokens, 32);
   assert(n >= 3);
   /* echo, hello world, grep, hello */
   assert(strcmp(tokens[0], "echo") == 0);
   assert(strcmp(tokens[1], "hello world") == 0);
   for (int i = 0; i < n; i++)
      free(tokens[i]);
}

static void test_split_camel_case(void)
{
   char *parts[16];
   int n = split_camel_case("getUserName", parts, 16);
   assert(n == 3);
   assert(strcmp(parts[0], "get") == 0);
   assert(strcmp(parts[1], "User") == 0);
   assert(strcmp(parts[2], "Name") == 0);
   for (int i = 0; i < n; i++)
      free(parts[i]);
}

static void test_is_contradiction(void)
{
   assert(is_contradiction("always deploy on Friday", "never deploy on Friday") == 1);
   assert(is_contradiction("use tabs", "use spaces") == 0);
}

#ifndef AIMEE_WINDOWS
static void test_run_cmd(void)
{
   int ec = -1;
   char *out;

   printf("test_run_cmd\n");

   out = run_cmd("echo hello", &ec);
   assert(out != NULL);
   assert(strcmp(out, "hello\n") == 0);
   assert(ec == 0);
   free(out);

   out = run_cmd("false", &ec);
   assert(out != NULL);
   assert(strcmp(out, "") == 0);
   assert(ec != 0);
   free(out);

   /* Verify output larger than the initial 64KB buffer is captured in full.
    * yes(1) produces an endless stream; head limits it to ~200KB. */
   out = run_cmd("yes x | head -c 204800", &ec);
   assert(out != NULL);
   assert(strlen(out) == 204800);
   assert(ec == 0);
   free(out);
}

static void test_run_cmd_env(void)
{
   int ec = -1;
   char *out;

   printf("test_run_cmd_env\n");

   /* The provided env reaches the child (the forge broker relies on this to pass
    * GH_TOKEN), and crosses ONLY into the child — never the process env. */
   char *envp[] = {(char *)"PATH=/usr/bin:/bin", (char *)"AIMEE_SECRET=s3cr3t", NULL};
   out = run_cmd_env("printf '%s' \"$AIMEE_SECRET\"", envp, &ec);
   assert(out != NULL);
   assert(strcmp(out, "s3cr3t") == 0);
   assert(ec == 0);
   free(out);
   /* the secret did not leak into this (parent) process's environment */
   assert(getenv("AIMEE_SECRET") == NULL);

   /* combined stdout+stderr capture + non-zero exit propagation */
   out = run_cmd_env("echo err 1>&2; exit 3", envp, &ec);
   assert(out != NULL);
   assert(strstr(out, "err") != NULL);
   assert(ec != 0);
   free(out);

   /* honors the thread-local run_cmd cwd */
   run_cmd_set_cwd("/tmp");
   out = run_cmd_env("pwd", envp, &ec);
   run_cmd_set_cwd(NULL);
   assert(out != NULL && strstr(out, "tmp") != NULL);
   assert(ec == 0);
   free(out);
}
#endif

static void test_shell_escape(void)
{
   char *out;

   printf("test_shell_escape\n");

   out = shell_escape("hello");
   assert(out != NULL);
   assert(strcmp(out, "hello") == 0);
   free(out);

   out = shell_escape("it's");
   assert(out != NULL);
   assert(strcmp(out, "it'\\''s") == 0);
   free(out);

   out = shell_escape(NULL);
   assert(out != NULL);
   assert(strcmp(out, "") == 0);
   free(out);
}

static void test_shell_escape_injection_payloads(void)
{
   /* Verify shell_escape handles injection payloads correctly */
   char *e;

   /* Single quote injection: '; rm -rf / # */
   e = shell_escape("'; rm -rf / #");
   assert(e != NULL);
   assert(strstr(e, "rm -rf") != NULL); /* content preserved */
   assert(e[0] == '\'');                /* leading quote is escaped */
   assert(strstr(e, "'\\''") != NULL);  /* quote properly escaped */
   free(e);

   /* Backtick injection */
   e = shell_escape("`whoami`");
   assert(strcmp(e, "`whoami`") == 0); /* backticks inside single quotes are safe */
   free(e);

   /* Dollar expansion */
   e = shell_escape("$(cat /etc/passwd)");
   assert(strcmp(e, "$(cat /etc/passwd)") == 0); /* $ inside single quotes is literal */
   free(e);

   /* Null input */
   e = shell_escape(NULL);
   assert(e != NULL);
   assert(strcmp(e, "") == 0);
   free(e);

   /* Empty input */
   e = shell_escape("");
   assert(e != NULL);
   assert(strcmp(e, "") == 0);
   free(e);
}

#ifndef AIMEE_WINDOWS
static void test_regex_match(void)
{
   printf("test_regex_match\n");

   assert(regex_match("^hello", "hello world", REG_EXTENDED) == 1);
   assert(regex_match("^world", "hello world", REG_EXTENDED) == 0);
   assert(regex_match("HELLO", "hello", REG_EXTENDED | REG_ICASE) == 1);
   assert(regex_match("[", "test", REG_EXTENDED) == 0);
   assert(regex_match(NULL, "test", 0) == 0);
}
#endif

static void test_strip_ai_attribution(void)
{
   char buf[512];

   /* The standard Claude Code footer: trailer + attribution line both go. */
   snprintf(buf, sizeof(buf),
            "fix: handle empty input\n\nDetails here.\n\n"
            "\xF0\x9F\xA4\x96 Generated with [Claude Code](https://claude.com/claude-code)\n\n"
            "Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>\n");
   assert(strip_ai_attribution(buf) == 2);
   assert(strcmp(buf, "fix: handle empty input\n\nDetails here.") == 0);

   /* Case-insensitive, leading whitespace, codex variant. */
   snprintf(buf, sizeof(buf), "subject\n  CO-AUTHORED-BY: bot <b@x>\nGenerated with [Codex CLI]\n");
   assert(strip_ai_attribution(buf) == 2);
   assert(strcmp(buf, "subject") == 0);

   /* Prose mentioning the concepts (unanchored trailer, no markdown link) stays. */
   snprintf(buf, sizeof(buf),
            "docs: explain co-authored-by handling\n\n"
            "CI rejects any Co-Authored-By: trailer and text generated with Claude Code.\n");
   assert(strip_ai_attribution(buf) == 0);
   assert(strstr(buf, "co-authored-by handling") != NULL);
   assert(strstr(buf, "generated with Claude") != NULL);

   /* Attribution-only message strips to empty; NULL is a no-op. */
   snprintf(buf, sizeof(buf), "Co-authored-by: X <x@y>\n");
   assert(strip_ai_attribution(buf) == 1);
   assert(buf[0] == '\0');
   assert(strip_ai_attribution(NULL) == 0);

   /* Interior attribution line is removed without joining its neighbours. */
   snprintf(buf, sizeof(buf), "line one\nCo-Authored-By: A <a@b>\nline two");
   assert(strip_ai_attribution(buf) == 1);
   assert(strcmp(buf, "line one\nline two") == 0);
}

int main(void)
{
   test_normalize_key();
   test_trigram_similarity();
   test_stem_word();
   test_is_likely_path();
   test_shlex_split();
   test_split_camel_case();
   test_is_contradiction();
   test_shell_escape();
   test_shell_escape_injection_payloads();
   test_strip_ai_attribution();
#ifndef AIMEE_WINDOWS
   test_run_cmd();
   test_run_cmd_env();
   test_regex_match();
#endif
   printf("util: all tests passed\n");
   return 0;
}
