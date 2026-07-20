#include "kb_mgmt_endpoint.h"
#include <assert.h>
int main(void)
{
   assert(kb_mgmt_endpoint_validate("https://server.example:9443") == 0);
   assert(kb_mgmt_endpoint_validate("http://server.example") == -1);
   assert(kb_mgmt_endpoint_validate("https://127.0.0.1:9443") == -1);
   assert(kb_mgmt_endpoint_validate("https://169.254.169.254") == -1);
   assert(kb_mgmt_endpoint_validate("https://10.1.2.3") == -1);
   assert(kb_mgmt_endpoint_validate("https://[::1]") == -1);
   return 0;
}
