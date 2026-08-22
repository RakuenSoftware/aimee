package result

import (
	"os"
	"path/filepath"
	"regexp"
	"strconv"
	"testing"
)

// The C header and this package are two spellings of one table. A value that
// agrees in one and not the other is worse than no convention at all: a caller
// on either side of the DB2 boundary would read the same integer as a different
// outcome. This reads the header and pins every constant against it.
func TestMirrorsTheCHeader(t *testing.T) {
	path := filepath.Join("..", "..", "src", "headers", "aimee_result.h")
	source, err := os.ReadFile(path)
	if err != nil {
		t.Fatalf("read %s: %v", path, err)
	}

	// The band constants carry a trailing comment naming their domain, so the
	// value is not the end of its line.
	define := regexp.MustCompile(`(?m)^#define (AIMEE_[A-Z_]+) +(-?\d+)\s*(?:/\*.*)?$`)
	inHeader := map[string]int64{}
	for _, match := range define.FindAllStringSubmatch(string(source), -1) {
		value, err := strconv.ParseInt(match[2], 10, 64)
		if err != nil {
			t.Fatalf("%s is not an integer: %v", match[1], err)
		}
		inHeader[match[1]] = value
	}
	if len(inHeader) == 0 {
		t.Fatal("no constants found; the header's shape must have changed")
	}

	inGo := map[string]Code{
		"AIMEE_PENDING":         Pending,
		"AIMEE_DONE":            Done,
		"AIMEE_DONE_NO_CHANGE":  DoneNoChange,
		"AIMEE_DONE_ALREADY":    DoneAlready,
		"AIMEE_DONE_PARTIAL":    DonePartial,
		"AIMEE_DONE_EMPTY":      DoneEmpty,
		"AIMEE_DONE_REFUSED":    DoneRefused,
		"AIMEE_FAILED":          Failed,
		"AIMEE_INVALID":         Invalid,
		"AIMEE_DENIED":          Denied,
		"AIMEE_UNAUTHENTICATED": Unauthenticated,
		"AIMEE_UNAVAILABLE":     Unavailable,
		"AIMEE_CONFLICT":        Conflict,
		"AIMEE_NOT_FOUND":       NotFound,
		"AIMEE_TOO_LARGE":       TooLarge,
		"AIMEE_TIMEOUT":         Timeout,
		"AIMEE_INTEGRITY":       Integrity,
		"AIMEE_CANCELLED":       Cancelled,
		"AIMEE_BAND_TENANCY":    BandTenancy,
		"AIMEE_BAND_STORAGE":    BandStorage,
		"AIMEE_BAND_ORG":        BandOrg,
		"AIMEE_BAND_CUSTODY":    BandCustody,
		"AIMEE_BAND_CODEINDEX":  BandCodeIndex,
		"AIMEE_BAND_KBDOC":      BandKBDoc,
		"AIMEE_BAND_MEMORY":     BandMemory,
		"AIMEE_BAND_CSS":        BandCSS,
		"AIMEE_BAND_TRANSPORT":  BandTransport,
	}

	for name, want := range inHeader {
		got, ok := inGo[name]
		if !ok {
			t.Errorf("%s is in the header and not in this package", name)
			continue
		}
		if int64(got) != want {
			t.Errorf("%s: header %d, Go %d", name, want, got)
		}
	}
	for name := range inGo {
		if _, ok := inHeader[name]; !ok {
			t.Errorf("%s is in this package and not in the header", name)
		}
	}
}

// The three predicates partition every integer, and the partition is the whole
// convention: an outcome is exactly one of finished-well, finished-badly, or
// not finished.
func TestPredicatesPartition(t *testing.T) {
	for _, code := range []Code{-1000, Integrity, Failed, Pending, Done, DoneRefused, 1000} {
		succeeded, failed, pending := code.Succeeded(), code.Failed(), code.Pending()
		count := 0
		for _, set := range []bool{succeeded, failed, pending} {
			if set {
				count++
			}
		}
		if count != 1 {
			t.Errorf("%d: succeeded=%v failed=%v pending=%v -- not exactly one",
				code, succeeded, failed, pending)
		}
	}
}

// Zero is reserved, so no band and no universal code may collide with it, and
// the two universal ranges may not reach into the bands.
func TestRangesDoNotOverlap(t *testing.T) {
	if Pending != 0 {
		t.Fatalf("Pending must be 0, is %d", Pending)
	}
	for _, code := range []Code{Done, DoneNoChange, DoneAlready, DonePartial, DoneEmpty, DoneRefused} {
		if code < 1 || code > 99 {
			t.Errorf("universal success %d is outside 1..99", code)
		}
	}
	for _, code := range []Code{Failed, Invalid, Denied, Unauthenticated, Unavailable,
		Conflict, NotFound, TooLarge, Timeout, Integrity, Cancelled} {
		if code > -1 || code < -99 {
			t.Errorf("universal failure %d is outside -1..-99", code)
		}
	}
	seen := map[Code]bool{}
	for _, band := range []Code{BandTenancy, BandStorage, BandOrg, BandCustody, BandCodeIndex,
		BandKBDoc, BandMemory, BandCSS, BandTransport} {
		if band < 100 || band > 900 || band%100 != 0 {
			t.Errorf("band %d is not a hundred-aligned value in 100..900", band)
		}
		if seen[band] {
			t.Errorf("band %d is assigned twice", band)
		}
		seen[band] = true
	}
}
