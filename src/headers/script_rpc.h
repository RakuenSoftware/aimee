#ifndef DEC_SCRIPT_RPC_H
#define DEC_SCRIPT_RPC_H 1

#include "aimee.h"
#include <aimee/tools/agent_tools.h>
#include <stdatomic.h>
#include <pthread.h>

typedef struct script_rpc_server
{
   int listen_fd;
   int client_fd;
   int use_fd_pair;
   atomic_int stop;
   /* Immutable permission copy; destroyed only after the RPC worker joins. */
   agent_tools_scope_snapshot_t tools;
   pthread_t thread;
   int thread_started;
   char stub_dir[MAX_PATH_LEN];
   int tool_calls;
   int policy_denials;
   /* Retained in the result schema for compatibility; descriptor-authenticated
    * RPC has no HMAC credential, so this remains zero. */
   int hmac_failures;
} script_rpc_server_t;

int script_rpc_prepare(script_rpc_server_t *srv);
int script_rpc_start(script_rpc_server_t *srv);
void script_rpc_stop(script_rpc_server_t *srv);
void script_rpc_cleanup(script_rpc_server_t *srv);

#endif /* DEC_SCRIPT_RPC_H */
