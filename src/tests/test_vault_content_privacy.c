#define _GNU_SOURCE
#include "vault_content_privacy.h"
#include "vault_store.h"
#include "vault_internal.h"
#include "vault_server_key.h"
#include "vault_service.h"
#include "request_context.h"
#include "cJSON.h"
#include <assert.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static char home[256];
static cJSON *inventory(const char *principal)
{
   char directory[512];
   snprintf(directory, sizeof(directory), "%s/.vault", home);
   DIR *d = opendir(directory);
   assert(d);
   struct dirent *entry;
   cJSON *found = NULL;
   while ((entry = readdir(d)))
   {
      size_t n = strlen(entry->d_name);
      if (n < 6 || strcmp(entry->d_name + n - 5, ".json"))
         continue;
      char path[1024];
      snprintf(path, sizeof(path), "%s/%s", directory, entry->d_name);
      FILE *file = fopen(path, "rb");
      assert(file);
      char bytes[262144];
      size_t used = fread(bytes, 1, sizeof(bytes) - 1, file);
      assert(feof(file));
      fclose(file);
      bytes[used] = 0;
      cJSON *root = cJSON_Parse(bytes);
      const char *owner = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(root, "principal"));
      if (owner && !strcmp(owner, principal))
      {
         found = root;
         break;
      }
      cJSON_Delete(root);
   }
   closedir(d);
   assert(found);
   return found;
}
static int check_plaintext(const char *secret, size_t length, void *expected)
{
   assert(strlen(expected) == length && !memcmp(secret, expected, length));
   return 0;
}
static void check_custody(const char *value, const char *safe)
{
   assert(!strstr(safe, value));
   const char *start = strstr(safe, "[[vault:v1:");
   assert(start && strlen(start) >= sizeof("[[vault:v1:") - 1 + 36 + 2);
   char id[37];
   memcpy(id, start + sizeof("[[vault:v1:") - 1, 36);
   id[36] = 0;
   cJSON *root = inventory("uid:1000"), *entry;
   int found = 0;
   cJSON_ArrayForEach(entry, cJSON_GetObjectItemCaseSensitive(root, "creds"))
   {
      const char *cred = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(entry, "cred"));
      if (!cred || strcmp(cred, id))
         continue;
      const char *generation =
          cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(entry, "immutable_generation_v1"));
      assert(generation);
      assert(vault_service_use_server_wrap_generation("uid:1000", "private-content", id, generation,
                                                      check_plaintext, (void *)value) == VAULT_OK);
      char *bytes = cJSON_PrintUnformatted(root);
      assert(bytes && !strstr(bytes, value));
      free(bytes);
      found = 1;
   }
   assert(found);
   cJSON_Delete(root);
}
/* The KB tenant backend has no server-wrap support. Host custody must remain
 * local and enumerable for master rotation while this backend is selected. */
static int tenant_list(void *ctx, const char *principal, vault_store_entry_t *out, int max)
{
   (void)ctx;
   (void)principal;
   (void)out;
   (void)max;
   return 0;
}
static int tenant_get(void *ctx, const char *principal, const uint8_t *kek, const char *agent,
                      const char *cred, char *out, size_t cap)
{
   (void)ctx;
   (void)principal;
   (void)kek;
   (void)agent;
   (void)cred;
   (void)out;
   (void)cap;
   return VAULT_STORE_NO_ENTRY;
}
static int tenant_delete(void *ctx, const char *principal, const char *agent, const char *cred)
{
   (void)ctx;
   (void)principal;
   (void)agent;
   (void)cred;
   return -1;
}
static int tenant_principals(void *ctx, char (*out)[VAULT_PRINCIPAL_MAX], int max)
{
   (void)ctx;
   (void)out;
   (void)max;
   return 0;
}
int main(void)
{
   const char *base = getenv("AIMEE_PRIVACY_TEST_ROOT");
   if (!base || !base[0])
      base = getenv("TMPDIR");
   snprintf(home, sizeof(home), "%s/privacy-unit-XXXXXX", base && base[0] ? base : ".");
   assert(mkdtemp(home));
   assert(!setenv("AIMEE_HOME", home, 1));
   request_context_t ctx = {.vault_transport = ATTEST_UDS_PEERCRED};
   strcpy(ctx.vault_principal, "uid:1000");
   request_context_set(&ctx);
   char *safe = NULL;
   assert(vault_content_protect_text("bad \xff email@example.invalid", &safe) == -1 && !safe);
   assert(!vault_content_protect_text("Ordinary retained control 界 🦊", &safe) && !safe);
   assert(!vault_content_protect_text("std::string and 2030-01-02", &safe) && !safe);
   assert(!vault_content_protect_text("call+12025550198ext42", &safe));
   assert(safe);
   check_custody("+12025550198", safe);
   free(safe);
   safe = NULL;
   assert(!vault_content_protect_text(
              "response_abcd123456789abcd source_1234567890 sha256:abcd9876543210abcd", &safe) &&
          !safe);
   assert(!vault_content_protect_text(
              "<memory_data>{\"timestamps\":{\"min\":1794672000,\"max\":1794758400},"
              "\"version\":9007199254740993}</memory_data>",
              &safe) &&
          !safe);
   assert(
       !vault_content_protect_text("{\"phone\":\"+1 202 555 0198\",\"time\":1794672000}", &safe));
   assert(safe && strstr(safe, "1794672000") && !strstr(safe, "+1 202 555 0198"));
   free(safe);
   safe = NULL;
   assert(
       !vault_content_protect_text("abcdef01-2345-6789-abcd-123456789012@example.invalid", &safe));
   assert(safe && !strstr(safe, "@example.invalid"));
   free(safe);
   safe = NULL;
   assert(!vault_content_protect_text(
              "{\"id\":\"1234567890123456789\",\"uuid\":\"abcdef01-2345-6789-abcd-123456789012\"}",
              &safe) &&
          !safe);
   const char *values[] = {
       "contact+fixture@example.invalid",
       "josé+fixture@例子.invalid",
       "+31 20 555 01 47",
       "+1 (202) 555-0147",
       "203.0.113.147",
       "10.83.47.219",
       "172.19.47.219",
       "2001:db8:abcd:1234::147",
       "847-39-2614",
       "date_of_birth=1988-03-17",
       "password=synthetic-private-value",
       "ghp_abcdefghij12345678901234567890",
       "AKIAABCDEFGHIJKLMNOP",
       "-----BEGIN RSA PRIVATE KEY-----\nsynthetic-fixture-only\n-----END RSA PRIVATE KEY-----",
       "4111 1111 1111 1111",
       "147 Canary Orchard Lane, Testville",
       "Patricia Rivera",
       "passport_number=QA17947"};
   for (size_t i = 0; i < sizeof(values) / sizeof(values[0]); ++i)
   {
      assert(!vault_content_protect_text(values[i], &safe) && safe);
      check_custody(values[i], safe);
      char *again = NULL;
      assert(!vault_content_protect_text(values[i], &again) && again && !strcmp(safe, again));
      free(again);
      assert(!vault_content_protect_text(safe, &again) && !again);
      free(safe);
      safe = NULL;
   }
   assert(!vault_content_protect_json(
              "{\"operation\":\"memory-query\",\"query\":{\"kind\":\"search_graph\","
              "\"arguments\":{\"query\":\"Patricia Rivera\",\"store\":\"user\"}},"
              "\"expected_owner\":\"abcdef01-2345-6789-abcd-123456789012\"}",
              &safe) &&
          safe);
   assert(!strstr(safe, "Patricia Rivera") && strstr(safe, "abcdef01-2345-6789-abcd-123456789012"));
   free(safe);
   safe = NULL;
   const char *json = "{\"content\":\"contact+fixture@example.invalid\",\"old_id\":"
                      "9007199254740993,\"expected_version\":{\"sequence\":18446744073709551615}}";
   assert(!vault_content_protect_json(json, &safe) && safe);
   assert(strstr(safe, "9007199254740993") && strstr(safe, "18446744073709551615"));
   assert(!strstr(safe, values[0]));
   free(safe);
   safe = NULL;
   assert(!vault_content_protect_json(
              "{\"key\":\"password\",\"content\":\"unguessable-fixture-value\"}", &safe) &&
          safe);
   assert(!strstr(safe, "unguessable-fixture-value"));
   free(safe);
   safe = NULL;
   assert(vault_service_set_server_wrap("uid:1000", "retention-probe", "slot",
                                        "retained version one") == VAULT_OK);
   cJSON *first_inventory = inventory("uid:1000"), *entry;
   char *first_cipher = NULL;
   cJSON_ArrayForEach(entry, cJSON_GetObjectItemCaseSensitive(first_inventory, "creds"))
   {
      const char *agent = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(entry, "agent"));
      if (agent && !strcmp(agent, "retention-probe"))
         first_cipher =
             strdup(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(entry, "ciphertext")));
   }
   assert(first_cipher);
   cJSON_Delete(first_inventory);
   assert(vault_service_set_server_wrap("uid:1000", "retention-probe", "slot",
                                        "retained version two") == VAULT_OK);
   assert(!vault_store_delete("uid:1000", "retention-probe", "slot"));
   cJSON *retained = inventory("uid:1000");
   int versions = 0, original = 0;
   cJSON_ArrayForEach(entry, cJSON_GetObjectItemCaseSensitive(retained, "creds"))
   {
      const char *agent = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(entry, "agent"));
      if (!agent || strcmp(agent, "retention-probe"))
         continue;
      assert(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(entry, "retained_unavailable_v1")));
      const char *cipher =
          cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(entry, "ciphertext"));
      assert(cipher);
      original += !strcmp(cipher, first_cipher);
      ++versions;
   }
   assert(versions == 2 && original == 1);
   cJSON_Delete(retained);
   assert(!vault_content_protect_json(
              "{\"key\":\"private-ssn\",\"content\":\"sensitive value with spaces\"}", &safe) &&
          safe);
   assert(!strstr(safe, "sensitive value with spaces") && !strstr(safe, "private-ssn"));
   free(safe);
   safe = NULL;
   char *before = NULL;
   assert(!vault_content_protect_text(values[0], &before) && before);
   int principals = 0, credentials = 0;
   char backup[1024], error[512];
   assert(!vault_server_key_rotate("server", &principals, &credentials, backup, sizeof(backup),
                                   error, sizeof(error)));
   assert(!vault_content_protect_text(values[0], &safe) && safe && !strcmp(before, safe));
   check_custody(values[0], safe);
   free(before);
   free(safe);
   safe = NULL;
   retained = inventory("uid:1000");
   versions = 0;
   original = 0;
   cJSON_ArrayForEach(entry, cJSON_GetObjectItemCaseSensitive(retained, "creds"))
   {
      const char *agent = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(entry, "agent"));
      if (!agent || strcmp(agent, "retention-probe"))
         continue;
      ++versions;
      original +=
          !strcmp(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(entry, "ciphertext")),
                  first_cipher);
   }
   assert(versions == 2 && original == 1);
   free(first_cipher);
   cJSON_Delete(retained);

   strcpy(ctx.vault_principal, "uid:2000");
   request_context_set(&ctx);
   assert(!vault_content_protect_text(values[0], &safe) && safe);
   cJSON *other = inventory("uid:2000");
   cJSON_Delete(other);
   free(safe);
   safe = NULL;
   /* The rotation API reserves one backup path per process. Archive this
    * fixture's first backup intact before the separate KB composition probe. */
   char first_backup[1200];
   snprintf(first_backup, sizeof(first_backup), "%s.first", backup);
   assert(!rename(backup, first_backup));
   uint8_t tenant_kek[VAULT_KEK_LEN];
   assert(!vault_server_kek(tenant_kek));
   vault_store_backend_t tenant = {.name = "tenant-without-server-wraps",
                                   .get = tenant_get,
                                   .list = tenant_list,
                                   .delete = tenant_delete,
                                   .list_principals = tenant_principals};
   assert(!vault_store_bind_tenant_backend(&tenant, tenant_kek));
   strcpy(ctx.vault_principal, "kb-content:rotation-fixture");
   request_context_set(&ctx);
   char *kb_reference = NULL;
   assert(!vault_content_protect_text(values[0], &kb_reference) && kb_reference);
   char local_principals[64][VAULT_PRINCIPAL_MAX];
   int local_count = vault_store_list_principals(local_principals, 64), found_index = 0,
       found_kb = 0;
   assert(local_count > 0);
   for (int i = 0; i < local_count; ++i)
   {
      found_index |= !strcmp(local_principals[i], "content-privacy-index");
      found_kb |= !strcmp(local_principals[i], ctx.vault_principal);
   }
   assert(found_index && found_kb);
   assert(!vault_server_key_rotate("server", &principals, &credentials, backup, sizeof(backup),
                                   error, sizeof(error)));
   assert(!vault_content_protect_text(values[0], &safe) && safe && !strcmp(safe, kb_reference));
   cJSON *kb_inventory = inventory(ctx.vault_principal);
   cJSON_ArrayForEach(entry, cJSON_GetObjectItemCaseSensitive(kb_inventory, "creds"))
   {
      const char *cred = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(entry, "cred"));
      const char *generation =
          cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(entry, "immutable_generation_v1"));
      assert(vault_service_use_server_wrap_generation(ctx.vault_principal, "private-content", cred,
                                                      generation, check_plaintext,
                                                      (void *)values[0]) == VAULT_OK);
   }
   cJSON_Delete(kb_inventory);
   free(safe);
   safe = NULL;
   free(kb_reference);
   vault_store_set_backend(NULL);
   request_context_clear();
   assert(vault_content_protect_text(values[0], &safe) == -1 && !safe);
   printf("PASS local PII custody, stable opaque locators, real authenticated decryption, "
          "rotation, exact JSON integers and unattested refusal\n");
   char command[512];
   snprintf(command, sizeof(command), "rm -rf -- '%s'", home);
   assert(system(command) == 0);
   return 0;
}
