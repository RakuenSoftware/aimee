package economizer

import (
	"fmt"
	"strings"
	"testing"
)

// DIFFERENTIAL test against the C implementation.
//
// The golden strings below are the VERBATIM stdout of src/compact.c's
// compact_body() for these fixtures, captured by compiling the real C against a
// throwaway harness. They are not hand-written expectations: they are what C
// actually emits.
//
// This matters more than a normal port test. These bodies land inside the folded
// prefix, and the prompt cache keys on exact bytes — so "close enough"
// formatting is a silent cache miss on every turn. The fixtures deliberately
// cover the two places a Go port drifts from C without anyone noticing: "%g"
// float spelling and the depth/size truncation boundaries.

func compactCfg() *CompactConfig {
	return &CompactConfig{Enabled: true, Threshold: 64, HeadBytes: 20, TailBytes: 20}
}

func TestCompactMatchesCNumbers(t *testing.T) {
	raw := `{"a":3.0,"b":0.000001234567,"c":1234567.0,"d":0.5,"e":100000000.0,` +
		`"f":1e300,"g":-0.0000001,"pad":"xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"}`
	want := "[compacted JSON summary]\n" +
		`{"a": 3, "b": 1.23457e-06, "c": 1.23457e+06, "d": 0.5, "e": 1e+08, "f": 1e+300, "g": -1e-07, "pad": "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"} /* 8 keys */` +
		fmt.Sprintf("\n[original: %d bytes]", len(raw))
	if got := CompactBody(raw, "", compactCfg()); got != want {
		t.Errorf("float spelling drifted from C:\n got: %q\nwant: %q", got, want)
	}
}

// C's "%.60s" is a BYTE precision, so the cut lands at 60 bytes.
func TestCompactMatchesCLongString(t *testing.T) {
	raw := `{"s":"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789abcdefghij"}`
	want := "[compacted JSON summary]\n" +
		`{"s": "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ01234567..."}` +
		fmt.Sprintf("\n[original: %d bytes]", len(raw))
	if got := CompactBody(raw, "", compactCfg()); got != want {
		t.Errorf("long-string truncation drifted from C:\n got: %q\nwant: %q", got, want)
	}
}

// Depth limit, the >5-keys annotation, and array rendering together.
func TestCompactMatchesCNested(t *testing.T) {
	raw := `{"k1":{"k2":{"k3":{"k4":1}}},"a":[1,2,3],"b":[],"c":true,` +
		`"d":null,"e":"s","f":2,"g":3}`
	want := "[compacted JSON summary]\n" +
		`{"k1": {"k2": {"k3": {...}}}, "a": [/* 3 items */ 1, ...], "b": [], "c": true, "d": null, "e": "s", "f": 2, "g": 3} /* 8 keys */` +
		fmt.Sprintf("\n[original: %d bytes]", len(raw))
	if got := CompactBody(raw, "", compactCfg()); got != want {
		t.Errorf("nested rendering drifted from C:\n got: %q\nwant: %q", got, want)
	}
}

func TestCompactMatchesCPlaintext(t *testing.T) {
	raw := "0123456789012345678901234567890123456789012345678901234567890123456789" +
		"0123456789012345678901234567890123456789"
	want := "01234567890123456789\n[... 70 bytes omitted ...]\n01234567890123456789"
	if got := CompactBody(raw, "", compactCfg()); got != want {
		t.Errorf("head+tail drifted from C:\n got: %q\nwant: %q", got, want)
	}
}

func TestCompactPassThroughBelowThreshold(t *testing.T) {
	raw := "tiny body"
	if got := CompactBody(raw, "", compactCfg()); got != raw {
		t.Errorf("below threshold must pass through: %q", got)
	}
}

func TestCompactDisabledPassesThrough(t *testing.T) {
	raw := "0123456789012345678901234567890123456789012345678901234567890123456789" +
		"0123456789"
	if got := CompactBody(raw, "", &CompactConfig{Enabled: false}); got != raw {
		t.Errorf("disabled must pass through: %q", got)
	}
}

// Behaviours carried from the C contract that the golden fixtures do not reach.

func TestCompactEmptyBody(t *testing.T) {
	if got := CompactBody("", "", compactCfg()); got != "" {
		t.Errorf("empty body must stay empty, got %q", got)
	}
}

func TestCompactThresholdFloor(t *testing.T) {
	// A threshold below 64 is floored at 64: never compact below 64 bytes.
	raw := strings.Repeat("x", 50)
	if got := CompactBody(raw, "", &CompactConfig{Enabled: true, Threshold: 1}); got != raw {
		t.Errorf("threshold floor not applied: %q", got)
	}
}

func TestCompactPerToolDisable(t *testing.T) {
	raw := strings.Repeat("y", 500)
	// Head/tail must be set small: with the 512/1024 defaults, head+tail exceeds
	// a 500-byte body and the plaintext strategy passes it through unchanged, so
	// the override would look like it applied when nothing had shrunk at all.
	cfg := &CompactConfig{
		Enabled:   true,
		Threshold: 64,
		HeadBytes: 20,
		TailBytes: 20,
		PerTool:   []PerToolCompact{{Tool: "read_file", Threshold: -1}},
	}
	if got := CompactBody(raw, "read_file", cfg); got != raw {
		t.Error("per-tool -1 must disable compaction for that tool")
	}
	// A different tool still compacts.
	if got := CompactBody(raw, "other_tool", cfg); got == raw {
		t.Error("per-tool override must not leak to other tools")
	}
	// The lazy seam passes no tool name and must not consult per-tool rules.
	if got := CompactBody(raw, "", cfg); got == raw {
		t.Error("empty tool name must skip per-tool overrides")
	}
}

// Invalid JSON falls back to head+tail rather than failing.
func TestCompactInvalidJSONFallsBack(t *testing.T) {
	raw := "{not valid json at all " + strings.Repeat("z", 200)
	got := CompactBody(raw, "", compactCfg())
	if !strings.Contains(got, "bytes omitted") {
		t.Errorf("invalid JSON should fall back to head+tail, got %q", got)
	}
}
