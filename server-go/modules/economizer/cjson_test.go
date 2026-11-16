package economizer

import "testing"

// DIFFERENTIAL test against real cJSON.
//
// Every `want` below is the VERBATIM stdout of cJSON_PrintUnformatted for that
// input, captured by compiling src/vendor/cJSON.c against a throwaway harness.
// They are not hand-written expectations.
//
// This printer exists because the fold embeds serialized sub-objects into its
// skeleton text and hashes serialized messages for the freeze digest, both of
// which sit in the cache-sensitive folded prefix. A byte of drift here is a cold
// cache on every turn — with every test that checks only CONTENT still green.

func roundTrip(t *testing.T, raw string) string {
	t.Helper()
	v := ParseJSON(raw)
	if v == nil {
		t.Fatalf("ParseJSON(%q) returned nil", raw)
	}
	return PrintJSONUnformatted(v)
}

// The two ways Go's encoding/json diverges: key reordering and HTML escaping.
func TestCJSONPreservesOrderAndDoesNotHTMLEscape(t *testing.T) {
	raw := `{"zeta":1,"alpha":"a<b&c>d","path":"/x/y"}`
	want := `{"zeta":1,"alpha":"a<b&c>d","path":"/x/y"}`
	if got := roundTrip(t, raw); got != want {
		t.Errorf("\n got: %s\nwant: %s", got, want)
	}
}

func TestCJSONNumberSpelling(t *testing.T) {
	raw := `{"a":3.0,"b":0.1,"c":1e300,"d":-0.0000001,"e":2147483648,` +
		`"f":1234567890123456789,"g":0.30000000000000004,"h":-2147483649}`
	want := raw // The vendored native codec now retains original decimal tokens.
	if got := roundTrip(t, raw); got != want {
		t.Errorf("\n got: %s\nwant: %s", got, want)
	}
}

// 0.30000000000000004 -> "0.3" is the case that pins the EPSILON round-trip
// check. With exact equality this would emit all 17 digits.
func TestCJSONNumberEpsilonRoundTrip(t *testing.T) {
	if got := CJSONNumber(0.30000000000000004); got != "0.3" {
		t.Errorf("CJSONNumber(0.30000000000000004) = %q, want \"0.3\"", got)
	}
	// And a value that genuinely needs 17 digits still gets them.
	if got := CJSONNumber(1234567890123456789); got != "1.2345678901234568e+18" {
		t.Errorf("CJSONNumber(1234567890123456789) = %q", got)
	}
}

// The control character is written as the six-character ESCAPE , not a raw
// 0x01 byte — a raw byte is invalid JSON that encoding/json rejects (see
// TestCJSONRawControlByteDivergence).
func TestCJSONEscapes(t *testing.T) {
	raw := "{\"q\":\"he said \\\"hi\\\"\",\"bs\":\"a\\\\b\",\"ctl\":\"x\\u0001y\"," +
		"\"ws\":\"a\\tb\\nc\\r\",\"uni\":\"héllo → 世界\",\"slash\":\"a/b\"}"
	want := "{\"q\":\"he said \\\"hi\\\"\",\"bs\":\"a\\\\b\",\"ctl\":\"x\\u0001y\"," +
		"\"ws\":\"a\\tb\\nc\\r\",\"uni\":\"héllo → 世界\",\"slash\":\"a/b\"}"
	if got := roundTrip(t, raw); got != want {
		t.Errorf("\n got: %s\nwant: %s", got, want)
	}
}

func TestCJSONNestingAndEmpties(t *testing.T) {
	raw := `{"o":{},"a":[],"n":null,"t":true,"f":false,"deep":[{"k":[1,2,{"z":"y"}]}]}`
	want := `{"o":{},"a":[],"n":null,"t":true,"f":false,"deep":[{"k":[1,2,{"z":"y"}]}]}`
	if got := roundTrip(t, raw); got != want {
		t.Errorf("\n got: %s\nwant: %s", got, want)
	}
}

func TestCJSONOddKeys(t *testing.T) {
	raw := `{"":1,"k":2}`
	want := `{"":1,"k":2}`
	if got := roundTrip(t, raw); got != want {
		t.Errorf("\n got: %s\nwant: %s", got, want)
	}
}

// Invalid input parses to nil, mirroring cJSON_ParseWithLength returning NULL.
func TestCJSONParseFailure(t *testing.T) {
	for _, bad := range []string{"", "{", "not json", `{"a":}`} {
		if v := ParseJSON(bad); v != nil {
			t.Errorf("ParseJSON(%q) should be nil, got %+v", bad, v)
		}
	}
}

func TestCJSONCanonicalFraming(t *testing.T) {
	for _, raw := range []string{`{"a":1}trailing`, `{"a":1,"a":2}`, `{"a":{"x":1,"x":2}}`, `{"a":1,"\u0061":2}`, `{"a":"\u0000"}`, `{"\u0000":1}`, "{\"c\":\"x\x01y\"}"} {
		if ParseJSON(raw) != nil {
			t.Errorf("accepted ambiguous/malformed JSON: %q", raw)
		}
	}
}

func TestCJSONOpaqueDecimalsSurviveCloneAndMutation(t *testing.T) {
	raw := `{"input":{"id":9007199254740993,"ratio":0.12345678901234567890123456789,"tiny":1e-9000,"huge":1e9000},"values":[1.000,1E+00]}`
	original := ParseJSON(raw)
	if original == nil || PrintJSONUnformatted(original.Clone()) != raw {
		t.Fatal("opaque decimal changed")
	}
	clone := original.Clone()
	clone.Get("input").Get("id").Num = 7
	if got := clone.Get("input").Get("id").numberText(); got != "7" {
		t.Fatal("stale parsed number survived mutation", got)
	}
	if PrintJSONUnformatted(original) != raw {
		t.Fatal("clone mutated original")
	}
}

// Non-finite doubles print as null, as cJSON does.
func TestCJSONNonFinite(t *testing.T) {
	one := 1.0
	zero := 0.0
	for _, d := range []float64{one / zero, -one / zero} {
		if got := CJSONNumber(d); got != "null" {
			t.Errorf("CJSONNumber(%v) = %q, want \"null\"", d, got)
		}
	}
}
