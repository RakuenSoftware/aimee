#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "kb_mgmt_token_authority_ipc.h"
#include "kb_mgmt_token_authority_service.h"
#include "vault_custody_kms.h"
#include "vault_server_key.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <openssl/crypto.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define SYSTEMD_LISTEN_FD 3

typedef struct
{
   const char *dsn;
} reopen_config_t;

static void fixed_error(const char *kind)
{
   (void)fprintf(stderr, "aimee-kb-token-authority: %s\n", kind);
}

static int fixed_text(const char *value, size_t max)
{
   if (!value || !*value || strnlen(value, max + 1) > max)
      return 0;
   for (const unsigned char *p = (const unsigned char *)value; *p; ++p)
      if (*p < 0x20 || *p == 0x7f)
         return 0;
   return 1;
}

static char *copy_env(const char *name, size_t max)
{
   const char *value = getenv(name);
   return fixed_text(value, max) ? strdup(value) : NULL;
}

static int root_owned_file(const char *path, int executable)
{
   struct stat st;
   if (!path || path[0] != '/' || lstat(path, &st) != 0 || !S_ISREG(st.st_mode) || st.st_uid != 0 ||
       (st.st_mode & 022) || (executable && !(st.st_mode & 0111)))
      return 0;
   char parent[PATH_MAX];
   size_t n = strlen(path);
   if (n >= sizeof(parent))
      return 0;
   memcpy(parent, path, n + 1);
   char *slash = strrchr(parent, '/');
   if (!slash)
      return 0;
   slash == parent ? (void)(parent[1] = 0) : (void)(*slash = 0);
   for (;;)
   {
      if (lstat(parent, &st) != 0 || !S_ISDIR(st.st_mode) || S_ISLNK(st.st_mode) ||
          st.st_uid != 0 || (st.st_mode & 022))
         return 0;
      if (parent[0] == '/' && parent[1] == 0)
         return 1;
      slash = strrchr(parent, '/');
      if (!slash)
         return 0;
      slash == parent ? (void)(parent[1] = 0) : (void)(*slash = 0);
   }
}

static int parse_id(const char *value, uint64_t max, uint64_t *out)
{
   char *end = NULL;
   errno = 0;
   unsigned long long parsed = value ? strtoull(value, &end, 10) : 0;
   if (errno || !value || !*value || !end || *end || !parsed || parsed > max)
      return -1;
   *out = (uint64_t)parsed;
   return 0;
}

static int reopen_database(void *opaque, db2_management_token_authority_ctx_t *db)
{
   reopen_config_t *config = opaque;
   char error[256] = "";
   if (!config || !config->dsn || !db)
      return -1;
   db2_management_token_authority_close(db);
   int rc = db2_management_token_authority_open(db, config->dsn, error, sizeof(error));
   OPENSSL_cleanse(error, sizeof(error));
   return rc;
}

static void stop_signal(int signal_number)
{
   (void)signal_number;
   kb_mgmt_token_authority_daemon_request_stop();
}

int main(int argc, char **argv)
{
   (void)argv;
   if (argc != 1 || getuid() == 0 || geteuid() != getuid())
   {
      fixed_error("usage-or-identity");
      return 64;
   }

   aimee_db2_register_token_record_validators(kb_mgmt_token_authority_record_valid,
                                              kb_identity_token_authority_record_valid);

   char *dsn = copy_env("AIMEE_KB_TOKEN_AUTHORITY_DSN", 4096);
   char *helper = copy_env("AIMEE_VAULT_KMS_HELPER", PATH_MAX - 1);
   char *kms_id = copy_env("AIMEE_VAULT_KMS_KEY_ID", 600);
   char *hwm_public = copy_env("AIMEE_VAULT_KMS_HWM_PUBKEY", PATH_MAX - 1);
   char *hwm_domain = copy_env("AIMEE_VAULT_KMS_HWM_DOMAIN", 256);
   const char *kb_uid_text = getenv("AIMEE_KB_RUNTIME_UID");
   const char *socket_gid_text = getenv("AIMEE_KB_TOKEN_AUTHORITY_SOCKET_GID");
   const char *listen_pid_text = getenv("LISTEN_PID");
   const char *listen_fds_text = getenv("LISTEN_FDS");
   uint64_t kb_uid = 0, socket_gid = 0, listen_pid = 0, listen_fds = 0;
   if (!dsn || !helper || !kms_id || !hwm_public || !hwm_domain || !root_owned_file(helper, 1) ||
       !root_owned_file(hwm_public, 0) || parse_id(kb_uid_text, UINT32_MAX, &kb_uid) ||
       parse_id(socket_gid_text, UINT32_MAX, &socket_gid) ||
       parse_id(listen_pid_text, INT32_MAX, &listen_pid) ||
       parse_id(listen_fds_text, INT32_MAX, &listen_fds) || listen_pid != (uint64_t)getpid() ||
       listen_fds != 1 || kb_uid == (uint64_t)getuid() || fcntl(SYSTEMD_LISTEN_FD, F_GETFD) < 0)
   {
      fixed_error("configuration");
      return 66;
   }

   if (clearenv() != 0 || setenv("AIMEE_VAULT_KMS_HELPER", helper, 1) != 0 ||
       setenv("AIMEE_VAULT_KMS_KEY_ID", kms_id, 1) != 0 ||
       setenv("AIMEE_VAULT_KMS_HWM_PUBKEY", hwm_public, 1) != 0 ||
       setenv("AIMEE_VAULT_KMS_HWM_DOMAIN", hwm_domain, 1) != 0)
   {
      fixed_error("environment");
      return 65;
   }
   struct sigaction action;
   memset(&action, 0, sizeof(action));
   action.sa_handler = stop_signal;
   sigemptyset(&action.sa_mask);
   if (sigaction(SIGINT, &action, NULL) != 0 || sigaction(SIGTERM, &action, NULL) != 0)
   {
      fixed_error("signal");
      return 70;
   }

   db2_management_token_authority_ctx_t database;
   memset(&database, 0, sizeof(database));
   reopen_config_t reopen = {.dsn = dsn};
   kb_mgmt_token_authority_service_t service = {
       .db = &database, .reopen_db = reopen_database, .reopen_opaque = &reopen};
   const int preserve[] = {SYSTEMD_LISTEN_FD};
   kb_mgmt_token_authority_daemon_config_t daemon = {
       .listen_fd = SYSTEMD_LISTEN_FD,
       .socket_path = KB_MGMT_TOKEN_AUTHORITY_SOCKET_PATH,
       .authority_uid = getuid(),
       .kb_uid = (uid_t)kb_uid,
       .socket_gid = (gid_t)socket_gid,
       .socket_mode = 0660,
       .timeout_ms = KB_MGMT_TOKEN_AUTHORITY_IO_TIMEOUT_MS,
       .preserve_fds = preserve,
       .preserve_fd_count = sizeof(preserve) / sizeof(preserve[0]),
       .issue = kb_mgmt_token_authority_service_issue,
       .issue_opaque = &service,
   };
   if (kb_mgmt_token_authority_daemon_harden(&daemon) != 0)
      return 69;

   /* Process and inherited-fd hardening is complete before the first custody
    * provider call can open, attest, or decrypt authority material. */
   vault_custody_set_provider(vault_custody_kms_provider());
   if (vault_unseal(NULL, 0) != 0 || vault_custody_kms_hwm_refresh() != 0 || vault_is_sealed() ||
       !vault_custody_kms_hwm_ready())
      return 68;

   int daemon_rc = kb_mgmt_token_authority_daemon_run(&daemon);
   db2_management_token_authority_close(&database);
   int seal_rc = vault_seal();
   OPENSSL_cleanse(dsn, strlen(dsn));
   free(dsn);
   free(helper);
   free(kms_id);
   free(hwm_public);
   free(hwm_domain);
   return daemon_rc == 0 && seal_rc == 0 ? 0 : 71;
}
