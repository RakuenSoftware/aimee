#ifndef AIMEE_DELEGATE_DOCKER_INTERNAL_H
#define AIMEE_DELEGATE_DOCKER_INTERNAL_H
/* Private backend state shared by Docker transport and its lease owner only. */
#include "aimee.h"
#include "cli_native_spawn_guard.h"
#include <sys/stat.h>
typedef struct
{
   char container_name[128];
   char container_id[65], image_id[72];
   char engine_endpoint[128];
   char controller_binds[16384];
   char docker_binary[MAX_PATH_LEN];
   int private_io;
   int retired, engine_fd, lease_fd, lease_parent_fd, lease_owners_fd;
   int artifact_helper_fd, artifact_socket_fd;
   int cleanup_slot, cleanup_failed, cleanup_pending;
   char lease_leaf[65];
   struct stat lease_identity, parent_identity, helper_identity, socket_identity;
   char lease_check[MAX_PATH_LEN];
   cli_native_stdio_t file_owner, forwarder, package_forwarder;
   int launcher_fd, launcher_cwd_fd, file_started;
   int file_ready, file_busy;
   unsigned file_requests;
   long long file_deadline;
   int (*file_validate)(void *);
   void *file_context;
   char prepared_command[4096], prepared_home[4096], prepared_mcp[4096];
   char image[256];
   char workspace_host[MAX_PATH_LEN]; /* host-side mount point */
   char cwd[MAX_PATH_LEN];            /* in-container cwd, defaults to /workspace */
   /* The in-container working directory, and what relative tool paths resolve
    * against. Per-state, not the global resolve_docker_workdir(): a caller-provided
    * tree is mounted at its OWN absolute host path (see docker_build_mounts), so
    * /workspace is not where its files live. Anchoring resolution on a global while
    * the mount moved is how a tool would read the wrong tree and report it as
    * missing. */
   char workdir[MAX_PATH_LEN];
} docker_state_t;
const char *resolve_docker_bin(void);
const char *resolve_delegate_egress_bin(void);
int docker_controller_snapshot(docker_state_t *owner, const char *encoded, char **table);
int docker_capture_binary(docker_state_t *state);
int docker_lease_open(docker_state_t *state, const char *private_socket);
int docker_cleanup_reserve(docker_state_t *state);
int docker_cleanup_lease(docker_state_t *state);
void docker_state_destroy(docker_state_t *state);
int docker_reap_pending(void);
int docker_pin_engine(docker_state_t *state);
int docker_recover_journal(void);
#endif
