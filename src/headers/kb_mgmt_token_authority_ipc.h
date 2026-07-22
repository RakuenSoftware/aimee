#ifndef AIMEE_KB_MGMT_TOKEN_AUTHORITY_IPC_H
#define AIMEE_KB_MGMT_TOKEN_AUTHORITY_IPC_H

#include "kb_mgmt_token.h"

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#ifndef KB_MGMT_TOKEN_AUTHORITY_SOCKET_PATH
#define KB_MGMT_TOKEN_AUTHORITY_SOCKET_PATH "/run/aimee/token-authority.sock"
#endif
#define KB_MGMT_TOKEN_AUTHORITY_CORRELATION_LEN 64u
#define KB_MGMT_TOKEN_AUTHORITY_JTI_LEN         64u
#define KB_MGMT_TOKEN_AUTHORITY_IO_TIMEOUT_MS   5000u

#ifdef __cplusplus
extern "C"
{
#endif

   typedef enum
   {
      KB_MGMT_TOKEN_AUTHORITY_IPC_OK = 0,
      KB_MGMT_TOKEN_AUTHORITY_IPC_INVALID = 1,
      KB_MGMT_TOKEN_AUTHORITY_IPC_DENIED = 2,
      KB_MGMT_TOKEN_AUTHORITY_IPC_CONFLICT = 3,
      KB_MGMT_TOKEN_AUTHORITY_IPC_EXPIRED = 4,
      KB_MGMT_TOKEN_AUTHORITY_IPC_SEALED = 5,
      KB_MGMT_TOKEN_AUTHORITY_IPC_INTEGRITY = 6,
      KB_MGMT_TOKEN_AUTHORITY_IPC_UNAVAILABLE = 7,
      KB_MGMT_TOKEN_AUTHORITY_IPC_COMMIT_AMBIGUOUS = 8,
      KB_MGMT_TOKEN_AUTHORITY_IPC_ALREADY_USED = 9,
   } kb_mgmt_token_authority_ipc_result_t;

   typedef struct
   {
      char jwt[KB_MGMT_TOKEN_WIRE_MAX + 1];
      size_t jwt_len;
   } kb_mgmt_token_authority_output_t;

   /* Ordinary-KB client. The path must be absolute and name a root-owned Unix
    * socket with exactly socket_mode/socket_gid. authority_uid is the expected
    * kernel peer UID: zero for a root-created socket-activation listener, or
    * the daemon UID for an authority-created listener. Root is outside this
    * unprivileged-compromise boundary. No claim, key, digest, or signing input
    * crosses this seam. */
   typedef struct
   {
      const char *socket_path;
      uid_t authority_uid;
      gid_t socket_gid;
      mode_t socket_mode;
      uint32_t timeout_ms;
   } kb_mgmt_token_authority_client_config_t;

   kb_mgmt_token_authority_ipc_result_t
   kb_mgmt_token_authority_client_issue(const kb_mgmt_token_authority_client_config_t *config,
                                        const char *correlation_id, const char *jti,
                                        kb_mgmt_token_authority_output_t *out);

   /* The authority's only application callback. Implementations must derive
    * every claim and the one permitted RS256 operation from correlation/JTI.
    * This deliberately cannot express an arbitrary message or signature. */
   typedef kb_mgmt_token_authority_ipc_result_t (*kb_mgmt_token_authority_ipc_issue_fn)(
       const char *correlation_id, const char *jti, kb_mgmt_token_authority_output_t *out,
       void *opaque);

   typedef struct
   {
      int listen_fd; /* Pre-opened by the root-owned socket unit/launcher. */
      const char *socket_path;
      uid_t authority_uid;
      uid_t kb_uid;
      gid_t socket_gid;
      mode_t socket_mode;
      uint32_t timeout_ms;
      const int *preserve_fds; /* Includes listen_fd and authority-private service fds. */
      size_t preserve_fd_count;
      kb_mgmt_token_authority_ipc_issue_fn issue;
      void *issue_opaque;
   } kb_mgmt_token_authority_daemon_config_t;

   /* Harden the current, already-separated authority process and serve one
    * connection at a time until accept(2) is interrupted by a signal. */
   int kb_mgmt_token_authority_daemon_run(const kb_mgmt_token_authority_daemon_config_t *config);

#ifdef __cplusplus
}
#endif
#endif
