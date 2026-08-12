package economizer

import "testing"

// Ported from src/tests/test_episode_seal.c.

func TestEpisodeInventoryAndTouch(t *testing.T) {
	var s EpisodeSeal
	s.SetConclusion("concluded: the index rebuild is idempotent")
	s.AddFile("src/db2/code_index.c")
	s.AddFile("src/headers/code_span.h")
	s.AddFile("src/db2/code_index.c") // duplicate
	s.AddFile("")                     // ignored

	if len(s.Files) != 2 {
		t.Fatalf("file count = %d, want 2", len(s.Files))
	}
	if !s.Touches("src/db2/code_index.c") || !s.Touches("src/headers/code_span.h") {
		t.Error("a member file must be recognised")
	}
	if s.Touches("src/unrelated.c") {
		t.Error("a non-member file must not match")
	}
}

func TestEpisodeSerializeParseRoundTrip(t *testing.T) {
	var s EpisodeSeal
	s.SetConclusion("concluded: cache stays warm with freeze")
	s.AddFile("src/context_fold.c")
	s.AddFile("src/coord_closet.c")

	j1, err := s.Serialize()
	if err != nil || j1 == "" {
		t.Fatalf("serialize: %v", err)
	}

	var s2 EpisodeSeal
	if err := ParseEpisodeSeal(&s2, j1); err != nil {
		t.Fatalf("parse: %v", err)
	}
	if s2.Conclusion != "concluded: cache stays warm with freeze" {
		t.Errorf("conclusion = %q", s2.Conclusion)
	}
	if len(s2.Files) != 2 || !s2.Touches("src/context_fold.c") {
		t.Errorf("files did not round-trip: %v", s2.Files)
	}

	j2, err := s2.Serialize()
	if err != nil || j2 != j1 {
		t.Error("round-trip must be deterministic")
	}
}

// A malformed blob must leave the caller's existing seal UNCHANGED — a seal is a
// checkpoint, and half a checkpoint is worse than none.
func TestEpisodeParseAllOrNothing(t *testing.T) {
	for _, bad := range []string{"{bad", "", "[1,2]", `{"files":"not-an-array"}`} {
		s := EpisodeSeal{Conclusion: "original", Files: []string{"keep.c"}}
		if err := ParseEpisodeSeal(&s, bad); err == nil {
			t.Errorf("ParseEpisodeSeal(%q) should have failed", bad)
		}
		if s.Conclusion != "original" || len(s.Files) != 1 || s.Files[0] != "keep.c" {
			t.Errorf("failed parse clobbered the existing seal: %+v", s)
		}
	}
}

// A non-string entry is skipped rather than failing the whole parse: one bad
// element should not cost the checkpoint.
func TestEpisodeParseSkipsNonStringFiles(t *testing.T) {
	var s EpisodeSeal
	if err := ParseEpisodeSeal(&s, `{"conclusion":"c","files":["a.c",7,null,"b.c"]}`); err != nil {
		t.Fatalf("parse: %v", err)
	}
	if len(s.Files) != 2 || !s.Touches("a.c") || !s.Touches("b.c") {
		t.Errorf("files = %v, want [a.c b.c]", s.Files)
	}
}

func TestEpisodeNilSafety(t *testing.T) {
	var nilSeal *EpisodeSeal
	if nilSeal.Touches("x") {
		t.Error("nil seal must not match")
	}
	if _, err := nilSeal.Serialize(); err == nil {
		t.Error("serializing a nil seal should fail")
	}
	nilSeal.AddFile("x")       // must not panic
	nilSeal.SetConclusion("x") // must not panic
}

// Insertion order is preserved, which is what makes serialization deterministic
// and keeps the record readable as a narrative of the work.
func TestEpisodePreservesInsertionOrder(t *testing.T) {
	var s EpisodeSeal
	for _, f := range []string{"z.c", "a.c", "m.c"} {
		s.AddFile(f)
	}
	blob, err := s.Serialize()
	if err != nil {
		t.Fatal(err)
	}
	if blob != `{"conclusion":"","files":["z.c","a.c","m.c"]}` {
		t.Errorf("order not preserved: %s", blob)
	}
}
