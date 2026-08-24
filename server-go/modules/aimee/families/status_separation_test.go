package families

import (
	"os"
	"path/filepath"
	"regexp"
	"strings"
	"testing"
)

// No family may name a STORE status.
//
// The store wire and the operation wire are separate contracts with separate
// owners, and their integers currently agree: OK is 0 in both, TooLong and
// LimitExceeded are both 3, Failed is 4 in both. So a family that returned a
// store status as its operation status would compile, look right, and mostly BE
// right -- which is exactly why the day they diverge is the day it silently
// stops being right, with no wrong-looking line anywhere.
//
// The store statuses are agreed with the postgres module and can gain a value
// this module does not have; a status for "at the transaction cap" was proposed
// today and would land there and nowhere else. The operation statuses are the
// catalog's five, shared with 461 C call sites, and cannot gain a sixth without
// those callers.
//
// So a store reply becomes an ERROR and a family decides what operation status
// that error deserves. Nothing converts one enum to the other, and this is what
// keeps it that way.
//
// The peer-messaging module has the sharp version: its 1 is no_peer where this
// module's 1 is MISSING, and its 4 is hop_limit where this module's 4 is FAILED.
// Same integers, unrelated meanings.
func TestNoFamilyNamesAStoreStatus(t *testing.T) {
	entries, err := os.ReadDir(".")
	if err != nil {
		t.Fatalf("read families package: %v", err)
	}

	// store.StoreStatusX, or a bare StoreStatusX if the package is ever dot
	// imported.
	reference := regexp.MustCompile(`\bStoreStatus[A-Za-z]*\b`)
	// The operation statuses, which these files must still be using for the
	// absence above to mean anything. This cannot collide with the pattern
	// above: after `store.` the two diverge on the first character, Status
	// against Store.
	operation := regexp.MustCompile(`\bstore\.Status[A-Za-z]+\b`)

	scanned := 0
	usesOperationStatus := false
	for _, entry := range entries {
		name := entry.Name()
		// PRODUCTION SOURCES ONLY, and skipping just this file was not enough.
		//
		// With every store.Status reference stripped from the real families, this
		// test still passed: a _test.go file in the package mentioned one, which
		// was enough to satisfy the "these files still answer with operation
		// statuses" assertion below. The guard was being kept alive by the tests
		// it is supposed to be independent of.
		//
		// Tests are also the one place a store status may legitimately appear --
		// building a fake store reply needs the store's vocabulary -- so they are
		// out of scope for both halves.
		if entry.IsDir() || !strings.HasSuffix(name, ".go") || strings.HasSuffix(name, "_test.go") {
			continue
		}
		body, err := os.ReadFile(filepath.Join(".", name))
		if err != nil {
			t.Fatalf("read %s: %v", name, err)
		}
		scanned++
		for _, line := range strings.Split(string(body), "\n") {
			if strings.HasPrefix(strings.TrimSpace(line), "//") {
				continue // naming it in a comment is how it gets explained
			}
			if operation.MatchString(line) {
				usesOperationStatus = true
			}
			if reference.MatchString(line) {
				t.Errorf("%s names a store status: %s\n"+
					"    A store reply becomes an error; a family chooses the "+
					"operation status that error deserves. The two enums agree "+
					"today by coincidence and are separately owned.",
					name, strings.TrimSpace(line))
			}
		}
	}

	// Guard the guard, and one line of it is not enough.
	//
	// Reading no files would pass silently, so scanned must be non-zero. But so
	// would a families package that had stopped answering with operation
	// statuses at all -- the absence of StoreStatus proves something only while
	// Status is what these files DO use. Asserting the absence of a thing
	// nothing needs is a guard that has quietly stopped guarding, which is the
	// same shape as a glob that finds no files and reports success.
	//
	// (The peer-messaging session arrived at this from the other side: its
	// equivalent check requires the RIGHT decoder to still be called, not just
	// the wrong one to be absent.)
	if scanned == 0 {
		t.Fatal("scanned no family sources; this test proved nothing")
	}
	if !usesOperationStatus {
		t.Fatal("no family names an operation status, so the absence of a store " +
			"status proves nothing: this test would pass over a package that had " +
			"stopped answering altogether")
	}
}
