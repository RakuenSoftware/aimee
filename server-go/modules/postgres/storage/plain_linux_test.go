//go:build linux

package storage

import (
	"context"
	"os"
	"path/filepath"
	"testing"
)

func TestPlainStorePersistsAndRefusesImplicitEncryption(t *testing.T) {
	root := t.TempDir()
	ctx := context.Background()
	for i := 0; i < 2; i++ {
		v := &Volume{Root: root}
		if err := v.RunPlain(ctx, []string{"/bin/true"}); err != nil {
			t.Fatal(err)
		}
		canary := filepath.Join(root, "plain", "canary")
		if i == 0 {
			if err := os.WriteFile(canary, []byte("persistent"), 0600); err != nil {
				t.Fatal(err)
			}
		} else if data, err := os.ReadFile(canary); err != nil || string(data) != "persistent" {
			t.Fatal("lost data", err)
		}
	}
	v := fixtureVolume(t)
	v.Root = root
	defer v.Close()
	if err := v.Prepare(); err == nil {
		t.Fatal("LUKS adopted a plain store")
	}
}

func TestPlainStoreRefusesEncryptedAndUnknownData(t *testing.T) {
	for _, name := range []string{"volume.json", "postgres.luks", "PG_VERSION"} {
		t.Run(name, func(t *testing.T) {
			root := t.TempDir()
			path := filepath.Join(root, name)
			if err := os.WriteFile(path, []byte("preserve"), 0600); err != nil {
				t.Fatal(err)
			}
			v := &Volume{Root: root}
			if err := v.RunPlain(context.Background(), []string{"/bin/true"}); err == nil {
				t.Fatal("accepted existing store")
			}
			data, err := os.ReadFile(path)
			if err != nil || string(data) != "preserve" {
				t.Fatal("changed existing store", err)
			}
			if _, err := os.Stat(filepath.Join(root, "plain")); !os.IsNotExist(err) {
				t.Fatal("created replacement database directory")
			}
		})
	}
}

func TestPlainLegacyMigrationPersistsCompletionAcrossRestart(t *testing.T) {
	legacy := migrationFixture(t)
	root := t.TempDir()
	newVolume := func() *Volume {
		return &Volume{Root: root, Legacy: legacy.Legacy, DatabaseUID: os.Geteuid(), DatabaseGID: os.Getegid()}
	}
	if err := newVolume().RunPlain(context.Background(), []string{"/bin/true"}); err != nil {
		t.Fatal(err)
	}
	target := filepath.Join(root, "plain", "pgdata", "global", "pg_control")
	if err := os.WriteFile(target, []byte("new database writes"), 0600); err != nil {
		t.Fatal(err)
	}
	if err := newVolume().RunPlain(context.Background(), []string{"/bin/true"}); err != nil {
		t.Fatal("replayed migration", err)
	}
	data, err := os.ReadFile(target)
	if err != nil || string(data) != "new database writes" {
		t.Fatal("lost new database writes", err)
	}
}
