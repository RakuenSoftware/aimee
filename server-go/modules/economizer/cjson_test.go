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
	want := `{"a":3,"b":0.1,"c":1e+300,"d":-1e-07,"e":2147483648,` +
		`"f":1.2345678901234568e+18,"g":0.3,"h":-2147483649}`
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

// Trailing bytes are IGNORED, matching cJSON_ParseWithLength, which is called
// without require_null_terminated. Verified against C: it parses
// `{"a":1}trailing` as `{"a":1}`.
func TestCJSONTrailingBytesIgnored(t *testing.T) {
	if got := roundTrip(t, `{"a":1}trailing`); got != `{"a":1}` {
		t.Errorf("got %s, want {\"a\":1}", got)
	}
}

// KNOWN DIVERGENCE, pinned so it cannot drift silently.
//
// cJSON accepts a RAW control byte inside a string and re-emits it escaped:
// C turns {"c":"x<0x01>y"} into {"c":"xy"}. Go's encoding/json rejects
// that input, so ParseJSON returns nil where cJSON would have succeeded, and the
// compress lever then falls back to head+tail where C produced a JSON summary.
func TestCJSONRawControlByteDivergence(t *testing.T) {
	raw := "{\"c\":\"x\x01y\"}"
	if v := ParseJSON(raw); v != nil {
		t.Fatal("encoding/json unexpectedly accepted a raw control byte — the " +
			"documented divergence has closed; update the comment in cjson.go")
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
