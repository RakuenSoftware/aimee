#include "kb_scope.h" /* KB_SERVER_CLIENT_SCOPE */
#include "managed_server_identity_install.h"

#include "managed_server_identity.h"
#include "modules/db2/c/db2.h"
#include "modules/db2/c/db2_tenant.h"
#include "modules/db2/c/membership.h"
#include "modules/db2/c/server_registry.h"
#include "modules/db2/c/team.h"
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
   /* set_tenant_context validates membership even for the bootstrap owner. Keep
    * team creation and the owner membership in the same transaction so a fresh
    * managed install cannot commit a team that its next registry transaction is
    * forbidden to select. Preserve an existing default when upgrading a KB
    * with more than one team. */
   char owner_key[576];
   int64_t existing_default = 0;
   if (rc == 0 && (kb_identity_key(owner, owner_key, sizeof(owner_key)) != 0 ||
                   db2_membership_add(
                       owner_key, *team_id,
                       db2_membership_default_team(owner_key, &existing_default) != 0, NULL) != 0))
      rc = -1;
   if (rc != 0)
   {
      db2_tenant_scope_rollback();
      return -1;
   }
   return db2_tenant_scope_commit();
}

static int ensure_owner_membership(const kb_principal_t *owner, int64_t team_id)
{
   char owner_key[576];
   int64_t existing_default = 0;
   if (!owner || team_id < 1 || kb_identity_key(owner, owner_key, sizeof(owner_key)) != 0 ||
       db2_tenant_scope_begin(owner, 0) != 0)
      return -1;
   int rc = db2_membership_add(
       owner_key, team_id, db2_membership_default_team(owner_key, &existing_default) != 0, NULL);
   if (rc != 0)
   {
      db2_tenant_scope_rollback();
      return -1;
   }
   return db2_tenant_scope_commit();
}

static int ensure_principal_membership(const kb_principal_t *owner, const kb_principal_t *member,
                                       int64_t team_id)
{
   char member_key[576];
   if (!owner || !member || team_id < 1 ||
       kb_identity_key(member, member_key, sizeof(member_key)) != 0 ||
       db2_tenant_scope_begin(owner, 0) != 0)
      return -1;
   /* Every managed request names the registry-bound team explicitly, so these
    * service/operator rows need no default-team side effect. The insert is
    * idempotent, which makes deploy a membership repair operation too. */
   int rc = db2_membership_add(member_key, team_id, 0, NULL);
   if (rc != 0)
   {
      db2_tenant_scope_rollback();
      return -1;
   }
   return db2_tenant_scope_commit();
}

static int pending_registry(const kb_principal_t *owner,
                            const kb_managed_server_identity_t *identity, char status[32])
{
   db2_server_pending_t pending = {
       .operation = identity->operation,
       .server_id = identity->server_id,
       .endpoint = identity->endpoint,
       .client_cn = KB_SERVER_CLIENT_SCOPE,
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

static int cert_identity(const char *cert, db2_server_cert_identity_t *out, char issuer[601],
                         char serial[129], char fingerprint[65])
{
   char raw_serial[129];
   if (!cert || !out ||
       kb_pki_cert_metadata(cert, issuer, 601, raw_serial, sizeof(raw_serial)) != 0 ||
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
       cert_identity(identity->management_cert, &management, management_issuer, management_serial,
                     management_fp) ||
       db2_tenant_scope_begin(owner, identity->team_id) != 0)
      return -1;
   int rc = db2_server_registry_finalize(identity->operation, identity->client_csr_digest,
                                         identity->management_csr_digest, &client, &management,
                                         status, sizeof(status));
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
   int rc = db2_server_registry_heartbeat(identity->server_id, client.issuer, client.serial_norm,
                                          client.fingerprint, "identity-ready", "installer-v2");
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
       lstat(options->server_home, &st) != 0 || !S_ISDIR(st.st_mode) || S_ISLNK(st.st_mode))
      return -1;
   char lock_path[4096];
   int a = snprintf(identity_path, 4096, "%s/kb-client-identity.json", options->server_home);
   int b =
       snprintf(lock_path, sizeof(lock_path), "%s/.kb-client-identity.lock", options->server_home);
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

int kb_managed_server_identity_install(const kb_managed_server_identity_install_options_t *options)
{
   if (!options || !options->host || !options->endpoint || options->port < 1 ||
       options->port > 65535 || (geteuid() != 0 && options->owner != geteuid()))
      return -1;

   kb_principal_t service_member, operator_member;
   memset(&service_member, 0, sizeof(service_member));
   memset(&operator_member, 0, sizeof(operator_member));
   static const char service_prefix[] = "service:";
   const char *service_name = KB_SERVER_CLIENT_SCOPE;
   if (strncmp(service_name, service_prefix, sizeof(service_prefix) - 1) != 0 ||
       kb_principal_from_host_account(service_name + sizeof(service_prefix) - 1, &service_member) !=
           0 ||
       (options->member && options->member[0] &&
        kb_principal_from_host_account(options->member, &operator_member) != 0))
   {
      fputs("managed-server-identity: invalid managed service or operator identity\n", stderr);
      return -1;
   }

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

   /* Every precondition below used to be one silent `goto done`: the process
    * exited 1 with no reason, so an operator repairing broken trust could not
    * tell a rotated CA from an unreadable file from a wrong --host. Name the
    * cause on stderr; none of these strings carry key material. */
   struct stat identity_stat;
   int have_identity = lstat(identity_path, &identity_stat) == 0;
   int reissue = 0;
   if (have_identity)
   {
      if (!S_ISREG(identity_stat.st_mode))
      {
         fputs("managed-server-identity: stored identity is not a regular file\n", stderr);
         goto done;
      }
      if (kb_managed_server_identity_load(identity_path, options->owner, &identity) != 0)
      {
         fputs("managed-server-identity: stored identity could not be loaded (wrong owner or "
               "corrupt file); re-run with --force to replace it\n",
               stderr);
         goto done;
      }
      int targets_differ = strcmp(identity.host, options->host) != 0 ||
                           identity.port != options->port ||
                           strcmp(identity.endpoint, options->endpoint) != 0;
      int ca_differs = strcmp(identity.ca, ca.cert_pem) != 0;
      if ((targets_differ || ca_differs) && !options->force)
      {
         fprintf(stderr,
                 "managed-server-identity: stored identity %s; re-run with --force to re-issue\n",
                 targets_differ ? "targets a different KB endpoint"
                                : "was issued by a different CA");
         goto done;
      }
      reissue = options->force;
   }
   else if (errno != ENOENT)
   {
      perror("managed-server-identity: cannot stat stored identity");
      goto done;
   }

   if (!have_identity || reissue)
   {
      /* Keep the server on the team it already belongs to. Re-selecting could
       * silently re-home an enrolled server when the owner has several. */
      int64_t team_id = reissue ? identity.team_id : 0;
      if (team_id <= 0 && select_team(&owner, &team_id) != 0)
      {
         fputs("managed-server-identity: no team available for the bootstrap owner\n", stderr);
         goto done;
      }
      kb_managed_server_identity_clear(&identity);
      memset(&identity, 0, sizeof(identity));
      if (kb_managed_server_identity_generate(&ca, options->host, options->port, options->endpoint,
                                              team_id, &identity) != 0)
      {
         fputs("managed-server-identity: could not generate a server identity\n", stderr);
         goto done;
      }
      if (kb_managed_server_identity_save(identity_path, options->owner, &identity) != 0)
      {
         fputs("managed-server-identity: could not persist the server identity\n", stderr);
         goto done;
      }
   }

   /* Loading a pending identity is the normal crash-recovery path. Older
    * managed installers could persist that file after creating the team but
    * before enrolling the bootstrap owner, so repair the invariant after both
    * load and generation. The insert is idempotent and does not change an
    * existing default-team choice. */
   if (ensure_owner_membership(&owner, identity.team_id) != 0)
      goto done;
   /* The registry team is KB-owned authority. Enroll the independently
    * authenticated application identity and the appliance's current PAM
    * operator here, rather than letting aimee-server manufacture membership on
    * each content request. Without these rows a fresh wizard clone can ingest
    * successfully but every search is denied by the service/caller intersection. */
   if (ensure_principal_membership(&owner, &service_member, identity.team_id) != 0 ||
       (operator_member.authenticated &&
        ensure_principal_membership(&owner, &operator_member, identity.team_id) != 0))
   {
      fputs("managed-server-identity: could not enroll managed service/operator membership\n",
            stderr);
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

   printf("{\"state\":\"ready\",\"server_id\":\"%s\",\"team_id\":%lld}\n", identity.server_id,
          (long long)identity.team_id);
   rc = 0;
done:
   kb_managed_server_identity_clear(&identity);
   OPENSSL_cleanse(&ca, sizeof(ca));
   flock(lock_fd, LOCK_UN);
   close(lock_fd);
   return rc;
}
