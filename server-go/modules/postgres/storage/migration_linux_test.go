//go:build linux

package storage

import (
	"context"
	"os"
	"path/filepath"
	"testing"
)

func migrationFixture(t *testing.T) *Volume {
	t.Helper()
	v := fixtureVolume(t)
	if err := v.Prepare(); err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { v.Close() })
	v.Legacy = t.TempDir()
	v.DatabaseUID = os.Geteuid()
	v.DatabaseGID = os.Getegid()
	for _, dir := range []string{v.Mount, filepath.Join(v.Legacy, "pgdata", "global")} {
		if err := os.MkdirAll(dir, 0700); err != nil {
			t.Fatal(err)
		}
	}
	for path, data := range map[string][]byte{"PG_VERSION": []byte("18\n"), "global/pg_control": []byte("offline-fixture\x00\xff🦊")} {
		if err := os.WriteFile(filepath.Join(v.Legacy, "pgdata", path), data, 0600); err != nil {
			t.Fatal(err)
		}
	}
	v.state.Phase = "filesystem"
	return v
}
func TestLegacyCopyVerifiedBeforeAdoptionAndNotReplayed(t *testing.T) {
	v := migrationFixture(t)
	ctx := context.Background()
	original, err := clusterDigest(ctx, filepath.Join(v.Legacy, "pgdata"))
	if err != nil {
		t.Fatal(err)
	}
	if err := v.migrateLegacy(ctx); err != nil {
		t.Fatal(err)
	}
	target := filepath.Join(v.Mount, "pgdata")
	copied, err := clusterDigest(ctx, target)
	if err != nil || copied != original || !v.state.Migrated {
		t.Fatal("copy not verified and committed", err)
	}
	unchanged, _ := clusterDigest(ctx, filepath.Join(v.Legacy, "pgdata"))
	if unchanged != original {
		t.Fatal("original cluster mutated")
	}
	if err := os.WriteFile(filepath.Join(target, "global", "new-data"), []byte("post-migration write"), 0600); err != nil {
		t.Fatal(err)
	}
	if err := v.migrateLegacy(ctx); err != nil {
		t.Fatal("completed migration replayed", err)
	}
}
func TestLegacyMigrationRefusesUnsafeSources(t *testing.T) {
	for _, variant := range []string{"running", "symlink", "old-major", "missing-version", "canceled"} {
		t.Run(variant, func(t *testing.T) {
			v := migrationFixture(t)
			source := filepath.Join(v.Legacy, "pgdata")
			ctx, cancel := context.WithCancel(context.Background())
			defer cancel()
			switch variant {
			case "running":
				if err := os.WriteFile(filepath.Join(source, "postmaster.pid"), []byte("42"), 0600); err != nil {
					t.Fatal(err)
				}
			case "symlink":
				if err := os.Symlink(t.TempDir(), filepath.Join(source, "pg_wal")); err != nil {
					t.Fatal(err)
				}
			case "old-major":
				if err := os.WriteFile(filepath.Join(source, "PG_VERSION"), []byte("17\n"), 0600); err != nil {
					t.Fatal(err)
				}
			case "missing-version":
				if err := os.Remove(filepath.Join(source, "PG_VERSION")); err != nil {
					t.Fatal(err)
				}
			case "canceled":
				cancel()
			}
			if err := v.migrateLegacy(ctx); err == nil {
				t.Fatal("unsafe migration accepted")
			}
			if v.state.Migrated {
				t.Fatal("failed migration committed")
			}
			if _, err := os.Stat(filepath.Join(v.Mount, "pgdata")); !os.IsNotExist(err) {
				t.Fatal("partial database adopted")
			}
		})
	}
}
func TestInterruptedLegacyStagingRebuiltWithoutChangingSource(t *testing.T) {
	v := migrationFixture(t)
	staging := filepath.Join(v.Mount, ".legacy-migration")
	if err := os.Mkdir(staging, 0700); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(staging, "partial"), []byte("incomplete"), 0600); err != nil {
		t.Fatal(err)
	}
	if err := v.migrateLegacy(context.Background()); err != nil {
		t.Fatal(err)
	}
	if _, err := os.Stat(staging); !os.IsNotExist(err) {
		t.Fatal("staging not adopted")
	}
	if _, err := os.Stat(filepath.Join(v.Mount, "pgdata", "partial")); !os.IsNotExist(err) {
		t.Fatal("partial file copied into cluster")
	}
}
func TestExistingDifferentDatabaseNeverOverwritten(t *testing.T) {
	v := migrationFixture(t)
	target := filepath.Join(v.Mount, "pgdata")
	if err := os.Mkdir(target, 0700); err != nil {
		t.Fatal(err)
	}
	path := filepath.Join(target, "PG_VERSION")
	if err := os.WriteFile(path, []byte("existing database"), 0600); err != nil {
		t.Fatal(err)
	}
	if err := v.migrateLegacy(context.Background()); err == nil {
		t.Fatal("existing database replaced")
	}
	data, _ := os.ReadFile(path)
	if string(data) != "existing database" {
		t.Fatal("existing database changed")
	}
}
