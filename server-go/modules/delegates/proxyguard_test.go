package delegates

import (
	"net"
	"testing"
)

func TestProxyPortAllowed(t *testing.T) {
	for _, port := range []int{80, 443} {
		if !ProxyPortAllowed(port) {
			t.Errorf("port %d should be allowed", port)
		}
	}
	// Anything else is a different protocol wearing a proxy request.
	for _, port := range []int{22, 25, 445, 3306, 5432, 6379, 8080, 0, -1, 65535} {
		if ProxyPortAllowed(port) {
			t.Errorf("port %d should be refused", port)
		}
	}
}

func TestProxyHostAllowedExactAndWildcard(t *testing.T) {
	allow := DefaultPackageAllowlist()

	for _, host := range []string{
		"deb.debian.org", "pypi.org", "files.pythonhosted.org",
		"DEB.DEBIAN.ORG",     // DNS is case-insensitive
		"deb.debian.org.",    // a trailing dot is the same name
		"archive.ubuntu.com", // the wildcard covers the suffix itself
		"gb.archive.ubuntu.com",
		"us.archive.ubuntu.com",
	} {
		if !ProxyHostAllowed(host, allow) {
			t.Errorf("%q should be allowed", host)
		}
	}

	for _, host := range []string{
		"", "  ", "evil.com", "pypi.org.evil.com",
		// The wildcard boundary is a label, not a substring.
		"notarchive.ubuntu.com",
		"archive.ubuntu.com.evil.com",
		"debian.org", // the allowlist names deb.debian.org, not the apex
	} {
		if ProxyHostAllowed(host, allow) {
			t.Errorf("%q should be refused", host)
		}
	}
}

// Every range a package proxy has no business reaching. Each is a way to turn
// the sandbox's one permitted channel into a request against the host network.
func TestProxyAddressBlocksNonPublicIPv4(t *testing.T) {
	blocked := []string{
		"0.0.0.0", "10.0.0.1", "100.64.0.1", "127.0.0.1",
		"169.254.169.254", // cloud metadata
		"172.16.0.1", "172.31.255.255", "192.0.0.1", "192.0.2.1",
		"192.168.1.1", "198.18.0.1", "198.51.100.1", "203.0.113.1",
		"224.0.0.1", "240.0.0.1", "255.255.255.255",
	}
	for _, s := range blocked {
		if !ProxyAddressBlocked(net.ParseIP(s)) {
			t.Errorf("%s should be blocked", s)
		}
	}

	for _, s := range []string{"1.1.1.1", "8.8.8.8", "93.184.216.34", "151.101.0.1"} {
		if ProxyAddressBlocked(net.ParseIP(s)) {
			t.Errorf("%s should be allowed", s)
		}
	}
}

// Without this a hostile resolver could map a private v4 address into v6 and
// slip past the v4 checks entirely.
func TestProxyAddressBlocksIPv4EmbeddedInIPv6(t *testing.T) {
	cases := []struct {
		name string
		addr string
	}{
		{"v4-mapped loopback", "::ffff:127.0.0.1"},
		{"v4-mapped private", "::ffff:10.0.0.1"},
		{"v4-mapped metadata", "::ffff:169.254.169.254"},
		{"v4-compatible private", "::192.168.1.1"},
		{"NAT64 metadata", "64:ff9b::169.254.169.254"},
		{"NAT64 private", "64:ff9b::10.0.0.1"},
	}
	for _, c := range cases {
		ip := net.ParseIP(c.addr)
		if ip == nil {
			t.Fatalf("%s: could not parse %q", c.name, c.addr)
		}
		if !ProxyAddressBlocked(ip) {
			t.Errorf("%s (%s) was not blocked", c.name, c.addr)
		}
	}

	// 6to4 carries the v4 address at bytes 2..5: 2002:c0a8:0101:: is
	// 192.168.1.1.
	sixToFour := net.ParseIP("2002:c0a8:101::1")
	if sixToFour == nil {
		t.Fatal("could not parse the 6to4 address")
	}
	if !ProxyAddressBlocked(sixToFour) {
		t.Error("a 6to4 address embedding a private v4 was not blocked")
	}

	// A v4-mapped PUBLIC address must still be reachable.
	if ProxyAddressBlocked(net.ParseIP("::ffff:1.1.1.1")) {
		t.Error("a v4-mapped public address was blocked")
	}
}

func TestProxyAddressBlocksNonPublicIPv6(t *testing.T) {
	for _, s := range []string{
		"::",      // unspecified
		"::1",     // loopback
		"fc00::1", // unique-local
		"fd00::1",
		"fe80::1", // link-local
		"ff02::1", // multicast
	} {
		if !ProxyAddressBlocked(net.ParseIP(s)) {
			t.Errorf("%s should be blocked", s)
		}
	}
	for _, s := range []string{"2606:4700:4700::1111", "2001:4860:4860::8888"} {
		if ProxyAddressBlocked(net.ParseIP(s)) {
			t.Errorf("%s should be allowed", s)
		}
	}
}

// An address that cannot be understood cannot be shown to be public.
func TestProxyAddressFailsClosed(t *testing.T) {
	if !ProxyAddressBlocked(nil) {
		t.Error("a nil address was allowed")
	}
	if !ProxyAddressBlocked(net.IP{1, 2, 3}) {
		t.Error("a malformed address was allowed")
	}
}

// The allowlist is specific hosts, not broad wildcards -- one wildcard, for the
// geographic mirrors.
func TestDefaultAllowlistIsNarrow(t *testing.T) {
	wildcards := 0
	for _, entry := range DefaultPackageAllowlist() {
		if len(entry) > 0 && entry[0] == '*' {
			wildcards++
		}
	}
	if wildcards != 1 {
		t.Errorf("allowlist has %d wildcards, want exactly 1", wildcards)
	}
	// A bare wildcard would allow everything.
	for _, entry := range DefaultPackageAllowlist() {
		if entry == "*" || entry == "*." {
			t.Errorf("allowlist contains a catch-all: %q", entry)
		}
	}
}
