package families

import (
	"strings"
	"testing"
)

// The migration history is append-only and the store has recorded it. These pin
// the properties that make that safe, because every one of them fails as a
// refused migration on a running deployment rather than as a test.

func TestTheHistoryCoversEverySchemaFile(t *testing.T) {
	// A schema file that is embedded but not in the history never gets applied,
	// and the tables it declares are missing at runtime with nothing to say why.
	entries, err := schemaFS.ReadDir(".")
	if err != nil {
		t.Fatalf("read embedded schema: %v", err)
	}
	embedded := map[string]bool{}
	for _, e := range entries {
		if !e.IsDir() {
			embedded[e.Name()] = true
		}
	}

	inHistory := map[string]bool{}
	for _, entry := range schemaHistory {
		inHistory[entry.File] = true
		if !embedded[entry.File] {
			t.Errorf("version %d names %s, which is not embedded",
				entry.Version, entry.File)
		}
	}
	for name := range embedded {
		if !inHistory[name] {
			t.Errorf("%s is embedded but has no version: it would never be applied", name)
		}
	}
}

func TestMigrationsAreOrderedAndComplete(t *testing.T) {
	all, err := Migrations()
	if err != nil {
		t.Fatalf("Migrations: %v", err)
	}
	if len(all) != len(schemaHistory) {
		t.Fatalf("got %d migrations, the history has %d", len(all), len(schemaHistory))
	}
	for i, m := range all {
		if m.Version != int64(i+1) {
			t.Errorf("migration %d is version %d; the history has no gaps", i, m.Version)
		}
		if m.Owner != SchemaOwner {
			t.Errorf("version %d has owner %q, want %q", m.Version, m.Owner, SchemaOwner)
		}
		if len(m.Statements) == 0 || strings.TrimSpace(m.Statements[0]) == "" {
			t.Errorf("version %d (%s) carries no statements", m.Version, m.Name)
		}
	}
}

func TestEveryMigrationHasItsOwnChecksum(t *testing.T) {
	// The store refuses a version whose checksum has changed. Two migrations
	// sharing one would let an edit to either go unnoticed against the other's
	// recorded row.
	all, err := Migrations()
	if err != nil {
		t.Fatalf("Migrations: %v", err)
	}
	seen := map[string]string{}
	for _, m := range all {
		if len(m.Checksum) != 64 {
			t.Errorf("version %d has a %d-character checksum, want 64",
				m.Version, len(m.Checksum))
		}
		if prev, dup := seen[m.Checksum]; dup {
			t.Errorf("%s and %s have the same checksum", prev, m.Name)
		}
		seen[m.Checksum] = m.Name
	}
}

func TestTheChecksumFollowsTheContent(t *testing.T) {
	// This is the whole point of recording one: an edited migration must not
	// hash the same as the version the store already applied.
	base := checksum([]string{"CREATE TABLE t (a int);"})
	edited := checksum([]string{"CREATE TABLE t (a bigint);"})
	if base == edited {
		t.Fatal("editing a statement did not change its checksum")
	}
	// And the separator must keep a split from colliding with a join: one
	// statement "AB" is not the same migration as two statements "A" and "B".
	if checksum([]string{"AB"}) == checksum([]string{"A", "B"}) {
		t.Error("a joined statement hashes the same as the split one")
	}
}

func TestPendingSkipsWhatTheStoreHasApplied(t *testing.T) {
	all, err := Migrations()
	if err != nil {
		t.Fatalf("Migrations: %v", err)
	}
	last := all[len(all)-1].Version
	for _, invalid := range []int64{-1, last + 1} {
		if _, err := PendingMigrations(invalid); err == nil {
			t.Errorf("unsupported installed version %d accepted", invalid)
		}
	}

	// The ordinary case on every start after the first: nothing to do.
	pending, err := PendingMigrations(last)
	if err != nil {
		t.Fatalf("PendingMigrations: %v", err)
	}
	if len(pending) != 0 {
		t.Errorf("a current store has %d migrations pending, want 0", len(pending))
	}

	// A fresh database gets the lot, in order.
	pending, err = PendingMigrations(0)
	if err != nil {
		t.Fatalf("PendingMigrations: %v", err)
	}
	if len(pending) != len(all) {
		t.Fatalf("a fresh store has %d pending, want %d", len(pending), len(all))
	}

	// A store part-way through gets only what it is missing.
	pending, err = PendingMigrations(last - 1)
	if err != nil {
		t.Fatalf("PendingMigrations: %v", err)
	}
	if len(pending) != 1 || pending[0].Version != last {
		t.Errorf("a store one behind got %d migrations, want just version %d",
			len(pending), last)
	}
}
