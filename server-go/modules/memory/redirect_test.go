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

// A remote server's own home (/var/lib/aimee in the container) prefixes no client
// path; the client's home comes from the path. CT100 stored no agent memory until
// this held (2026-10-27).
func TestClassifyRedirectOnRemoteServer(t *testing.T) {
	tests := []struct {
		path, verdict, name string
	}{
		{"/home/virant/.claude/projects/-home-virant-dev-aimee/memory/no-local-rpool.md", redirectStore, "no-local-rpool"},
		{"/home/virant/.claude/projects/p/memory/MEMORY.md", redirectReject, ""},
		{"/home/virant/dev/aimee/memory/note.md", redirectAllow, ""},
		{"/home/.hidden/.claude/projects/p/memory/note.md", redirectAllow, ""},
		{"/.claude/projects/p/memory/note.md", redirectAllow, ""},
	}
	for _, test := range tests {
		got := classifyRedirect("claude", "Write", test.path, "/var/lib/aimee", "", "")
		if got.Verdict != test.verdict || got.Name != test.name {
			t.Fatalf("%s: got %#v", test.path, got)
		}
	}
	// The server's own home still works when client and server share one.
	if got := classifyRedirect("claude", "Write", "/var/lib/aimee/.claude/projects/p/memory/n.md", "/var/lib/aimee", "", ""); got.Verdict != redirectStore {
		t.Fatalf("shared home: got %#v", got)
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
