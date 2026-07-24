package main

import (
	"os"
	"regexp"
	"testing"
)

// TestACLNoDriftWithC locks the Go allowlist (acl.go) against the C allowlist
// (../src/kb/http/kb_route_acl.c). Both are the containment source of truth; if a
// future slice adds a route to one and forgets the other, this fails. It parses
// the CONSOLE_ADMIN_ACL table out of the C source and compares the (method,
// pattern) set to consoleAdminACL.
func TestACLNoDriftWithC(t *testing.T) {
	const cPath = "../src/kb/http/kb_route_acl.c"
	src, err := os.ReadFile(cPath)
	if err != nil {
		t.Skipf("C ACL source not readable (%v) — skipping drift lock", err)
	}
	// Match entries like: {"GET", "/v1/console/overview"},
	re := regexp.MustCompile(`\{"([A-Z]+)",\s*"(/v1/[^"]+)"\}`)
	matches := re.FindAllStringSubmatch(string(src), -1)
	if len(matches) == 0 {
		t.Fatalf("parsed 0 entries from %s — parser or source changed", cPath)
	}

	cSet := map[string]bool{}
	for _, m := range matches {
		cSet[m[1]+" "+m[2]] = true
	}
	goSet := map[string]bool{}
	for _, e := range consoleAdminACL {
		goSet[e.method+" "+e.pattern] = true
	}

	for k := range cSet {
		if !goSet[k] {
			t.Errorf("C allowlist has %q but Go acl.go does not", k)
		}
	}
	for k := range goSet {
		if !cSet[k] {
			t.Errorf("Go acl.go has %q but C allowlist does not", k)
		}
	}
	if len(cSet) != len(goSet) {
		t.Errorf("entry count drift: C=%d Go=%d", len(cSet), len(goSet))
	}
}
