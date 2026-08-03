#include <aimee/core/connection/auth.h>
#include <aimee/core/connection/control.h>
#include <aimee/core/connection/endpoint.h>

#include <stdio.h>

int main(void)
{
   aimee_core_endpoint_t endpoint;
   if (aimee_core_endpoint_parse("https://localhost:443/v1", &endpoint) != 0)
      return 1;
   aimee_core_control_t control;
   if (aimee_core_control_init_timeout(&control, 1000, 100, NULL, NULL) != AIMEE_CORE_OK)
      return 1;
   printf("%s:%s\n", endpoint.host, endpoint.port);
   return aimee_core_credential_equal("token", "token") ? 0 : 1;
}
