#include "kb_mgmt_endpoint.h"
#include <arpa/inet.h>
#include <assert.h>
#include <string.h>

static int allowed4(const char *ip)
{
   struct sockaddr_in a = {.sin_family = AF_INET};
   assert(inet_pton(AF_INET, ip, &a.sin_addr) == 1);
   return kb_mgmt_sockaddr_permitted((const struct sockaddr *)&a, sizeof(a));
}

static int allowed6(const char *ip)
{
   struct sockaddr_in6 a = {.sin6_family = AF_INET6};
   assert(inet_pton(AF_INET6, ip, &a.sin6_addr) == 1);
   return kb_mgmt_sockaddr_permitted((const struct sockaddr *)&a, sizeof(a));
}

int main(void)
{
   kb_mgmt_endpoint_t ep;
   assert(kb_mgmt_endpoint_parse("https://server.example:9443", &ep) == 0);
   assert(!strcmp(ep.host, "server.example") && ep.port == 9443 &&
          !strcmp(ep.host_header, "server.example:9443"));
   assert(kb_mgmt_endpoint_validate("https://[2001:4860:4860::8888]:9443") == 0);
   assert(kb_mgmt_endpoint_validate("http://server.example") == -1);
   assert(kb_mgmt_endpoint_validate("https://user@server.example") == -1);
   assert(kb_mgmt_endpoint_validate("https://server.example/path") == -1);
   assert(kb_mgmt_endpoint_validate("https://server.example.") == -1);
   assert(kb_mgmt_endpoint_validate("https://[fe80::1%eth0]") == -1);
   assert(kb_mgmt_endpoint_validate("https://2001:4860::1") == -1);
   assert(kb_mgmt_endpoint_validate("https://-bad.example") == -1);

   assert(allowed4("8.8.8.8"));
   assert(!allowed4("0.0.0.0"));
   assert(!allowed4("10.0.0.1"));
   assert(!allowed4("127.0.0.1"));
   assert(!allowed4("169.254.169.254"));
   assert(!allowed4("172.16.0.1") && !allowed4("172.31.255.255"));
   assert(allowed4("172.32.0.1"));
   assert(!allowed4("192.168.0.1"));
   assert(!allowed4("224.0.0.1"));
   assert(allowed6("2001:4860:4860::8888"));
   assert(!allowed6("::") && !allowed6("::1"));
   assert(!allowed6("fe80::1") && !allowed6("fc00::1") && !allowed6("ff02::1"));
   assert(!allowed6("::ffff:127.0.0.1"));
   assert(allowed6("::ffff:8.8.8.8"));
   /* v4-mapped is only one spelling. NAT64, 6to4, Teredo and the deprecated
    * v4-compatible form all name an IPv4 endpoint without matching a blocked v6
    * prefix, so checking only ::ffff: left an internal endpoint permitted. */
   assert(!allowed6("64:ff9b::7f00:1"));      /* NAT64 -> 127.0.0.1 */
   assert(!allowed6("64:ff9b::a9fe:a9fe"));   /* NAT64 -> 169.254.169.254 */
   assert(!allowed6("64:ff9b:1::a9fe:a9fe")); /* local-use NAT64 prefix */
   assert(!allowed6("2002:7f00:1::"));        /* 6to4 -> 127.0.0.1 */
   assert(!allowed6("2002:a9fe:a9fe::"));     /* 6to4 -> 169.254.169.254 */
   assert(!allowed6("::a9fe:a9fe"));          /* v4-compatible -> metadata */
   assert(!allowed6("2001::5601:5601"));      /* Teredo -> ^169.254.169.254 */
   assert(!allowed6("2001::80ff:fffe"));      /* Teredo -> ^127.0.0.1 */
   /* The same forms carrying a public v4 stay reachable, and 2001:4860::/32 is
    * ordinary global unicast rather than Teredo. */
   assert(allowed6("64:ff9b::808:808"));
   assert(allowed6("2002:808:808::"));
   assert(allowed6("2001::f7f7:f7f7"));
   assert(allowed6("2001:4860:4860::8888"));
   return 0;
}
