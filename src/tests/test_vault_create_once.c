/* Provider-atomic create/replay, not a memory policy or authenticated caller test. */
#define _GNU_SOURCE
#include "vault_store.h"
#include "vault_service.h"
#include "vault_internal.h"
#include "vault_server_key.h"
#include "cJSON.h"
#include <aimee/core/codec/base64.h>
#include <assert.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
static int fail_directory_sync;
static unsigned sync_calls;
extern int __real_fsync(int fd);
int __wrap_fsync(int fd)
{
   sync_calls++;
   struct stat st;
   if (fail_directory_sync && !fstat(fd, &st) && S_ISDIR(st.st_mode))
   {
      fail_directory_sync = 0;
      errno = EIO;
      return -1;
   }
   return __real_fsync(fd);
}
extern const unsigned aimee_vault_immutable_custody_contract_v2;
static char home[512];
static uint8_t key[VAULT_KEK_LEN];
static char generation[VAULT_IMMUTABLE_GENERATION_CAP];
static void path(const char *principal, char out[1024])
{
   char encoded[256];
   size_t written;
   assert(aimee_codec_base64_encode((const uint8_t *)principal, strlen(principal), encoded,
                                    sizeof encoded, AIMEE_BASE64_URL, &written));
   assert(snprintf(out, 1024, "%s/.vault/%s.json", home, encoded) < 1024);
}
static char *blob(const char *principal)
{
   char filename[1024];
   path(principal, filename);
   FILE *f = fopen(filename, "rb");
   assert(f);
   assert(!fseek(f, 0, SEEK_END));
   long n = ftell(f);
   assert(n > 0 && n < 1024 * 1024);
   rewind(f);
   char *out = calloc((size_t)n + 1, 1);
   assert(out && fread(out, 1, (size_t)n, f) == (size_t)n);
   assert(!fclose(f));
   return out;
}
static void replace(const char *principal, const char *bytes)
{
   char filename[1024];
   path(principal, filename);
   FILE *f = fopen(filename, "wb");
   assert(f);
   assert(fwrite(bytes, 1, strlen(bytes), f) == strlen(bytes));
   assert(!fclose(f));
}
static vault_create_once_result_t create(const char *principal, const char *slot,
                                         const char *secret)
{
   return vault_store_create_server_once(principal, key, "opaque-provider", slot, secret,
                                         generation, sizeof generation);
}
static void file_contract(void)
{
   const char *p = "uid:7001", *secret = "synthetic exact custody value";
   assert(create(p, "entry", secret) == VAULT_CREATE_ONCE_CREATED);
   char pin[sizeof generation];
   memcpy(pin, generation, sizeof pin);
   char *before = blob(p);
   assert(!strstr(before, secret));
   assert(create(p, "entry", secret) == VAULT_CREATE_ONCE_REPLAY);
   assert(!strcmp(pin, generation));
   char *after = blob(p);
   assert(!strcmp(before, after));
   free(after);
   assert(create(p, "entry", "different value") == VAULT_CREATE_ONCE_CONFLICT);
   after = blob(p);
   assert(!strcmp(before, after));
   free(after);
   char out[128];
   assert(!vault_store_get_server_generation(p, key, "opaque-provider", "entry", pin, out,
                                             sizeof out));
   memset(out, 'x', sizeof out);
   assert(vault_store_get_server(p, key, "opaque-provider", "entry", out, sizeof out) != 0);
   assert(!out[0]);
   assert(!vault_store_get_server_generation(p, key, "opaque-provider", "entry", pin, out,
                                             sizeof out));
   assert(!strcmp(out, secret));
   uint8_t wrong[VAULT_KEK_LEN];
   memcpy(wrong, key, sizeof wrong);
   wrong[0] ^= 1;
   assert(vault_store_create_server_once(p, wrong, "opaque-provider", "entry", secret, generation,
                                         sizeof generation) == VAULT_CREATE_ONCE_CRYPTO);
   after = blob(p);
   assert(!strcmp(before, after));
   free(after);
   /* Same locator is independent under another actual principal. */
   assert(create("uid:7002", "entry", "other principal value") == VAULT_CREATE_ONCE_CREATED);
   char *other = blob("uid:7002");
   cJSON *a = cJSON_Parse(before), *b = cJSON_Parse(other);
   assert(a && b);
   assert(cJSON_ReplaceItemInObjectCaseSensitive(
       b, "creds", cJSON_Duplicate(cJSON_GetObjectItemCaseSensitive(a, "creds"), 1)));
   char *forged = cJSON_PrintUnformatted(b);
   assert(forged);
   replace("uid:7002", forged);
   assert(create("uid:7002", "entry", secret) == VAULT_CREATE_ONCE_CRYPTO);
   after = blob("uid:7002");
   assert(!strcmp(after, forged));
   free(after);
   free(forged);
   free(other);
   cJSON_Delete(a);
   cJSON_Delete(b);
   free(before);
   assert(create("", "entry", secret) == VAULT_CREATE_ONCE_BADARG);
   assert(create(p, "bad|slot", secret) == VAULT_CREATE_ONCE_BADARG);
   char *large = malloc(65538);
   assert(large);
   memset(large, 'Q', 65537);
   large[65537] = 0;
   assert(create(p, "oversize", large) == VAULT_CREATE_ONCE_BADARG);
   large[65536] = 0;
   assert(create(p, "maximum", large) == VAULT_CREATE_ONCE_CREATED);
   assert(create(p, "maximum", large) == VAULT_CREATE_ONCE_REPLAY);
   free(large);
   assert(create(p, "empty", "") == VAULT_CREATE_ONCE_CREATED);
   assert(create(p, "empty", "") == VAULT_CREATE_ONCE_REPLAY);
   puts("PASS encrypted create, byte-identical replay/conflict, wrong-key/principal AAD, 64KiB "
        "bound");
}
static void contenders(int same)
{
   int pipefd[2];
   assert(!pipe(pipefd));
   pid_t children[6];
   const char *principal = same ? "uid:7101" : "uid:7102";
   for (int i = 0; i < 6; i++)
   {
      children[i] = fork();
      assert(children[i] >= 0);
      if (!children[i])
      {
         close(pipefd[0]);
         char value[64];
         snprintf(value, sizeof value, "contender-%d", same ? 0 : i);
         int result[2] = {i, create(principal, "contended", value)};
         assert(write(pipefd[1], result, sizeof result) == sizeof result);
         close(pipefd[1]);
         _exit(0);
      }
   }
   close(pipefd[1]);
   int created = 0, replay = 0, conflict = 0, winner = -1;
   for (int i = 0; i < 6; i++)
   {
      int result[2];
      size_t n = 0;
      while (n < sizeof result)
      {
         ssize_t got = read(pipefd[0], (char *)result + n, sizeof result - n);
         assert(got > 0);
         n += (size_t)got;
      }
      if (result[1] == VAULT_CREATE_ONCE_CREATED)
      {
         created++;
         winner = result[0];
      }
      else if (result[1] == VAULT_CREATE_ONCE_REPLAY)
         replay++;
      else if (result[1] == VAULT_CREATE_ONCE_CONFLICT)
         conflict++;
      else
         assert(!"unexpected contender error");
   }
   close(pipefd[0]);
   for (int i = 0; i < 6; i++)
   {
      int status;
      assert(waitpid(children[i], &status, 0) == children[i]);
      assert(WIFEXITED(status) && !WEXITSTATUS(status));
   }
   assert(created == 1 && replay == (same ? 5 : 0) && conflict == (same ? 0 : 5));
   char expected[64], out[64];
   snprintf(expected, sizeof expected, "contender-%d", same ? 0 : winner);
   char *stored = blob(principal);
   cJSON *root = cJSON_Parse(stored);
   free(stored);
   assert(root);
   const cJSON *entry = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(root, "creds"), 0);
   const char *pin =
       cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(entry, "immutable_generation_v1"));
   assert(pin);
   assert(!vault_store_get_server_generation(principal, key, "opaque-provider", "contended", pin,
                                             out, sizeof out));
   assert(!strcmp(out, expected));
   cJSON_Delete(root);
   puts(same ? "PASS six processes: one Created + five exact Replay"
             : "PASS six processes: one Created + five Conflict; winner unchanged");
}
static int forbidden_set(void *ctx, const char *p, const uint8_t *k, const char *a, const char *c,
                         const char *s)
{
   (void)ctx;
   (void)p;
   (void)k;
   (void)a;
   (void)c;
   (void)s;
   abort();
}
static vault_create_once_result_t provider_create(void *ctx, const char *p, const uint8_t *k,
                                                  const char *a, const char *c, const char *s,
                                                  char *g, size_t cap)
{
   assert(ctx && p && k && a && c && s && g && cap >= 33);
   int *calls = ctx;
   (*calls)++;
   memcpy(g, "11111111111111111111111111111111", 33);
   return VAULT_CREATE_ONCE_REPLAY;
}
static vault_create_once_result_t bad_provider_create(void *ctx, const char *p, const uint8_t *k,
                                                      const char *a, const char *c, const char *s,
                                                      char *g, size_t cap)
{
   (void)ctx;
   (void)p;
   (void)k;
   (void)a;
   (void)c;
   (void)s;
   memset(g, 'x', cap);
   return VAULT_CREATE_ONCE_CREATED;
}
static int bad_provider_read(void *ctx, const char *p, const uint8_t *k, const char *a,
                             const char *c, const char *g, char *out, size_t cap)
{
   (void)ctx;
   (void)p;
   (void)k;
   (void)a;
   (void)c;
   (void)g;
   memset(out, 'x', cap);
   return 0;
}
static int unavailable_key(void *ctx, uint8_t *out)
{
   (void)ctx;
   (void)out;
   return -1;
}
static char audit[1024];
static unsigned audit_count;
static void hook(const char *op, const char *principal, const char *agent, const char *cred,
                 attested_transport_t transport, vault_status_t status)
{
   (void)transport;
   (void)status;
   audit_count++;
   snprintf(audit, sizeof audit, "%s:%s:%s:%s", op, principal, agent, cred);
   assert(!strstr(audit, "synthetic service secret"));
}
static vault_create_once_result_t service_create(const char *p, const char *a, const char *c,
                                                 const char *value)
{
   return vault_service_create_server_wrap_once(p, a, c, value, generation, sizeof generation);
}
static int use_secret(const char *value, size_t length, void *context)
{
   assert(length == strlen("synthetic service secret") &&
          !strcmp(value, "synthetic service secret"));
   (*(int *)context)++;
   return 0;
}
static int failing_use(const char *value, size_t length, void *context)
{
   (void)use_secret(value, length, context);
   return -1;
}
static void service_and_seam(void)
{
   vault_service_set_audit_hook(hook);
   const char *secret = "synthetic service secret";
   assert(service_create("uid:7201", "opaque-provider", "entry", secret) ==
          VAULT_CREATE_ONCE_CREATED);
   assert(strstr(audit, ".created:"));
   char service_pin[sizeof generation];
   memcpy(service_pin, generation, sizeof service_pin);
   assert(service_create("uid:7201", "opaque-provider", "entry", secret) ==
          VAULT_CREATE_ONCE_REPLAY);
   assert(strstr(audit, ".replay:"));
   assert(service_create("uid:7201", "opaque-provider", "entry", "changed") ==
          VAULT_CREATE_ONCE_CONFLICT);
   assert(strstr(audit, ".conflict:"));
   assert(audit_count == 3);
   int uses = 0;
   assert(vault_service_use_server_wrap_generation("uid:7201", "opaque-provider", "entry",
                                                   service_pin, use_secret, &uses) == VAULT_OK &&
          uses == 1);
   char bad_pin[sizeof generation];
   memcpy(bad_pin, service_pin, sizeof bad_pin);
   bad_pin[0] = bad_pin[0] == '0' ? '1' : '0';
   assert(vault_service_use_server_wrap_generation("uid:7201", "opaque-provider", "entry", bad_pin,
                                                   use_secret,
                                                   &uses) == VAULT_ERR_GENERATION_MISMATCH &&
          uses == 1);
   assert(vault_service_use_server_wrap_generation("uid:7201", "opaque-provider", "entry",
                                                   service_pin, failing_use,
                                                   &uses) == VAULT_ERR_IO &&
          uses == 2);
   vault_store_backend_t unsupported = {.name = "unsupported", .set_server = forbidden_set};
   vault_store_set_backend(&unsupported);
   assert(create("uid:7201", "entry", secret) == VAULT_CREATE_ONCE_UNSUPPORTED);
   assert(service_create("uid:7201", "opaque-provider", "entry", secret) ==
          VAULT_CREATE_ONCE_UNSUPPORTED);
   char protected_out[32];
   memset(protected_out, 'x', sizeof protected_out);
   assert(vault_store_get_server_generation("uid:7201", key, "opaque-provider", "entry",
                                            service_pin, protected_out, sizeof protected_out) ==
              VAULT_STORE_GENERATION_UNSUPPORTED &&
          !protected_out[0]);
   int calls = 0;
   unsupported.ctx = &calls;
   unsupported.create_server_once = provider_create;
   assert(create("uid:7201", "entry", secret) == VAULT_CREATE_ONCE_REPLAY && calls == 1);
   unsupported.create_server_once = bad_provider_create;
   assert(create("uid:7201", "entry", secret) == VAULT_CREATE_ONCE_ERROR && !generation[0]);
   unsupported.get_server_generation = bad_provider_read;
   assert(vault_service_use_server_wrap_generation("uid:7201", "opaque-provider", "entry",
                                                   service_pin, use_secret, &uses) != VAULT_OK &&
          uses == 2);
   vault_store_set_backend(NULL);
   vault_custody_provider_t unavailable = {.name = "unavailable", .get_kek = unavailable_key};
   vault_custody_set_provider(&unavailable);
   assert(service_create("uid:7201", "opaque-provider", "new", secret) == VAULT_CREATE_ONCE_CRYPTO);
   vault_custody_set_provider(NULL);
   vault_service_set_audit_hook(NULL);
   puts("PASS trusted service, secret-free audit outcomes, optional backend delegation/unsupported "
        "and key failure");
}
static void actual_master_rotation(void)
{
   assert(create(VAULT_SERVER_PRINCIPAL, "unsupported", "value") == VAULT_CREATE_ONCE_UNSUPPORTED &&
          !generation[0]);
   assert(service_create(VAULT_SERVER_PRINCIPAL, "opaque-provider", "unsupported", "value") ==
              VAULT_CREATE_ONCE_UNSUPPORTED &&
          !generation[0]);
   int forbidden_uses = 0;
   assert(vault_service_use_server_wrap_generation(VAULT_SERVER_PRINCIPAL, "opaque-provider",
                                                   "unsupported",
                                                   "11111111111111111111111111111111", use_secret,
                                                   &forbidden_uses) == VAULT_ERR_UNSUPPORTED_OP &&
          !forbidden_uses);

   assert(vault_service_set_server("opaque-provider", "rotation-probe", "legacy instance value") ==
          VAULT_OK);
   assert(service_create("uid:7391", "opaque-provider", "protected", "synthetic service secret") ==
          VAULT_CREATE_ONCE_CREATED);
   char pin[sizeof generation];
   memcpy(pin, generation, sizeof pin);
   uint8_t old[VAULT_KEK_LEN];
   memcpy(old, key, sizeof old);
   assert(create("uid:7391", "retired-rotation", "value") == VAULT_CREATE_ONCE_CREATED);
   char retired_pin[sizeof generation];
   memcpy(retired_pin, generation, sizeof retired_pin);
   assert(!vault_store_delete("uid:7391", "opaque-provider", "retired-rotation"));
   int principals = 0, credentials = 0;
   char backup[1024], error[256];
   assert(!vault_server_key_rotate(VAULT_SERVER_PRINCIPAL, &principals, &credentials, backup,
                                   sizeof backup, error, sizeof error));
   assert(principals == 2 && credentials == 2 && backup[0]);
   assert(!vault_server_kek(key) && memcmp(old, key, sizeof old));
   int uses = 0;
   assert(vault_service_use_server_wrap_generation("uid:7391", "opaque-provider", "protected", pin,
                                                   use_secret, &uses) == VAULT_OK &&
          uses == 1);
   assert(create("uid:7391", "protected", "synthetic service secret") == VAULT_CREATE_ONCE_REPLAY &&
          !strcmp(generation, pin));
   char out[64];
   assert(vault_store_get_server_generation("uid:7391", old, "opaque-provider", "protected", pin,
                                            out, sizeof out) != 0 &&
          !out[0]);
   assert(vault_service_use_server_wrap_generation("uid:7391", "opaque-provider",
                                                   "retired-rotation", retired_pin, use_secret,
                                                   &uses) == VAULT_ERR_RETIRED &&
          uses == 1);
   assert(create("uid:7391", "retired-rotation", "value") == VAULT_CREATE_ONCE_RETIRED &&
          !generation[0]);
   puts("PASS actual file master rotation: instance credential + immutable per-principal "
        "generation/read preserved; old server key refused");
}
static void immutable_lifecycle(void)
{
   const char *p = "uid:7401";
   assert(create(p, "fixed", "fixed value") == VAULT_CREATE_ONCE_CREATED);
   char original[sizeof generation];
   memcpy(original, generation, sizeof original);
   char *before = blob(p);
   assert(vault_store_set(p, key, "opaque-provider", "fixed", "changed") != 0);
   assert(vault_store_set_dual(p, key, key, "opaque-provider", "fixed", "changed") != 0);
   assert(vault_store_set_server(p, key, "opaque-provider", "fixed", "fixed value") != 0);
   assert(service_create(p, "opaque-provider", "fixed", "changed") == VAULT_CREATE_ONCE_CONFLICT);
   assert(vault_service_set_server_wrap(p, "opaque-provider", "fixed", "changed") != VAULT_OK);
   vault_store_write_t writes[] = {{"unrelated", "staged"}, {"fixed", "changed"}};
   assert(vault_store_write_batch(p, key, "opaque-provider", writes, 2) != 0);
   char *after = blob(p);
   assert(!strcmp(before, after));
   free(after);
   uint8_t rotated[VAULT_KEK_LEN];
   memcpy(rotated, key, sizeof rotated);
   rotated[0] ^= 7;
   assert(vault_store_rekey_field(p, "wrapped_dek_server", key, rotated) == 1);
   assert(vault_store_create_server_once(p, rotated, "opaque-provider", "fixed", "fixed value",
                                         generation,
                                         sizeof generation) == VAULT_CREATE_ONCE_REPLAY);
   assert(!strcmp(original, generation));
   char out[64];
   assert(!vault_store_get_server_generation(p, rotated, "opaque-provider", "fixed", original, out,
                                             sizeof out) &&
          !strcmp(out, "fixed value"));
   assert(!vault_store_add_server_wraps(p, key, rotated));
   char *rotated_blob = blob(p);
   cJSON *root = cJSON_Parse(rotated_blob);
   assert(root);
   cJSON *entry = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(root, "creds"), 0);
   const char *marker =
       cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(entry, "immutable_generation_v1"));
   assert(marker && !strcmp(marker, original));
   char forged[sizeof original];
   memcpy(forged, original, sizeof forged);
   forged[0] = forged[0] == '0' ? '1' : '0';
   assert(cJSON_ReplaceItemInObjectCaseSensitive(entry, "immutable_generation_v1",
                                                 cJSON_CreateString(forged)));
   char *wire = cJSON_PrintUnformatted(root);
   replace(p, wire);
   free(wire);
   assert(vault_store_get_server_generation(p, rotated, "opaque-provider", "fixed", forged, out,
                                            sizeof out) != 0 &&
          !out[0]);
   const char *malformed[] = {"", "ABCDEF0123456789ABCDEF0123456789AB",
                              "00000000000000000000000000000000",
                              "gggggggggggggggggggggggggggggggg", "123"};
   for (size_t i = 0; i < sizeof malformed / sizeof malformed[0]; i++)
   {
      assert(cJSON_ReplaceItemInObjectCaseSensitive(entry, "immutable_generation_v1",
                                                    cJSON_CreateString(malformed[i])));
      wire = cJSON_PrintUnformatted(root);
      replace(p, wire);
      free(wire);
      assert(vault_store_create_server_once(p, rotated, "opaque-provider", "fixed", "fixed value",
                                            generation,
                                            sizeof generation) == VAULT_CREATE_ONCE_CRYPTO &&
             !generation[0]);
      assert(vault_store_get_server_generation(p, rotated, "opaque-provider", "fixed", original,
                                               out, sizeof out) != 0 &&
             !out[0]);
   }
   assert(cJSON_ReplaceItemInObjectCaseSensitive(entry, "immutable_generation_v1",
                                                 cJSON_CreateFalse()));
   wire = cJSON_PrintUnformatted(root);
   replace(p, wire);
   free(wire);
   assert(vault_store_create_server_once(p, rotated, "opaque-provider", "fixed", "fixed value",
                                         generation,
                                         sizeof generation) == VAULT_CREATE_ONCE_CRYPTO &&
          !generation[0]);
   cJSON_DeleteItemFromObjectCaseSensitive(entry, "immutable_generation_v1");
   wire = cJSON_PrintUnformatted(root);
   replace(p, wire);
   free(wire);
   assert(vault_store_get_server(p, rotated, "opaque-provider", "fixed", out, sizeof out) != 0 &&
          !out[0]);
   assert(vault_store_get_server_generation(p, rotated, "opaque-provider", "fixed", original, out,
                                            sizeof out) == VAULT_STORE_GENERATION_MISMATCH);
   replace(p, rotated_blob);
   free(rotated_blob);
   cJSON_Delete(root);
   assert(!vault_store_delete(p, "opaque-provider", "fixed"));
   char *retired_blob = blob(p);
   cJSON *retired_root = cJSON_Parse(retired_blob);
   assert(retired_root);
   const cJSON *retired_entry =
       cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(retired_root, "creds"), 0);
   assert(cJSON_GetArraySize(retired_entry) == 4 &&
          cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(retired_entry, "immutable_retired_v1")));
   assert(!strcmp(cJSON_GetStringValue(
                      cJSON_GetObjectItemCaseSensitive(retired_entry, "immutable_generation_v1")),
                  original));
   assert(!cJSON_HasObjectItem(retired_entry, "ciphertext") &&
          !cJSON_HasObjectItem(retired_entry, "nonce") &&
          !cJSON_HasObjectItem(retired_entry, "tag") &&
          !cJSON_HasObjectItem(retired_entry, "wrapped_dek") &&
          !cJSON_HasObjectItem(retired_entry, "wrapped_dek_server"));
   unsigned before_retry = sync_calls;
   assert(create(p, "fixed", "fixed value") == VAULT_CREATE_ONCE_RETIRED && !generation[0]);
   assert(create(p, "fixed", "other value") == VAULT_CREATE_ONCE_RETIRED && !generation[0]);
   assert(sync_calls == before_retry); /* Retired create refuses without I/O. */
   assert(!vault_store_delete(p, "opaque-provider", "fixed"));
   assert(sync_calls == before_retry + 2); /* Retirement retry re-syncs file + directory. */
   after = blob(p);
   assert(!strcmp(after, retired_blob));
   free(after);
   assert(vault_store_get_server_generation(p, key, "opaque-provider", "fixed", original, out,
                                            sizeof out) == VAULT_STORE_RETIRED &&
          !out[0]);
   assert(vault_store_has_entry(p, "opaque-provider", "fixed") == 0);
   vault_store_entry_t listing[4];
   assert(vault_store_list(p, listing, 4) == 0);
   assert(vault_store_set_server(p, key, "opaque-provider", "fixed", "revive") != 0);
   assert(create(p, "fixed-next", "new logical value") == VAULT_CREATE_ONCE_CREATED &&
          strcmp(generation, original));
   char fresh_pin[sizeof generation];
   memcpy(fresh_pin, generation, sizeof fresh_pin);
   vault_store_write_t deletion = {"fixed-next", NULL};
   assert(!vault_store_write_batch(p, key, "opaque-provider", &deletion, 1));
   vault_store_write_t revival = {"fixed-next", "revive"};
   assert(vault_store_write_batch(p, key, "opaque-provider", &revival, 1) != 0);
   assert(create(p, "fixed-next", "new logical value") == VAULT_CREATE_ONCE_RETIRED &&
          !generation[0]);
   assert(vault_store_get_server_generation(p, key, "opaque-provider", "fixed-next", fresh_pin, out,
                                            sizeof out) == VAULT_STORE_RETIRED &&
          !out[0]);
   free(retired_blob);
   cJSON_Delete(retired_root);
   free(before);

   /* Existing mutable values cannot silently become immutable references. */
   assert(!vault_store_get_or_create_salt("uid:7402", rotated));
   assert(!vault_store_set_server("uid:7402", key, "opaque-provider", "legacy", "legacy value"));
   assert(create("uid:7402", "legacy", "legacy value") == VAULT_CREATE_ONCE_CONFLICT);
   assert(!vault_store_set_server("uid:7402", key, "opaque-provider", "legacy",
                                  "updated legacy value"));
   assert(!vault_store_get_server("uid:7402", key, "opaque-provider", "legacy", out, sizeof out) &&
          !strcmp(out, "updated legacy value"));
   char *legacy_blob = blob("uid:7402");
   cJSON *legacy_root = cJSON_Parse(legacy_blob);
   assert(legacy_root);
   cJSON *legacy_entry =
       cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(legacy_root, "creds"), 0);
   assert(cJSON_AddStringToObject(legacy_entry, "immutable_generation_v1", original));
   char *forged_legacy = cJSON_PrintUnformatted(legacy_root);
   replace("uid:7402", forged_legacy);
   assert(vault_store_get_server_generation("uid:7402", key, "opaque-provider", "legacy", original,
                                            out, sizeof out) != 0 &&
          !out[0]);
   replace("uid:7402", legacy_blob);
   free(forged_legacy);
   free(legacy_blob);
   cJSON_Delete(legacy_root);
   assert(!vault_store_delete("uid:7402", "opaque-provider", "legacy"));
   assert(
       !vault_store_set_server("uid:7402", key, "opaque-provider", "legacy", "legacy recreation"));
   puts("PASS immutable setters/batch refusal, authenticated marker, key rotation, delete/recreate "
        "Retired zero-value late-replay refusal, unchanged legacy mutable behavior");
}
static void mixed_password_rekey(void)
{
   const char *p = "uid:7451";
   uint8_t salt[VAULT_SALT_LEN], next[VAULT_KEK_LEN];
   memcpy(next, key, sizeof next);
   next[1] ^= 9;
   assert(!vault_store_get_or_create_salt(p, salt));
   assert(!vault_store_unlock_check(p, key));
   assert(!vault_store_set(p, key, "opaque-provider", "legacy-user", "user value"));
   assert(create(p, "protected", "protected value") == VAULT_CREATE_ONCE_CREATED);
   char pin[sizeof generation];
   memcpy(pin, generation, sizeof pin);
   assert(!vault_store_rekey(p, key, next));
   char out[128];
   assert(vault_store_get(p, key, "opaque-provider", "legacy-user", out, sizeof out) != 0 &&
          !out[0]);
   assert(!vault_store_get(p, next, "opaque-provider", "legacy-user", out, sizeof out) &&
          !strcmp(out, "user value"));
   assert(!vault_store_get_server_generation(p, key, "opaque-provider", "protected", pin, out,
                                             sizeof out) &&
          !strcmp(out, "protected value"));
   assert(create(p, "protected", "protected value") == VAULT_CREATE_ONCE_REPLAY &&
          !strcmp(pin, generation));
   char *valid_mixed = blob(p);
   cJSON *missing_root = cJSON_Parse(valid_mixed);
   assert(missing_root);
   cJSON *missing_entry = NULL;
   cJSON_ArrayForEach(missing_entry, cJSON_GetObjectItemCaseSensitive(missing_root, "creds")) if (
       !strcmp(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(missing_entry, "cred")),
               "protected"))
       cJSON_DeleteItemFromObjectCaseSensitive(missing_entry, "wrapped_dek_server");
   char *missing_wire = cJSON_PrintUnformatted(missing_root);
   replace(p, missing_wire);
   assert(vault_store_rekey(p, next, key) != 0);
   char *missing_after = blob(p);
   assert(!strcmp(missing_wire, missing_after));
   assert(!vault_store_unlock_check(p, next) && vault_store_unlock_check(p, key) != 0);
   replace(p, valid_mixed);
   free(valid_mixed);
   free(missing_wire);
   free(missing_after);
   cJSON_Delete(missing_root);
   char *before = blob(p);
   cJSON *root = cJSON_Parse(before);
   assert(root);
   cJSON *entries = cJSON_GetObjectItemCaseSensitive(root, "creds"), *entry = NULL;
   cJSON_ArrayForEach(
       entry,
       entries) if (!strcmp(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(entry, "cred")),
                            "legacy-user"))
   {
      assert(cJSON_ReplaceItemInObjectCaseSensitive(entry, "wrapped_dek", cJSON_CreateFalse()));
   }
   char *wire = cJSON_PrintUnformatted(root);
   replace(p, wire);
   assert(vault_store_rekey(p, next, key) != 0);
   char *after = blob(p);
   assert(!strcmp(after, wire));
   replace(p, before);
   free(before);
   free(wire);
   free(after);
   cJSON_Delete(root);
   puts("PASS verified user-password rotation skips server-only protected row; malformed present "
        "userwrap still aborts");
}
static void retirement_rotation_and_migration(void)
{
   const char *p = "uid:7461";
   uint8_t salt[VAULT_SALT_LEN], next[VAULT_KEK_LEN];
   memcpy(next, key, sizeof next);
   next[2] ^= 3;
   assert(!vault_store_get_or_create_salt(p, salt) && !vault_store_unlock_check(p, key));
   assert(!vault_store_set(p, key, "opaque-provider", "legacy-password", "value"));
   assert(create(p, "dead", "retired secret") == VAULT_CREATE_ONCE_CREATED);
   char dead[sizeof generation];
   memcpy(dead, generation, sizeof dead);
   assert(!vault_store_delete(p, "opaque-provider", "dead"));
   assert(!vault_store_rekey(p, key, next));
   assert(!vault_store_add_server_wraps(p, next, key));
   assert(create(p, "dead", "retired secret") == VAULT_CREATE_ONCE_RETIRED && !generation[0]);
   char out[128];
   assert(vault_store_get_server_generation(p, key, "opaque-provider", "dead", dead, out,
                                            sizeof out) == VAULT_STORE_RETIRED &&
          !out[0]);
   /* Real unlock executes the legacy-principal migration; only legacy data may copy. */
   uint8_t root_key[VAULT_ROOT_KEY_LEN];
   memset(root_key, 17, sizeof root_key);
   const char *m = "uid:7462";
   assert(vault_service_unlock(m, ATTEST_UDS_PEERCRED, root_key, sizeof root_key, 100000) ==
          VAULT_OK);
   assert(vault_service_set(m, "opaque-migration", "ordinary", "ordinary credential", 100000) ==
          VAULT_OK);
   assert(service_create(m, "opaque-migration", "protected", "synthetic service secret") ==
          VAULT_CREATE_ONCE_CREATED);
   char live[sizeof generation];
   memcpy(live, generation, sizeof live);
   assert(service_create(m, "opaque-migration", "dead", "retired secret") ==
          VAULT_CREATE_ONCE_CREATED);
   assert(!vault_store_delete(m, "opaque-migration", "dead"));
   assert(vault_service_unlock(m, ATTEST_UDS_PEERCRED, root_key, sizeof root_key, 100001) ==
          VAULT_OK);
   assert(vault_service_get_server_principal("opaque-migration", "ordinary", out, sizeof out) ==
              VAULT_OK &&
          !strcmp(out, "ordinary credential"));
   assert(vault_service_get_server_principal("opaque-migration", "protected", out, sizeof out) ==
          VAULT_NO_ENTRY);
   assert(vault_service_get_server_principal("opaque-migration", "dead", out, sizeof out) ==
          VAULT_NO_ENTRY);
   int uses = 0;
   assert(vault_service_use_server_wrap_generation(m, "opaque-migration", "protected", live,
                                                   use_secret, &uses) == VAULT_OK &&
          uses == 1);
   assert(!vault_store_delete(m, "opaque-migration", "protected"));
   assert(service_create(m, "opaque-migration", "protected", "synthetic service secret") ==
          VAULT_CREATE_ONCE_RETIRED);
   puts("PASS Retired survives password rekey/backfill; real legacy migration excludes live "
        "protected and retired entries");
}
static void retirement_process_race(void)
{
   const char *p = "uid:7471";
   assert(create(p, "entry", "value") == VAULT_CREATE_ONCE_CREATED);
   int fds[2];
   assert(!pipe(fds));
   pid_t children[6];
   for (int i = 0; i < 6; i++)
   {
      children[i] = fork();
      assert(children[i] >= 0);
      if (!children[i])
      {
         close(fds[0]);
         int result[2] = {i, i ? create(p, "entry", "value")
                               : vault_store_delete(p, "opaque-provider", "entry")};
         assert(write(fds[1], result, sizeof result) == sizeof result);
         close(fds[1]);
         _exit(0);
      }
   }
   close(fds[1]);
   for (int i = 0; i < 6; i++)
   {
      int result[2];
      size_t n = 0;
      while (n < sizeof result)
      {
         ssize_t got = read(fds[0], (char *)result + n, sizeof result - n);
         assert(got > 0);
         n += (size_t)got;
      }
      assert(result[0]
                 ? result[1] == VAULT_CREATE_ONCE_REPLAY || result[1] == VAULT_CREATE_ONCE_RETIRED
                 : result[1] == 0);
   }
   close(fds[0]);
   for (int i = 0; i < 6; i++)
   {
      int status;
      assert(waitpid(children[i], &status, 0) == children[i] && WIFEXITED(status) &&
             !WEXITSTATUS(status));
   }
   char *before = blob(p);
   assert(create(p, "entry", "value") == VAULT_CREATE_ONCE_RETIRED);
   char *after = blob(p);
   assert(!strcmp(before, after));
   free(before);
   free(after);
   puts("PASS six-process retired-delete vs retry: no Created resurrection, same-ID remains "
        "Retired");
}
static void failures(void)
{
   const char *p = "uid:7301";
   assert(create(p, "entry", "initial") == VAULT_CREATE_ONCE_CREATED);
   /* The real rename commits before this fixture's exact fsync failure. */
   fail_directory_sync = 1;
   assert(create(p, "ambiguous-commit", "committed value") == VAULT_CREATE_ONCE_IO &&
          !fail_directory_sync && !generation[0]);
   char *committed = blob(p);
   cJSON *parsed = cJSON_Parse(committed);
   assert(parsed);
   cJSON *entry = NULL;
   char committed_pin[VAULT_IMMUTABLE_GENERATION_CAP] = {0};
   cJSON_ArrayForEach(
       entry,
       cJSON_GetObjectItemCaseSensitive(
           parsed,
           "creds")) if (!strcmp(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(entry,
                                                                                       "cred")),
                                 "ambiguous-commit"))
       snprintf(committed_pin, sizeof committed_pin, "%s",
                cJSON_GetStringValue(
                    cJSON_GetObjectItemCaseSensitive(entry, "immutable_generation_v1")));
   assert(committed_pin[0]);
   unsigned committed_syncs = sync_calls;
   assert(create(p, "ambiguous-commit", "different value") == VAULT_CREATE_ONCE_CONFLICT &&
          !generation[0] && sync_calls == committed_syncs);
   fail_directory_sync = 1;
   assert(create(p, "ambiguous-commit", "committed value") == VAULT_CREATE_ONCE_IO &&
          !fail_directory_sync && !generation[0] && sync_calls == committed_syncs + 2);
   assert(create(p, "ambiguous-commit", "committed value") == VAULT_CREATE_ONCE_REPLAY &&
          !strcmp(committed_pin, generation) && sync_calls == committed_syncs + 4);
   char *replayed = blob(p);
   assert(!strcmp(committed, replayed));
   free(committed);
   free(replayed);
   cJSON_Delete(parsed);
   assert(create(p, "ambiguous-retire", "retire value") == VAULT_CREATE_ONCE_CREATED);
   char retire_pin[sizeof generation];
   memcpy(retire_pin, generation, sizeof retire_pin);
   fail_directory_sync = 1;
   assert(vault_store_delete(p, "opaque-provider", "ambiguous-retire") != 0 &&
          !fail_directory_sync);
   char out[64];
   assert(vault_store_get_server_generation(p, key, "opaque-provider", "ambiguous-retire",
                                            retire_pin, out, sizeof out) == VAULT_STORE_RETIRED &&
          !out[0]);
   unsigned retired_syncs = sync_calls;
   assert(create(p, "ambiguous-retire", "retire value") == VAULT_CREATE_ONCE_RETIRED &&
          !generation[0]);
   assert(sync_calls == retired_syncs);
   /* An earlier rename is visible despite an unknown durability outcome.
    * The same delete must not acknowledge until both sync barriers succeed. */
   fail_directory_sync = 1;
   assert(vault_store_delete(p, "opaque-provider", "ambiguous-retire") != 0 &&
          !fail_directory_sync && sync_calls == retired_syncs + 2);
   assert(!vault_store_delete(p, "opaque-provider", "ambiguous-retire") &&
          sync_calls == retired_syncs + 4);
   char *before = blob(p);
   replace(p, "malformed vault json");
   assert(create(p, "new", "value") == VAULT_CREATE_ONCE_IO);
   char *after = blob(p);
   assert(!strcmp(after, "malformed vault json"));
   free(after);
   replace(p, before);
   /* Force the actual temporary-file write to fail, preserving the old file. */
   struct rlimit limit;
   assert(!getrlimit(RLIMIT_FSIZE, &limit));
   struct rlimit zero = limit;
   zero.rlim_cur = 0;
   void (*old)(int) = signal(SIGXFSZ, SIG_IGN);
   assert(old != SIG_ERR);
   assert(!setrlimit(RLIMIT_FSIZE, &zero));
   assert(create(p, "write-failure", "value") == VAULT_CREATE_ONCE_IO);
   assert(vault_store_delete(p, "opaque-provider", "entry") != 0);
   assert(!setrlimit(RLIMIT_FSIZE, &limit));
   assert(signal(SIGXFSZ, old) != SIG_ERR);
   after = blob(p);
   assert(!strcmp(before, after));
   free(before);
   free(after);
   char lock[1024], saved[1024];
   snprintf(lock, sizeof lock, "%s/.vault/.write.lock", home);
   snprintf(saved, sizeof saved, "%s/.vault/saved-lock", home);
   assert(!rename(lock, saved));
   assert(!symlink("/dev/null", lock));
   assert(create(p, "lock-failure", "value") == VAULT_CREATE_ONCE_IO);
   assert(!unlink(lock));
   assert(!rename(saved, lock));
   puts("PASS malformed-file, actual write-failure and nofollow-lock failure refuse without "
        "replacing values");
}
static void batch_retirement_retry_durability(void)
{
   const char *p = "uid:7397";
   assert(create(p, "batch-retired", "batch value") == VAULT_CREATE_ONCE_CREATED);
   char pin[sizeof generation];
   memcpy(pin, generation, sizeof pin);
   vault_store_write_t deletes[] = {{"batch-retired", NULL}, {"absent", NULL}};
   fail_directory_sync = 1;
   assert(vault_store_write_batch(p, key, "opaque-provider", deletes, 2) != 0 &&
          !fail_directory_sync);
   unsigned before = sync_calls;
   fail_directory_sync = 1;
   assert(vault_store_write_batch(p, key, "opaque-provider", deletes, 2) != 0 &&
          !fail_directory_sync && sync_calls == before + 2);
   assert(!vault_store_write_batch(p, key, "opaque-provider", deletes, 2) &&
          sync_calls == before + 4);
   char out[64];
   assert(vault_store_get_server_generation(p, key, "opaque-provider", "batch-retired",
                                          pin, out, sizeof out) == VAULT_STORE_RETIRED);
   vault_store_write_t mixed[] = {{"batch-retired", NULL}, {"mutable", "safe batch value"}};
   before = sync_calls;
   assert(!vault_store_write_batch(p, key, "opaque-provider", mixed, 2) &&
          sync_calls == before + 2);
   vault_store_write_t absent[] = {{"absent", NULL}};
   before = sync_calls;
   assert(!vault_store_write_batch(p, key, "opaque-provider", absent, 1) &&
          sync_calls == before);
}

int main(void)
{
   assert(aimee_vault_immutable_custody_contract_v2 == 2);
   const char *base = getenv("TMPDIR");
   if (!base || !base[0])
      base = "/tmp";
   assert(snprintf(home, sizeof home, "%s/vault-create-once-XXXXXX", base) < (int)sizeof home);
   assert(mkdtemp(home));
   setvbuf(stdout, NULL, _IOLBF, 0);
   assert(!setenv("AIMEE_HOME", home, 1));
   assert(!vault_server_kek(key)); /* Provision before forks: not a key-create race test. */
   actual_master_rotation();
   file_contract();
   contenders(1);
   contenders(0);
   service_and_seam();
   immutable_lifecycle();
   mixed_password_rekey();
   retirement_rotation_and_migration();
   retirement_process_race();
   failures();
   batch_retirement_retry_durability();
   puts("vault_create_once: all tests passed");
   return 0;
}
