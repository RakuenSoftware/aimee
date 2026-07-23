#include "management_read.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
   unsigned char nonce[32];
   for (size_t i = 0; i < sizeof(nonce); ++i)
      nonce[i] = (unsigned char)i;
   server_mgmt_read_digest_input_t in = {.server_id = "server-a",
                                         .team_id = 42,
                                         .nonce = nonce,
                                         .kb_issuer = "/CN=kb-ca",
                                         .kb_serial = "01af",
                                         .server_issuer = "/CN=server-ca",
                                         .server_serial = "10be",
                                         .revocation_generation = 7,
                                         .publication_generation = 9,
                                         .selector = SERVER_MGMT_READ_SELECTOR_AGENTS};
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
   in.selector = SERVER_MGMT_READ_SELECTOR_CONFIG;
   assert(server_mgmt_read_digest(&in, digest) == 0);
   assert(!strcmp(digest, "375cbf75bfed105ee9a44fc7b7f0be02aec715e674e9e6fbd041622a7cc07276"));
   in.selector = 0;
   assert(server_mgmt_read_digest(&in, digest) < 0 && !digest[0]);

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

   server_mgmt_read_config_t config = {.mtls = 2,
                                       .remote_writes = 1,
                                       .client_transport = "http",
                                       .cli_session_forwarding = 1,
                                       .require_aimee_git = 0};
   const char *config_wire =
       "{\"server_id\":\"server-a\",\"team\":42,\"config\":{\"mtls\":\"required\","
       "\"remote_writes\":\"data\",\"client_transport\":\"http\","
       "\"cli_session_forwarding\":true,\"require_aimee_git\":false}}";
   assert(server_mgmt_read_project_config("server-a", 42, &config, wire, sizeof(wire)) > 0);
   assert(!strcmp(wire, config_wire));
   memset(config.client_transport, 0, sizeof(config.client_transport));
   assert(server_mgmt_read_project_config("server-a", 42, &config, wire, sizeof(wire)) > 0);
   assert(strstr(wire, "\"client_transport\":\"socket\""));
   config.client_transport[0] = 'x';
   assert(server_mgmt_read_project_config("server-a", 42, &config, wire, sizeof(wire)) < 0);
   memset(config.client_transport, 'x', sizeof(config.client_transport));
   assert(server_mgmt_read_project_config("server-a", 42, &config, wire, sizeof(wire)) < 0);
   memset(config.client_transport, 0, sizeof(config.client_transport));
   config.mtls = 3;
   assert(server_mgmt_read_project_config("server-a", 42, &config, wire, sizeof(wire)) < 0);
   config.mtls = 1;
   config.cli_session_forwarding = 2;
   assert(server_mgmt_read_project_config("server-a", 42, &config, wire, sizeof(wire)) < 0);
   config.cli_session_forwarding = 1;
   assert(server_mgmt_read_project_config("server-a", 42, &config, wire, 8) < 0 && !wire[0]);
   puts("server management read tests passed");
   return 0;
}
