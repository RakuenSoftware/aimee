#ifndef SERVER_HTTP_MGMT_READ_ROUTES_H
#define SERVER_HTTP_MGMT_READ_ROUTES_H

#include "server_mgmt_read_endpoint.h"

int server_http_mgmt_read_agents(char *resp, int cap);
int server_http_mgmt_read_error(server_mgmt_read_result_t result, char *resp, int cap);

#endif
