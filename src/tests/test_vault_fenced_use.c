#define _GNU_SOURCE
#include "vault_service.h"
#include "vault_internal.h"
#include "vault_server_key.h"
#include "cJSON.h"
#include <aimee/core/codec/base64.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <openssl/crypto.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/wait.h>
#include <unistd.h>

static char home[512], generation[VAULT_IMMUTABLE_GENERATION_CAP];
static const char *principal = "uid:7701", *agent = "fenced-provider", *slot = "entry";
static const char *value = "synthetic bounded fenced use value";
static unsigned callbacks, cleaned;
static int fail_callback, reenter, mock_mode, mock_calls, snapshot_calls, fail_allocate;
static void *watched;
static size_t watched_capacity;
extern void __real_OPENSSL_cleanse(void *, size_t);
extern void *__real_calloc(size_t, size_t);
void *__wrap_calloc(size_t count, size_t size)
{
   if (fail_allocate && count == 1 && size == 65537)
   {
      fail_allocate = 0;
      errno = ENOMEM;
      return NULL;
   }
   return __real_calloc(count, size);
}
void __wrap_OPENSSL_cleanse(void *p, size_t n)
{
   __real_OPENSSL_cleanse(p, n);
   if (p == watched && n == watched_capacity)
   {
      for (size_t i = 0; i < n; i++)
         assert(!((unsigned char *)p)[i]);
      watched = NULL;
      cleaned++;
   }
}
static int use_value(const char *secret, size_t length, void *context)
{
   (void)context;
   callbacks++;
   assert(vault_fenced_use_callback_active());
   assert(length == strlen(value) && !memcmp(secret, value, length));
   if (reenter)
   {
      uint8_t key[VAULT_KEK_LEN];
      memset(key, 0xa5, sizeof key);
      assert(vault_server_kek(key) == -1);
      for (size_t i = 0; i < sizeof key; i++)
         assert(!key[i]);
      assert(vault_store_delete(principal, agent, slot) == -1);
      assert(vault_service_use_server_wrap_generation_fenced(principal, agent, slot, generation,
                                                             use_value, NULL) == VAULT_ERR_BADARG);
      assert(vault_server_key_rotate(VAULT_SERVER_PRINCIPAL, NULL, NULL, NULL, 0, NULL, 0) == -1);
      assert(vault_seal() == -1 && vault_unseal(NULL, 0) == -1);
      assert(vault_is_sealed() == 1 &&
             vault_custody_selected_local_status() == VAULT_CUSTODY_LOCAL_UNAVAILABLE);
      uint64_t epoch = 123;
      assert(vault_custody_selected_authorization_preflight_current(secret, length, &epoch) ==
                 VAULT_CUSTODY_AUTH_INTEGRITY_FAILURE &&
             !epoch);
      assert(vault_use_epoch_snapshot() == 0);
      memset(key, 0xa5, sizeof key);
      assert(vault_use_begin(1, 1, key) == -1);
      for (size_t i = 0; i < sizeof key; i++)
         assert(!key[i]);
   }
   return fail_callback;
}
static int snapshot(void *ctx, const char *p, const uint8_t *key, const char *a, const char *c,
                    const char *g, char *out, size_t cap)
{
   (void)ctx;
   (void)p;
   (void)key;
   (void)a;
   (void)c;
   (void)g;
   (void)out;
   (void)cap;
   snapshot_calls++;
   return -1;
}
static int mock_fenced(void *ctx, const char *p, const uint8_t *key, const char *a, const char *c,
                       const char *g, char *out, size_t cap, vault_store_secret_use_fn use,
                       void *opaque)
{
   (void)ctx;
   (void)p;
   (void)key;
   (void)a;
   (void)c;
   (void)g;
   mock_calls++;
   watched = out;
   watched_capacity = cap;
   if (mock_mode == 1)
   {
      memset(out, 'x', cap);
      return use(out, cap - 1, opaque);
   }
   if (mock_mode == 2)
      return use("foreign", 7, opaque);
   if (mock_mode == 3)
      return 0;
   if (mock_mode == 7)
   {
      assert(use("foreign", 7, opaque) != 0);
      return VAULT_STORE_RETIRED;
   }
   strcpy(out, value);
   if (mock_mode == 4)
      return use(out, strlen(out) - 1, opaque);
   int rc = use(out, strlen(out), opaque);
   if (mock_mode == 5)
   {
      assert(use(out, strlen(out), opaque) != 0);
      return 0;
   }
   if (mock_mode == 6)
      return -1;
   return rc;
}
static void provider_contract(void)
{
   vault_store_backend_t backend = {.name = "bounded-fenced-test",
                                    .get_server_generation = snapshot};
   uint8_t key[VAULT_KEK_LEN] = {0};
   vault_store_set_backend(&backend);
   assert(vault_store_use_server_generation_fenced(principal, key, agent, slot, generation,
                                                   use_value,
                                                   NULL) == VAULT_STORE_GENERATION_UNSUPPORTED);
   assert(vault_service_use_server_wrap_generation_fenced(
              principal, agent, slot, generation, use_value, NULL) == VAULT_ERR_UNSUPPORTED_OP);
   assert(!snapshot_calls && !callbacks);
   backend.use_server_generation_fenced = mock_fenced;
   fail_allocate = 1;
   assert(vault_store_use_server_generation_fenced(principal, key, agent, slot, generation,
                                                   use_value, NULL) == -1);
   assert(!fail_allocate && !mock_calls && !callbacks && !vault_fenced_use_callback_active());
   for (mock_mode = 1; mock_mode <= 7; mock_mode++)
   {
      unsigned before = callbacks, scrubbed = cleaned;
      int rc = vault_store_use_server_generation_fenced(principal, key, agent, slot, generation,
                                                        use_value, NULL);
      assert(rc != 0 && cleaned == scrubbed + 1 && !watched);
      assert(callbacks == before + (mock_mode == 5 || mock_mode == 6));
   }
   mock_mode = 0;
   reenter = 1;
   unsigned before = callbacks;
   assert(!vault_store_use_server_generation_fenced(principal, key, agent, slot, generation,
                                                    use_value, NULL));
   assert(callbacks == before + 1);
   reenter = 0;
   fail_callback = 1;
   before = callbacks;
   assert(vault_store_use_server_generation_fenced(principal, key, agent, slot, generation,
                                                   use_value, NULL) == VAULT_STORE_USE_FAILED);
   assert(callbacks == before + 1);
   fail_callback = 0;
   assert(!snapshot_calls && !vault_fenced_use_callback_active());
   vault_store_set_backend(NULL);
}
static void malformed_actual_entry(void)
{
   char encoded[256], path[1024];
   size_t length;
   assert(aimee_codec_base64_encode((const uint8_t *)principal, strlen(principal), encoded,
                                    sizeof encoded, AIMEE_BASE64_URL, &length));
   assert(snprintf(path, sizeof path, "%s/.vault/%s.json", home, encoded) < (int)sizeof path);
   FILE *file = fopen(path, "rb");
   assert(file && !fseek(file, 0, SEEK_END));
   long n = ftell(file);
   assert(n > 0);
   rewind(file);
   char *original = calloc((size_t)n + 1, 1);
   assert(original && fread(original, 1, (size_t)n, file) == (size_t)n && !fclose(file));
   unsigned before = callbacks;
   for (int mode = 0; mode < 3; mode++)
   {
      cJSON *root = cJSON_Parse(original),
            *entry = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(root, "creds"), 0);
      assert(entry);
      char pin[33];
      strcpy(pin, generation);
      if (mode == 0)
         cJSON_ReplaceItemInObjectCaseSensitive(entry, "immutable_generation_v1",
                                                cJSON_CreateBool(0));
      if (mode == 1)
      {
         pin[0] = pin[0] == 'a' ? 'b' : 'a';
         cJSON_ReplaceItemInObjectCaseSensitive(entry, "immutable_generation_v1",
                                                cJSON_CreateString(pin));
      }
      if (mode == 2)
         cJSON_DeleteItemFromObjectCaseSensitive(entry, "immutable_generation_v1");
      char *bytes = cJSON_PrintUnformatted(root);
      assert(bytes);
      file = fopen(path, "wb");
      assert(file && fwrite(bytes, 1, strlen(bytes), file) == strlen(bytes) && !fclose(file));
      free(bytes);
      cJSON_Delete(root);
      assert(vault_service_use_server_wrap_generation_fenced(principal, agent, slot, pin, use_value,
                                                             NULL) != VAULT_OK &&
             callbacks == before);
   }
   file = fopen(path, "wb");
   assert(file && fwrite(original, 1, (size_t)n, file) == (size_t)n && !fclose(file));
   free(original);
}
static void actual_value_bounds(void)
{
   char pin[33];
   const char *saved = value;
   value = "";
   assert(vault_service_create_server_wrap_once(principal, agent, "empty", value, pin,
                                                sizeof pin) == VAULT_CREATE_ONCE_CREATED);
   assert(vault_service_use_server_wrap_generation_fenced(principal, agent, "empty", pin, use_value,
                                                          NULL) == VAULT_OK);
   char *maximum = malloc(65537);
   assert(maximum);
   memset(maximum, 'q', 65536);
   maximum[65536] = 0;
   value = maximum;
   assert(vault_service_create_server_wrap_once(principal, agent, "maximum", value, pin,
                                                sizeof pin) == VAULT_CREATE_ONCE_CREATED);
   assert(vault_service_use_server_wrap_generation_fenced(principal, agent, "maximum", pin,
                                                          use_value, NULL) == VAULT_OK);
   value = saved;
   OPENSSL_cleanse(maximum, 65537);
   free(maximum);
}
static int snapshot_retire(const char *secret, size_t length, void *context)
{
   (void)context;
   assert(!vault_fenced_use_callback_active());
   assert(length == strlen(value) && !memcmp(secret, value, length));
   assert(vault_service_delete(principal, agent, "snapshot") == VAULT_OK);
   return 0;
}
static void snapshot_unchanged(void)
{
   char pin[33];
   assert(vault_service_create_server_wrap_once(principal, agent, "snapshot", value, pin,
                                                sizeof pin) == VAULT_CREATE_ONCE_CREATED);
   assert(vault_service_use_server_wrap_generation(principal, agent, "snapshot", pin,
                                                   snapshot_retire, NULL) == VAULT_OK);
   unsigned before = callbacks;
   assert(vault_service_use_server_wrap_generation_fenced(principal, agent, "snapshot", pin,
                                                          use_value, NULL) == VAULT_ERR_RETIRED);
   assert(callbacks == before);
}
static int started[2], attempted[2], finished[2];
static int guarded_use(const char *secret, size_t length, void *context)
{
   assert(!use_value(secret, length, context));
   assert(write(started[1], "s", 1) == 1);
   char b;
   assert(read(attempted[0], &b, 1) == 1 && b == 'a');
   struct pollfd wait = {.fd = finished[0], .events = POLLIN};
   assert(poll(&wait, 1, 100) == 0); /* Actual lock probe in child already refused. */
   return 0;
}
static void actual_process_retirement(void)
{
   assert(!pipe(started) && !pipe(attempted) && !pipe(finished));
   pid_t child = fork();
   assert(child >= 0); /* Fork before any provider lock is held. */
   if (!child)
   {
      close(started[1]);
      close(attempted[0]);
      close(finished[0]);
      char b;
      assert(read(started[0], &b, 1) == 1);
      char lock_path[1024];
      assert(snprintf(lock_path, sizeof lock_path, "%s/.vault/.write.lock", home) <
             (int)sizeof lock_path);
      int fd = open(lock_path, O_RDWR | O_CLOEXEC);
      assert(fd >= 0);
      assert(flock(fd, LOCK_EX | LOCK_NB) == -1 && (errno == EWOULDBLOCK || errno == EAGAIN));
      close(fd);
      assert(write(attempted[1], "a", 1) == 1);
      assert(vault_service_delete(principal, agent, slot) == VAULT_OK);
      assert(write(finished[1], "d", 1) == 1);
      _exit(0);
   }
   close(started[0]);
   close(attempted[1]);
   close(finished[1]);
   unsigned before = callbacks;
   assert(vault_service_use_server_wrap_generation_fenced(principal, agent, slot, generation,
                                                          guarded_use, NULL) == VAULT_OK);
   assert(callbacks == before + 1);
   char b;
   assert(read(finished[0], &b, 1) == 1 && b == 'd');
   int status;
   assert(waitpid(child, &status, 0) == child && WIFEXITED(status) && !WEXITSTATUS(status));
   close(started[1]);
   close(attempted[0]);
   close(finished[0]);
   assert(vault_service_use_server_wrap_generation_fenced(principal, agent, slot, generation,
                                                          use_value, NULL) == VAULT_ERR_RETIRED);
   assert(callbacks == before + 1);
}
int main(void)
{
   const char *tmp = getenv("TMPDIR");
   if (!tmp)
      tmp = "/tmp";
   assert(snprintf(home, sizeof home, "%s/fenced-use-XXXXXX", tmp) < (int)sizeof home);
   assert(mkdtemp(home) && !setenv("AIMEE_HOME", home, 1));
   assert(aimee_vault_generation_fenced_use_contract_v1 == 1);
   assert(vault_service_create_server_wrap_once(principal, agent, slot, value, generation,
                                                sizeof generation) == VAULT_CREATE_ONCE_CREATED);
   provider_contract();
   unsigned before = callbacks;
   reenter = 1;
   assert(vault_service_use_server_wrap_generation_fenced(principal, agent, slot, generation,
                                                          use_value, NULL) == VAULT_OK);
   assert(callbacks == before + 1);
   reenter = 0;
   fail_callback = 1;
   before = callbacks;
   assert(vault_service_use_server_wrap_generation_fenced(principal, agent, slot, generation,
                                                          use_value, NULL) == VAULT_ERR_IO);
   assert(callbacks == before + 1);
   fail_callback = 0;
   assert(vault_service_use_server_wrap_generation_fenced(
              principal, agent, slot, generation, use_value, NULL) == VAULT_OK); /* released */
   before = callbacks;
   char bad[33];
   strcpy(bad, generation);
   bad[0] = bad[0] == 'a' ? 'b' : 'a';
   assert(vault_service_use_server_wrap_generation_fenced(principal, agent, slot, bad, use_value,
                                                          NULL) == VAULT_ERR_GENERATION_MISMATCH);
   assert(vault_service_use_server_wrap_generation_fenced("uid:7702", agent, slot, generation,
                                                          use_value,
                                                          NULL) == VAULT_ERR_GENERATION_MISMATCH);
   assert(vault_service_use_server_wrap_generation_fenced(VAULT_SERVER_PRINCIPAL, agent, slot,
                                                          generation, use_value,
                                                          NULL) == VAULT_ERR_UNSUPPORTED_OP);
   assert(callbacks == before);
   assert(vault_service_use_server_wrap_generation_fenced(principal, agent, slot, "malformed",
                                                          use_value, NULL) != VAULT_OK &&
          callbacks == before);
   char long_name[161];
   memset(long_name, 'x', 160);
   long_name[160] = 0;
   assert(vault_service_use_server_wrap_generation_fenced(long_name, agent, slot, generation,
                                                          use_value, NULL) == VAULT_ERR_BADARG);
   long_name[64] = 0;
   assert(vault_service_use_server_wrap_generation_fenced(principal, long_name, slot, generation,
                                                          use_value, NULL) == VAULT_ERR_BADARG);
   assert(callbacks == before);
   snapshot_unchanged();
   malformed_actual_entry();
   actual_value_bounds();
   actual_process_retirement();
   puts("PASS actual encrypted fenced use, malformed provider bounds, reentry and two-process "
        "retirement");
   return 0;
}
