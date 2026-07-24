#include "kb_mgmt_token_authority_ipc.h"

#include <arpa/inet.h>
#include <assert.h>
#include <stdint.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

int kb_mgmt_token_authority_ipc_read_request(int fd, uint32_t timeout_ms, char correlation_id[65],
                                             char jti[65]);
int kb_mgmt_token_authority_ipc_write_response(int fd, uint32_t timeout_ms,
                                               kb_mgmt_token_authority_ipc_result_t status,
                                               const kb_mgmt_token_authority_output_t *out);

static void put16(unsigned char *out, uint16_t value)
{
   value = htons(value);
   memcpy(out, &value, sizeof(value));
}

static void request(int fd, int extra)
{
   unsigned char wire[12 + 64 + 64 + 1];
   memset(wire, 0, sizeof(wire));
   memcpy(wire, "AMTQ", 4);
   wire[4] = 1;
   wire[5] = 1;
   put16(wire + 6, 64);
   put16(wire + 8, 64);
   memset(wire + 12, '1', 64);
   memset(wire + 76, '2', 64);
   assert(write(fd, wire, 140 + extra) == 140 + extra);
   assert(shutdown(fd, SHUT_WR) == 0);
}

int main(void)
{
   int pair[2];
   assert(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
   request(pair[0], 0);
   char correlation[65] = "", jti[65] = "";
   assert(kb_mgmt_token_authority_ipc_read_request(pair[1], 1000, correlation, jti) == 0);
   assert(strlen(correlation) == 64 && correlation[0] == '1');
   assert(strlen(jti) == 64 && jti[0] == '2');
   close(pair[0]);
   close(pair[1]);

   assert(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
   request(pair[0], 1);
   assert(kb_mgmt_token_authority_ipc_read_request(pair[1], 1000, correlation, jti) != 0);
   close(pair[0]);
   close(pair[1]);

   assert(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
   kb_mgmt_token_authority_output_t out;
   memset(&out, 0, sizeof(out));
   strcpy(out.jwt, "a.b.c");
   out.jwt_len = 5;
   assert(kb_mgmt_token_authority_ipc_write_response(pair[0], 1000, KB_MGMT_TOKEN_AUTHORITY_IPC_OK,
                                                     &out) == 0);
   unsigned char response[17];
   assert(recv(pair[1], response, sizeof(response), MSG_WAITALL) == (ssize_t)sizeof(response));
   assert(memcmp(response, "AMTR\1\1\0\0\0\0\0\5", 12) == 0);
   assert(memcmp(response + 12, "a.b.c", 5) == 0);
   close(pair[0]);
   close(pair[1]);
   return 0;
}
