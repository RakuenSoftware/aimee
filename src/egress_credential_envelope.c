#include "egress_credential_envelope.h"
#include "module_json_call.h"

#include <aimee/egress/module_api.h>

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define CREDENTIAL_VERSION       1
#define CREDENTIAL_KEY_ID_LEN    32u
#define CREDENTIAL_KEY_LEN       32u
#define CREDENTIAL_NONCE_LEN     12u
#define CREDENTIAL_TAG_LEN       16u
#define CREDENTIAL_TOKEN_MAX     4096u
#define CREDENTIAL_LIFETIME_SECS 30

static const unsigned char kdf_domain[] = "aimee.egress.x25519.v1";
static const unsigned char aad_domain[] = "aimee.egress.credential.v1";

static int operation_ok(const char *operation)
{
   static const char *const allowed[] = {"default_branch", "pr_create", "pr_find_open",
                                         "pr_list_open",   "pr_info",   "pr_edit",
                                         "pr_merge"};
   if (!operation)
      return 0;
   for (size_t i = 0; i < sizeof(allowed) / sizeof(allowed[0]); i++)
      if (strcmp(operation, allowed[i]) == 0)
         return 1;
   return 0;
}

static int name_ok(const char *name, size_t len)
{
   if (!name || len == 0 || len > 100)
      return 0;
   for (size_t i = 0; i < len; i++)
   {
      unsigned char c = (unsigned char)name[i];
      if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.'))
         return 0;
   }
   return 1;
}

static int resource_ok(const char *resource)
{
   if (!resource)
      return 0;
   const char *slash = strchr(resource, '/');
   return slash && slash != resource && slash[1] && !strchr(slash + 1, '/') &&
          name_ok(resource, (size_t)(slash - resource)) && name_ok(slash + 1, strlen(slash + 1));
}

static int hex_id_ok(const char *id)
{
   if (!id || strlen(id) != CREDENTIAL_KEY_ID_LEN)
      return 0;
   for (size_t i = 0; i < CREDENTIAL_KEY_ID_LEN; i++)
      if (!((id[i] >= '0' && id[i] <= '9') || (id[i] >= 'a' && id[i] <= 'f')))
         return 0;
   return 1;
}

static int b64_decode_32(const char *encoded, unsigned char out[CREDENTIAL_KEY_LEN])
{
   if (!encoded || strlen(encoded) != 44 || encoded[43] != '=')
      return -1;
   unsigned char decoded[36];
   int n = EVP_DecodeBlock(decoded, (const unsigned char *)encoded, 44);
   if (n != 33)
   {
      OPENSSL_cleanse(decoded, sizeof(decoded));
      return -1;
   }
   memcpy(out, decoded, CREDENTIAL_KEY_LEN);
   OPENSSL_cleanse(decoded, sizeof(decoded));
   return 0;
}

static char *b64_encode(const unsigned char *input, size_t len)
{
   if (!input || len > (size_t)INT_MAX)
      return NULL;
   size_t cap = 4 * ((len + 2) / 3) + 1;
   char *out = malloc(cap);
   if (!out)
      return NULL;
   int n = EVP_EncodeBlock((unsigned char *)out, input, (int)len);
   if (n < 0 || (size_t)n + 1 != cap)
   {
      free(out);
      return NULL;
   }
   return out;
}

static void put_u32be(unsigned char *out, uint32_t value)
{
   out[0] = (unsigned char)(value >> 24);
   out[1] = (unsigned char)(value >> 16);
   out[2] = (unsigned char)(value >> 8);
   out[3] = (unsigned char)value;
}

static void put_u64be(unsigned char *out, uint64_t value)
{
   for (unsigned i = 0; i < 8; i++)
      out[i] = (unsigned char)(value >> (56 - 8 * i));
}

static int aad_build(const char *operation, const char *resource, int64_t expires_at,
                     unsigned char *out, size_t cap, size_t *out_len)
{
   static const char handle[] = "forge";
   static const char host[] = "api.github.com";
   const char *fields[] = {handle, host, operation, resource};
   size_t need = sizeof(aad_domain);
   for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); i++)
      need += strlen(fields[i]) + 1;
   need += 12;
   if (!out || !out_len || need > cap)
      return -1;
   size_t offset = 0;
   memcpy(out + offset, aad_domain, sizeof(aad_domain));
   offset += sizeof(aad_domain);
   for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); i++)
   {
      size_t n = strlen(fields[i]) + 1;
      memcpy(out + offset, fields[i], n);
      offset += n;
   }
   put_u32be(out + offset, AIMEE_EGRESS_GIT_CLIENT_REF);
   offset += 4;
   put_u64be(out + offset, (uint64_t)expires_at);
   offset += 8;
   *out_len = offset;
   return 0;
}

static int derive_key(const unsigned char receiver_public[CREDENTIAL_KEY_LEN], EVP_PKEY **sender,
                      unsigned char sender_public[CREDENTIAL_KEY_LEN],
                      unsigned char key[CREDENTIAL_KEY_LEN])
{
   EVP_PKEY_CTX *keygen = EVP_PKEY_CTX_new_id(EVP_PKEY_X25519, NULL);
   EVP_PKEY *peer = NULL;
   EVP_PKEY_CTX *derive = NULL;
   EVP_MD_CTX *digest = NULL;
   unsigned char shared[CREDENTIAL_KEY_LEN];
   size_t public_len = CREDENTIAL_KEY_LEN, shared_len = CREDENTIAL_KEY_LEN;
   unsigned int key_len = 0;
   int rc = -1;
   memset(shared, 0, sizeof(shared));
   memset(key, 0, CREDENTIAL_KEY_LEN);
   *sender = NULL;
   if (!keygen || EVP_PKEY_keygen_init(keygen) != 1 || EVP_PKEY_keygen(keygen, sender) != 1 ||
       EVP_PKEY_get_raw_public_key(*sender, sender_public, &public_len) != 1 ||
       public_len != CREDENTIAL_KEY_LEN)
      goto done;
   peer = EVP_PKEY_new_raw_public_key(EVP_PKEY_X25519, NULL, receiver_public, CREDENTIAL_KEY_LEN);
   derive = peer ? EVP_PKEY_CTX_new(*sender, NULL) : NULL;
   if (!derive || EVP_PKEY_derive_init(derive) != 1 ||
       EVP_PKEY_derive_set_peer(derive, peer) != 1 ||
       EVP_PKEY_derive(derive, shared, &shared_len) != 1 || shared_len != CREDENTIAL_KEY_LEN)
      goto done;
   digest = EVP_MD_CTX_new();
   if (!digest || EVP_DigestInit_ex(digest, EVP_sha256(), NULL) != 1 ||
       EVP_DigestUpdate(digest, kdf_domain, sizeof(kdf_domain)) != 1 ||
       EVP_DigestUpdate(digest, shared, sizeof(shared)) != 1 ||
       EVP_DigestUpdate(digest, sender_public, CREDENTIAL_KEY_LEN) != 1 ||
       EVP_DigestUpdate(digest, receiver_public, CREDENTIAL_KEY_LEN) != 1 ||
       EVP_DigestFinal_ex(digest, key, &key_len) != 1 || key_len != CREDENTIAL_KEY_LEN)
      goto done;
   rc = 0;
done:
   EVP_MD_CTX_free(digest);
   EVP_PKEY_CTX_free(derive);
   EVP_PKEY_free(peer);
   EVP_PKEY_CTX_free(keygen);
   OPENSSL_cleanse(shared, sizeof(shared));
   if (rc != 0)
   {
      EVP_PKEY_free(*sender);
      *sender = NULL;
      OPENSSL_cleanse(sender_public, CREDENTIAL_KEY_LEN);
      OPENSSL_cleanse(key, CREDENTIAL_KEY_LEN);
   }
   return rc;
}

static int encrypt_gcm(const unsigned char key[CREDENTIAL_KEY_LEN], const unsigned char *aad,
                       size_t aad_len, const unsigned char *plaintext, size_t plaintext_len,
                       unsigned char nonce[CREDENTIAL_NONCE_LEN], unsigned char *ciphertext)
{
   EVP_CIPHER_CTX *ctx = NULL;
   int out_len = 0, final_len = 0, aad_out = 0, rc = -1;
   if (RAND_bytes(nonce, CREDENTIAL_NONCE_LEN) != 1)
      return -1;
   ctx = EVP_CIPHER_CTX_new();
   if (ctx && EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), NULL, NULL, NULL) == 1 &&
       EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, CREDENTIAL_NONCE_LEN, NULL) == 1 &&
       EVP_EncryptInit_ex(ctx, NULL, NULL, key, nonce) == 1 &&
       EVP_EncryptUpdate(ctx, NULL, &aad_out, aad, (int)aad_len) == 1 &&
       EVP_EncryptUpdate(ctx, ciphertext, &out_len, plaintext, (int)plaintext_len) == 1 &&
       EVP_EncryptFinal_ex(ctx, ciphertext + out_len, &final_len) == 1 &&
       (size_t)(out_len + final_len) == plaintext_len &&
       EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, CREDENTIAL_TAG_LEN,
                           ciphertext + plaintext_len) == 1)
      rc = 0;
   EVP_CIPHER_CTX_free(ctx);
   if (rc != 0)
   {
      OPENSSL_cleanse(nonce, CREDENTIAL_NONCE_LEN);
      OPENSSL_cleanse(ciphertext, plaintext_len + CREDENTIAL_TAG_LEN);
   }
   return rc;
}

cJSON *aimee_egress_wrap_forge_credential(const char *token, const char *operation,
                                          const char *resource)
{
   size_t token_len = token ? strlen(token) : 0;
   if (token_len == 0 || token_len > CREDENTIAL_TOKEN_MAX || !operation_ok(operation) ||
       !resource_ok(resource))
      return NULL;

   cJSON *key_reply =
       aimee_module_json_call_raw(AIMEE_EGRESS_EVENT_CREDENTIAL_KEY,
                                  AIMEE_EGRESS_STAGE_CREDENTIAL_KEY, "", 0, 512, 5000, NULL);
   const cJSON *version = key_reply ? cJSON_GetObjectItemCaseSensitive(key_reply, "version") : NULL;
   const cJSON *key_id = key_reply ? cJSON_GetObjectItemCaseSensitive(key_reply, "key_id") : NULL;
   const cJSON *public_key =
       key_reply ? cJSON_GetObjectItemCaseSensitive(key_reply, "public_key") : NULL;
   unsigned char receiver_public[CREDENTIAL_KEY_LEN], sender_public[CREDENTIAL_KEY_LEN];
   unsigned char key[CREDENTIAL_KEY_LEN], nonce[CREDENTIAL_NONCE_LEN];
   unsigned char aad[512];
   unsigned char *ciphertext = NULL;
   EVP_PKEY *sender = NULL;
   char *sender_b64 = NULL, *nonce_b64 = NULL, *ciphertext_b64 = NULL;
   cJSON *envelope = NULL;
   size_t aad_len = 0;
   time_t now = time(NULL);
   int64_t expires_at = now < 0 ? 0 : (int64_t)now + CREDENTIAL_LIFETIME_SECS;
   memset(receiver_public, 0, sizeof(receiver_public));
   memset(sender_public, 0, sizeof(sender_public));
   memset(key, 0, sizeof(key));
   memset(nonce, 0, sizeof(nonce));
   memset(aad, 0, sizeof(aad));
   if (!cJSON_IsNumber(version) || version->valueint != CREDENTIAL_VERSION ||
       !cJSON_IsString(key_id) || !hex_id_ok(key_id->valuestring) || !cJSON_IsString(public_key) ||
       b64_decode_32(public_key->valuestring, receiver_public) != 0 || expires_at <= 0 ||
       aad_build(operation, resource, expires_at, aad, sizeof(aad), &aad_len) != 0 ||
       derive_key(receiver_public, &sender, sender_public, key) != 0)
      goto done;
   ciphertext = malloc(token_len + CREDENTIAL_TAG_LEN);
   if (!ciphertext || encrypt_gcm(key, aad, aad_len, (const unsigned char *)token, token_len, nonce,
                                  ciphertext) != 0)
      goto done;
   sender_b64 = b64_encode(sender_public, sizeof(sender_public));
   nonce_b64 = b64_encode(nonce, sizeof(nonce));
   ciphertext_b64 = b64_encode(ciphertext, token_len + CREDENTIAL_TAG_LEN);
   envelope = cJSON_CreateObject();
   if (!sender_b64 || !nonce_b64 || !ciphertext_b64 || !envelope ||
       !cJSON_AddNumberToObject(envelope, "version", CREDENTIAL_VERSION) ||
       !cJSON_AddStringToObject(envelope, "key_id", key_id->valuestring) ||
       !cJSON_AddStringToObject(envelope, "ephemeral_public_key", sender_b64) ||
       !cJSON_AddStringToObject(envelope, "nonce", nonce_b64) ||
       !cJSON_AddStringToObject(envelope, "ciphertext", ciphertext_b64) ||
       !cJSON_AddNumberToObject(envelope, "expires_at", (double)expires_at) ||
       !cJSON_AddStringToObject(envelope, "handle", "forge") ||
       !cJSON_AddStringToObject(envelope, "host", "api.github.com") ||
       !cJSON_AddStringToObject(envelope, "operation", operation) ||
       !cJSON_AddStringToObject(envelope, "resource", resource) ||
       !cJSON_AddNumberToObject(envelope, "principal_ref", AIMEE_EGRESS_GIT_CLIENT_REF))
   {
      cJSON_Delete(envelope);
      envelope = NULL;
   }
done:
   cJSON_Delete(key_reply);
   EVP_PKEY_free(sender);
   OPENSSL_cleanse(receiver_public, sizeof(receiver_public));
   OPENSSL_cleanse(sender_public, sizeof(sender_public));
   OPENSSL_cleanse(key, sizeof(key));
   OPENSSL_cleanse(nonce, sizeof(nonce));
   OPENSSL_cleanse(aad, sizeof(aad));
   if (ciphertext)
   {
      OPENSSL_cleanse(ciphertext, token_len + CREDENTIAL_TAG_LEN);
      free(ciphertext);
   }
   free(sender_b64);
   free(nonce_b64);
   free(ciphertext_b64);
   return envelope;
}
