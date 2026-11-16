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
