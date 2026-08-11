/* test_log.c: tests for logging infrastructure */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "log.h"

static void test_log_parse_level(void)
{
   log_level_t level;

   assert(log_parse_level("error", &level) == 0 && level == LOG_ERROR);
   assert(log_parse_level("WARN", &level) == 0 && level == LOG_WARN);
   assert(log_parse_level("Info", &level) == 0 && level == LOG_INFO);
   assert(log_parse_level("debug", &level) == 0 && level == LOG_DEBUG);
   assert(log_parse_level("invalid", &level) != 0);
   assert(log_parse_level(NULL, &level) != 0);
   assert(log_parse_level("error", NULL) != 0);
}

static void test_log_level_control(void)
{
   log_init(LOG_WARN);
   assert(log_get_level() == LOG_WARN);

   log_set_level(LOG_DEBUG);
   assert(log_get_level() == LOG_DEBUG);

   log_set_level(LOG_ERROR);
   assert(log_get_level() == LOG_ERROR);
}

static void test_log_calls_do_not_crash(void)
{
   log_init(LOG_DEBUG);

   /* These should not crash even without audit file open */
   aimee_log(LOG_ERROR, "test", "error message %d", 42);
   aimee_log(LOG_WARN, "test", "warn message");
   aimee_log(LOG_INFO, "test", "info message");
   aimee_log(LOG_DEBUG, "test", "debug message");

   /* Audit should not crash without open file */
   audit_log("test_event", "some detail %s", "here");
}

static void test_log_level_filtering(void)
{
   /* When level is ERROR, only errors should be logged.
    * We can't easily capture stderr, but at least verify no crash. */
   log_init(LOG_ERROR);
   aimee_log(LOG_ERROR, "test", "this should appear");
   aimee_log(LOG_WARN, "test", "this should be suppressed");
   aimee_log(LOG_INFO, "test", "this should be suppressed");
   aimee_log(LOG_DEBUG, "test", "this should be suppressed");
}

/* Rotation, end to end: redirect stderr to a real file, push it past the cap,
 * and check both halves of the contract.
 *
 * The second half is the one worth having. Renaming a file that is still open
 * leaves the descriptor on the SAME inode, so a rotation that forgets to reopen
 * keeps writing into the ROTATED file while the current one stays empty — the
 * log looks rotated and is silently being lost. The size assertions below would
 * pass without the reopen; the "still writes to the current file" assertion is
 * what fails. */
static void test_server_log_rotates_and_keeps_writing(void)
{
   char dir[256], path[300], rotated[320];
   snprintf(dir, sizeof(dir), "/tmp/aimee-logrot-%d", (int)getpid());
   char cmd[512];
   snprintf(cmd, sizeof(cmd), "rm -rf %s && mkdir -p %s", dir, dir);
   assert(system(cmd) == 0);
   snprintf(path, sizeof(path), "%s/server.log", dir);
   snprintf(rotated, sizeof(rotated), "%s.0", path);

   FILE *fp = freopen(path, "a", stderr);
   assert(fp != NULL);
   setvbuf(stderr, NULL, _IOLBF, 0);

   log_init(LOG_INFO);
   log_set_rotating_sink(path);

   /* A 64MB cap with ~1KB lines needs a lot of lines; the sampling interval
    * means the check runs every 512 messages either way. */
   char big[1024];
   memset(big, 'x', sizeof(big) - 1);
   big[sizeof(big) - 1] = '\0';
   for (int i = 0; i < 80000; i++)
      aimee_log(LOG_INFO, "rot", "%s", big);

   fflush(stderr);
   struct stat st_cur, st_rot;
   int have_rotated = (stat(rotated, &st_rot) == 0);

   /* Put stderr back before asserting, so a failure is still reportable. */
   log_set_rotating_sink(NULL);
   (void)freopen("/dev/null", "a", stderr);

   assert(have_rotated); /* the cap was crossed and a generation was made */
   assert(st_rot.st_size >= (64 * 1024 * 1024));

   /* THE REOPEN: the current file exists and is receiving the newest lines. */
   assert(stat(path, &st_cur) == 0);
   assert(st_cur.st_size > 0);
   assert(st_cur.st_size < st_rot.st_size); /* fresh file, not the old inode */

   snprintf(cmd, sizeof(cmd), "rm -rf %s", dir);
   assert(system(cmd) == 0);
}

int main(void)
{
   printf("test_log:\n");
   test_log_parse_level();
   printf("  log_parse_level: OK\n");
   test_log_level_control();
   printf("  log_level_control: OK\n");
   test_log_calls_do_not_crash();
   printf("  log_calls_no_crash: OK\n");
   test_log_level_filtering();
   printf("  log_level_filtering: OK\n");
   test_server_log_rotates_and_keeps_writing();
   printf("  server_log_rotation: OK\n");
   printf("All log tests passed.\n");
   return 0;
}
