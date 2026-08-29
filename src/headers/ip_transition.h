/* ip_transition.h -- IPv6 transition-address normalization for SSRF guards.
 *
 * WHY THIS EXISTS
 *
 * An SSRF deny-list that reasons about IPv6 prefixes alone is bypassable. Half
 * a dozen transition mechanisms let an IPv4 destination be spelled as an IPv6
 * literal that matches no blocked v6 prefix: ::ffff:169.254.169.254,
 * 64:ff9b::a9fe:a9fe, 2002:a9fe:a9fe::, ::a9fe:a9fe, and the Teredo encoding
 * all name the cloud metadata service. A guard that checks only v4-mapped
 * catches the first and admits the rest.
 *
 * So every guard extracts the embedded IPv4 FIRST and applies the v4 policy to
 * it. This header is the one definition of "which IPv4 does this IPv6 name",
 * shared by the server's web egress guard and aimee-kb's management-endpoint
 * guard so the two cannot drift. The Go planes carry the same table in
 * server-go/modules/delegates/proxyguard.go and
 * server-go/modules/sandbox/proxy_policy.go.
 *
 * Reachability is not the point. Whether a given deployment has a NAT64 gateway
 * or a 6to4 relay is a property of the network, not of the request, and a guard
 * that assumes their absence is asserting something it cannot check. */
#ifndef DEC_IP_TRANSITION_H
#define DEC_IP_TRANSITION_H 1

#include <netinet/in.h>
#include <stdint.h>

/* Extract the IPv4 address an IPv6 address translates or tunnels to.
 *
 * Returns 1 and writes the address to *out_v4 in HOST byte order when `a`
 * carries an embedded IPv4; returns 0 when it names no IPv4 and must be judged
 * as a native IPv6 address.
 *
 * :: and ::1 deliberately return 0: they are the unspecified and loopback
 * addresses, judged by the v6 rules, not IPv4-compatible forms. */
static inline int ip_embedded_ipv4(const struct in6_addr *a, uint32_t *out_v4)
{
   const uint8_t *b = a->s6_addr;
   uint32_t tail =
       ((uint32_t)b[12] << 24) | ((uint32_t)b[13] << 16) | ((uint32_t)b[14] << 8) | (uint32_t)b[15];

   int top12_zero = 1;
   for (int i = 0; i < 12; i++)
      if (b[i])
      {
         top12_zero = 0;
         break;
      }

   /* ::ffff:0:0/96 IPv4-mapped */
   int mapped_prefix = 1;
   for (int i = 0; i < 10; i++)
      if (b[i])
      {
         mapped_prefix = 0;
         break;
      }
   if (mapped_prefix && b[10] == 0xff && b[11] == 0xff)
   {
      *out_v4 = tail;
      return 1;
   }

   /* 64:ff9b::/96 well-known NAT64, and 64:ff9b:1::/48 local-use NAT64. The
    * local-use prefix carries the v4 in the same trailing octets for the /96
    * suffix length, which is the only one aimee emits or accepts. */
   if (b[0] == 0x00 && b[1] == 0x64 && b[2] == 0xff && b[3] == 0x9b)
   {
      int wellknown_zero = 1;
      for (int i = 4; i < 12; i++)
         if (b[i])
         {
            wellknown_zero = 0;
            break;
         }
      int localuse = b[4] == 0x00 && b[5] == 0x01;
      for (int i = 6; i < 12 && localuse; i++)
         if (b[i])
            localuse = 0;
      if (wellknown_zero || localuse)
      {
         *out_v4 = tail;
         return 1;
      }
   }

   /* 2002::/16 6to4 carries the v4 at bytes 2..5. */
   if (b[0] == 0x20 && b[1] == 0x02)
   {
      *out_v4 =
          ((uint32_t)b[2] << 24) | ((uint32_t)b[3] << 16) | ((uint32_t)b[4] << 8) | (uint32_t)b[5];
      return 1;
   }

   /* 2001::/32 Teredo. The client's v4 is the trailing 32 bits, obfuscated by
    * a bitwise complement. */
   if (b[0] == 0x20 && b[1] == 0x01 && b[2] == 0x00 && b[3] == 0x00)
   {
      *out_v4 = ~tail;
      return 1;
   }

   /* ::a.b.c.d IPv4-compatible (deprecated, still routed by some stacks).
    * Excludes :: and ::1, which the v6 rules own. */
   if (top12_zero && tail > 1)
   {
      *out_v4 = tail;
      return 1;
   }

   return 0;
}

#endif /* DEC_IP_TRANSITION_H */
