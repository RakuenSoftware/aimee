#include "managed_server_identity_install.h"

#include "managed_server_identity.h"
#include "db2/db2.h"
#include "db2/db2_tenant.h"
#include "db2/server_registry.h"
#include "db2/team.h"
#include "kb_identity.h"
#include "kb_paths.h"

#include <errno.h>
#include <fcntl.h>
#include <openssl/crypto.h>
#include <stdio.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

static int owner_principal(kb_principal_t *owner)
{
   kb_verify_result_t verified;
   memset(&verified, 0, sizeof(verified));
   return kb_principal_from_verify(&verified, "", owner);
}

static int select_team(const kb_principal_t *owner, int64_t *team_id)
{
   if (!owner || !team_id || db2_tenant_scope_begin(owner, 0) != 0)
      return -1;
   db2_team_row_t teams[256];
   int count = db2_team_list(teams, (int)(sizeof(teams) / sizeof(teams[0])));
   int rc = -1;
   if (count == 0)
      rc = db2_team_create("default", "managed-install", team_id);
   else if (count == 1)
   {
      *team_id = teams[0].id;
      rc = 0;
   }
   else
   {
      int matches = 0;
      for (int i = 0; i < count; i++)
         if (strcmp(teams[i].name, "default") == 0)
         {
            *team_id = teams[i].id;
            matches++;
         }
      rc = matches == 1 ? 0 : -1;
   }
   if (rc != 0)
   {
      db2_tenant_scope_rollback();
      return -1;
   }
   return db2_tenant_scope_commit();
}

static int pending_registry(const kb_principal_t *owner,
                            const kb_managed_server_identity_t *identity,
                            char status[32])
{
   db2_server_pending_t pending = {
       .operation = identity->operation,
       .server_id = identity->server_id,
       .endpoint = identity->endpoint,
       .client_cn = "p5-server-client",
       .management_cn = "p5-server-management",
       .client_csr_digest = identity->client_csr_digest,
       .management_csr_digest = identity->management_csr_digest,
       .team_id = identity->team_id,
       .ttl_seconds = 3600,
   };
   if (db2_tenant_scope_begin(owner, identity->team_id) != 0)
      return -1;
   int rc = db2_server_registry_pending(&pending, status, 32);
   if (rc != 0)
   {
      db2_tenant_scope_rollback();
      return -1;
   }
   return db2_tenant_scope_commit();
}

static int cert_identity(const char *cert, db2_server_cert_identity_t *out,
                         char issuer[601], char serial[129], char fingerprint[65])
{
   char raw_serial[129];
   if (!cert || !out || kb_pki_cert_metadata(cert, issuer, 601, raw_serial,
                                              sizeof(raw_serial)) != 0 ||
       kb_cert_serial_normalize(raw_serial, serial, 129) != 0 ||
       kb_pki_ca_fingerprint(cert, fingerprint, 65) != 0)
      return -1;
   out->issuer = issuer;
   out->serial_norm = serial;
   out->fingerprint = fingerprint;
   return 0;
}

static int finalize_registry(const kb_principal_t *owner,
                             const kb_managed_server_identity_t *identity)
{
   db2_server_cert_identity_t client, management;
   char client_issuer[601], client_serial[129], client_fp[65];
   char management_issuer[601], management_serial[129], management_fp[65];
   char status[32];
   if (cert_identity(identity->client_cert, &client, client_issuer, client_serial, client_fp) ||
       cert_identity(identity->management_cert, &management, management_issuer,
                     management_serial, management_fp) ||
       db2_tenant_scope_begin(owner, identity->team_id) != 0)
      return -1;
   int rc = db2_server_registry_finalize(
       identity->operation, identity->client_csr_digest, identity->management_csr_digest,
       &client, &management, status, sizeof(status));
   if (rc != 0 || strcmp(status, "active") != 0)
   {
      db2_tenant_scope_rollback();
      return -1;
   }
   return db2_tenant_scope_commit();
}

static int heartbeat_registry(const kb_principal_t *owner,
                              const kb_managed_server_identity_t *identity)
{
   db2_server_cert_identity_t client;
   char issuer[601], serial[129], fingerprint[65];
   if (cert_identity(identity->client_cert, &client, issuer, serial, fingerprint) ||
       db2_tenant_scope_begin(owner, identity->team_id) != 0)
      return -1;
   int rc = db2_server_registry_heartbeat(identity->server_id, client.issuer,
                                          client.serial_norm, client.fingerprint,
                                          "identity-ready", "installer-v2");
   if (rc != 0)
   {
      db2_tenant_scope_rollback();
      return -1;
   }
   return db2_tenant_scope_commit();
}

static int paths_and_lock(const kb_managed_server_identity_install_options_t *options,
                          char identity_path[4096], int *lock_fd)
{
   struct stat st;
   if (!options || !options->server_home || options->server_home[0] != '/' ||
       lstat(options->server_home, &st) != 0 || !S_ISDIR(st.st_mode) ||
       S_ISLNK(st.st_mode))
      return -1;
   char lock_path[4096];
   int a = snprintf(identity_path, 4096, "%s/kb-client-identity.json", options->server_home);
   int b = snprintf(lock_path, sizeof(lock_path), "%s/.kb-client-identity.lock",
                    options->server_home);
   if (a <= 0 || a >= 4096 || b <= 0 || (size_t)b >= sizeof(lock_path))
      return -1;
   int fd = open(lock_path, O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
   if (fd < 0 || flock(fd, LOCK_EX) != 0)
   {
      if (fd >= 0)
         close(fd);
      return -1;
   }
   *lock_fd = fd;
   return 0;
}

int kb_managed_server_identity_install(
    const kb_managed_server_identity_install_options_t *options)
{
   if (!options || !options->host || !options->endpoint || options->port < 1 ||
       options->port > 65535 || (geteuid() != 0 && options->owner != geteuid()))
      return -1;

   char identity_path[4096];
   int lock_fd = -1;
   if (paths_and_lock(options, identity_path, &lock_fd) != 0)
      return -1;

   int rc = -1;
   kb_principal_t owner;
   kb_pki_ca_t ca;
   kb_managed_server_identity_t identity;
   memset(&ca, 0, sizeof(ca));
   memset(&identity, 0, sizeof(identity));
   char ca_dir[4096];
   int n = snprintf(ca_dir, sizeof(ca_dir), "%s/kb-ca", kb_default_config_dir());
   if (n <= 0 || (size_t)n >= sizeof(ca_dir) || owner_principal(&owner) != 0 ||
       kb_pki_ca_load_custodied(ca_dir, &ca) != 0)
      goto done;

   struct stat identity_stat;
   if (lstat(identity_path, &identity_stat) == 0)
   {
      if (!S_ISREG(identity_stat.st_mode) ||
          kb_managed_server_identity_load(identity_path, options->owner, &identity) != 0 ||
          strcmp(identity.host, options->host) != 0 || identity.port != options->port ||
          strcmp(identity.endpoint, options->endpoint) != 0 ||
          strcmp(identity.ca, ca.cert_pem) != 0)
         goto done;
   }
   else
   {
      if (errno != ENOENT)
         goto done;
      int64_t team_id = 0;
      if (select_team(&owner, &team_id) != 0 ||
          kb_managed_server_identity_generate(&ca, options->host, options->port,
                                               options->endpoint, team_id, &identity) != 0 ||
          kb_managed_server_identity_save(identity_path, options->owner, &identity) != 0)
         goto done;
   }

   if (strcmp(identity.state, "ready") != 0)
   {
      char pending_status[32];
      if (pending_registry(&owner, &identity, pending_status) != 0 ||
          (strcmp(pending_status, "pending") != 0 && strcmp(pending_status, "active") != 0))
         goto done;
      if (strcmp(identity.state, "pending") == 0)
      {
         if (strcmp(pending_status, "active") == 0 ||
             kb_managed_server_identity_issue(&ca, &identity) != 0 ||
             kb_managed_server_identity_save(identity_path, options->owner, &identity) != 0)
            goto done;
      }
      if (finalize_registry(&owner, &identity) != 0)
         goto done;
      snprintf(identity.state, sizeof(identity.state), "ready");
      if (kb_managed_server_identity_save(identity_path, options->owner, &identity) != 0)
         goto done;
   }
   if (heartbeat_registry(&owner, &identity) != 0)
      goto done;

   printf("{\"state\":\"ready\",\"server_id\":\"%s\",\"team_id\":%lld}\n",
          identity.server_id, (long long)identity.team_id);
   rc = 0;
done:
   kb_managed_server_identity_clear(&identity);
   OPENSSL_cleanse(&ca, sizeof(ca));
   flock(lock_fd, LOCK_UN);
   close(lock_fd);
   return rc;
}
