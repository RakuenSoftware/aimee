#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "gateway/stt.h"
#include "agent_exec.h"
#include "log.h"
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const unsigned char audio[] = {'R', 'I', 'F', 'F', 0, 1, 0, 255, 'X'};
static const char *reply = "{\"text\":\"hello\"}";
static size_t reply_length;
static int status = 200, calls;
static const char *expected_model = "whisper-1", *expected_filename, *expected_provider = "openai";
static char root[128], input[256];
void aimee_log(log_level_t level, const char *module, const char *format, ...)
{
   (void)level;
   (void)module;
   (void)format;
}
int agent_http_post_content_type_bytes(const char *url, const char *auth, const char *type,
                                       const void *bytes, size_t length, char **response,
                                       size_t *response_length, int timeout, const char *extra)
{
   calls++;
   assert(strstr(url, strcmp(expected_provider, "groq") ? "api.openai.com" : "api.groq.com"));
   assert(!strcmp(auth, "Authorization: Bearer test-key"));
   assert(timeout == 30000 && !extra);
   const char *boundary = strstr(type, "boundary=");
   assert(boundary && strstr(type, "multipart/form-data; ") == type);
   boundary += strlen("boundary=");
   char model[256];
   snprintf(model, sizeof model, "name=\"model\"\r\n\r\n%s\r\n--%s", expected_model, boundary);
   assert(memmem(bytes, length, model, strlen(model)));
   char file[256];
   snprintf(file, sizeof file, "filename=\"%s\"\r\n", expected_filename);
   assert(memmem(bytes, length, file, strlen(file)));
   assert(memmem(bytes, length, audio, sizeof audio));
   assert(memmem(bytes, length, "name=\"response_format\"\r\n\r\njson\r\n",
                 strlen("name=\"response_format\"\r\n\r\njson\r\n")));
   char tail[128];
   snprintf(tail, sizeof tail, "\r\n--%s--\r\n", boundary);
   assert(length >= strlen(tail) &&
          !memcmp((const char *)bytes + length - strlen(tail), tail, strlen(tail)));
   size_t n = reply_length ? reply_length : strlen(reply);
   *response = malloc(n + 1);
   assert(*response);
   memcpy(*response, reply, n);
   (*response)[n] = 0;
   *response_length = n;
   return status;
}
static void expect_reply(const char *json, size_t length, size_t capacity, int ok,
                         const char *expected)
{
   reply = json;
   reply_length = length;
   char output[256] = "previous secret";
   assert((stt_transcribe_file(input, output, capacity) == 0) == ok);
   assert(!strcmp(output, ok ? expected : ""));
}
/* Test executable doubles as a controlled Whisper CLI through an owned PATH.
 * This pins argv and result-file behavior without invoking a model or shell. */
static int whisper_fixture(int argc, char **argv)
{
   assert(argc == 11);
   assert(!strcmp(argv[1], "--model") && !strcmp(argv[2], getenv("EXPECT_MODEL")));
   assert(!strcmp(argv[3], "--output_format") && !strcmp(argv[4], "json"));
   assert(!strcmp(argv[5], "--output_dir") && !strcmp(argv[7], "--verbose"));
   assert(!strcmp(argv[8], "False") && !strcmp(argv[9], "--"));
   assert(!strcmp(argv[10], getenv("EXPECT_AUDIO")));
   const char *temporary = getenv("TMPDIR");
   assert(temporary && !strncmp(argv[6], temporary, strlen(temporary)) &&
          !strncmp(argv[6] + strlen(temporary), "/aimee-stt-", 11));
   struct stat st;
   assert(!stat(argv[6], &st) && (st.st_mode & 0777) == 0700);
   if (getenv("WHISPER_FAIL"))
      return 7;
   if (getenv("WHISPER_MISSING"))
      return 0;
   const char *name = strrchr(argv[10], '/') + 1;
   const char *dot = strrchr(name, '.');
   size_t stem = dot && dot != name ? (size_t)(dot - name) : strlen(name);
   char output[512];
   snprintf(output, sizeof output, "%s/%.*s.json", argv[6], (int)stem, name);
   FILE *f = fopen(output, "wb");
   assert(f);
   const char *json = getenv("WHISPER_JSON");
   assert(fputs(json ? json : "{\"text\":\"local semantic text\\n\"}", f) >= 0);
   assert(!fclose(f));
   puts("[00:00.000 --> 00:01.000] diagnostic output must not become transcript");
   return 0;
}
int main(int argc, char **argv)
{
   if (argc > 1)
      return whisper_fixture(argc, argv);
   char pattern[] = "/tmp/aimee-stt-test-XXXXXX";
   assert(mkdtemp(pattern));
   snprintf(root, sizeof root, "%s", pattern);
   snprintf(input, sizeof input, "%s/voice \"quoted\"\r\n.wav", root);
   FILE *f = fopen(input, "wb");
   assert(f);
   assert(fwrite(audio, 1, sizeof audio, f) == sizeof audio && !fclose(f));
   expected_filename = "voice %22quoted%22%0D%0A.wav";
   assert(!setenv("AIMEE_GATEWAY_STT_PROVIDER", "openai", 1));
   assert(!setenv("OPENAI_API_KEY", "test-key", 1));
   unsetenv("AIMEE_GATEWAY_STT_MODEL");
   expect_reply("{\"text\":\"literal \\\"quoted\\\"\\nline\\n\",\"language\":\"en\"}", 0, 256, 1,
                "literal \"quoted\"\nline");
   const char *bad[] = {"{}",
                        "[]",
                        "{\"text\":4}",
                        "{\"text\":null}",
                        "{\"text\":\"a\",\"text\":\"b\"}",
                        "{\"text\":\"a\"} trailing",
                        "{\"text\":\"a\"}{\"text\":\"b\"}",
                        "{\"text\":\"x\\u0000hidden\"}",
                        "{\"text\":\"unterminated}",
                        "{\"text\":\"x\",\"meta\":{\"a\":1,\"a\":2}}"};
   for (size_t i = 0; i < sizeof bad / sizeof *bad; i++)
      expect_reply(bad[i], 0, 256, 0, NULL);
   expect_reply("{\"text\":\"12345\"}", 0, 5, 0, NULL);
   expect_reply("{\"text\":\"1234\"}", 0, 5, 1, "1234");
   expect_reply("{\"text\":\"\"}", 0, 1, 1, "");
   const char nul[] = "{\"text\":\"ok\"}\0ignored";
   expect_reply(nul, sizeof nul - 1, 256, 0, NULL);
   expect_reply(" \n {\"text\":\"whitespace\"} \t\n", 0, 256, 1, "whitespace");
   expect_reply("{\"text\":\"a\",\"\\u0074ext\":\"b\"}", 0, 256, 0, NULL);
   status = 503;
   expect_reply("{\"text\":\"not success\"}", 0, 256, 0, NULL);
   status = 200;
   assert(!setenv("AIMEE_GATEWAY_STT_PROVIDER", "groq", 1));
   assert(!setenv("GROQ_API_KEY", "test-key", 1));
   expected_provider = "groq";
   expected_model = "whisper-large-v3-turbo";
   expect_reply("{\"text\":\"Groq\"}", 0, 256, 1, "Groq");
   expected_model = "whisper-large-v3";
   assert(!setenv("AIMEE_GATEWAY_STT_MODEL", expected_model, 1));
   expect_reply("{\"text\":\"configured\"}", 0, 256, 1, "configured");
   int before = calls;
   unsetenv("GROQ_API_KEY");
   expect_reply("{}", 0, 256, 0, NULL);
   assert(calls == before);
   assert(!setenv("AIMEE_GATEWAY_STT_PROVIDER", "local", 1));
   assert(!setenv("TMPDIR", root, 1));
   char self[4096];
   assert(realpath(argv[0], self));
   char executable[256];
   snprintf(executable, sizeof executable, "%s/whisper", root);
   assert(!symlink(self, executable));
   char *old_path = strdup(getenv("PATH") ? getenv("PATH") : "");
   assert(old_path);
   char *path = malloc(strlen(root) + strlen(old_path) + 2);
   assert(path);
   sprintf(path, "%s:%s", root, old_path);
   assert(!setenv("PATH", path, 1));
   assert(!setenv("EXPECT_AUDIO", input, 1));
   const char *local_model = "base; touch forbidden-model-shell";
   assert(!setenv("EXPECT_MODEL", local_model, 1));
   assert(!setenv("AIMEE_GATEWAY_STT_MODEL", local_model, 1));
   expect_reply("{}", 0, 256, 1, "local semantic text");
   assert(!setenv("WHISPER_JSON", "{\"text\":\"overflow\"}", 1));
   expect_reply("{}", 0, 5, 0, NULL);
   assert(!setenv("WHISPER_JSON", "{\"text\":\"x\",\"text\":\"y\"}", 1));
   expect_reply("{}", 0, 256, 0, NULL);
   unsetenv("WHISPER_JSON");
   assert(!setenv("WHISPER_FAIL", "1", 1));
   expect_reply("{}", 0, 256, 0, NULL);
   unsetenv("WHISPER_FAIL");
   assert(!setenv("WHISPER_MISSING", "1", 1));
   expect_reply("{}", 0, 256, 0, NULL);
   unsetenv("WHISPER_MISSING");
   unsetenv("AIMEE_GATEWAY_STT_MODEL");
   assert(!setenv("EXPECT_MODEL", "base", 1));
   expect_reply("{}", 0, 256, 1, "local semantic text");
   assert(!setenv("PATH", old_path, 1));
   free(path);
   free(old_path);
   unlink(executable);
   unlink(input);
   assert(!rmdir(root));
   puts("gateway STT: multipart bytes, provider models/auth, strict transcript and literal local "
        "argv passed");
   return 0;
}
