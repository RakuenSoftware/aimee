#ifndef AIMEE_MANAGEMENT_READ_H
#define AIMEE_MANAGEMENT_READ_H

#include <stddef.h>
#include <stdint.h>

#define SERVER_MGMT_READ_AGENT_MAX 16
#define SERVER_MGMT_READ_BODY_MAX  (32u * 1024u)

typedef enum
{
   SERVER_MGMT_READ_SELECTOR_AGENTS = 1,
   SERVER_MGMT_READ_SELECTOR_CONFIG = 2
} server_mgmt_read_selector_t;

typedef struct
{
   const char *server_id;
   int64_t team_id;
   const unsigned char *nonce;
   const char *kb_issuer;
   const char *kb_serial;
   const char *server_issuer;
   const char *server_serial;
   uint64_t revocation_generation;
   uint64_t publication_generation;
   server_mgmt_read_selector_t selector;
} server_mgmt_read_digest_input_t;

typedef struct
{
   char name[64];
   char provider[16];
   char model[128];
   int enabled;
   int delegate_available;
   int primary_only;
   int max_parallel;
} server_mgmt_read_agent_t;

typedef struct
{
   int mtls;
   int remote_writes;
   char client_transport[16];
   int cli_session_forwarding;
   int require_aimee_git;
} server_mgmt_read_config_t;

const char *server_mgmt_read_selector_name(server_mgmt_read_selector_t);
const char *server_mgmt_read_selector_purpose(server_mgmt_read_selector_t);
int server_mgmt_read_selector_path(server_mgmt_read_selector_t, const char *, char *, size_t);
int server_mgmt_read_digest(const server_mgmt_read_digest_input_t *, char out[65]);
int server_mgmt_read_project(const char *server_id, int64_t team_id,
                             const server_mgmt_read_agent_t *agents, size_t agent_count, char *out,
                             size_t cap);
int server_mgmt_read_project_config(const char *, int64_t, const server_mgmt_read_config_t *,
                                    char *, size_t);
int server_mgmt_read_load_agents(server_mgmt_read_agent_t *out, size_t cap, size_t *count);
int server_mgmt_read_load_config(server_mgmt_read_config_t *out);

#endif
