/* test_lsp.c: unit tests for LSP JSON-RPC framing, diagnostic parsing,
 * and prompt rendering. */
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#include "cJSON.h"
#include "aimee_sha256.h"
#include "config_client.h"
#include "lsp.h"
#include "lsp_context.h"
#include "platform_path.h"
#include "platform_test_util.h"

/* -----------------------------------------------------------------------
 * Helpers
 * ----------------------------------------------------------------------- */

/* Create a pipe pair and return {read_fd, write_fd} */
static void make_pipe(int *rfd, int *wfd)
{
   int fds[2];
   assert(pipe(fds) == 0);
   *rfd = fds[0];
   *wfd = fds[1];
}

static void write_text_file(const char *path, const char *text)
{
   FILE *fp = fopen(path, "w");
   assert(fp != NULL);
   assert(fputs(text, fp) >= 0);
   fclose(fp);
}

static char *read_text_file(const char *path)
{
   FILE *fp = fopen(path, "rb");
   if (!fp || fseek(fp, 0, SEEK_END) != 0)
   {
      if (fp)
         fclose(fp);
      return NULL;
   }
   long size = ftell(fp);
   if (size < 0 || fseek(fp, 0, SEEK_SET) != 0)
   {
      fclose(fp);
      return NULL;
   }
   char *text = malloc((size_t)size + 1);
   if (!text || fread(text, 1, (size_t)size, fp) != (size_t)size)
   {
      free(text);
      fclose(fp);
      return NULL;
   }
   fclose(fp);
   text[size] = '\0';
   return text;
}

static void restore_env_var(const char *name, const char *old_value)
{
   if (old_value)
      assert(platform_setenv(name, old_value) == 0);
   else
      assert(platform_unsetenv(name) == 0);
}

static long long monotonic_milliseconds(void)
{
   struct timespec now;
   assert(clock_gettime(CLOCK_MONOTONIC, &now) == 0);
   return (long long)now.tv_sec * 1000LL + now.tv_nsec / 1000000LL;
}

static int configure_real_provider(const char *command, const char *server_arg, const char *ext)
{
   cJSON *servers = cJSON_CreateArray();
   cJSON *server = cJSON_CreateObject();
   cJSON *args = cJSON_CreateArray();
   cJSON *extensions = cJSON_CreateArray();
   if (!servers || !server || !args || !extensions ||
       !cJSON_AddStringToObject(server, "name", "real-provider-probe") ||
       !cJSON_AddStringToObject(server, "command", command) ||
       (server_arg && !cJSON_AddItemToArray(args, cJSON_CreateString(server_arg))) ||
       !cJSON_AddItemToArray(extensions, cJSON_CreateString(ext)))
   {
      cJSON_Delete(servers);
      cJSON_Delete(server);
      cJSON_Delete(args);
      cJSON_Delete(extensions);
      return -1;
   }
   cJSON_AddItemToObject(server, "args", args);
   cJSON_AddItemToObject(server, "extensions", extensions);
   cJSON_AddItemToArray(servers, server);
   if (config_client_set_value("lsp_servers", servers) != 0)
      return -1;
   return config_client_set_number("lsp_server_count", 1.0);
}

static int real_provider_probe_main(int argc, char **argv)
{
   if (argc != 11)
   {
      fprintf(stderr,
              "usage: %s --real-provider[-synced] <command> <arg-or-dash> <workspace> <file> "
              "<line> <col> <expected-file> <expected-line> <min-refs>\n",
              argv[0]);
      return 2;
   }

   const char *command = argv[2];
   const char *server_arg = strcmp(argv[3], "-") == 0 ? NULL : argv[3];
   const char *workspace = argv[4];
   const char *file = argv[5];
   int line = atoi(argv[6]) - 1;
   int col = atoi(argv[7]) - 1;
   const char *expected_file = argv[8];
   int expected_line = atoi(argv[9]) - 1;
   int min_refs = atoi(argv[10]);
   int synced = strcmp(argv[1], "--real-provider-synced") == 0;
   const char *ext = strrchr(file, '.');
   if (!command[0] || !workspace[0] || !file[0] || !expected_file[0] || !ext || line < 0 ||
       col < 0 || expected_line < 0 || min_refs < 0)
   {
      fprintf(stderr, "real-provider probe received an invalid argument\n");
      return 2;
   }

   if (configure_real_provider(command, server_arg, ext) != 0)
   {
      fprintf(stderr, "real-provider probe could not configure the in-process config peer\n");
      return 2;
   }

   lsp_manager_init();
   lsp_diag_t cold_diags[4];
   int cold_diag_count = lsp_manager_diagnostics(workspace, file, cold_diags, 4);
   int cold_errors = 0, cold_warnings = 0, cold_active = 0;
   lsp_manager_diag_summary(&cold_errors, &cold_warnings, &cold_active);

   int document_version = 0;
   unsigned long provider_generation = 0;
   char sync_err[256] = "";
   int sync_rc = 0;
   if (synced)
   {
      char *text = read_text_file(file);
      if (!text)
         sync_rc = -1;
      else
      {
         sync_rc = lsp_manager_sync_document(workspace, file, text, &document_version,
                                             &provider_generation, sync_err, sizeof(sync_err));
         free(text);
      }
   }

   long long definition_started_ms = monotonic_milliseconds();
   lsp_location_t defs[32];
   char def_err[256] = "";
   int def_count =
       lsp_manager_definition(workspace, file, line, col, defs, 32, def_err, sizeof(def_err));
   long long cold_definition_ms = monotonic_milliseconds() - definition_started_ms;
   int definition_matched = 0;
   for (int i = 0; i < def_count; i++)
      if (strcmp(defs[i].file, expected_file) == 0 && defs[i].line == expected_line)
         definition_matched = 1;

   lsp_location_t refs[64];
   char refs_err[256] = "";
   long long references_started_ms = monotonic_milliseconds();
   int ref_count =
       lsp_manager_references(workspace, file, line, col, refs, 64, refs_err, sizeof(refs_err));
   long long warm_references_ms = monotonic_milliseconds() - references_started_ms;
   int active_after_query = 0;
   lsp_manager_diag_summary(NULL, NULL, &active_after_query);

   cJSON *result = cJSON_CreateObject();
   assert(result != NULL);
   cJSON_AddNumberToObject(result, "cold_diagnostics", cold_diag_count);
   cJSON_AddNumberToObject(result, "cold_active_servers", cold_active);
   cJSON_AddNumberToObject(result, "definition_count", def_count);
   cJSON_AddBoolToObject(result, "definition_matched", definition_matched);
   cJSON_AddNumberToObject(result, "reference_count", ref_count);
   cJSON_AddNumberToObject(result, "cold_definition_ms", (double)cold_definition_ms);
   cJSON_AddNumberToObject(result, "warm_references_ms", (double)warm_references_ms);
   cJSON_AddNumberToObject(result, "active_servers_after_query", active_after_query);
   cJSON_AddBoolToObject(result, "synchronized", synced && sync_rc == 0);
   cJSON_AddNumberToObject(result, "document_version", document_version);
   cJSON_AddNumberToObject(result, "provider_generation", (double)provider_generation);
   cJSON_AddStringToObject(result, "synchronization_error", sync_err);
   cJSON_AddStringToObject(result, "definition_error", def_err);
   cJSON_AddStringToObject(result, "references_error", refs_err);
   char *rendered = cJSON_PrintUnformatted(result);
   assert(rendered != NULL);
   printf("%s\n", rendered);
   fflush(stdout);
   free(rendered);
   cJSON_Delete(result);

   lsp_manager_shutdown_all();

   return cold_diag_count == 0 && cold_active == 0 && (!synced || sync_rc == 0) &&
                  (!synced || (document_version > 0 && provider_generation > 0)) &&
                  definition_matched && ref_count >= min_refs
              ? 0
              : 1;
}

static int fake_lsp_main(void)
{
   char buf[8192];
   const char *diag =
       "{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/publishDiagnostics\",\"params\":{"
       "\"uri\":\"file:///tmp/fake.c\",\"diagnostics\":[{\"range\":{\"start\":{\"line\":2,"
       "\"character\":3}},\"severity\":2,\"message\":\"interleaved warning\"}]}}";
   const char *init_resp = "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{\"capabilities\":{}}}";
   const char *def_resp =
       "{\"jsonrpc\":\"2.0\",\"id\":2,\"result\":[{\"uri\":\"file:///tmp/fake.c\","
       "\"range\":{\"start\":{\"line\":7,\"character\":1}}}]}";

   int n = lsp_frame_read(STDIN_FILENO, buf, sizeof(buf));
   if (n <= 0 || !strstr(buf, "\"method\":\"initialize\""))
      return 1;
   if (lsp_frame_write(STDOUT_FILENO, diag) != 0)
      return 2;
   if (lsp_frame_write(STDOUT_FILENO, init_resp) != 0)
      return 3;

   n = lsp_frame_read(STDIN_FILENO, buf, sizeof(buf));
   if (n <= 0 || !strstr(buf, "\"method\":\"initialized\""))
      return 4;

   int saw_open = 0, saw_change = 0;
   n = lsp_frame_read(STDIN_FILENO, buf, sizeof(buf));
   while (n > 0 && (strstr(buf, "\"method\":\"textDocument/didOpen\"") ||
                    strstr(buf, "\"method\":\"textDocument/didChange\"")))
   {
      if (strstr(buf, "\"method\":\"textDocument/didOpen\""))
      {
         saw_open = 1;
         if (!strstr(buf, "int target(void) { return 1; }\\n"))
            return 5;
      }
      if (strstr(buf, "\"method\":\"textDocument/didChange\""))
      {
         saw_change = 1;
         if (!strstr(buf, "int target(void) { return 2; }\\n"))
            return 5;
      }
      n = lsp_frame_read(STDIN_FILENO, buf, sizeof(buf));
   }
   if (n <= 0 || !saw_open || !saw_change || !strstr(buf, "\"method\":\"textDocument/definition\""))
      return 5;
   if (lsp_frame_write(STDOUT_FILENO, diag) != 0)
      return 6;
   if (lsp_frame_write(STDOUT_FILENO, def_resp) != 0)
      return 7;

   n = lsp_frame_read(STDIN_FILENO, buf, sizeof(buf));
   if (n <= 0 || !strstr(buf, "\"method\":\"shutdown\""))
      return 8;
   n = lsp_frame_read(STDIN_FILENO, buf, sizeof(buf));
   if (n <= 0 || !strstr(buf, "\"method\":\"exit\""))
      return 9;

   return 0;
}

/* -----------------------------------------------------------------------
 * Test: lsp_severity_label
 * ----------------------------------------------------------------------- */

static void test_severity_label(void)
{
   printf("test_severity_label\n");

   assert(strcmp(lsp_severity_label(LSP_SEV_ERROR), "error") == 0);
   assert(strcmp(lsp_severity_label(LSP_SEV_WARNING), "warning") == 0);
   assert(strcmp(lsp_severity_label(LSP_SEV_INFO), "info") == 0);
   assert(strcmp(lsp_severity_label(LSP_SEV_HINT), "hint") == 0);
}

/* -----------------------------------------------------------------------
 * Test: lsp_frame_write / lsp_frame_read roundtrip
 * ----------------------------------------------------------------------- */

static void test_frame_roundtrip(void)
{
   printf("test_frame_roundtrip\n");

   int rfd, wfd;
   make_pipe(&rfd, &wfd);

   const char *msg = "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"test\"}";
   assert(lsp_frame_write(wfd, msg) == 0);
   close(wfd);

   char buf[4096];
   int n = lsp_frame_read(rfd, buf, sizeof(buf));
   close(rfd);

   assert(n > 0);
   assert(strcmp(buf, msg) == 0);
}

static void test_frame_multiple_messages(void)
{
   printf("test_frame_multiple_messages\n");

   int rfd, wfd;
   make_pipe(&rfd, &wfd);

   const char *m1 = "{\"id\":1}";
   const char *m2 = "{\"id\":2,\"result\":null}";
   assert(lsp_frame_write(wfd, m1) == 0);
   assert(lsp_frame_write(wfd, m2) == 0);
   close(wfd);

   char buf[4096];
   int n1 = lsp_frame_read(rfd, buf, sizeof(buf));
   assert(n1 > 0);
   assert(strcmp(buf, m1) == 0);

   int n2 = lsp_frame_read(rfd, buf, sizeof(buf));
   assert(n2 > 0);
   assert(strcmp(buf, m2) == 0);

   /* EOF on third read */
   int n3 = lsp_frame_read(rfd, buf, sizeof(buf));
   assert(n3 < 0);

   close(rfd);
}

static void test_frame_large_body(void)
{
   printf("test_frame_large_body\n");

   int rfd, wfd;
   make_pipe(&rfd, &wfd);

   /* Build a ~2KB JSON string */
   char large[2048];
   memset(large, 'x', sizeof(large) - 3);
   large[0] = '"';
   large[sizeof(large) - 3] = '"';
   large[sizeof(large) - 2] = '\0';

   char msg[2100];
   snprintf(msg, sizeof(msg), "{\"data\":%s}", large);

   assert(lsp_frame_write(wfd, msg) == 0);
   close(wfd);

   char buf[8192];
   int n = lsp_frame_read(rfd, buf, sizeof(buf));
   close(rfd);

   assert(n > 0);
   assert(strcmp(buf, msg) == 0);
}

static void test_frame_buffer_too_small(void)
{
   printf("test_frame_buffer_too_small\n");

   int rfd, wfd;
   make_pipe(&rfd, &wfd);

   const char *msg = "{\"jsonrpc\":\"2.0\",\"id\":1}";
   assert(lsp_frame_write(wfd, msg) == 0);
   close(wfd);

   char tiny[4]; /* too small for the message */
   int n = lsp_frame_read(rfd, tiny, sizeof(tiny));
   close(rfd);

   assert(n == -2); /* buffer too small */
}

/* -----------------------------------------------------------------------
 * Test: lsp_parse_diagnostics
 * ----------------------------------------------------------------------- */

static void test_parse_diagnostics_empty(void)
{
   printf("test_parse_diagnostics_empty\n");

   const char *json = "{\"method\":\"textDocument/publishDiagnostics\","
                      "\"params\":{\"uri\":\"file:///tmp/foo.c\","
                      "\"diagnostics\":[]}}";
   lsp_diag_t out[8];
   int n = lsp_parse_diagnostics(json, "/tmp/foo.c", out, 8);
   assert(n == 0);
}

static void test_parse_diagnostics_basic(void)
{
   printf("test_parse_diagnostics_basic\n");

   const char *json = "{\"method\":\"textDocument/publishDiagnostics\","
                      "\"params\":{"
                      "  \"uri\":\"file:///tmp/test.c\","
                      "  \"diagnostics\":["
                      "    {\"range\":{\"start\":{\"line\":41,\"character\":4}},"
                      "     \"severity\":1,"
                      "     \"message\":\"implicit declaration of function 'free_tier'\"},"
                      "    {\"range\":{\"start\":{\"line\":86,\"character\":11}},"
                      "     \"severity\":2,"
                      "     \"message\":\"unused variable 'tmp'\"}"
                      "  ]"
                      "}}";

   lsp_diag_t out[8];
   int n = lsp_parse_diagnostics(json, "/tmp/test.c", out, 8);
   assert(n == 2);

   assert(out[0].line == 41);
   assert(out[0].col == 4);
   assert(out[0].severity == LSP_SEV_ERROR);
   assert(strstr(out[0].message, "free_tier") != NULL);
   assert(strcmp(out[0].file, "/tmp/test.c") == 0);

   assert(out[1].line == 86);
   assert(out[1].col == 11);
   assert(out[1].severity == LSP_SEV_WARNING);
   assert(strstr(out[1].message, "unused variable") != NULL);
}

static void test_parse_diagnostics_caps_at_max(void)
{
   printf("test_parse_diagnostics_caps_at_max\n");

   /* Build JSON with 20 diagnostics */
   char json[8192];
   int pos = snprintf(json, sizeof(json), "{\"params\":{\"diagnostics\":[");
   for (int i = 0; i < 20; i++)
   {
      if (i > 0)
         json[pos++] = ',';
      pos += snprintf(json + pos, sizeof(json) - (size_t)pos,
                      "{\"range\":{\"start\":{\"line\":%d,\"character\":0}},"
                      "\"severity\":1,\"message\":\"err%d\"}",
                      i, i);
   }
   pos += snprintf(json + pos, sizeof(json) - (size_t)pos, "]}}");
   (void)pos;

   lsp_diag_t out[5];
   int n = lsp_parse_diagnostics(json, "/tmp/f.c", out, 5);
   assert(n == 5); /* capped at max */
}

static void test_parse_diagnostics_collapses_newlines(void)
{
   printf("test_parse_diagnostics_collapses_newlines\n");

   const char *json = "{\"params\":{\"diagnostics\":["
                      "{\"range\":{\"start\":{\"line\":0,\"character\":0}},"
                      "\"severity\":1,\"message\":\"line one\\nline two\"}"
                      "]}}";

   lsp_diag_t out[2];
   int n = lsp_parse_diagnostics(json, "/tmp/x.c", out, 2);
   assert(n == 1);
   /* newline should be collapsed to space */
   assert(strchr(out[0].message, '\n') == NULL);
   assert(strstr(out[0].message, "line one") != NULL);
   assert(strstr(out[0].message, "line two") != NULL);
}

static void test_parse_diagnostics_missing_severity_defaults_error(void)
{
   printf("test_parse_diagnostics_missing_severity_defaults_error\n");

   const char *json = "{\"params\":{\"diagnostics\":["
                      "{\"range\":{\"start\":{\"line\":0,\"character\":0}},"
                      "\"message\":\"oops\"}"
                      "]}}";

   lsp_diag_t out[2];
   int n = lsp_parse_diagnostics(json, "/f.c", out, 2);
   assert(n == 1);
   assert(out[0].severity == LSP_SEV_ERROR);
}

/* -----------------------------------------------------------------------
 * Test: lsp_render_context
 * ----------------------------------------------------------------------- */

static void test_render_empty(void)
{
   printf("test_render_empty\n");

   char buf[1024];
   int n = lsp_render_context("/tmp/a.c", NULL, 0, NULL, 0, NULL, 0, buf, sizeof(buf));
   assert(n == 0);
   assert(buf[0] == '\0');
}

static void test_render_diagnostics_only(void)
{
   printf("test_render_diagnostics_only\n");

   lsp_diag_t diags[2];
   memset(diags, 0, sizeof(diags));
   snprintf(diags[0].file, sizeof(diags[0].file), "src/memory.c");
   diags[0].line = 41;
   diags[0].col = 4;
   diags[0].severity = LSP_SEV_ERROR;
   snprintf(diags[0].message, sizeof(diags[0].message), "implicit declaration of 'free_tier'");

   snprintf(diags[1].file, sizeof(diags[1].file), "src/memory.c");
   diags[1].line = 86;
   diags[1].col = 11;
   diags[1].severity = LSP_SEV_WARNING;
   snprintf(diags[1].message, sizeof(diags[1].message), "unused variable 'tmp'");

   char buf[2048];
   int n = lsp_render_context("src/memory.c", diags, 2, NULL, 0, NULL, 0, buf, sizeof(buf));
   assert(n > 0);

   /* Check structure */
   assert(strstr(buf, "# LSP context") != NULL);
   assert(strstr(buf, "src/memory.c") != NULL);
   assert(strstr(buf, "[error]") != NULL);
   assert(strstr(buf, "[warning]") != NULL);
   assert(strstr(buf, "free_tier") != NULL);
   assert(strstr(buf, "unused variable") != NULL);
   /* Lines use 1-based */
   assert(strstr(buf, ":42:") != NULL);
   assert(strstr(buf, ":87:") != NULL);
}

static void test_render_caps_at_max(void)
{
   printf("test_render_caps_at_max\n");

   /* Provide more than LSP_RENDER_MAX_DIAG diagnostics */
   lsp_diag_t diags[20];
   memset(diags, 0, sizeof(diags));
   for (int i = 0; i < 20; i++)
   {
      diags[i].line = i;
      diags[i].severity = LSP_SEV_ERROR;
      snprintf(diags[i].message, sizeof(diags[i].message), "err%d", i);
      snprintf(diags[i].file, sizeof(diags[i].file), "f.c");
   }

   char buf[8192];
   lsp_render_context("f.c", diags, 20, NULL, 0, NULL, 0, buf, sizeof(buf));

   /* Count occurrences of "[error]" — should equal LSP_RENDER_MAX_DIAG */
   int count = 0;
   const char *p = buf;
   while ((p = strstr(p, "[error]")) != NULL)
   {
      count++;
      p++;
   }
   assert(count == LSP_RENDER_MAX_DIAG);
}

static void test_render_with_definitions_and_refs(void)
{
   printf("test_render_with_definitions_and_refs\n");

   lsp_diag_t diags[1];
   memset(diags, 0, sizeof(diags));
   diags[0].severity = LSP_SEV_WARNING;
   snprintf(diags[0].file, sizeof(diags[0].file), "main.c");
   diags[0].line = 0;
   snprintf(diags[0].message, sizeof(diags[0].message), "warning msg");

   lsp_location_t defs[1];
   memset(defs, 0, sizeof(defs));
   snprintf(defs[0].symbol, sizeof(defs[0].symbol), "my_func");
   snprintf(defs[0].file, sizeof(defs[0].file), "impl.c");
   defs[0].line = 9;

   lsp_location_t refs[2];
   memset(refs, 0, sizeof(refs));
   snprintf(refs[0].symbol, sizeof(refs[0].symbol), "my_func");
   snprintf(refs[0].file, sizeof(refs[0].file), "a.c");
   refs[0].line = 4;
   snprintf(refs[1].symbol, sizeof(refs[1].symbol), "my_func");
   snprintf(refs[1].file, sizeof(refs[1].file), "b.c");
   refs[1].line = 19;

   char buf[4096];
   int n = lsp_render_context("main.c", diags, 1, defs, 1, refs, 2, buf, sizeof(buf));
   assert(n > 0);

   assert(strstr(buf, "Definitions") != NULL);
   assert(strstr(buf, "impl.c") != NULL);
   assert(strstr(buf, ":10") != NULL); /* 0-based 9 → 1-based 10 */
   assert(strstr(buf, "References") != NULL);
   assert(strstr(buf, "a.c") != NULL);
   assert(strstr(buf, "b.c") != NULL);
}

/* -----------------------------------------------------------------------
 * Test: lsp_manager_diag_summary — no active servers
 * ----------------------------------------------------------------------- */

static void test_diag_summary_no_servers(void)
{
   printf("test_diag_summary_no_servers\n");

   /* With no active LSP servers, all counts should be zero */
   int errors = -1, warnings = -1, active = -1;
   lsp_manager_diag_summary(&errors, &warnings, &active);
   assert(errors == 0);
   assert(warnings == 0);
   assert(active == 0);
}

static void test_diag_summary_null_args(void)
{
   printf("test_diag_summary_null_args\n");

   /* Passing NULL out-parameters must not crash */
   lsp_manager_diag_summary(NULL, NULL, NULL);
}

/* -----------------------------------------------------------------------
 * Test: lsp_manager_rename — missing server returns -1
 * ----------------------------------------------------------------------- */

static void test_rename_no_server(void)
{
   printf("test_rename_no_server\n");

   lsp_manager_init();
   char out[256] = "";
   char errbuf[256] = "";
   /* Attempt rename on a workspace with no configured LSP server */
   int n = lsp_manager_rename("/tmp", "/tmp/nonexistent.c", 0, 0, "new_name", out, sizeof(out),
                              errbuf, sizeof(errbuf));
   assert(n == -1);
   assert(errbuf[0] != '\0'); /* error message must be set */
}

static void test_definition_with_interleaved_notifications(const char *argv0)
{
   printf("test_definition_with_interleaved_notifications\n");

   char tmp_home[512];
   snprintf(tmp_home, sizeof(tmp_home), "%s/aimee-test-lsp-home-XXXXXX", platform_tmpdir());
   assert(platform_mkdtemp(tmp_home) != NULL);

   char cfgdir[PATH_MAX];
   char appdir[PATH_MAX];
   char cfgpath[PATH_MAX];
   snprintf(cfgdir, sizeof(cfgdir), "%s/.config", tmp_home);
   snprintf(appdir, sizeof(appdir), "%s/.config/aimee", tmp_home);
   snprintf(cfgpath, sizeof(cfgpath), "%s/aimee.yaml", appdir);
   assert(platform_mkdir_p(cfgdir, 0700) == 0);
   assert(platform_mkdir_p(appdir, 0700) == 0);

   char exe[PATH_MAX];
   assert(realpath(argv0, exe) != NULL);

   char cfg[4096];
   snprintf(cfg, sizeof(cfg),
            "lsp_servers:\n"
            "  - name: fake\n"
            "    command: %s\n"
            "    args:\n"
            "      - --fake-lsp\n"
            "    extensions:\n"
            "      - .c\n",
            exe);
   write_text_file(cfgpath, cfg);

   char workspace[PATH_MAX];
   snprintf(workspace, sizeof(workspace), "%s/workspace", tmp_home);
   assert(platform_mkdir_p(workspace, 0700) == 0);

   char file[PATH_MAX];
   snprintf(file, sizeof(file), "%s/test.c", workspace);
   write_text_file(file, "int target(void) { return 1; }\n");

   const char *old_home = getenv("HOME");
   char *old_home_copy = old_home ? strdup(old_home) : NULL;
   const char *old_aimee_home = getenv("AIMEE_HOME");
   char *old_aimee_home_copy = old_aimee_home ? strdup(old_aimee_home) : NULL;
   const char *old_aimee_profile = getenv("AIMEE_PROFILE");
   char *old_aimee_profile_copy = old_aimee_profile ? strdup(old_aimee_profile) : NULL;
   const char *old_no_cache = getenv("AIMEE_NO_CACHE");
   char *old_no_cache_copy = old_no_cache ? strdup(old_no_cache) : NULL;

   assert(platform_setenv("HOME", tmp_home) == 0);
   assert(platform_unsetenv("AIMEE_HOME") == 0);
   assert(platform_unsetenv("AIMEE_PROFILE") == 0);
   assert(platform_setenv("AIMEE_NO_CACHE", "1") == 0);
   assert(configure_real_provider(exe, "--fake-lsp", ".c") == 0);

   lsp_manager_init();
   int version = 0;
   char sync_err[256] = "";
   unsigned long generation = 0;
   assert(lsp_manager_sync_document(workspace, file, "int target(void) { return 1; }\n", &version,
                                    &generation, sync_err, sizeof(sync_err)) == 0);
   assert(version == 1);
   assert(generation > 0);
   unsigned long first_generation = generation;
   assert(lsp_manager_sync_document(workspace, file, "int target(void) { return 2; }\n", &version,
                                    &generation, sync_err, sizeof(sync_err)) == 0);
   assert(version == 2);
   assert(generation == first_generation);
   lsp_location_t defs[4];
   char errbuf[256] = "";
   int n = lsp_manager_definition(workspace, file, 0, 0, defs, 4, errbuf, sizeof(errbuf));
   if (n != 1)
      fprintf(stderr, "lsp definition rc=%d err=%s\n", n, errbuf);
   assert(n == 1);
   assert(strcmp(defs[0].file, "/tmp/fake.c") == 0);
   assert(defs[0].line == 7);
   assert(defs[0].col == 1);

   lsp_diag_t diags[4];
   int ndiags = lsp_manager_diagnostics(workspace, NULL, diags, 4);
   assert(ndiags == 1);
   assert(strcmp(diags[0].file, "/tmp/fake.c") == 0);
   assert(diags[0].severity == LSP_SEV_WARNING);
   assert(strstr(diags[0].message, "interleaved warning") != NULL);

   lsp_manager_shutdown_all();

   restore_env_var("HOME", old_home_copy);
   restore_env_var("AIMEE_HOME", old_aimee_home_copy);
   restore_env_var("AIMEE_PROFILE", old_aimee_profile_copy);
   restore_env_var("AIMEE_NO_CACHE", old_no_cache_copy);
   free(old_home_copy);
   free(old_aimee_home_copy);
   free(old_aimee_profile_copy);
   free(old_no_cache_copy);

   unlink(cfgpath);
   platform_test_rmrf(tmp_home);
}

typedef struct
{
   char root[PATH_MAX];
   char location[PATH_MAX];
   int sync_count;
   int read_count;
   int query_count;
   int return_empty;
   int mutate_on_final_read;
   int sync_unavailable;
   int binary_input;
   int source_version_mismatch;
} context_fixture_t;

static int context_authorize(void *opaque, const char *relative, char *resolved, size_t cap)
{
   context_fixture_t *fixture = opaque;
   char joined[PATH_MAX];
   if ((size_t)snprintf(joined, sizeof(joined), "%s/%s", fixture->root, relative) >= sizeof(joined))
      return -1;
   return realpath(joined, resolved) && strlen(resolved) < cap ? 0 : -1;
}

static int context_read(void *opaque, const char *path, char **out, size_t *out_len)
{
   context_fixture_t *fixture = opaque;
   fixture->read_count++;
   if (fixture->binary_input)
   {
      *out = malloc(3);
      assert(*out);
      (*out)[0] = 'a';
      (*out)[1] = '\0';
      (*out)[2] = 'b';
      *out_len = 3;
      return 0;
   }
   if (fixture->mutate_on_final_read && fixture->read_count == 3)
      write_text_file(path, "int changed_after_answer = 1;\n");
   *out = read_text_file(path);
   if (!*out)
      return -1;
   *out_len = strlen(*out);
   return 0;
}

static int context_sync(void *opaque, const char *workspace, const char *file, const char *text,
                        int *version, unsigned long *generation, char *errbuf, size_t errbuf_size)
{
   context_fixture_t *fixture = opaque;
   assert(strcmp(workspace, fixture->root) == 0);
   assert(text && file);
   if (fixture->sync_unavailable)
   {
      snprintf(errbuf, errbuf_size, "no LSP server configured for extension '.c'");
      return -1;
   }
   fixture->sync_count++;
   *version = fixture->sync_count;
   *generation = 17;
   return 0;
}

static int context_query(void *opaque, const char *operation, const char *workspace,
                         const char *file, int line, int column, lsp_location_t *out, int max,
                         char *errbuf, size_t errbuf_size)
{
   (void)errbuf;
   (void)errbuf_size;
   context_fixture_t *fixture = opaque;
   assert(!strcmp(operation, "definition") || !strcmp(operation, "references"));
   assert(strcmp(workspace, fixture->root) == 0 && file && line >= 0 && column >= 0 && max == 65);
   fixture->query_count++;
   if (fixture->return_empty)
      return 0;
   memset(&out[0], 0, sizeof(out[0]));
   snprintf(out[0].file, sizeof(out[0].file), "%s", fixture->location);
   out[0].line = 0;
   out[0].col = 4;
   return 1;
}

static cJSON *context_source(void *opaque, const char *relative, int start, int end, int max_lines)
{
   (void)start;
   (void)end;
   (void)max_lines;
   context_fixture_t *fixture = opaque;
   char path[PATH_MAX];
   snprintf(path, sizeof(path), "%s/%s", fixture->root, relative);
   char *text = read_text_file(path);
   if (!text)
      return NULL;
   cJSON *span = cJSON_CreateObject();
   cJSON_AddStringToObject(span, "content", text);
   char hash[65];
   assert(aimee_sha256_hex(text, strlen(text), hash) == 0);
   if (fixture->source_version_mismatch)
      hash[0] = hash[0] == '0' ? '1' : '0';
   cJSON_AddStringToObject(span, "source_version", hash);
   free(text);
   return span;
}

static cJSON *context_args(const char *operation, const char *file, int anchors, int budget)
{
   cJSON *args = cJSON_CreateObject();
   cJSON_AddStringToObject(args, "operation", operation);
   cJSON *array = cJSON_AddArrayToObject(args, "anchors");
   for (int i = 0; i < anchors; i++)
   {
      cJSON *anchor = cJSON_CreateObject();
      cJSON_AddStringToObject(anchor, "file", file);
      cJSON_AddNumberToObject(anchor, "line", 1);
      cJSON_AddNumberToObject(anchor, "column", 5);
      cJSON_AddItemToArray(array, anchor);
   }
   if (budget)
      cJSON_AddNumberToObject(args, "max_source_bytes", budget);
   return args;
}

static const char *json_string(cJSON *object, const char *name)
{
   cJSON *item = cJSON_GetObjectItemCaseSensitive(object, name);
   return cJSON_IsString(item) ? item->valuestring : "";
}

static void test_context_envelope_and_failures(void)
{
   printf("test_context_envelope_and_failures\n");
   char root[PATH_MAX];
   snprintf(root, sizeof(root), "%s/aimee-test-lsp-context-XXXXXX", platform_tmpdir());
   assert(platform_mkdtemp(root) != NULL);
   char source[PATH_MAX];
   snprintf(source, sizeof(source), "%s/sample.c", root);
   char large[700];
   memset(large, 'x', sizeof(large) - 2);
   large[sizeof(large) - 2] = '\n';
   large[sizeof(large) - 1] = '\0';
   write_text_file(source, large);

   context_fixture_t fixture = {0};
   snprintf(fixture.root, sizeof(fixture.root), "%s", root);
   snprintf(fixture.location, sizeof(fixture.location), "%s", source);
   lsp_context_provider_t provider = {.provider = "local_lsp",
                                      .root = fixture.root,
                                      .project = "project-id",
                                      .worktree = "worktree-id",
                                      .ctx = &fixture,
                                      .authorize = context_authorize,
                                      .read_file = context_read,
                                      .sync = context_sync,
                                      .query = context_query,
                                      .source = context_source};

   cJSON *args = context_args("definition", "sample.c", 2, 256);
   cJSON *result = lsp_context_execute(&provider, args);
   assert(result && strcmp(json_string(result, "status"), "ok") == 0);
   assert(strcmp(json_string(result, "project"), "project-id") == 0);
   assert(strcmp(json_string(result, "worktree"), "worktree-id") == 0);
   cJSON *generation = cJSON_GetObjectItemCaseSensitive(result, "provider_generation");
   assert(cJSON_IsNumber(generation) && generation->valuedouble == 17);
   assert(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(result, "truncated")));
   cJSON *source_bytes = cJSON_GetObjectItemCaseSensitive(result, "source_bytes");
   assert(cJSON_IsNumber(source_bytes) && source_bytes->valuedouble == 256);
   cJSON *items = cJSON_GetObjectItemCaseSensitive(result, "results");
   assert(cJSON_GetArraySize(items) == 2 && fixture.sync_count == 2 && fixture.query_count == 2);
   cJSON *first = cJSON_GetArrayItem(items, 0);
   assert(strcmp(json_string(first, "status"), "ok") == 0);
   assert(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(first, "truncated")));
   cJSON *locations = cJSON_GetObjectItemCaseSensitive(first, "locations");
   cJSON *location = cJSON_GetArrayItem(locations, 0);
   cJSON *span = cJSON_GetObjectItemCaseSensitive(location, "source");
   assert(strlen(json_string(span, "content")) == 256);
   cJSON *document = cJSON_GetObjectItemCaseSensitive(first, "document");
   assert(strcmp(json_string(document, "freshness"), "current") == 0);
   assert(strlen(json_string(document, "content_sha256")) == 64);
   cJSON_Delete(result);
   cJSON_Delete(args);

   char target_source[PATH_MAX];
   snprintf(target_source, sizeof(target_source), "%s/target.c", root);
   write_text_file(target_source, "int target(void) { return 1; }\n");
   snprintf(fixture.location, sizeof(fixture.location), "%s", target_source);
   fixture.read_count = fixture.sync_count = fixture.query_count = 0;
   args = context_args("definition", "sample.c", 1, 0);
   result = lsp_context_execute(&provider, args);
   assert(strcmp(json_string(result, "status"), "ok") == 0);
   first = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(result, "results"), 0);
   locations = cJSON_GetObjectItemCaseSensitive(first, "locations");
   location = cJSON_GetArrayItem(locations, 0);
   span = cJSON_GetObjectItemCaseSensitive(location, "source");
   char *target_text = read_text_file(target_source);
   char target_hash[65];
   assert(target_text && aimee_sha256_hex(target_text, strlen(target_text), target_hash) == 0);
   assert(strcmp(json_string(span, "source_version"), target_hash) == 0);
   free(target_text);
   cJSON_Delete(result);
   cJSON_Delete(args);
   snprintf(fixture.location, sizeof(fixture.location), "%s", source);

   args = context_args("definition", "../outside.c", 1, 0);
   result = lsp_context_execute(&provider, args);
   assert(strcmp(json_string(result, "status"), "unauthorized") == 0);
   cJSON_Delete(result);
   cJSON_Delete(args);

   fixture.binary_input = 1;
   fixture.read_count = fixture.sync_count = fixture.query_count = 0;
   args = context_args("definition", "sample.c", 1, 0);
   result = lsp_context_execute(&provider, args);
   assert(strcmp(json_string(result, "status"), "unsupported") == 0);
   assert(fixture.sync_count == 0 && fixture.query_count == 0);
   cJSON_Delete(result);
   cJSON_Delete(args);

   fixture.binary_input = 0;
   fixture.source_version_mismatch = 1;
   fixture.read_count = fixture.sync_count = fixture.query_count = 0;
   args = context_args("definition", "sample.c", 1, 0);
   result = lsp_context_execute(&provider, args);
   assert(strcmp(json_string(result, "status"), "stale") == 0);
   first = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(result, "results"), 0);
   assert(strcmp(json_string(first, "status"), "stale") == 0);
   assert(!cJSON_GetObjectItemCaseSensitive(first, "locations"));
   cJSON_Delete(result);
   cJSON_Delete(args);

   fixture.source_version_mismatch = 0;
   fixture.sync_unavailable = 1;
   args = context_args("definition", "sample.c", 1, 0);
   result = lsp_context_execute(&provider, args);
   assert(strcmp(json_string(result, "status"), "unavailable") == 0);
   assert(strcmp(json_string(
                     cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(result, "results"), 0),
                     "status"),
                 "unavailable") == 0);
   cJSON_Delete(result);
   cJSON_Delete(args);

   fixture.sync_unavailable = 0;
   fixture.read_count = fixture.sync_count = fixture.query_count = 0;
   fixture.mutate_on_final_read = 1;
   write_text_file(source, "int answer(void) { return 1; }\n");
   args = context_args("references", "sample.c", 1, 0);
   result = lsp_context_execute(&provider, args);
   assert(strcmp(json_string(result, "status"), "stale") == 0);
   first = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(result, "results"), 0);
   assert(strcmp(json_string(first, "status"), "stale") == 0);
   document = cJSON_GetObjectItemCaseSensitive(first, "document");
   assert(strcmp(json_string(document, "freshness"), "stale") == 0);
   assert(!cJSON_GetObjectItemCaseSensitive(first, "locations"));
   cJSON_Delete(result);
   cJSON_Delete(args);

   fixture.mutate_on_final_read = 0;
   fixture.return_empty = 1;
   fixture.read_count = fixture.sync_count = fixture.query_count = 0;
   args = context_args("definition", "sample.c", 1, 0);
   result = lsp_context_execute(&provider, args);
   assert(strcmp(json_string(result, "status"), "empty") == 0);
   first = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(result, "results"), 0);
   assert(strcmp(json_string(first, "status"), "empty") == 0);
   assert(cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(first, "locations")) == 0);
   cJSON_Delete(result);
   cJSON_Delete(args);

   fixture.return_empty = 0;
   snprintf(fixture.location, sizeof(fixture.location), "%s", "/etc/hosts");
   fixture.read_count = fixture.sync_count = fixture.query_count = 0;
   args = context_args("definition", "sample.c", 1, 0);
   result = lsp_context_execute(&provider, args);
   assert(strcmp(json_string(result, "status"), "unauthorized") == 0);
   first = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(result, "results"), 0);
   assert(strcmp(json_string(first, "status"), "unauthorized") == 0);
   assert(cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(first, "locations")) == 0);
   cJSON_Delete(result);
   cJSON_Delete(args);

   platform_test_rmrf(root);
}

/* -----------------------------------------------------------------------
 * main
 * ----------------------------------------------------------------------- */

int main(int argc, char **argv)
{
   if (argc > 1 && strcmp(argv[1], "--fake-lsp") == 0)
      return fake_lsp_main();
   if (argc > 1 && strcmp(argv[1], "--real-provider") == 0)
      return real_provider_probe_main(argc, argv);
   if (argc > 1 && strcmp(argv[1], "--real-provider-synced") == 0)
      return real_provider_probe_main(argc, argv);

   test_severity_label();

   test_frame_roundtrip();
   test_frame_multiple_messages();
   test_frame_large_body();
   test_frame_buffer_too_small();

   test_parse_diagnostics_empty();
   test_parse_diagnostics_basic();
   test_parse_diagnostics_caps_at_max();
   test_parse_diagnostics_collapses_newlines();
   test_parse_diagnostics_missing_severity_defaults_error();

   test_render_empty();
   test_render_diagnostics_only();
   test_render_caps_at_max();
   test_render_with_definitions_and_refs();

   test_diag_summary_no_servers();
   test_diag_summary_null_args();
   test_rename_no_server();
   test_definition_with_interleaved_notifications(argv[0]);
   test_context_envelope_and_failures();

   printf("All LSP tests passed.\n");
   return 0;
}
