package families

import "testing"

// URL canonicalisation decides what the web page cache keys on and, through
// web_search_fuse, whether two search hits are one page. It moved here from C
// unchanged in intent, and these pin the behaviours the C entry point had --
// the port suppression in particular was lost in the port and only a C test
// caught it.

func TestCanonicalURLFoldsSpellingsOfOnePage(t *testing.T) {
	same := []struct {
		name string
		a, b string
	}{
		{"a default https port is not a different page", "https://example.com:443/p", "https://example.com/p"},
		{"a default http port is not a different page", "http://example.com:80/p", "http://example.com/p"},
		{"host case is not significant", "https://Example.COM/p", "https://example.com/p"},
		{"scheme case is not significant", "HTTPS://example.com/p", "https://example.com/p"},
		{"a fragment names a place within a page", "https://example.com/p#frag", "https://example.com/p"},
		{"a bare host is the root path", "https://example.com", "https://example.com/"},
		{"a leading zero still reads as the default port", "https://example.com:0443/p", "https://example.com/p"},
	}
	for _, c := range same {
		t.Run(c.name, func(t *testing.T) {
			got, ok := canonicalURL(c.a)
			if !ok {
				t.Fatalf("canonicalURL(%q) refused", c.a)
			}
			want, ok := canonicalURL(c.b)
			if !ok {
				t.Fatalf("canonicalURL(%q) refused", c.b)
			}
			if got != want {
				t.Errorf("canonicalURL(%q) = %q, want %q (from %q)", c.a, got, want, c.b)
			}
		})
	}
}

func TestCanonicalURLKeepsDistinctPagesDistinct(t *testing.T) {
	// Merging two distinct pages silently loses one, which is the worse of the
	// two failures -- so the folding above must not reach any of these.
	distinct := [][2]string{
		{"https://example.com:8443/p", "https://example.com/p"},
		{"https://example.com/p", "https://example.com/q"},
		{"https://example.com/p?a=1", "https://example.com/p"},
		{"https://example.com/p", "http://example.com/p"},
		{"https://a.example.com/p", "https://b.example.com/p"},
	}
	for _, c := range distinct {
		a, aok := canonicalURL(c[0])
		b, bok := canonicalURL(c[1])
		if !aok || !bok {
			t.Fatalf("canonicalURL refused %q or %q", c[0], c[1])
		}
		if a == b {
			t.Errorf("canonicalURL merged distinct pages %q and %q into %q", c[0], c[1], a)
		}
	}
}

func TestCanonicalURLRefusesWhatTheCacheMustNotKeyOn(t *testing.T) {
	// A refusal is not the same as passing the input through: callers read it
	// as "do not cache this page" and skip the store entirely.
	refused := []string{
		"",
		"example.com/p",            // no scheme separator at all
		"ftp://example.com/p",      // not a web page
		"file:///etc/passwd",       // nor this
		"https:///p",               // empty authority
		"https://" + longHost(256), // an authority the C buffer could not hold
	}
	for _, in := range refused {
		if got, ok := canonicalURL(in); ok {
			t.Errorf("canonicalURL(%q) = %q, want refusal", in, got)
		}
	}
}

func longHost(n int) string {
	b := make([]byte, n)
	for i := range b {
		b[i] = 'a'
	}
	return string(b)
}

func TestWebPageCanonicalURLRepliesWithTheRcCell(t *testing.T) {
	// The C server emitted the canonical form AND the entry point's rc, and the
	// C client still reads two cells. A one-cell reply leaves the client
	// reading an absent slot.
	status, cells, err := webPageCanonicalURL(t.Context(), nil, []string{"https://Example.com:443/p#frag"})
	if err != nil {
		t.Fatalf("webPageCanonicalURL: %v", err)
	}
	if status != 0 {
		t.Fatalf("status = %d, want OK", status)
	}
	if len(cells) != 2 {
		t.Fatalf("reply width = %d, want 2", len(cells))
	}
	if cells[0] != "https://example.com/p" {
		t.Errorf("canonical = %q, want %q", cells[0], "https://example.com/p")
	}
	if cells[1] != "0" {
		t.Errorf("rc = %q, want %q", cells[1], "0")
	}

	// A URL that will not canonicalise still replies two wide, carrying the
	// failure in the rc exactly as the C did.
	_, cells, err = webPageCanonicalURL(t.Context(), nil, []string{"ftp://example.com/p"})
	if err != nil {
		t.Fatalf("webPageCanonicalURL: %v", err)
	}
	if len(cells) != 2 || cells[1] != "-1" {
		t.Errorf("refusal reply = %v, want two cells with rc -1", cells)
	}
}
