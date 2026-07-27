/* test_cli_v1_subcommands.c — cli_v1_subcommands(), which turns a failed route
 * lookup into an actionable message.
 *
 * Before this helper existed, `aimee economizer status` reported
 * "command 'economizer' has no /v1 route; add a /v1 route" — blaming the command
 * (and telling the user to add a route) when the command is routed fine and only
 * the SUBCOMMAND was wrong. That message sends people hunting for a missing route
 * that already exists.
 *
 * The first cut of the helper walked rpc_routes with sizeof/sizeof, which runs
 * off the end into the table's {NULL,...} sentinel and segfaults in strcmp. The
 * unknown-command case below is the regression guard for that crash. */

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "cli_client.h"

/* A command that is not in the table at all: must report zero and must not walk
 * into the NULL sentinel. This is the segfault guard. */
static void test_unknown_command_is_safe(void)
{
   char buf[256] = "sentinel";
   assert(cli_v1_subcommands("no-such-command-anywhere", buf, sizeof(buf)) == 0);
   assert(buf[0] == '\0');

   /* every rejected lookup path, including the ones a crash would take */
   assert(cli_v1_subcommands("", buf, sizeof(buf)) == 0);
   assert(cli_v1_subcommands(NULL, buf, sizeof(buf)) == 0);
   printf("  unknown command is safe (no sentinel walk)\n");
}

/* A routed family reports its real subcommands. */
static void test_known_command_lists_subcommands(void)
{
   char buf[512];
   int n = cli_v1_subcommands("economizer", buf, sizeof(buf));
   assert(n > 0);
   assert(strstr(buf, "stats") != NULL);

   n = cli_v1_subcommands("kb", buf, sizeof(buf));
   assert(n >= 4);
   assert(strstr(buf, "search") != NULL);
   assert(strstr(buf, "build") != NULL); /* the row whose absence made kb build unreachable */
   assert(strstr(buf, "status") != NULL);

   n = cli_v1_subcommands("curator", buf, sizeof(buf));
   assert(n > 0);
   assert(strstr(buf, "contradictions") != NULL);
   printf("  known commands list their subcommands\n");
}

/* Entries are comma-separated with no leading/trailing separator. */
static void test_list_formatting(void)
{
   char buf[512];
   assert(cli_v1_subcommands("kb", buf, sizeof(buf)) > 0);
   assert(buf[0] != ',');
   assert(buf[0] != ' ');
   size_t len = strlen(buf);
   assert(len > 0);
   assert(buf[len - 1] != ',');
   assert(buf[len - 1] != ' ');
   assert(strstr(buf, ",,") == NULL);
   printf("  list formatting ok\n");
}

/* A tiny buffer must truncate, never overflow. The count still reports the true
 * number of subcommands so the caller can tell the list was clipped. */
static void test_small_buffer_does_not_overflow(void)
{
   char guard[64];
   memset(guard, 0x7f, sizeof(guard));
   char *buf = guard + 16; /* poisoned bytes on both sides */
   int n = cli_v1_subcommands("kb", buf, 8);
   assert(n > 0);
   assert(strlen(buf) < 8);
   for (int i = 0; i < 16; i++)
      assert((unsigned char)guard[i] == 0x7f); /* nothing written before */
   for (size_t i = 16 + 8; i < sizeof(guard); i++)
      assert((unsigned char)guard[i] == 0x7f); /* nothing written past cap */

   /* a zero-cap / NULL out must still count without writing */
   assert(cli_v1_subcommands("kb", NULL, 0) > 0);
   printf("  small buffer truncates without overflow\n");
}

/* Rows that are NULL (match-any) or "" (bare command) are not typeable names and
 * must not appear in the suggestion list. `workers` is registered only as a bare
 * command, so it contributes no subcommand names. */
static void test_bare_and_matchany_rows_excluded(void)
{
   char buf[256];
   int n = cli_v1_subcommands("workers", buf, sizeof(buf));
   assert(n == 0);
   assert(buf[0] == '\0');
   printf("  bare / match-any rows excluded\n");
}

int main(void)
{
   printf("test_cli_v1_subcommands\n");
   test_unknown_command_is_safe();
   test_known_command_lists_subcommands();
   test_list_formatting();
   test_small_buffer_does_not_overflow();
   test_bare_and_matchany_rows_excluded();
   printf("test_cli_v1_subcommands: all passed\n");
   return 0;
}
