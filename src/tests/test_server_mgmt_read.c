#include "server/server_mgmt_read.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
   unsigned char nonce[32];
   for (size_t i = 0; i < sizeof(nonce); ++i)
      nonce[i] = (unsigned char)i;
   server_mgmt_read_digest_input_t in = {
       "server-a", 42, nonce, "/CN=kb-ca", "01af", "/CN=server-ca", "10be", 7, 9};
   char digest[65];
   assert(server_mgmt_read_digest(&in, digest) == 0);
   assert(!strcmp(digest, "c66354428fbcdb9648b532b8de71b748e0d058711cfb441c608f5460564efcbf"));
   in.revocation_generation++;
   assert(server_mgmt_read_digest(&in, digest) == 0);
   assert(strcmp(digest, "c66354428fbcdb9648b532b8de71b748e0d058711cfb441c608f5460564efcbf"));
   in.revocation_generation--;
   in.kb_issuer = "/CN=kb-\xc3\xa9";
   assert(server_mgmt_read_digest(&in, digest) == 0);
   in.kb_issuer = "/CN=kb-\xc0\xaf";
   assert(server_mgmt_read_digest(&in, digest) < 0);
   in.kb_issuer = "/CN=kb-ca";

   server_mgmt_read_agent_t agents[2] = {
       {.name = "zeta",
        .provider = "openai",
        .model = "gpt-5.2",
        .enabled = 1,
        .delegate_available = 1,
        .primary_only = 0,
        .max_parallel = 4},
       {.name = "alpha",
        .provider = "local",
        .model = "org/model:v1",
        .enabled = 0,
        .delegate_available = 0,
        .primary_only = 1,
        .max_parallel = 0},
   };
   char wire[4096];
   assert(server_mgmt_read_project("server-a", 42, agents, 2, wire, sizeof(wire)) > 0);
   const char *alpha = strstr(wire, "\"name\":\"alpha\"");
   const char *zeta = strstr(wire, "\"name\":\"zeta\"");
   assert(alpha && zeta && alpha < zeta);
   assert(!strstr(wire, "endpoint") && !strstr(wire, "api_key") && !strstr(wire, "roles"));
   agents[1] = agents[0];
   assert(server_mgmt_read_project("server-a", 42, agents, 2, wire, sizeof(wire)) < 0);
   snprintf(agents[1].name, sizeof(agents[1].name), "%s", "bad/name");
   assert(server_mgmt_read_project("server-a", 42, agents, 2, wire, sizeof(wire)) < 0);
   assert(server_mgmt_read_project("server-a", 42, agents, 17, wire, sizeof(wire)) < 0);
   puts("server management read tests passed");
   return 0;
}
