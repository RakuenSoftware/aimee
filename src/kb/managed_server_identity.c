#include "kb_scope.h" /* KB_SERVER_CLIENT_SCOPE */
#include "managed_server_identity.h"

#include "cJSON.h"
#include "platform_random.h"

#include <fcntl.h>
#include <math.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define MANAGED_IDENTITY_MAX (64 * 1024)

static int lowercase_hex(const char *value, size_t n)
{
   if (!value || strlen(value) != n)
      return 0;
   for (size_t i = 0; i < n; i++)
      if (!((value[i] >= '0' && value[i] <= '9') || (value[i] >= 'a' && value[i] <= 'f')))
         return 0;
   return 1;
}

static int token(const char *value, size_t cap)
{
   size_t n = value ? strnlen(value, cap + 1) : 0;
   if (!n || n > cap)
      return 0;
   for (size_t i = 0; i < n; i++)
      if (!((value[i] >= 'A' && value[i] <= 'Z') || (value[i] >= 'a' && value[i] <= 'z') ||
            (value[i] >= '0' && value[i] <= '9') || strchr("._-", value[i])))
         return 0;
   return 1;
}

static int digest_hex(const char *value, char out[65])
{
   unsigned char digest[32];
   unsigned int n = 0;
   if (!value || EVP_Digest(value, strlen(value), digest, &n, EVP_sha256(), NULL) != 1 || n != 32)
      return -1;
   for (size_t i = 0; i < sizeof(digest); i++)
      snprintf(out + i * 2, 3, "%02x", digest[i]);
   out[64] = '\0';
   OPENSSL_cleanse(digest, sizeof(digest));
   return 0;
}

static int cert_key_match(const char *cert_pem, const char *key_pem)
{
   BIO *cert_bio = BIO_new_mem_buf(cert_pem, -1);
   BIO *key_bio = BIO_new_mem_buf(key_pem, -1);
   X509 *cert = cert_bio ? PEM_read_bio_X509(cert_bio, NULL, NULL, NULL) : NULL;
   EVP_PKEY *key = key_bio ? PEM_read_bio_PrivateKey(key_bio, NULL, NULL, NULL) : NULL;
   EVP_PKEY *public_key = cert ? X509_get_pubkey(cert) : NULL;
   int match = public_key && key && EVP_PKEY_eq(public_key, key) == 1;
   EVP_PKEY_free(public_key);
   EVP_PKEY_free(key);
   X509_free(cert);
   BIO_free(key_bio);
   BIO_free(cert_bio);
   return match;
}

static int cert_public_keys_distinct(const char *a_pem, const char *b_pem)
{
   BIO *a_bio = a_pem ? BIO_new_mem_buf(a_pem, -1) : NULL;
   BIO *b_bio = b_pem ? BIO_new_mem_buf(b_pem, -1) : NULL;
   X509 *a = a_bio ? PEM_read_bio_X509(a_bio, NULL, NULL, NULL) : NULL;
   X509 *b = b_bio ? PEM_read_bio_X509(b_bio, NULL, NULL, NULL) : NULL;
   EVP_PKEY *a_key = a ? X509_get_pubkey(a) : NULL;
   EVP_PKEY *b_key = b ? X509_get_pubkey(b) : NULL;
   int distinct = a_key && b_key && EVP_PKEY_eq(a_key, b_key) == 0;
   EVP_PKEY_free(a_key);
   EVP_PKEY_free(b_key);
   X509_free(a);
   X509_free(b);
   BIO_free(a_bio);
   BIO_free(b_bio);
   return distinct;
}

int kb_managed_server_identity_validate(const kb_managed_server_identity_t *identity)
{
   if (!identity || identity->version != 2 ||
       (strcmp(identity->state, "pending") && strcmp(identity->state, "issued") &&
        strcmp(identity->state, "ready")) ||
       !token(identity->host, 255) || identity->port < 1 || identity->port > 65535 ||
       !identity->endpoint[0] || strlen(identity->endpoint) >= sizeof(identity->endpoint) ||
       !token(identity->server_id, 127) || identity->team_id < 1 ||
       !lowercase_hex(identity->operation, 32) || !lowercase_hex(identity->client_csr_digest, 64) ||
       !lowercase_hex(identity->management_csr_digest, 64) || !identity->ca[0] ||
       kb_pki_csr_validate(identity->client_csr) != 0 ||
       kb_pki_csr_validate(identity->management_csr) != 0 || !identity->client_key[0] ||
       !identity->management_key[0])
      return 0;
   char client_digest[65], management_digest[65];
   if (digest_hex(identity->client_csr, client_digest) ||
       digest_hex(identity->management_csr, management_digest) ||
       strcmp(client_digest, identity->client_csr_digest) ||
       strcmp(management_digest, identity->management_csr_digest) ||
       !strcmp(identity->client_csr_digest, identity->management_csr_digest))
      return 0;
   if (!strcmp(identity->state, "pending"))
      return !identity->client_cert[0] && !identity->management_cert[0];
   return identity->client_cert[0] && identity->management_cert[0] &&
          kb_pki_verify_client_cert(identity->ca, identity->client_cert) == 1 &&
          kb_pki_verify_client_cert(identity->ca, identity->management_cert) == 1 &&
          cert_key_match(identity->client_cert, identity->client_key) &&
          cert_key_match(identity->management_cert, identity->management_key) &&
          cert_public_keys_distinct(identity->client_cert, identity->management_cert);
}

int kb_managed_server_identity_generate(const kb_pki_ca_t *ca, const char *host, int port,
                                        const char *endpoint, int64_t team_id,
                                        kb_managed_server_identity_t *out)
{
   if (!ca || !ca->cert_pem[0] || !host || !endpoint || !out || team_id < 1)
      return -1;
   memset(out, 0, sizeof(*out));
   char suffix[33];
   if (platform_random_hex(suffix, 32) || platform_random_hex(out->operation, 32) ||
       kb_pki_generate_csr(KB_SERVER_CLIENT_SCOPE, out->client_csr, sizeof(out->client_csr),
                           out->client_key, sizeof(out->client_key)) ||
       kb_pki_generate_csr("p5-server-management", out->management_csr, sizeof(out->management_csr),
                           out->management_key, sizeof(out->management_key)) ||
       digest_hex(out->client_csr, out->client_csr_digest) ||
       digest_hex(out->management_csr, out->management_csr_digest))
      goto failed;
   out->version = 2;
   snprintf(out->state, sizeof(out->state), "pending");
   snprintf(out->host, sizeof(out->host), "%s", host);
   out->port = port;
   snprintf(out->endpoint, sizeof(out->endpoint), "%s", endpoint);
   snprintf(out->server_id, sizeof(out->server_id), "managed-%s", suffix);
   out->team_id = team_id;
   snprintf(out->ca, sizeof(out->ca), "%s", ca->cert_pem);
   if (kb_managed_server_identity_validate(out))
      return 0;
failed:
   kb_managed_server_identity_clear(out);
   return -1;
}

int kb_managed_server_identity_issue(const kb_pki_ca_t *ca, kb_managed_server_identity_t *identity)
{
   if (!ca || !identity || strcmp(identity->state, "pending") ||
       !kb_managed_server_identity_validate(identity))
      return -1;
   if (kb_pki_sign_server_role_csrs(ca, identity->client_csr, KB_SERVER_CLIENT_SCOPE,
                                    identity->management_csr, "p5-server-management",
                                    60L * 60 * 24 * 90, identity->client_cert,
                                    sizeof(identity->client_cert), identity->management_cert,
                                    sizeof(identity->management_cert)) != 0)
      return -1;
   snprintf(identity->state, sizeof(identity->state), "issued");
   if (kb_managed_server_identity_validate(identity))
      return 0;
   identity->state[0] = '\0';
   OPENSSL_cleanse(identity->client_cert, sizeof(identity->client_cert));
   OPENSSL_cleanse(identity->management_cert, sizeof(identity->management_cert));
   return -1;
}

static int add_string(cJSON *root, const char *name, const char *value)
{
   return cJSON_AddStringToObject(root, name, value) != NULL;
}

int kb_managed_server_identity_save(const char *path, uid_t owner,
                                    const kb_managed_server_identity_t *identity)
{
   if (!path || path[0] != '/' || !kb_managed_server_identity_validate(identity))
      return -1;
   cJSON *root = cJSON_CreateObject();
   if (!root || !cJSON_AddNumberToObject(root, "version", 2) ||
       !add_string(root, "state", identity->state) || !add_string(root, "host", identity->host) ||
       !cJSON_AddNumberToObject(root, "port", identity->port) ||
       !add_string(root, "endpoint", identity->endpoint) ||
       !add_string(root, "server_id", identity->server_id) ||
       !cJSON_AddNumberToObject(root, "team_id", (double)identity->team_id) ||
       !add_string(root, "operation", identity->operation) ||
       !add_string(root, "client_csr_digest", identity->client_csr_digest) ||
       !add_string(root, "management_csr_digest", identity->management_csr_digest) ||
       !add_string(root, "ca", identity->ca) ||
       !add_string(root, "client_csr", identity->client_csr) ||
       !add_string(root, "key", identity->client_key) ||
       !add_string(root, "cert", identity->client_cert) ||
       !add_string(root, "management_csr", identity->management_csr) ||
       !add_string(root, "management_key", identity->management_key) ||
       !add_string(root, "management_cert", identity->management_cert))
   {
      cJSON_Delete(root);
      return -1;
   }
   char *raw = cJSON_PrintUnformatted(root);
   cJSON_Delete(root);
   if (!raw || strlen(raw) >= MANAGED_IDENTITY_MAX)
   {
      free(raw);
      return -1;
   }
   char temporary[4096];
   int n = snprintf(temporary, sizeof(temporary), "%s.tmp.%ld", path, (long)getpid());
   if (n <= 0 || (size_t)n >= sizeof(temporary))
   {
      OPENSSL_cleanse(raw, strlen(raw));
      free(raw);
      return -1;
   }
   unlink(temporary);
   int fd = open(temporary, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
   size_t length = strlen(raw), used = 0;
   while (fd >= 0 && used < length)
   {
      ssize_t wrote = write(fd, raw + used, length - used);
      if (wrote <= 0)
         break;
      used += (size_t)wrote;
   }
   int ok = fd >= 0 && used == length && fchmod(fd, 0600) == 0 && fchown(fd, owner, owner) == 0 &&
            fsync(fd) == 0;
   if (fd >= 0 && close(fd) != 0)
      ok = 0;
   OPENSSL_cleanse(raw, length);
   free(raw);
   if (!ok || rename(temporary, path) != 0)
   {
      unlink(temporary);
      return -1;
   }
   return 0;
}

static int copy_json_string(const cJSON *root, const char *name, char *out, size_t cap)
{
   const cJSON *value = cJSON_GetObjectItemCaseSensitive(root, name);
   if (!cJSON_IsString(value) || !value->valuestring || strlen(value->valuestring) >= cap)
      return -1;
   memcpy(out, value->valuestring, strlen(value->valuestring) + 1);
   return 0;
}

int kb_managed_server_identity_load(const char *path, uid_t expected_owner,
                                    kb_managed_server_identity_t *out)
{
   if (!path || path[0] != '/' || !out)
      return -1;
   memset(out, 0, sizeof(*out));
   int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
   struct stat st;
   if (fd < 0 || fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_uid != expected_owner ||
       st.st_nlink != 1 || (st.st_mode & 077) || st.st_size < 1 ||
       st.st_size >= MANAGED_IDENTITY_MAX)
   {
      if (fd >= 0)
         close(fd);
      return -1;
   }
   char *raw = calloc(1, (size_t)st.st_size + 1);
   size_t used = 0;
   while (raw && used < (size_t)st.st_size)
   {
      ssize_t got = read(fd, raw + used, (size_t)st.st_size - used);
      if (got <= 0)
         break;
      used += (size_t)got;
   }
   char extra;
   ssize_t trailing = raw ? read(fd, &extra, 1) : -1;
   close(fd);
   if (!raw || used != (size_t)st.st_size || trailing != 0 || memchr(raw, '\0', used))
      goto failed;
   const char *end = NULL;
   cJSON *root = cJSON_ParseWithLengthOpts(raw, used + 1, &end, 1);
   if (!root || end != raw + used)
   {
      cJSON_Delete(root);
      goto failed;
   }
   const cJSON *version = cJSON_GetObjectItemCaseSensitive(root, "version");
   const cJSON *port = cJSON_GetObjectItemCaseSensitive(root, "port");
   const cJSON *team = cJSON_GetObjectItemCaseSensitive(root, "team_id");
   int ok = cJSON_IsNumber(version) && version->valuedouble == 2 && cJSON_IsNumber(port) &&
            floor(port->valuedouble) == port->valuedouble && cJSON_IsNumber(team) &&
            floor(team->valuedouble) == team->valuedouble && team->valuedouble >= 1 &&
            team->valuedouble <= 9007199254740991.0 &&
            copy_json_string(root, "state", out->state, sizeof(out->state)) == 0 &&
            copy_json_string(root, "host", out->host, sizeof(out->host)) == 0 &&
            copy_json_string(root, "endpoint", out->endpoint, sizeof(out->endpoint)) == 0 &&
            copy_json_string(root, "server_id", out->server_id, sizeof(out->server_id)) == 0 &&
            copy_json_string(root, "operation", out->operation, sizeof(out->operation)) == 0 &&
            copy_json_string(root, "client_csr_digest", out->client_csr_digest,
                             sizeof(out->client_csr_digest)) == 0 &&
            copy_json_string(root, "management_csr_digest", out->management_csr_digest,
                             sizeof(out->management_csr_digest)) == 0 &&
            copy_json_string(root, "ca", out->ca, sizeof(out->ca)) == 0 &&
            copy_json_string(root, "client_csr", out->client_csr, sizeof(out->client_csr)) == 0 &&
            copy_json_string(root, "key", out->client_key, sizeof(out->client_key)) == 0 &&
            copy_json_string(root, "cert", out->client_cert, sizeof(out->client_cert)) == 0 &&
            copy_json_string(root, "management_csr", out->management_csr,
                             sizeof(out->management_csr)) == 0 &&
            copy_json_string(root, "management_key", out->management_key,
                             sizeof(out->management_key)) == 0 &&
            copy_json_string(root, "management_cert", out->management_cert,
                             sizeof(out->management_cert)) == 0;
   if (ok)
   {
      out->version = 2;
      out->port = (int)port->valuedouble;
      out->team_id = (int64_t)team->valuedouble;
   }
   cJSON_Delete(root);
   OPENSSL_cleanse(raw, (size_t)st.st_size + 1);
   free(raw);
   if (ok && kb_managed_server_identity_validate(out))
      return 0;
   kb_managed_server_identity_clear(out);
   return -1;
failed:
   if (raw)
   {
      OPENSSL_cleanse(raw, (size_t)st.st_size + 1);
      free(raw);
   }
   return -1;
}

void kb_managed_server_identity_clear(kb_managed_server_identity_t *identity)
{
   if (identity)
      OPENSSL_cleanse(identity, sizeof(*identity));
}
