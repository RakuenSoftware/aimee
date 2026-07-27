/* test_web_egress.c -- the one guarded outbound path.
 *
 * The property that matters is not "the deny-list has the right CIDRs" -- it is
 * that SEARCH now goes through the same guard the page reader always had. Before
 * this module, web_search.c called agent_http_get directly and nothing checked
 * where it landed. That was survivable while search returned engine snippets,
 * and stops being survivable the moment search fetches the pages it finds.
 *
 * These tests run the classifier directly. They do not open sockets. */
#include <assert.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

#include "web_egress.h"

/* These tests exercise classification and policy, never the network. The
 * transport is stubbed so a test can never accidentally make a real request --
 * if one of the "must be refused" cases ever stopped being refused, it would
 * reach this and abort loudly rather than quietly fetching something. */
int agent_http_get_pinned(const char *url, const char *pinned_ip, const char *extra_headers,
                          char **response_buf, int timeout_ms)
{
   (void)url;
   (void)pinned_ip;
   (void)extra_headers;
   (void)response_buf;
   (void)timeout_ms;
   fprintf(stderr, "transport reached in a test that should never fetch: %s\n", url);
   abort();
}

static int blocked_v4(const char *ip)
{
   struct sockaddr_in sa;
   memset(&sa, 0, sizeof(sa));
   sa.sin_family = AF_INET;
   assert(inet_pton(AF_INET, ip, &sa.sin_addr) == 1);
   return web_egress_addr_blocked((struct sockaddr *)&sa);
}

static int blocked_v6(const char *ip)
{
   struct sockaddr_in6 sa;
   memset(&sa, 0, sizeof(sa));
   sa.sin6_family = AF_INET6;
   assert(inet_pton(AF_INET6, ip, &sa.sin6_addr) == 1);
   return web_egress_addr_blocked((struct sockaddr *)&sa);
}

/* The destinations that make an unguarded fetch dangerous. */
static void test_blocks_the_dangerous_destinations(void)
{
   /* cloud instance metadata -- the reason a "search" tool pointed at an
    * internal address is a credential-exfiltration primitive */
   assert(blocked_v4("169.254.169.254"));
   assert(blocked_v4("169.254.0.1"));
   /* loopback: the aimee server's own admin surfaces */
   assert(blocked_v4("127.0.0.1"));
   assert(blocked_v4("127.1.2.3"));
   assert(blocked_v6("::1"));
   /* RFC1918 */
   assert(blocked_v4("10.0.0.1"));
   assert(blocked_v4("172.16.0.1"));
   assert(blocked_v4("172.31.255.254"));
   assert(blocked_v4("192.168.1.254"));
   /* carrier NAT, reserved, broadcast */
   assert(blocked_v4("100.64.0.1"));
   assert(blocked_v4("0.0.0.0"));
   assert(blocked_v4("255.255.255.255"));
   /* v6 unique-local and link-local */
   assert(blocked_v6("fc00::1"));
   assert(blocked_v6("fd12:3456::1"));
   assert(blocked_v6("fe80::1"));
   printf("  PASS: private, loopback, link-local and metadata addresses are blocked\n");
}

/* An IPv4-mapped v6 address must be judged on the embedded v4, or the whole
 * deny-list is bypassable by asking for ::ffff:169.254.169.254. */
static void test_v4_mapped_is_judged_on_the_v4(void)
{
   assert(blocked_v6("::ffff:169.254.169.254"));
   assert(blocked_v6("::ffff:127.0.0.1"));
   assert(blocked_v6("::ffff:10.0.0.1"));
   assert(!blocked_v6("::ffff:93.184.216.34")); /* a public v4, mapped */
   printf("  PASS: IPv4-mapped v6 is judged on the embedded v4\n");
}

/* Over-blocking would make the tool useless; public addresses must pass. */
static void test_public_addresses_pass(void)
{
   assert(!blocked_v4("93.184.216.34"));
   assert(!blocked_v4("1.1.1.1"));
   assert(!blocked_v4("8.8.8.8"));
   assert(!blocked_v6("2606:4700:4700::1111"));
   printf("  PASS: public addresses are permitted\n");
}

/* Permitting a private configured endpoint is a DEPLOY-time decision. It must
 * not be reachable from the config surface, which an admin-capable session can
 * reach from inside the running system. */
static void test_private_endpoint_opt_in_is_env_only(void)
{
   unsetenv("AIMEE_SEARCH_ALLOW_PRIVATE_ENDPOINT");
   assert(!web_egress_private_endpoint_allowed());

   setenv("AIMEE_SEARCH_ALLOW_PRIVATE_ENDPOINT", "0", 1);
   assert(!web_egress_private_endpoint_allowed());
   setenv("AIMEE_SEARCH_ALLOW_PRIVATE_ENDPOINT", "true", 1);
   assert(!web_egress_private_endpoint_allowed()); /* exact "1" only */
   setenv("AIMEE_SEARCH_ALLOW_PRIVATE_ENDPOINT", "1 ", 1);
   assert(!web_egress_private_endpoint_allowed()); /* no trimming */
   setenv("AIMEE_SEARCH_ALLOW_PRIVATE_ENDPOINT", "11", 1);
   assert(!web_egress_private_endpoint_allowed());

   setenv("AIMEE_SEARCH_ALLOW_PRIVATE_ENDPOINT", "1", 1);
   assert(web_egress_private_endpoint_allowed());
   unsetenv("AIMEE_SEARCH_ALLOW_PRIVATE_ENDPOINT");
   printf("  PASS: private-endpoint opt-in defaults off and matches exactly \"1\"\n");
}

/* A malformed or non-http URL must be refused before any resolution happens. */
static void test_rejects_non_http_and_malformed(void)
{
   const char *err = NULL;
   assert(web_egress_fetch("file:///etc/passwd", WEB_EGRESS_UNTRUSTED, NULL, 1000, 0, &err) ==
          NULL);
   assert(err != NULL);
   err = NULL;
   assert(web_egress_fetch("gopher://example.com/", WEB_EGRESS_UNTRUSTED, NULL, 1000, 0, &err) ==
          NULL);
   assert(err != NULL);
   err = NULL;
   assert(web_egress_fetch("", WEB_EGRESS_UNTRUSTED, NULL, 1000, 0, &err) == NULL);
   assert(err != NULL);
   printf("  PASS: non-http schemes and malformed URLs are refused\n");
}

/* An untrusted destination that resolves privately must be refused even when
 * the deployment has opted in for CONFIGURED endpoints -- the opt-in is scoped
 * to the operator's own search backend, not to arbitrary fetched URLs. */
static void test_opt_in_does_not_leak_to_untrusted(void)
{
   setenv("AIMEE_SEARCH_ALLOW_PRIVATE_ENDPOINT", "1", 1);
   const char *err = NULL;
   char *r = web_egress_fetch("http://127.0.0.1/", WEB_EGRESS_UNTRUSTED, NULL, 1000, 0, &err);
   assert(r == NULL);
   assert(err != NULL);
   unsetenv("AIMEE_SEARCH_ALLOW_PRIVATE_ENDPOINT");
   printf("  PASS: the configured-endpoint opt-in does not widen untrusted fetches\n");
}

int main(void)
{
   test_blocks_the_dangerous_destinations();
   test_v4_mapped_is_judged_on_the_v4();
   test_public_addresses_pass();
   test_private_endpoint_opt_in_is_env_only();
   test_rejects_non_http_and_malformed();
   test_opt_in_does_not_leak_to_untrusted();
   printf("web_egress: all tests passed\n");
   return 0;
}
