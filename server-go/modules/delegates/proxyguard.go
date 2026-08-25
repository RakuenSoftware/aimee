package delegates

import (
	"net"
	"strings"
)

// What the delegate's one outward channel is allowed to reach.
//
// The sandbox has no network. The single exception is a proxy to the parent,
// and this is the rule that proxy applies: a narrow allowlist for software
// updates, ports 80 and 443 only, and an address guard that refuses anything
// that is not publicly routable.
//
// This is the only hole in the sandbox, so it is decided here, in one place,
// with no I/O. Resolving a name and opening a socket belong to the caller; what
// may be reached does not.

// DefaultPackageAllowlist is the set of registry hosts a delegate may install
// from. Specific hosts, not broad wildcards -- the one wildcard covers the many
// geographic mirrors that live under archive.ubuntu.com.
func DefaultPackageAllowlist() []string {
	return []string{
		"deb.debian.org",
		"security.debian.org",
		"*.archive.ubuntu.com",
		"security.ubuntu.com",
		"registry.npmjs.org",
		"pypi.org",
		"files.pythonhosted.org",
	}
}

// ProxyPortAllowed permits only the two ports a package fetch uses. Anything
// else is a different protocol wearing a proxy request.
func ProxyPortAllowed(port int) bool {
	return port == 80 || port == 443
}

// ProxyHostAllowed matches a host against the allowlist.
//
// An entry is either an exact host or a "*.suffix" wildcard, and both match
// case-insensitively because DNS does. A wildcard matches the suffix itself as
// well as anything beneath it, so "*.archive.ubuntu.com" covers
// archive.ubuntu.com and gb.archive.ubuntu.com alike -- but never
// notarchive.ubuntu.com, because the boundary is a label, not a substring.
func ProxyHostAllowed(host string, allowlist []string) bool {
	host = strings.TrimSpace(strings.ToLower(host))
	if host == "" {
		return false
	}
	// A trailing dot is the same name in DNS; normalise so it cannot be used to
	// dodge an exact match.
	host = strings.TrimSuffix(host, ".")

	for _, entry := range allowlist {
		entry = strings.TrimSpace(strings.ToLower(entry))
		if entry == "" {
			continue
		}
		if suffix, ok := strings.CutPrefix(entry, "*."); ok {
			if host == suffix || strings.HasSuffix(host, "."+suffix) {
				return true
			}
			continue
		}
		if host == entry {
			return true
		}
	}
	return false
}

// ProxyAddressBlocked reports whether an address must not be connected to.
//
// Everything that is not publicly routable is refused: loopback, private
// ranges, CGNAT, link-local -- which includes the 169.254.169.254 cloud
// metadata endpoint -- multicast, and the reserved and documentation ranges. A
// package proxy has no reason to reach any of them, and each is a way to turn
// the sandbox's one permitted channel into a request against the host's own
// network.
//
// A nil or unparseable address is blocked. Failing closed is the only safe
// direction here: an address that cannot be understood cannot be shown to be
// public.
func ProxyAddressBlocked(ip net.IP) bool {
	if ip == nil {
		return true
	}

	// Any IPv6 form that embeds an IPv4 address inherits the IPv4 policy.
	// Without this a hostile resolver could map a private v4 address into v6 --
	// v4-mapped, v4-compatible, NAT64 or 6to4 -- and slip past the v4 checks
	// entirely.
	if v4 := embeddedIPv4(ip); v4 != nil {
		return ipv4Blocked(v4)
	}
	if v4 := ip.To4(); v4 != nil {
		return ipv4Blocked(v4)
	}

	v6 := ip.To16()
	if v6 == nil {
		return true
	}
	if ip.IsUnspecified() || ip.IsLoopback() {
		return true
	}
	if v6[0]&0xFE == 0xFC {
		return true // fc00::/7 unique-local
	}
	if v6[0] == 0xFE && v6[1]&0xC0 == 0x80 {
		return true // fe80::/10 link-local
	}
	if v6[0] == 0xFF {
		return true // ff00::/8 multicast
	}
	return false
}

// embeddedIPv4 returns the IPv4 address carried inside an IPv6 address, or nil.
//
// Covers v4-mapped (::ffff:a.b.c.d), v4-compatible (::a.b.c.d, deprecated),
// NAT64 (64:ff9b::a.b.c.d) and 6to4 (2002:AABB:CCDD::), each of which is a way
// to name an IPv4 destination without looking like one.
func embeddedIPv4(ip net.IP) net.IP {
	v6 := ip.To16()
	if v6 == nil || ip.To4() != nil {
		return nil
	}

	isPrefix := func(prefix []byte) bool {
		for i, b := range prefix {
			if v6[i] != b {
				return false
			}
		}
		return true
	}

	// v4-mapped ::ffff:0:0/96
	if isPrefix([]byte{0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF}) {
		return net.IPv4(v6[12], v6[13], v6[14], v6[15])
	}
	// NAT64 64:ff9b::/96
	if isPrefix([]byte{0x00, 0x64, 0xFF, 0x9B, 0, 0, 0, 0, 0, 0, 0, 0}) {
		return net.IPv4(v6[12], v6[13], v6[14], v6[15])
	}
	// v4-compatible ::a.b.c.d, excluding :: and ::1 which are handled as v6.
	if isPrefix(make([]byte, 12)) && (v6[12]|v6[13]|v6[14]|v6[15]) != 0 {
		return net.IPv4(v6[12], v6[13], v6[14], v6[15])
	}
	// 6to4 2002::/16 carries the v4 address at bytes 2..5.
	if v6[0] == 0x20 && v6[1] == 0x02 {
		return net.IPv4(v6[2], v6[3], v6[4], v6[5])
	}
	return nil
}

// blockedIPv4Nets are every range a package proxy has no business reaching.
var blockedIPv4Nets = []string{
	"0.0.0.0/8",       // this network / unspecified
	"10.0.0.0/8",      // private
	"100.64.0.0/10",   // carrier-grade NAT
	"127.0.0.0/8",     // loopback
	"169.254.0.0/16",  // link-local, includes 169.254.169.254 cloud metadata
	"172.16.0.0/12",   // private
	"192.0.0.0/24",    // IETF protocol assignments
	"192.0.2.0/24",    // TEST-NET-1
	"192.168.0.0/16",  // private
	"198.18.0.0/15",   // benchmarking
	"198.51.100.0/24", // TEST-NET-2
	"203.0.113.0/24",  // TEST-NET-3
	"224.0.0.0/4",     // multicast
	"240.0.0.0/4",     // reserved, includes 255.255.255.255
}

var blockedIPv4 = func() []*net.IPNet {
	nets := make([]*net.IPNet, 0, len(blockedIPv4Nets))
	for _, cidr := range blockedIPv4Nets {
		if _, n, err := net.ParseCIDR(cidr); err == nil {
			nets = append(nets, n)
		}
	}
	return nets
}()

func ipv4Blocked(ip net.IP) bool {
	v4 := ip.To4()
	if v4 == nil {
		return true
	}
	for _, n := range blockedIPv4 {
		if n.Contains(v4) {
			return true
		}
	}
	return false
}
