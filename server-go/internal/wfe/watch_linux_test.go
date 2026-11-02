package wfe

import (
	"context"
	"os"
	"path/filepath"
	"sync/atomic"
	"testing"
	"time"
)

func TestWatchDefinitionsReportsChangesAndStopsWithItsContext(t *testing.T) {
	dir := t.TempDir()
	var changes atomic.Int64
	ctx, cancel := context.WithCancel(context.Background())
	done := make(chan error, 1)
	go func() { done <- WatchDefinitions(ctx, dir, func() { changes.Add(1) }) }()
	time.Sleep(50 * time.Millisecond) // let the watch be installed

	time.Sleep(100 * time.Millisecond)
	if got := changes.Load(); got != 0 {
		t.Fatalf("reported %d changes with nothing changed", got)
	}
	// An editor's save: a temporary file renamed into place.
	tmp := filepath.Join(dir, ".flow.yaml.tmp")
	if err := os.WriteFile(tmp, []byte("name: flow\n"), 0o600); err != nil {
		t.Fatal(err)
	}
	if err := os.Rename(tmp, filepath.Join(dir, "flow.yaml")); err != nil {
		t.Fatal(err)
	}
	deadline := time.Now().Add(5 * time.Second)
	for changes.Load() == 0 && time.Now().Before(deadline) {
		time.Sleep(5 * time.Millisecond)
	}
	if changes.Load() == 0 {
		t.Fatal("a file renamed into the directory was not reported")
	}
	cancel()
	select {
	case err := <-done:
		if err != nil {
			t.Fatalf("watch ended with %v", err)
		}
	case <-time.After(2 * time.Second):
		t.Fatal("watch did not stop when its context ended")
	}
}

func TestWatchDefinitionsRefusesAMissingDirectory(t *testing.T) {
	if err := WatchDefinitions(context.Background(), filepath.Join(t.TempDir(), "absent"), func() {}); err == nil {
		t.Fatal("watching a directory that does not exist must fail")
	}
}
