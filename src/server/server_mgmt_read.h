#ifndef AIMEE_SERVER_MGMT_READ_H
#define AIMEE_SERVER_MGMT_READ_H

#include <stddef.h>
#include <stdint.h>

#define SERVER_MGMT_READ_AGENT_MAX 16
#define SERVER_MGMT_READ_BODY_MAX  (32u * 1024u)

typedef struct
{
   const char *server_id;
   int64_t team_id;
   const unsigned char *nonce; /* exactly 32 bytes */
   const char *kb_issuer;
   const char *kb_serial;
   const char *server_issuer;
   const char *server_serial;
   uint64_t revocation_generation;
   uint64_t publication_generation;
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

/* Exact P5-D2a SHA-256 transcript. Output is 64 lowercase hex plus NUL. */
int server_mgmt_read_digest(const server_mgmt_read_digest_input_t *, char out[65]);

/* Render a fresh, sorted allowlist object. Never truncates or emits a partial object. */
int server_mgmt_read_project(const char *server_id, int64_t team_id,
                             const server_mgmt_read_agent_t *agents, size_t agent_count, char *out,
                             size_t cap);

/* Production-only frozen getter. Copies only the seven public fields. */
int server_mgmt_read_load_agents(server_mgmt_read_agent_t *out, size_t cap, size_t *count);

#endif
