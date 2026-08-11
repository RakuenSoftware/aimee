package delegates

import "testing"

// The same fixtures the C parser is pinned on, so the ported rule is checked
// against the behaviour it replaces rather than against my reading of it.
func TestParseCreatedEpochMatchesTheKnownFixtures(t *testing.T) {
	cases := []struct {
		in   string
		want int64
	}{
		{"1970-01-01 00:00:00 +0000 UTC", 0},
		{"2000-01-01 00:00:00 +0000 UTC", 946684800},
		{"2026-07-15 12:34:56 +0000 UTC", 1784118896},
		// A missing offset still parses, and is read as UTC.
		{"2026-07-15 12:34:56", 1784118896},
		// A named zone after the numeric offset is ignored; the NUMBER is what
		// counts, and it is what docker actually varies.
		{"2026-07-15 12:34:56 +0200 CEST", 1784118896 - 2*3600},
		// The offset is what makes it UTC: the same wall clock an hour east is
		// an hour earlier in absolute terms.
		{"2026-07-15 12:34:56 +0100 UTC", 1784118896 - 3600},
		{"2026-07-15 12:34:56 -0500 UTC", 1784118896 + 5*3600},
		// A leap day, which the civil-date arithmetic has to get right.
		{"2024-02-29 00:00:00 +0000 UTC", 1709164800},
	}
	for _, c := range cases {
		got, ok := ParseCreatedEpoch(c.in)
		if !ok {
			t.Errorf("%q did not parse", c.in)
			continue
		}
		if got != c.want {
			t.Errorf("%q = %d, want %d (off by %d)", c.in, got, c.want, got-c.want)
		}
	}
}

func TestParseCreatedEpochRefusesGarbage(t *testing.T) {
	for _, in := range []string{
		"", "   ", "not a date", "2026-07-15", "2026-13-01 00:00:00 +0000 UTC",
		"2026-07-32 00:00:00 +0000 UTC", "2026-07-15 25:00:00 +0000 UTC",
		"xxxx-07-15 12:34:56 +0000 UTC",
	} {
		if _, ok := ParseCreatedEpoch(in); ok {
			t.Errorf("%q should not parse", in)
		}
	}
}

const day = int64(86400)

// The four verdicts, in the order the policy applies them.
func TestJudgeSandboxImagesAppliesThePolicyInOrder(t *testing.T) {
	now := int64(1784118896)
	stamp := func(offsetDays int64) string {
		// Build a timestamp `offsetDays` before `now` by going through the
		// parser's own format.
		switch offsetDays {
		case 0:
			return "2026-07-15 12:34:56 +0000 UTC"
		case 1:
			return "2026-07-14 12:34:56 +0000 UTC"
		case 2:
			return "2026-07-13 12:34:56 +0000 UTC"
		case 30:
			return "2026-06-15 12:34:56 +0000 UTC"
		}
		t.Fatalf("no fixture for %d days", offsetDays)
		return ""
	}

	images := []SandboxImage{
		{Tag: "newest", CreatedAt: stamp(0)},
		{Tag: "second", CreatedAt: stamp(1)},
		{Tag: "old", CreatedAt: stamp(30)},
		{Tag: "old-but-used", CreatedAt: stamp(30), InUse: true},
	}
	// keep the 2 most recent; anything older than 7 days may go
	got := JudgeSandboxImages(images, now, 2, 7*day)

	if len(got) != len(images) {
		t.Fatalf("got %d verdicts for %d images", len(got), len(images))
	}
	want := map[string]struct {
		remove bool
		reason string
	}{
		"newest":       {false, "kept-recent"},
		"second":       {false, "kept-recent"},
		"old":          {true, "aged-out"},
		"old-but-used": {false, "in-use"},
	}
	for i, v := range got {
		if v.Tag != images[i].Tag {
			t.Errorf("verdict %d is for %q, want %q -- order must match the input", i, v.Tag,
				images[i].Tag)
		}
		w := want[v.Tag]
		if v.Remove != w.remove || v.Reason != w.reason {
			t.Errorf("%s: remove=%v reason=%q, want remove=%v reason=%q", v.Tag, v.Remove,
				v.Reason, w.remove, w.reason)
		}
	}
}

// in-use beats every other consideration, including being ancient and outside
// the keep window: deleting an image a container references breaks that
// container, and age is no reason to.
func TestInUseAlwaysWins(t *testing.T) {
	now := int64(1784118896)
	images := []SandboxImage{
		{Tag: "a", CreatedAt: "2020-01-01 00:00:00 +0000 UTC", InUse: true},
	}
	got := JudgeSandboxImages(images, now, 0, 1)
	if got[0].Remove || got[0].Reason != "in-use" {
		t.Errorf("remove=%v reason=%q, want kept as in-use", got[0].Remove, got[0].Reason)
	}
}

// Recency protection is POSITIONAL, so it has to follow the timestamps and not
// the order the caller happened to collect them in.
func TestKeepMinFollowsRecencyNotInputOrder(t *testing.T) {
	now := int64(1784118896)
	images := []SandboxImage{
		{Tag: "oldest", CreatedAt: "2026-06-15 12:34:56 +0000 UTC"},
		{Tag: "newest", CreatedAt: "2026-07-15 12:34:56 +0000 UTC"},
	}
	got := JudgeSandboxImages(images, now, 1, 1) // keep 1, age out everything else

	if got[1].Tag != "newest" || got[1].Remove {
		t.Errorf("the newest image was not protected: %+v", got[1])
	}
	if got[0].Tag != "oldest" || !got[0].Remove {
		t.Errorf("the oldest image was protected instead: %+v", got[0])
	}
}

// A timestamp that cannot be read sorts last, so keep_min does not cover it and
// it ages out. Stated as a test because it is a deletion, and a deletion driven
// by unreadable input deserves to be deliberate rather than incidental.
func TestUnreadableTimestampAgesOut(t *testing.T) {
	now := int64(1784118896)
	images := []SandboxImage{
		{Tag: "unreadable", CreatedAt: "who knows"},
		{Tag: "recent", CreatedAt: "2026-07-15 12:34:56 +0000 UTC"},
	}
	got := JudgeSandboxImages(images, now, 1, 7*day)
	if !got[0].Remove || got[0].Reason != "aged-out" {
		t.Errorf("unreadable: %+v, want removed as aged-out", got[0])
	}
	if got[1].Remove {
		t.Errorf("the readable recent image was removed: %+v", got[1])
	}
}

// A young image is kept even when keep_min is zero: max_age is the other half
// of the policy, not a tiebreak.
func TestWithinMaxAgeIsKept(t *testing.T) {
	now := int64(1784118896)
	images := []SandboxImage{{Tag: "young", CreatedAt: "2026-07-15 12:00:00 +0000 UTC"}}
	got := JudgeSandboxImages(images, now, 0, 7*day)
	if got[0].Remove || got[0].Reason != "within-max-age" {
		t.Errorf("%+v, want kept as within-max-age", got[0])
	}
}

func TestNegativeBoundsAreClamped(t *testing.T) {
	now := int64(1784118896)
	images := []SandboxImage{{Tag: "a", CreatedAt: "2020-01-01 00:00:00 +0000 UTC"}}
	if got := JudgeSandboxImages(images, now, -5, -5); !got[0].Remove {
		t.Errorf("%+v, want an old image removed with clamped bounds", got[0])
	}
}

func TestNoImagesIsNoVerdicts(t *testing.T) {
	if got := JudgeSandboxImages(nil, 0, 2, day); len(got) != 0 {
		t.Errorf("got %d verdicts for no images", len(got))
	}
}

// The boundary the C pinned: age EXACTLY equal to max_age is removed, because
// the keep test is strictly less-than. Stated so the edge cannot drift when
// someone reads "older than max_age" and reaches for <=.
func TestAgeExactlyAtMaxAgeIsRemoved(t *testing.T) {
	now := int64(1784118896)
	week := 7 * day
	// created exactly one week before now
	images := []SandboxImage{{Tag: "boundary", CreatedAt: "2026-07-08 12:34:56 +0000 UTC"}}
	got := JudgeSandboxImages(images, now, 0, week)
	if !got[0].Remove || got[0].Reason != "aged-out" {
		t.Errorf("%+v, want removed as aged-out at the boundary", got[0])
	}

	// One second younger is kept.
	images = []SandboxImage{{Tag: "just-inside", CreatedAt: "2026-07-08 12:34:57 +0000 UTC"}}
	got = JudgeSandboxImages(images, now, 0, week)
	if got[0].Remove || got[0].Reason != "within-max-age" {
		t.Errorf("%+v, want kept one second inside the window", got[0])
	}
}
