package main

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
)

// The record is what a replaced container restores from, so what it does NOT
// contain matters as much as what it does.
func TestRenderIdentityRecordKeepsOnlyRestorableAccounts(t *testing.T) {
	got := renderIdentityRecord([]managedIdentity{
		{Username: "admin", Hash: "$6$salt$verifier"},
		// Retired through the wizard: group membership survives, the verifier is
		// locked. Restoring it would recreate an account nobody can sign in as,
		// whose membership then suppresses minting a real one.
		{Username: "retired", Hash: "!$y$locked"},
		{Username: "disabled", Hash: "*"},
		{Username: "nohash", Hash: ""},
		// A colon or newline would forge a second record on restore.
		{Username: "evil:x", Hash: "$6$a$b"},
		{Username: "eviln", Hash: "$6$a$b\nroot:$6$x$y"},
	})
	if got != "admin:$6$salt$verifier\n" {
		t.Fatalf("record should hold only the restorable account; got %q", got)
	}
}

// Sorted output: an unchanged set of accounts must produce an unchanged file, or
// every mutation rewrites it and the diff is noise.
func TestRenderIdentityRecordIsStable(t *testing.T) {
	a := renderIdentityRecord([]managedIdentity{
		{Username: "zoe", Hash: "$6$a$1"}, {Username: "abe", Hash: "$6$a$2"},
	})
	b := renderIdentityRecord([]managedIdentity{
		{Username: "abe", Hash: "$6$a$2"}, {Username: "zoe", Hash: "$6$a$1"},
	})
	if a != b {
		t.Fatalf("order of the input changed the record:\n%q\n%q", a, b)
	}
	if !strings.HasPrefix(a, "abe:") {
		t.Fatalf("expected sorted output, got %q", a)
	}
}

func TestParseIdentityRecordRoundTripsAndRejectsUnusable(t *testing.T) {
	ids := parseIdentityRecord("admin:$6$salt$verifier\nretired:!$y$locked\n\nbroken\n")
	if len(ids) != 1 || ids[0].Username != "admin" || ids[0].Hash != "$6$salt$verifier" {
		t.Fatalf("parse kept the wrong entries: %#v", ids)
	}
}

// The verifier is the same secret /etc/shadow holds; the file must not be
// readable by anyone else, and a half-written one must never be visible.
func TestWriteIdentityRecordIsPrivateAndReplacesAtomically(t *testing.T) {
	home := t.TempDir()
	path := identityRecordPath(home)
	if err := writeIdentityRecord(path, "admin:$6$a$b\n"); err != nil {
		t.Fatalf("write: %v", err)
	}
	st, err := os.Stat(path)
	if err != nil {
		t.Fatalf("stat: %v", err)
	}
	if perm := st.Mode().Perm(); perm != 0o600 {
		t.Fatalf("record must be 0600, got %o", perm)
	}
	if err := writeIdentityRecord(path, "other:$6$c$d\n"); err != nil {
		t.Fatalf("rewrite: %v", err)
	}
	body, err := os.ReadFile(path)
	if err != nil {
		t.Fatalf("read: %v", err)
	}
	if string(body) != "other:$6$c$d\n" {
		t.Fatalf("rewrite did not replace the record: %q", body)
	}
	// The temp file must not survive to be picked up as stray state.
	if _, err := os.Stat(path + ".tmp"); !os.IsNotExist(err) {
		t.Fatalf("temp file left behind")
	}
}

// The end-to-end shape the restore reads: group members in, "user:hash" out,
// sourced from the shadow table rather than from anything the caller supplies.
func TestSnapshotManagedIdentitiesRecordsGroupMembers(t *testing.T) {
	home := t.TempDir()
	shadow := filepath.Join(home, "shadow")
	os.WriteFile(shadow, []byte(
		"root:$6$r$oot:19000:0:99999:7:::\n"+
			"admin:$6$salt$verifier:19000:0:99999:7:::\n"+
			"retired:!$y$locked:19000:0:99999:7:::\n"), 0o600)

	if err := snapshotManagedIdentities(home, []string{"admin", "retired", "ghost"}, shadow); err != nil {
		t.Fatalf("snapshot: %v", err)
	}
	body, err := os.ReadFile(identityRecordPath(home))
	if err != nil {
		t.Fatalf("read: %v", err)
	}
	if string(body) != "admin:$6$salt$verifier\n" {
		t.Fatalf("expected only the restorable member; got %q", body)
	}
	// root is not in the managed group and must never be recorded, however the
	// shadow table is ordered.
	if strings.Contains(string(body), "root") {
		t.Fatalf("recorded an account outside the managed group: %q", body)
	}
}
