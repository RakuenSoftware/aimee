package economizer

import (
	"strings"
	"testing"
)

// DIFFERENTIAL test against econ_json_compact.
//
// Each row is the verbatim result/output captured from the retired C
// economizer_json implementation before its removal. The REFUSALS matter as much as
// the successes: this compactor runs over authenticated tool output, so its
// acceptance set is a security surface, not a convenience.
func TestJSONCompactMatchesC(t *testing.T) {
	cases := []struct {
		name string
		in   string
		res  JSONResult
		out  string
	}{
		{"simple", `{ "a" : 1 , "b" : [ 1 , 2 ] }`, JSONOK, `{"a":1,"b":[1,2]}`},
		{"already compact", `{"a":1}`, JSONNotShorter, ""},
		// Whitespace INSIDE a string is content and must survive untouched.
		{"whitespace in string is kept", `{ "a" : "x  y\tz" }`, JSONOK, `{"a":"x  y\tz"}`},
		{"nested", `[ { "k" : { "j" : [ ] } } ]`, JSONOK, `[{"k":{"j":[]}}]`},
		{"empty containers", `[ {} , [] ]`, JSONOK, `[{},[]]`},
		// Numbers are copied VERBATIM, never renormalised: -0 stays -0, 1.5E-3
		// keeps its capital E. Re-encoding would break a digest over the original.
		{"number forms", `[ -0 , 1e10 , 1.5E-3 , 0.0 ]`, JSONOK, `[-0,1e10,1.5E-3,0.0]`},
		{"escapes", `{ "a" : "\"\\\/\b\f\n\r\t" }`, JSONOK, `{"a":"\"\\\/\b\f\n\r\t"}`},
		// Escape SPELLING is preserved too — not decoded and re-emitted.
		{"unicode escapes", `{ "a" : "\u00e9\ud83d\ude00" }`, JSONOK, `{"a":"\u00e9\ud83d\ude00"}`},
		{"literals", `[ true , false , null ]`, JSONOK, `[true,false,null]`},

		// Refusals.
		{"duplicate key", `{"a":1,"a":2}`, JSONDuplicateKey, ""},
		// Names are compared DECODED, so an escaped spelling of the same name
		// still collides. A compactor that missed this would let a document mean
		// two different things to two parsers.
		{"duplicate key via escape", `{"a":1,"\u0061":2}`, JSONDuplicateKey, ""},
		{"trailing garbage", `{"a":1} x`, JSONInvalidSyntax, ""},
		{"trailing comma", `{"a":1,}`, JSONInvalidSyntax, ""},
		{"lone high surrogate", `{"a":"\ud800"}`, JSONInvalidSyntax, ""},
		{"lone low surrogate", `{"a":"\udc00"}`, JSONInvalidSyntax, ""},
		{"bad escape", `{"a":"\x"}`, JSONInvalidSyntax, ""},
		{"raw control byte", "{\"a\":\"\x01\"}", JSONInvalidSyntax, ""},
		{"bad utf8", "{\"a\":\"\xc3\x28\"}", JSONInvalidUTF8, ""},
		{"overlong utf8", "{\"a\":\"\xc0\xaf\"}", JSONInvalidUTF8, ""},
		{"surrogate encoded as utf8", "{\"a\":\"\xed\xa0\x80\"}", JSONInvalidUTF8, ""},
		{"leading zero", `[01]`, JSONInvalidSyntax, ""},
		{"bare dot", `[1.]`, JSONInvalidSyntax, ""},
		{"plus number", `[+1]`, JSONInvalidSyntax, ""},
		{"NaN", `[NaN]`, JSONInvalidSyntax, ""},
		{"single quotes", `{'a':1}`, JSONInvalidSyntax, ""},
		{"unterminated", `{"a":1`, JSONInvalidSyntax, ""},
		{"empty", ``, JSONInvalidSyntax, ""},
		{"only whitespace", `   `, JSONInvalidSyntax, ""},
	}
	for _, c := range cases {
		out, res := JSONCompact([]byte(c.in))
		if res != c.res {
			t.Errorf("%s: result = %v, want %v", c.name, res, c.res)
			continue
		}
		if string(out) != c.out {
			t.Errorf("%s:\n got: %q\nwant: %q", c.name, string(out), c.out)
		}
	}
}

func TestJSONCompactLimits(t *testing.T) {
	if _, res := JSONCompact(make([]byte, JSONMaxInput+1)); res != JSONTooLarge {
		t.Errorf("over-size input = %v, want JSONTooLarge", res)
	}
	// Depth is a refusal boundary, not a truncation point.
	deep := strings.Repeat("[", JSONMaxDepth+2) + strings.Repeat("]", JSONMaxDepth+2)
	if _, res := JSONCompact([]byte(deep)); res != JSONTooDeep {
		t.Errorf("over-deep input = %v, want JSONTooDeep", res)
	}
	// Just inside the limit still parses.
	ok := strings.Repeat("[", 8) + " " + strings.Repeat("]", 8)
	if _, res := JSONCompact([]byte(ok)); res != JSONOK {
		t.Errorf("shallow nesting = %v, want JSONOK", res)
	}
}

func TestJSONResultStrings(t *testing.T) {
	cases := map[JSONResult]string{
		JSONOK: "ok", JSONInvalidArgument: "invalid_argument", JSONTooLarge: "too_large",
		JSONTooDeep: "too_deep", JSONInvalidUTF8: "invalid_utf8",
		JSONInvalidSyntax: "invalid_syntax", JSONDuplicateKey: "duplicate_key",
		JSONNotShorter: "not_shorter",
	}
	for r, want := range cases {
		if got := r.String(); got != want {
			t.Errorf("%d.String() = %q, want %q", r, got, want)
		}
	}
}

// Nothing is emitted for a document that fails validation — the copy pass runs
// only after the whole document is proven well-formed.
func TestJSONCompactEmitsNothingOnFailure(t *testing.T) {
	for _, bad := range []string{`{"a":1,"a":2}`, `{"a":1} x`, `{"a":`} {
		out, res := JSONCompact([]byte(bad))
		if res == JSONOK || out != nil {
			t.Errorf("%q produced output despite failing: %v %q", bad, res, out)
		}
	}
}
