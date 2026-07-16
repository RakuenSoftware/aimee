#include <arpa/inet.h>
#include <assert.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>

#include "sandbox_pkg_proxy.h"

static int blocked4(const char *ip)
{
   struct sockaddr_in s;
   memset(&s, 0, sizeof(s));
   s.sin_family = AF_INET;
   assert(inet_pton(AF_INET, ip, &s.sin_addr) == 1);
   return sandbox_pkg_ip_is_blocked((struct sockaddr *)&s);
}

static int blocked6(const char *ip)
{
   struct sockaddr_in6 s;
   memset(&s, 0, sizeof(s));
   s.sin6_family = AF_INET6;
   assert(inet_pton(AF_INET6, ip, &s.sin6_addr) == 1);
   return sandbox_pkg_ip_is_blocked((struct sockaddr *)&s);
}

static void test_ssrf_ipv4(void)
{
   /* blocked ranges */
   assert(blocked4("127.0.0.1"));
   assert(blocked4("127.255.255.255"));
   assert(blocked4("0.0.0.0"));
   assert(blocked4("10.0.0.1"));
   assert(blocked4("10.255.255.255"));
   assert(blocked4("172.16.0.1"));
   assert(blocked4("172.31.255.255"));
   assert(blocked4("192.168.1.1"));
   assert(blocked4("169.254.1.1"));
   assert(blocked4("169.254.169.254")); /* cloud metadata — the classic SSRF target */
   assert(blocked4("100.64.0.1"));      /* CGNAT */
   assert(blocked4("100.127.255.255"));
   assert(blocked4("224.0.0.1")); /* multicast */
   assert(blocked4("239.255.255.255"));
   assert(blocked4("240.0.0.1"));       /* reserved */
   assert(blocked4("255.255.255.255")); /* broadcast */
   assert(blocked4("192.0.0.1"));       /* IETF protocol assignments */
   assert(blocked4("198.18.0.1"));      /* benchmarking */

   /* public addresses — must NOT be blocked */
   assert(!blocked4("8.8.8.8"));
   assert(!blocked4("1.1.1.1"));
   assert(!blocked4("151.101.0.223")); /* fastly (pypi/npm CDN) */
   assert(!blocked4("140.82.112.3"));  /* github */
   /* boundary just outside 172.16/12 */
   assert(!blocked4("172.15.255.255"));
   assert(!blocked4("172.32.0.0"));
   /* boundary just outside 100.64/10 */
   assert(!blocked4("100.63.255.255"));
   assert(!blocked4("100.128.0.0"));
   printf("  PASS: ssrf ipv4\n");
}

static void test_ssrf_ipv6(void)
{
   assert(blocked6("::1"));     /* loopback */
   assert(blocked6("::"));      /* unspecified */
   assert(blocked6("fe80::1")); /* link-local */
   assert(blocked6("fc00::1")); /* ULA */
   assert(blocked6("fd12:3456::1"));
   assert(blocked6("ff02::1")); /* multicast */
   /* v4-mapped forms inherit the v4 policy */
   assert(blocked6("::ffff:127.0.0.1"));
   assert(blocked6("::ffff:169.254.169.254"));
   assert(blocked6("::ffff:10.0.0.1"));
   assert(!blocked6("::ffff:8.8.8.8")); /* mapped public v4 is fine */
   /* genuine public v6 */
   assert(!blocked6("2606:4700:4700::1111")); /* cloudflare */
   assert(!blocked6("2001:4860:4860::8888")); /* google */

   assert(sandbox_pkg_ip_is_blocked(NULL)); /* fail closed */
   printf("  PASS: ssrf ipv6\n");
}

static void test_ports(void)
{
   assert(sandbox_pkg_port_allowed(80));
   assert(sandbox_pkg_port_allowed(443));
   assert(!sandbox_pkg_port_allowed(22));
   assert(!sandbox_pkg_port_allowed(8080));
   assert(!sandbox_pkg_port_allowed(0));
   assert(!sandbox_pkg_port_allowed(-1));
   printf("  PASS: ports\n");
}

static void test_allowlist(void)
{
   const char *def = sandbox_pkg_default_allowlist();
   assert(sandbox_pkg_host_allowed("registry.npmjs.org", def));
   assert(sandbox_pkg_host_allowed("pypi.org", def));
   assert(sandbox_pkg_host_allowed("files.pythonhosted.org", def));
   assert(sandbox_pkg_host_allowed("deb.debian.org", def));
   assert(sandbox_pkg_host_allowed("security.ubuntu.com", def));
   /* wildcard: suffix itself and any label beneath it */
   assert(sandbox_pkg_host_allowed("archive.ubuntu.com", def));
   assert(sandbox_pkg_host_allowed("us.archive.ubuntu.com", def));
   assert(sandbox_pkg_host_allowed("a.b.archive.ubuntu.com", def));
   /* case-insensitive */
   assert(sandbox_pkg_host_allowed("REGISTRY.NPMJS.ORG", def));

   /* not allowed */
   assert(!sandbox_pkg_host_allowed("evil.com", def));
   assert(!sandbox_pkg_host_allowed("", def));
   assert(!sandbox_pkg_host_allowed("registry.npmjs.org", NULL));
   assert(!sandbox_pkg_host_allowed("registry.npmjs.org", ""));
   /* suffix-confusion attacks must NOT match *.archive.ubuntu.com */
   assert(!sandbox_pkg_host_allowed("archive.ubuntu.com.evil.com", def));
   assert(!sandbox_pkg_host_allowed("notarchive.ubuntu.com", def)); /* no dot boundary */
   assert(!sandbox_pkg_host_allowed("registry.npmjs.org.evil.com", def));

   /* custom list */
   assert(sandbox_pkg_host_allowed("mirror.internal", "mirror.internal, *.corp.example"));
   assert(sandbox_pkg_host_allowed("a.corp.example", "mirror.internal, *.corp.example"));
   assert(!sandbox_pkg_host_allowed("other.example", "mirror.internal, *.corp.example"));
   printf("  PASS: allowlist\n");
}

static void test_classify(void)
{
   char host[256];
   int port;

   assert(sandbox_pkg_classify_request_line("CONNECT registry.npmjs.org:443 HTTP/1.1", host,
                                            sizeof(host), &port) == SBX_REQ_CONNECT);
   assert(strcmp(host, "registry.npmjs.org") == 0 && port == 443);

   /* host is lowercased */
   assert(sandbox_pkg_classify_request_line("CONNECT DEB.Debian.ORG:443 HTTP/1.1", host,
                                            sizeof(host), &port) == SBX_REQ_CONNECT);
   assert(strcmp(host, "deb.debian.org") == 0);

   /* bracketed IPv6 authority: brackets stripped, split at the last colon */
   assert(sandbox_pkg_classify_request_line("CONNECT [2606:4700::1111]:443 HTTP/1.1", host,
                                            sizeof(host), &port) == SBX_REQ_CONNECT);
   assert(strcmp(host, "2606:4700::1111") == 0 && port == 443);

   /* CONNECT without a port is invalid */
   assert(sandbox_pkg_classify_request_line("CONNECT registry.npmjs.org HTTP/1.1", host,
                                            sizeof(host), &port) == SBX_REQ_INVALID);
   /* non-numeric / overflow port */
   assert(sandbox_pkg_classify_request_line("CONNECT h:44x HTTP/1.1", host, sizeof(host), &port) ==
          SBX_REQ_INVALID);
   assert(sandbox_pkg_classify_request_line("CONNECT h:99999 HTTP/1.1", host, sizeof(host),
                                            &port) == SBX_REQ_INVALID);

   /* origin-form belongs to the API stack */
   assert(sandbox_pkg_classify_request_line("GET /v1/agents HTTP/1.1", host, sizeof(host), &port) ==
          SBX_REQ_API);

   /* absolute-form http:// forwarding */
   assert(sandbox_pkg_classify_request_line("GET http://deb.debian.org/x/y HTTP/1.1", host,
                                            sizeof(host), &port) == SBX_REQ_ABSOLUTE);
   assert(strcmp(host, "deb.debian.org") == 0 && port == 80);
   assert(sandbox_pkg_classify_request_line("GET http://mirror:8080/x HTTP/1.1", host, sizeof(host),
                                            &port) == SBX_REQ_ABSOLUTE);
   assert(strcmp(host, "mirror") == 0 && port == 8080);

   /* malformed */
   assert(sandbox_pkg_classify_request_line("garbage", host, sizeof(host), &port) ==
          SBX_REQ_INVALID);
   assert(sandbox_pkg_classify_request_line("GET", host, sizeof(host), &port) == SBX_REQ_INVALID);
   assert(sandbox_pkg_classify_request_line("", host, sizeof(host), &port) == SBX_REQ_INVALID);
   /* https:// absolute-form is not forwarded (clients use CONNECT for TLS) */
   assert(sandbox_pkg_classify_request_line("GET https://x/y HTTP/1.1", host, sizeof(host),
                                            &port) == SBX_REQ_INVALID);
   printf("  PASS: classify\n");
}

int main(void)
{
   printf("sandbox_pkg_proxy: ");
   test_ssrf_ipv4();
   test_ssrf_ipv6();
   test_ports();
   test_allowlist();
   test_classify();
   printf("all tests passed\n");
   return 0;
}
