package memory

import "testing"

func TestClassifyRedirect(t *testing.T) {
	tests := []struct {
		tool, path, verdict, name string
	}{
		{"Read", "/home/u/.claude/projects/p/memory/note.md", redirectAllow, ""},
		{"Write", "/repo/memory/note.md", redirectAllow, ""},
		{"Write", "/home/u/.claude/projects/p/memory/note.txt", redirectAllow, ""},
		{"Write", "/home/u/.claude/projects/p/memory/MEMORY.md", redirectReject, ""},
		{"Edit", "/home/u/.claude/projects/p/memory/note.md", redirectReject, ""},
		{"Write", "/home/u/.claude/projects/p/memory/../note.md", redirectReject, ""},
		{"Write", "/home/u/.claude/projects/p/memory/nested/note.md", redirectStore, "nested/note"},
	}
	for _, test := range tests {
		got := classifyRedirect("claude", test.tool, test.path, "/home/u", "", "")
		if got.Verdict != test.verdict || got.Name != test.name {
			t.Fatalf("%s %s: got %#v", test.tool, test.path, got)
		}
	}
}

func TestBashTargetsMemory(t *testing.T) {
	const path = "/home/u/.claude/projects/p/memory/note.md"
	if !bashTargetsMemory("claude", "printf x > "+path, "/home/u", "", "") {
		t.Fatal("write redirection was not detected")
	}
	if bashTargetsMemory("claude", "cat "+path, "/home/u", "", "") {
		t.Fatal("read was classified as a write")
	}
	if bashTargetsMemory("claude", "printf 'a>b'; cat "+path, "/home/u", "", "") {
		t.Fatal("quoted operator from another command was classified as a write")
	}
}
