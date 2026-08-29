package egress

import (
	"net"
	"testing"
)

// publicIP is the SSRF gate for every module-initiated call. It must agree with
// the delegate proxy guard and the sandbox proxy policy: a destination refused
// on one plane must not be dialed on another.
func TestPublicIPBlocksNonPublicAndTransitionForms(t *testing.T) {
	blocked := []string{
		// Plain v4 the stdlib predicates alone do not all cover.
		"127.0.0.1", "10.0.0.1", "169.254.169.254", "172.16.0.1", "192.168.1.1",
		"0.0.0.0", "100.64.0.1", "192.0.0.1", "192.0.2.1", "192.88.99.1",
		"198.18.0.1", "198.51.100.1", "203.0.113.1", "224.0.0.1", "255.255.255.255",
		// Native v6.
		"::1", "::", "fe80::1", "fc00::1", "ff02::1",
		// An IPv4 destination spelled as an IPv6 literal.
		"::ffff:169.254.169.254", "::ffff:127.0.0.1", "::169.254.169.254",
		"64:ff9b::a9fe:a9fe", "64:ff9b:1::a9fe:a9fe", "2002:a9fe:a9fe::",
		"2001::5601:5601", // Teredo carrying ^169.254.169.254
		"2001::80ff:fffe", // Teredo carrying ^127.0.0.1
	}
	for _, address := range blocked {
		ip := net.ParseIP(address)
		if ip == nil {
			t.Fatalf("could not parse %q", address)
		}
		if publicIP(ip) {
			t.Errorf("%s must not be treated as public", address)
		}
	}

	public := []string{
		"8.8.8.8", "1.1.1.1", "::ffff:8.8.8.8", "64:ff9b::808:808",
		"2002:808:808::", "2606:4700:4700::1111",
		"2001:4860:4860::8888", // global unicast, not Teredo
		"2001::f7f7:f7f7",      // Teredo carrying public 8.8.8.8
	}
	for _, address := range public {
		ip := net.ParseIP(address)
		if ip == nil {
			t.Fatalf("could not parse %q", address)
		}
		if !publicIP(ip) {
			t.Errorf("%s must remain reachable", address)
		}
	}

	if publicIP(nil) {
		t.Fatal("a missing address must fail closed")
	}
}
