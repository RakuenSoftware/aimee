/* test_audit_action_log.c: verifies the S2 governed-action audit row format —
 * audit_action_log writes a single JSON line with kind=tool_action and the
 * expected fields (with JSON escaping) to the shared audit.log. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "log.h"

static char g_home[512];

static void set_home(void)
{
   snprintf(g_home, sizeof g_home, "/tmp/aimee-audit-log-test-%d", (int)getpid());
   mkdir(g_home, 0700);
   setenv("AIMEE_HOME", g_home, 1);
   char p[600];
   snprintf(p, sizeof p, "%s/audit.log", g_home);
   unlink(p); /* fresh file */
}

static char *read_audit_log(void)
{
   char p[600];
   snprintf(p, sizeof p, "%s/audit.log", g_home);
   FILE *f = fopen(p, "r");
   assert(f);
   static char buf[8192];
   size_t n = fread(buf, 1, sizeof buf - 1, f);
   buf[n] = '\0';
   fclose(f);
   return buf;
}

static void test_row_format(void)
{
   set_home();
   audit_log_open();
   audit_action_log("primary", "Write", "v1-abc123", "", "approve", "read_before_write", "block",
                    42);
   audit_log_close();

   char *log = read_audit_log();
   assert(strstr(log, "\"kind\":\"tool_action\"")); /* discriminator, not "event" */
   assert(strstr(log, "\"actor\":\"primary\""));
   assert(strstr(log, "\"tool\":\"Write\""));
   assert(strstr(log, "\"args_hash\":\"v1-abc123\""));
   assert(strstr(log, "\"command\":\"\"")); /* non-shell tool: no command surfaced */
   assert(strstr(log, "\"mode\":\"approve\""));
   assert(strstr(log, "\"reason_code\":\"read_before_write\""));
   assert(strstr(log, "\"verdict\":\"block\""));
   assert(strstr(log, "\"task_id\":42"));
}

static void test_json_escaping(void)
{
   set_home();
   audit_log_open();
   /* a tool name carrying a quote + backslash must be escaped, not break JSON */
   audit_action_log("primary", "We\"ird\\Tool", "v1-0", "", "approve", "blocked", "block", 0);
   audit_log_close();

   char *log = read_audit_log();
   assert(strstr(log, "We\\\"ird\\\\Tool")); /* \" and \\ escaped in the row */
   /* still a single well-formed line ending in }\n */
   size_t len = strlen(log);
   assert(len >= 2 && log[len - 1] == '\n' && log[len - 2] == '}');
}

static void test_null_fields_render_empty(void)
{
   set_home();
   audit_log_open();
   audit_action_log(NULL, "Read", NULL, NULL, NULL, NULL, "allow", 7);
   audit_log_close();

   char *log = read_audit_log();
   assert(strstr(log, "\"actor\":\"\""));
   assert(strstr(log, "\"command\":\"\"")); /* NULL command renders as "" */
   assert(strstr(log, "\"reason_code\":\"\""));
   assert(strstr(log, "\"verdict\":\"allow\""));
   assert(strstr(log, "\"tool\":\"Read\""));
}

static void test_control_char_escaping(void)
{
   set_home();
   audit_log_open();
   /* a tool name with a tab, CR, and a raw control byte (0x01) must be escaped
    * to valid JSON, not emitted raw (would break the S3 reader). */
   audit_action_log("primary",
                    "a\tb\rc\x01"
                    "d",
                    "v1-0", "", "approve", "blocked", "block", 0);
   audit_log_close();

   char *log = read_audit_log();
   assert(strstr(log, "a\\tb\\rc\\u0001d")); /* \t \r and , no raw control bytes */
}

int main(void)
{
   test_row_format();
   test_json_escaping();
   test_null_fields_render_empty();
   test_control_char_escaping();
   printf("test_audit_action_log: all passed\n");
   return 0;
}
