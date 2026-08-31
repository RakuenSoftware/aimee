package economizer

import "testing"

// DIFFERENTIAL test against the retired C primitives.
//
// Every `want` is the verbatim stdout of tc_strip_noise / tc_dedup_lines /
// tc_truncate_with_signal for that input, captured before the C implementation
// was removed.
//
// Line-splitting edge cases (trailing newline, all-blank input, empty input) are
// where a reimplementation silently drifts, so they are pinned explicitly rather
// than assumed.

func TestTCStripNoiseMatchesC(t *testing.T) {
	cases := []struct{ name, in, want string }{
		{
			"basic",
			"\x1b[31mred\x1b[0m text\nprogress 10%\rprogress 100%\ndone",
			"red text\nprogress 100%\ndone",
		},
		{"blank runs collapse to one", "a\n\n\n\nb\n\n\nc", "a\n\nb\n\nc"},
		// A truncated CSI still drops, and here it also eats the 't': after
		// ESC '[' the params run consumes '3' and ' ', then 't' (0x74) falls in
		// the final-byte range 0x40..0x7e and is consumed as the terminator.
		{"malformed CSI", "keep\x1b[3 truncated", "keepruncated"},
		{"trailing newline is not an extra line", "a\nb\n", "a\nb"},
		{"empty", "", ""},
		{"only blanks", "\n\n\n", ""},
	}
	for _, c := range cases {
		if got := TCStripNoise(c.in); got != c.want {
			t.Errorf("%s:\n got: %q\nwant: %q", c.name, got, c.want)
		}
	}
}

func TestTCDedupLinesMatchesC(t *testing.T) {
	cases := []struct{ name, in, want string }{
		{"interior run", "x\nx\nx\ny\nx", "x  (x3)\ny\nx"},
		{"no repeats", "a\nb\nc", "a\nb\nc"},
		{"trailing run", "a\nb\nb", "a\nb  (x2)"},
		{"all identical", "z\nz\nz", "z  (x3)"},
		{"single line", "solo", "solo"},
	}
	for _, c := range cases {
		if got := TCDedupLines(c.in); got != c.want {
			t.Errorf("%s:\n got: %q\nwant: %q", c.name, got, c.want)
		}
	}
}

func TestTCTruncateWithSignalMatchesC(t *testing.T) {
	cases := []struct {
		name, in   string
		head, tail int
		signal     string
		want       string
	}{
		{"head+tail", "1\n2\n3\n4\n5\n6\n7\n8", 2, 2, "", "1\n2\n... 4 lines elided ...\n7\n8"},
		{
			"signal rescues an interior line",
			"1\n2\nERROR here\n4\n5\n6\n7\n8", 1, 1, "ERROR",
			"1\n... 1 lines elided ...\nERROR here\n... 4 lines elided ...\n8",
		},
		{"already fits is verbatim", "1\n2\n3", 2, 2, "", "1\n2\n3"},
		{"zero head and tail", "1\n2\n3\n4", 0, 0, "", "... 4 lines elided ..."},
		{"negative clamps to zero", "1\n2\n3\n4", -5, -5, "", "... 4 lines elided ..."},
		{"signal keeps everything", "E1\nE2\nE3\nE4", 0, 0, "E", "E1\nE2\nE3\nE4"},
	}
	for _, c := range cases {
		if got := TCTruncateWithSignal(c.in, c.head, c.tail, c.signal); got != c.want {
			t.Errorf("%s:\n got: %q\nwant: %q", c.name, got, c.want)
		}
	}
}

// The elision marker is never silent: a dropped run always announces itself,
// because a silent elision reads as "that is all the output there was".
func TestTCTruncateAlwaysMarksElision(t *testing.T) {
	got := TCTruncateWithSignal("a\nb\nc\nd\ne", 1, 1, "")
	if got != "a\n... 3 lines elided ...\ne" {
		t.Errorf("got %q", got)
	}
}
