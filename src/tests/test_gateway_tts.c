#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "../gateway/tts.h"
#include "agent_exec.h"
#include "cJSON.h"
#include "log.h"
#include <assert.h>
#include <dirent.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *expected_text, *expected_voice, *expected_model, *expected_format;
static int http_status = 200, large_audio, empty_audio;
static const unsigned char audio[] = {'R', 0, 'I', 'F', 'F', 0xff, 0, 'X'};

void aimee_log(log_level_t level, const char *module, const char *format, ...)
{
   (void)level;
   (void)module;
   (void)format;
}

int agent_http_post_content_type_bytes(const char *url, const char *auth, const char *type,
                                       const void *body, size_t body_length, char **response,
                                       size_t *response_length, int timeout, const char *extra)
{
   assert(!strcmp(type, "application/json") && timeout == 30000 && !extra);
   assert(strlen(body) == body_length);
   cJSON *request = cJSON_Parse(body);
   assert(request && cJSON_GetArraySize(request) == 4);
   assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(request, "input")), expected_text));
   assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(request, "voice")), expected_voice));
   assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(request, "model")), expected_model));
   assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(request, "response_format")),
                  expected_format));
   if (!strcmp(expected_format, "mp3"))
      assert(!strcmp(url, "https://api.openai.com/v1/audio/speech") && auth &&
             !strcmp(auth, "Authorization: Bearer fixture-key"));
   else
      assert(!strcmp(url, "http://fixture:8880/v1/audio/speech") && !auth);
   cJSON_Delete(request);
   *response_length = empty_audio ? 0 : large_audio ? 32768 : sizeof audio;
   *response = malloc(*response_length + 1);
   assert(*response);
   for (size_t i = 0; i < *response_length; i++)
      (*response)[i] = (char)audio[i % sizeof audio];
   (*response)[*response_length] = '\0';
   return http_status;
}

static size_t file_count(const char *directory)
{
   DIR *dir = opendir(directory);
   assert(dir);
   size_t count = 0;
   struct dirent *entry;
   while ((entry = readdir(dir)))
      if (strcmp(entry->d_name, ".") && strcmp(entry->d_name, ".."))
         count++;
   assert(!closedir(dir));
   return count;
}

static void check_file(const char *path, const void *bytes, size_t length, const char *extension)
{
   assert(strlen(path) > strlen(extension));
   assert(!strcmp(path + strlen(path) - strlen(extension), extension));
   struct stat statbuf;
   assert(!stat(path, &statbuf) && statbuf.st_size == (off_t)length);
   assert((statbuf.st_mode & 0777) == 0600);
   FILE *file = fopen(path, "rb");
   assert(file);
   char *actual = malloc(length + 1);
   assert(actual && fread(actual, 1, length + 1, file) == length);
   assert(!memcmp(actual, bytes, length) && !fclose(file));
   free(actual);
}

static void remove_audio(char *path)
{
   assert(path && !unlink(path));
   free(path);
}

static void *synthesize(void *unused)
{
   (void)unused;
   char *path = NULL;
   assert(!tts_synthesize(expected_text, &path));
   return path;
}

static int espeak_fixture(int argc, char **argv)
{
   assert(argc == 4 && !strcmp(argv[1], "-w") && !strcmp(argv[3], "--stdin"));
   FILE *file = fopen(argv[2], "wb");
   assert(file);
   char buffer[8192];
   size_t length;
   while ((length = fread(buffer, 1, sizeof buffer, stdin)))
      assert(fwrite(buffer, 1, length, file) == length);
   assert(!ferror(stdin) && !fclose(file));
   return getenv("AIMEE_TTS_FIXTURE_FAIL") ? 7 : 0;
}

int main(int argc, char **argv)
{
   const char *name = strrchr(argv[0], '/');
   if (!strcmp(name ? name + 1 : argv[0], "espeak-ng"))
      return espeak_fixture(argc, argv);
   char directory[4096];
   const char *temporary = getenv("TMPDIR");
   if (!temporary || !temporary[0])
      temporary = "/tmp";
   int n = snprintf(directory, sizeof directory, "%s/aimee-tts-fixture-XXXXXX", temporary);
   assert(n > 0 && (size_t)n < sizeof directory);
   assert(mkdtemp(directory));
   assert(!setenv("TMPDIR", directory, 1));
   char *text = malloc(65537);
   assert(text);
   const char sample[] = "'\"\\\n--voice=bad;$(touch forbidden) ";
   for (size_t i = 0; i < 65536; i++)
      text[i] = sample[i % (sizeof sample - 1)];
   text[65536] = '\0';
   expected_text = text;
   assert(!setenv("OPENAI_API_KEY", "fixture-key", 1));
   assert(!setenv("AIMEE_GATEWAY_TTS_BASE_URL", "http://fixture:8880", 1));
   unsetenv("AIMEE_GATEWAY_TTS_PROVIDER");
   unsetenv("AIMEE_GATEWAY_TTS_MODEL");
   unsetenv("AIMEE_GATEWAY_TTS_VOICE");
   assert(!tts_enabled());
   char *path = (char *)1;
   assert(tts_synthesize(NULL, &path) < 0 && !path);
   assert(tts_synthesize("", &path) < 0 && !path);
   assert(tts_synthesize(text, NULL) < 0);
   for (int provider = 0; provider < 2; provider++)
   {
      assert(!setenv("AIMEE_GATEWAY_TTS_PROVIDER", provider ? "kokoro" : "openai", 1));
      assert(tts_enabled());
      expected_format = provider ? "wav" : "mp3";
      expected_voice = provider ? "af_sky" : "alloy";
      expected_model = provider ? "kokoro" : "tts-1";
      assert(!tts_synthesize(text, &path));
      check_file(path, audio, sizeof audio, provider ? ".wav" : ".mp3");
      remove_audio(path);
      pthread_t a, b;
      assert(!pthread_create(&a, NULL, synthesize, NULL));
      assert(!pthread_create(&b, NULL, synthesize, NULL));
      char *first, *second;
      assert(!pthread_join(a, (void **)&first) && !pthread_join(b, (void **)&second));
      assert(strcmp(first, second));
      check_file(first, audio, sizeof audio, provider ? ".wav" : ".mp3");
      check_file(second, audio, sizeof audio, provider ? ".wav" : ".mp3");
      remove_audio(first);
      remove_audio(second);
      http_status = 503;
      path = (char *)1;
      assert(tts_synthesize(text, &path) < 0 && !path && !file_count(directory));
      http_status = 200;
      empty_audio = 1;
      path = (char *)1;
      assert(tts_synthesize(text, &path) < 0 && !path && !file_count(directory));
      empty_audio = 0;
   }
   /* Environment-selected strings are JSON values, including escapes. */
   expected_voice = "a\"b\n\\c";
   assert(!setenv("AIMEE_GATEWAY_TTS_VOICE", expected_voice, 1));
   expected_model = "kokoro";
   assert(!tts_synthesize(text, &path));
   remove_audio(path);
   assert(!setenv("AIMEE_GATEWAY_TTS_PROVIDER", "openai", 1));
   expected_model = "model\"\\\n";
   expected_format = "mp3";
   assert(!setenv("AIMEE_GATEWAY_TTS_MODEL", expected_model, 1));
   assert(!tts_synthesize(text, &path));
   remove_audio(path);
   unsetenv("OPENAI_API_KEY");
   assert(tts_synthesize(text, &path) < 0 && !path && !file_count(directory));
   assert(!setenv("OPENAI_API_KEY", "fixture-key", 1));
   /* Buffered fclose failure and a partial large fwrite both remove the file. */
   struct rlimit old_limit;
   assert(!getrlimit(RLIMIT_FSIZE, &old_limit));
   void (*old_signal)(int) = signal(SIGXFSZ, SIG_IGN);
   for (int partial = 0; partial < 2; partial++)
   {
      struct rlimit limit = old_limit;
      limit.rlim_cur = partial ? 2048 : 0;
      assert(!setrlimit(RLIMIT_FSIZE, &limit));
      large_audio = partial;
      assert(tts_synthesize(text, &path) < 0 && !path);
      assert(!setrlimit(RLIMIT_FSIZE, &old_limit));
      assert(!file_count(directory));
   }
   signal(SIGXFSZ, old_signal);
   large_audio = 0;
   /* The same real executable supplies a controlled argv/stdin speech helper. */
   char *self = realpath(argv[0], NULL), *helper = NULL;
   assert(self && asprintf(&helper, "%s/espeak-ng", directory) >= 0);
   assert(!symlink(self, helper));
   free(self);
   assert(!setenv("PATH", directory, 1));
   assert(!setenv("AIMEE_GATEWAY_TTS_PROVIDER", "local", 1));
   assert(!tts_synthesize(text, &path));
   check_file(path, text, strlen(text), ".wav");
   remove_audio(path);
   pthread_t a, b;
   assert(!pthread_create(&a, NULL, synthesize, NULL));
   assert(!pthread_create(&b, NULL, synthesize, NULL));
   char *first, *second;
   assert(!pthread_join(a, (void **)&first) && !pthread_join(b, (void **)&second));
   assert(strcmp(first, second));
   check_file(first, text, strlen(text), ".wav");
   check_file(second, text, strlen(text), ".wav");
   remove_audio(first);
   remove_audio(second);
   assert(!setenv("AIMEE_TTS_FIXTURE_FAIL", "1", 1));
   assert(tts_synthesize(text, &path) < 0 && !path && file_count(directory) == 1);
   unsetenv("AIMEE_TTS_FIXTURE_FAIL");
   assert(!unlink(helper));
   assert(tts_synthesize(text, &path) < 0 && !path && !file_count(directory));
   assert(!rmdir(directory));
   free(helper);
   free(text);
   puts("TTS: structured requests, full binary audio, owned concurrent outputs and local stdin "
        "passed");
   return 0;
}
