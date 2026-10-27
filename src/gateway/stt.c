#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
/* stt.c: speech-to-text dispatcher */
#include "stt.h"
#include "aimee.h"
#include "log.h"
#include "agent_exec.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <spawn.h>
#include <stdint.h>
#include <sys/wait.h>
#include <unistd.h>
#include <openssl/rand.h>
#include <aimee/ir/aimee_ir.h>
#include "cJSON.h"

extern char **environ;

static const char *hallucination_phrases[] = {"thanks for watching", "subscribe to",
                                              "thank you for watching", "please like and subscribe",
                                              "see you in the next"};
static const int hallucination_count =
    sizeof(hallucination_phrases) / sizeof(hallucination_phrases[0]);

int stt_is_hallucination(const char *text)
{
   if (!text)
      return 0;

   /* case-insensitive substring check */
   size_t text_len = strlen(text);
   for (int i = 0; i < hallucination_count; i++)
   {
      const char *phrase = hallucination_phrases[i];
      size_t phrase_len = strlen(phrase);

      for (size_t j = 0; j + phrase_len <= text_len; j++)
      {
         size_t k;
         for (k = 0; k < phrase_len; k++)
         {
            if (tolower((unsigned char)text[j + k]) != tolower((unsigned char)phrase[k]))
               break;
         }
         if (k == phrase_len)
            return 1;
      }
   }
   return 0;
}

/* Domain boundary: provider/Whisper JSON never becomes message text. */
static int transcript(const char *json, size_t length, char *out, size_t capacity)
{
   cJSON *root = aimee_ir_json_parse_frame(json, length);
   const cJSON *text = cJSON_GetObjectItemCaseSensitive(root, "text");
   int ok = cJSON_IsObject(root) && cJSON_IsString(text) && text->valuestring;
   size_t n = ok ? strlen(text->valuestring) : 0;
   while (n && (text->valuestring[n - 1] == '\r' || text->valuestring[n - 1] == '\n'))
      n--;
   if (ok && n < capacity)
   {
      memcpy(out, text->valuestring, n);
      out[n] = '\0';
   }
   else
      ok = 0;
   cJSON_Delete(root);
   return ok ? 0 : -1;
}

static char *read_bytes(const char *path, size_t *length)
{
   FILE *file = fopen(path, "rb");
   if (!file)
      return NULL;
   size_t size = 0, capacity = 4096;
   char *bytes = malloc(capacity);
   while (bytes)
   {
      size_t n = fread(bytes + size, 1, capacity - size, file);
      size += n;
      if (ferror(file))
         break;
      if (feof(file))
      {
         fclose(file);
         *length = size;
         return bytes;
      }
      if (size == capacity)
      {
         if (capacity > SIZE_MAX / 2)
            break;
         char *grown = realloc(bytes, capacity * 2);
         if (!grown)
            break;
         bytes = grown;
         capacity *= 2;
      }
   }
   fclose(file);
   free(bytes);
   return NULL;
}

/* Whisper writes a JSON result file; console output is progress, not transcript.
 * Literal argv and a private result directory preserve path/model data and keep
 * concurrent calls separate. */
static int stt_transcribe_local(const char *path, char *out_text, size_t out_size)
{
   const char *model = getenv("AIMEE_GATEWAY_STT_MODEL");
   if (!model || !model[0])
      model = "base";
   char *audio = realpath(path, NULL);
   const char *temporary = getenv("TMPDIR");
   if (!temporary || !temporary[0])
      temporary = "/tmp";
   char *directory = NULL;
   if (!audio || asprintf(&directory, "%s/aimee-stt-XXXXXX", temporary) < 0 || !mkdtemp(directory))
   {
      free(directory);
      free(audio);
      return -1;
   }
   const char *name = strrchr(audio, '/');
   name = name ? name + 1 : audio;
   const char *dot = strrchr(name, '.');
   size_t stem = dot && dot != name ? (size_t)(dot - name) : strlen(name);
   size_t result_length = strlen(directory) + stem + sizeof "/.json";
   char *result = malloc(result_length);
   int rc = -1;
   if (!result)
      goto done;
   snprintf(result, result_length, "%s/%.*s.json", directory, (int)stem, name);
   char *const argv[] = {"whisper", "--model",      (char *)model, "--output_format",
                         "json",    "--output_dir", directory,     "--verbose",
                         "False",   "--",           audio,         NULL};
   posix_spawn_file_actions_t actions;
   int initialized = posix_spawn_file_actions_init(&actions) == 0;
   pid_t child = -1;
   int launched =
       initialized &&
       posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0) == 0 &&
       posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "/dev/null", O_WRONLY, 0) == 0 &&
       posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0) == 0 &&
       posix_spawnp(&child, "whisper", &actions, NULL, argv, environ) == 0;
   if (initialized)
      posix_spawn_file_actions_destroy(&actions);
   int status = 0;
   pid_t waited = -1;
   if (launched)
      do
         waited = waitpid(child, &status, 0);
      while (waited < 0 && errno == EINTR);
   if (launched && waited == child && WIFEXITED(status) && WEXITSTATUS(status) == 0)
   {
      size_t length = 0;
      char *json = read_bytes(result, &length);
      if (json)
         rc = transcript(json, length, out_text, out_size);
      free(json);
   }
   unlink(result);
done:
   free(result);
   rmdir(directory);
   free(directory);
   free(audio);
   return rc;
}

/* Filename is a quoted multipart parameter, never another MIME header. */
static char *multipart_filename(const char *path)
{
   const char *name = strrchr(path, '/');
   name = name ? name + 1 : path;
   size_t n = strlen(name);
   if (n > (SIZE_MAX - 1) / 3)
      return NULL;
   char *escaped = malloc(3 * n + 1), *at = escaped;
   if (!escaped)
      return NULL;
   for (size_t i = 0; i < n; i++)
   {
      unsigned char ch = (unsigned char)name[i];
      if (ch == '"' || ch == '\\' || ch == '\r' || ch == '\n')
      {
         snprintf(at, 4, "%%%02X", ch);
         at += 3;
      }
      else
         *at++ = (char)ch;
   }
   *at = '\0';
   return escaped;
}

static int stt_transcribe_api(const char *provider, const char *endpoint, const char *api_key_env,
                              const char *path, char *out_text, size_t out_size)
{
   const char *key = getenv(api_key_env), *model = getenv("AIMEE_GATEWAY_STT_MODEL");
   if (!key || !key[0])
      return -1;
   if (!model || !model[0])
      model = strcmp(provider, "groq") == 0 ? "whisper-large-v3-turbo" : "whisper-1";
   size_t audio_length = 0;
   char *audio = read_bytes(path, &audio_length), *filename = multipart_filename(path);
   char *body = NULL, *header = NULL, *response = NULL;
   int rc = -1;
   if (!audio || !filename)
      goto done;
   char boundary[53] = "aimee-stt-";
   unsigned char random[20];
   do
   {
      if (RAND_bytes(random, sizeof random) != 1)
         goto done;
      for (size_t i = 0; i < sizeof random; i++)
         snprintf(boundary + 10 + 2 * i, 3, "%02x", random[i]);
   } while (memmem(audio, audio_length, boundary, strlen(boundary)) || strstr(model, boundary));
#define STT_MULTIPART_PREFIX                                                                       \
   "--%s\r\nContent-Disposition: form-data; name=\"model\"\r\n\r\n%s\r\n"                          \
   "--%s\r\nContent-Disposition: form-data; name=\"response_format\"\r\n\r\njson\r\n"              \
   "--%s\r\nContent-Disposition: form-data; name=\"file\"; filename=\"%s\"\r\n"                    \
   "Content-Type: application/octet-stream\r\n\r\n"
   int prefix =
       snprintf(NULL, 0, STT_MULTIPART_PREFIX, boundary, model, boundary, boundary, filename);
   size_t suffix = 2 + 2 + strlen(boundary) + 4;
   if (prefix < 0 || audio_length > SIZE_MAX - (size_t)prefix - suffix - 1)
      goto done;
   size_t body_length = (size_t)prefix + audio_length + suffix;
   body = malloc(body_length + 1);
   size_t key_length = strlen(key);
   if (key_length > SIZE_MAX - sizeof "Authorization: Bearer ")
      goto done;
   header = malloc(key_length + sizeof "Authorization: Bearer ");
   if (!body || !header)
      goto done;
   snprintf(body, (size_t)prefix + 1, STT_MULTIPART_PREFIX, boundary, model, boundary, boundary,
            filename);
   memcpy(body + prefix, audio, audio_length);
   snprintf(body + prefix + audio_length, suffix + 1, "\r\n--%s--\r\n", boundary);
   snprintf(header, key_length + sizeof "Authorization: Bearer ", "Authorization: Bearer %s", key);
   char content_type[128];
   snprintf(content_type, sizeof content_type, "multipart/form-data; boundary=%s", boundary);
   size_t response_length = 0;
   int status = agent_http_post_content_type_bytes(
       endpoint, header, content_type, body, body_length, &response, &response_length, 30000, NULL);
   if (status == 200 && response)
      rc = transcript(response, response_length, out_text, out_size);
done:
   free(audio);
   free(filename);
   free(body);
   free(header);
   free(response);
   return rc;
}

int stt_transcribe_file(const char *path, char *out_text, size_t out_size)
{
   if (!out_text || !out_size)
      return -1;
   out_text[0] = '\0';
   if (!path)
      return -1;
   const char *provider = getenv("AIMEE_GATEWAY_STT_PROVIDER");
   if (!provider)
      provider = "local";
   if (strcmp(provider, "local") == 0)
      return stt_transcribe_local(path, out_text, out_size);
   if (strcmp(provider, "openai") == 0)
      return stt_transcribe_api(provider, "https://api.openai.com/v1/audio/transcriptions",
                                "OPENAI_API_KEY", path, out_text, out_size);
   if (strcmp(provider, "groq") == 0)
      return stt_transcribe_api(provider, "https://api.groq.com/openai/v1/audio/transcriptions",
                                "GROQ_API_KEY", path, out_text, out_size);
   LOG_WARN("stt", "unknown STT provider: %s", provider);
   return -1;
}
