/* test_git_oauth_gh.c — pure parts of the gh-CLI GitHub sign-in module: PTY
 * output scrubbing (ANSI/CR) + one-time-code extraction. No process spawn (the
 * exec/vault externals are stubbed for linking only; these tests never invoke
 * them). */

#include "git_oauth_gh.h"

#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

/* ── Link-only stubs for the module's externals (never called by these tests) ── */
int git_host_cred_set(const char *host, const char *token)
{
   (void)host;
   (void)token;
   return 0;
}
int safe_exec_capture(const char *const argv[], char **out_buf, size_t max_out)
{
   (void)argv;
   (void)max_out;
   if (out_buf)
      *out_buf = NULL;
   return 0;
}
int safe_exec_capture_env(const char *const argv[], char *const envp[], char **out_buf,
                          size_t max_out)
{
   (void)argv;
   (void)envp;
   (void)max_out;
   if (out_buf)
      *out_buf = NULL;
   return 0;
}

/* ── Tests ──────────────────────────────────────────────────────────────────── */

static void test_strip_term_noise(void)
{
   /* CSI styling + carriage returns go; the text stays. */
   char a[] = "\x1b[0;35m!\x1b[0m First copy your one-time code: \x1b[1mABCD-1234\x1b[0m\r\n";
   git_oauth_gh_strip_term_noise(a);
   assert(strcmp(a, "! First copy your one-time code: ABCD-1234\n") == 0);

   /* OSC (title-set) sequences terminated by BEL go too. */
   char b[] = "\x1b]0;gh\x07hello\x1b[2K\rworld";
   git_oauth_gh_strip_term_noise(b);
   assert(strcmp(b, "helloworld") == 0);

   /* Plain text is untouched; a dangling ESC at the end doesn't run off. */
   char c[] = "no escapes here";
   git_oauth_gh_strip_term_noise(c);
   assert(strcmp(c, "no escapes here") == 0);
   char d[] = "tail\x1b";
   git_oauth_gh_strip_term_noise(d);
   assert(strcmp(d, "tail") == 0);
}

static void test_parse_code(void)
{
   char code[32];

   /* Realistic gh output (post-strip). Digits and letters both appear. */
   const char *out = "- Logging in to github.com\n"
                     "! First copy your one-time code: 1D23-90A4\n"
                     "- Press Enter to open https://github.com/login/device in your browser...\n";
   assert(git_oauth_gh_parse_code(out, code, sizeof(code)) == 1);
   assert(strcmp(code, "1D23-90A4") == 0);

   /* No marker → no match, even if a code-shaped token exists. */
   assert(git_oauth_gh_parse_code("go enter ABCD-1234 somewhere", code, sizeof(code)) == 0);

   /* Marker but no code (yet) → no match; lowercase never matches. */
   assert(git_oauth_gh_parse_code("First copy your one-time code: ", code, sizeof(code)) == 0);
   assert(git_oauth_gh_parse_code("one-time code: abcd-efgh", code, sizeof(code)) == 0);

   /* The code may terminate the buffer (chunked reads). */
   assert(git_oauth_gh_parse_code("one-time code: WXYZ-0099", code, sizeof(code)) == 1);
   assert(strcmp(code, "WXYZ-0099") == 0);

   /* A 5th trailing alnum disqualifies the candidate (not a XXXX-XXXX code). */
   assert(git_oauth_gh_parse_code("one-time code: ABCD-12345", code, sizeof(code)) == 0);

   /* Too-small output buffer refuses instead of truncating. */
   char tiny[9];
   assert(git_oauth_gh_parse_code("one-time code: ABCD-1234", tiny, sizeof(tiny)) == 0);
}

int main(void)
{
   test_strip_term_noise();
   test_parse_code();
   printf("OK\n");
   return 0;
}
