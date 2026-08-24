package families

import (
	"strings"
	"testing"
	"unicode/utf8"
)

// A cache key is bounded in bytes but stored in a PostgreSQL TEXT column, which
// refuses invalid UTF-8. Cutting the string at a byte offset satisfied the
// bound and produced a value the database would not accept, so a long URL with
// a multi-byte character across the boundary failed to cache at all -- with an
// encoding error nothing upstream would connect to the length of a URL.

func TestTruncationNeverSplitsARune(t *testing.T) {
	// Every rune width, at every offset the rune can straddle. A single width
	// would pass by luck: with a one-byte prefix a 2- or 3-byte character lands
	// evenly on 2047 and only the 4-byte one splits.
	for _, ch := range []string{"é", "✓", "😀"} {
		for prefix := 0; prefix < 4; prefix++ {
			in := strings.Repeat("/", prefix) + strings.Repeat(ch, 2000)
			got := truncateBytesAtRune(in, 2047)

			if !utf8.ValidString(got) {
				t.Errorf("%q prefix=%d: truncation produced invalid UTF-8, trailing bytes % x",
					ch, prefix, got[max(0, len(got)-4):])
			}
			if len(got) > 2047 {
				t.Errorf("%q prefix=%d: %d bytes exceeds the 2047 bound", ch, prefix, len(got))
			}
			// The back-off may drop at most the straddling rune, never more.
			if 2047-len(got) > 3 {
				t.Errorf("%q prefix=%d: backed off %d bytes, at most 3 is correct",
					ch, prefix, 2047-len(got))
			}
			if !strings.HasPrefix(in, got) {
				t.Errorf("%q prefix=%d: result is not a prefix of the input", ch, prefix)
			}
		}
	}
}

// A value already within the bound is returned untouched -- the back-off must
// not shorten something that never needed cutting.
func TestTruncationLeavesAShortValueAlone(t *testing.T) {
	for _, in := range []string{"", "/", "/path?q=1", "/😀", strings.Repeat("a", 2047)} {
		if got := truncateBytesAtRune(in, 2047); got != in {
			t.Errorf("truncateBytesAtRune(%.16q) = %.16q, want it unchanged", in, got)
		}
	}
}

// ASCII is unaffected: the bound is exact when nothing straddles it, so the fix
// costs nothing for the ordinary case.
func TestAnAsciiValueIsCutExactlyAtTheBound(t *testing.T) {
	in := strings.Repeat("a", 3000)
	if got := truncateBytesAtRune(in, 2047); len(got) != 2047 {
		t.Errorf("ASCII truncated to %d bytes, want exactly 2047", len(got))
	}
}
