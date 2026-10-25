/* Optional speech adapters. Callers own and unlink each successful audio file. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "tts.h"
#include "agent_exec.h"
#include "cJSON.h"
#include "log.h"
#include <errno.h>
#include <fcntl.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

int tts_enabled(void)
{
   const char *provider = getenv("AIMEE_GATEWAY_TTS_PROVIDER");
   return provider && provider[0] ? 1 : 0;
}

static int tts_output(const char *format, char **path)
{
   const char *directory = getenv("TMPDIR");
   if (!directory || !directory[0])
      directory = "/tmp";
   if (asprintf(path, "%s/aimee-tts-XXXXXX.%s", directory, format) < 0)
   {
      *path = NULL;
      return -1;
   }
   int fd = mkstemps(*path, (int)strlen(format) + 1);
   if (fd >= 0 && fcntl(fd, F_SETFD, FD_CLOEXEC) < 0)
   {
      close(fd);
      unlink(*path);
      fd = -1;
   }
   if (fd < 0)
   {
      free(*path);
      *path = NULL;
   }
   return fd;
}

static int tts_write_audio(const char *audio, size_t length, const char *format, char **out)
{
   char *path = NULL;
   int fd = tts_output(format, &path);
   if (fd < 0)
      return -1;
   FILE *file = fdopen(fd, "wb");
   int failed = 1;
   if (file)
   {
      failed = fwrite(audio, 1, length, file) != length;
      if (fclose(file) != 0)
         failed = 1;
   }
   else
      close(fd);
   if (failed)
   {
      unlink(path);
      free(path);
      return -1;
   }
   *out = path;
   return 0;
}

static int tts_local(const char *text, char **out)
{
   char *path = NULL;
   int fd = tts_output("wav", &path);
   if (fd < 0)
      return -1;
   if (close(fd) != 0)
      goto fail;
   /* Feed text on stdin, never into a shell or the option parser. An owned
    * anonymous input file also avoids pipe backpressure and argv size limits. */
   FILE *input = tmpfile();
   if (!input)
      goto fail;
   size_t length = strlen(text);
   if (fcntl(fileno(input), F_SETFD, FD_CLOEXEC) < 0 || fwrite(text, 1, length, input) != length ||
       fflush(input) != 0 || fseek(input, 0, SEEK_SET) != 0)
   {
      fclose(input);
      goto fail;
   }
   posix_spawn_file_actions_t actions;
   if (posix_spawn_file_actions_init(&actions) != 0)
   {
      fclose(input);
      goto fail;
   }
   int error = posix_spawn_file_actions_adddup2(&actions, fileno(input), STDIN_FILENO);
   if (!error && fileno(input) != STDIN_FILENO)
      error = posix_spawn_file_actions_addclose(&actions, fileno(input));
   if (!error)
      error = posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
   if (!error)
      error = posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
   char *argv[] = {"espeak-ng", "-w", path, "--stdin", NULL};
   pid_t pid;
   if (!error)
      error = posix_spawnp(&pid, argv[0], &actions, NULL, argv, environ);
   posix_spawn_file_actions_destroy(&actions);
   int input_error = fclose(input);
   if (error)
      goto fail;
   int status;
   pid_t waited;
   do
      waited = waitpid(pid, &status, 0);
   while (waited < 0 && errno == EINTR);
   struct stat audio;
   if (input_error || waited != pid || !WIFEXITED(status) || WEXITSTATUS(status) != 0 ||
       stat(path, &audio) != 0 || audio.st_size == 0)
      goto fail;
   *out = path;
   return 0;
fail:
   unlink(path);
   free(path);
   return -1;
}

static int tts_remote(const char *text, const char *url, const char *auth, const char *model,
                      const char *voice, const char *format, char **out)
{
   cJSON *request = cJSON_CreateObject();
   if (!request)
      return -1;
   int complete = cJSON_AddStringToObject(request, "model", model) &&
                  cJSON_AddStringToObject(request, "voice", voice) &&
                  cJSON_AddStringToObject(request, "input", text) &&
                  cJSON_AddStringToObject(request, "response_format", format);
   char *body = complete ? cJSON_PrintUnformatted(request) : NULL;
   cJSON_Delete(request);
   if (!body)
      return -1;
   char *response = NULL;
   size_t length = 0;
   int status = agent_http_post_content_type_bytes(url, auth, "application/json", body,
                                                   strlen(body), &response, &length, 30000, NULL);
   free(body);
   int result = -1;
   if (status == 200 && response && length)
      result = tts_write_audio(response, length, format, out);
   else
      LOG_WARN("tts", "Speech provider HTTP %d", status);
   free(response);
   return result;
}

int tts_synthesize(const char *text, char **out)
{
   if (!out)
      return -1;
   *out = NULL;
   if (!text || !text[0])
      return -1;
   const char *provider = getenv("AIMEE_GATEWAY_TTS_PROVIDER");
   const char *voice = getenv("AIMEE_GATEWAY_TTS_VOICE");
   if (provider && strcmp(provider, "openai") == 0)
   {
      const char *key = getenv("OPENAI_API_KEY");
      if (!key || !key[0])
         return -1;
      const char *model = getenv("AIMEE_GATEWAY_TTS_MODEL");
      char *auth = NULL;
      if (asprintf(&auth, "Authorization: Bearer %s", key) < 0)
         return -1;
      int result = tts_remote(text, "https://api.openai.com/v1/audio/speech", auth,
                              model && model[0] ? model : "tts-1",
                              voice && voice[0] ? voice : "alloy", "mp3", out);
      free(auth);
      return result;
   }
   if (provider && strcmp(provider, "kokoro") == 0)
   {
      const char *base = getenv("AIMEE_GATEWAY_TTS_BASE_URL");
      char *url = NULL;
      if (asprintf(&url, "%s/v1/audio/speech", base && base[0] ? base : "http://localhost:8880") <
          0)
         return -1;
      int result =
          tts_remote(text, url, NULL, "kokoro", voice && voice[0] ? voice : "af_sky", "wav", out);
      free(url);
      return result;
   }
   return tts_local(text, out);
}
