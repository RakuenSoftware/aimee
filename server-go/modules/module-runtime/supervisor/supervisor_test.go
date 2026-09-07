//go:build !windows

package supervisor

import (
	"context"
	"net"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"

	"github.com/JBailes/aimee/server-go/modules/module-runtime/identity"
)

func fixture(t *testing.T, role string) (string, string, string) {
	t.Helper()
	home := t.TempDir()
	if _, err := identity.Ensure(home, role); err != nil {
		t.Fatal(err)
	}
	executable := filepath.Join(home, "worker")
	// A worker records the role and then fails. Its restart must be isolated,
	// and cancellation must stop every worker even during the retry interval.
	if err := os.WriteFile(executable, []byte("#!/bin/sh\nprintf '%s\\n' \"$AIMEE_MODULE_PLACEMENT\" >> \"$AIMEE_HOME/starts\"\nexit 1\n"), 0700); err != nil {
		t.Fatal(err)
	}
	manifest := filepath.Join(home, "modules")
	var rows []string
	for _, id := range []string{role, "config", "postgres", "memory"} {
		rows = append(rows, id+"\t"+executable)
	}
	if err := os.WriteFile(manifest, []byte(strings.Join(rows, "\n")+"\n"), 0600); err != nil {
		t.Fatal(err)
	}
	return home, manifest, executable
}

func TestCompositionValidationBeforeActivation(t *testing.T) {
	for _, role := range []string{"server", "kb"} {
		t.Run(role, func(t *testing.T) {
			home, manifest, exe := fixture(t, role)
			original, _ := os.ReadFile(manifest)
			if modules, err := Read(home, role, manifest); err != nil || len(modules) != 4 {
				t.Fatalf("valid composition refused: %v", err)
			}
			other := "kb"
			if role == other {
				other = "server"
			}
			if _, err := Read(home, other, manifest); err == nil {
				t.Fatal("role swap accepted")
			}
			for _, bad := range []string{
				string(original) + other + "\t" + exe + "\n",
				string(original) + role + "\t" + exe + "\n",
				strings.ReplaceAll(string(original), "memory\t"+exe+"\n", ""),
				strings.ReplaceAll(string(original), exe, "/does/not/exist"),
				string(original) + "unknown\t./relative\n",
			} {
				if err := os.WriteFile(manifest, []byte(bad), 0600); err != nil {
					t.Fatal(err)
				}
				if err := Run(context.Background(), home, role, filepath.Join(home, "bus"), manifest); err == nil {
					t.Fatal("invalid composition activated")
				}
				if _, err := os.Stat(filepath.Join(home, "starts")); !os.IsNotExist(err) {
					t.Fatal("invalid composition started a process")
				}
			}
		})
	}
}

func TestWaitForBusRestartAndBoundedStop(t *testing.T) {
	home, manifest, _ := fixture(t, "server")
	socket := filepath.Join(home, "bus")
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	done := make(chan error, 1)
	go func() { done <- Run(ctx, home, "server", socket, manifest) }()
	time.Sleep(150 * time.Millisecond)
	if _, err := os.Stat(filepath.Join(home, "starts")); !os.IsNotExist(err) {
		t.Fatal("module started before its bus")
	}
	listener, err := net.Listen("unix", socket)
	if err != nil {
		t.Fatal(err)
	}
	defer listener.Close()
	deadline := time.Now().Add(5 * time.Second)
	for {
		raw, _ := os.ReadFile(filepath.Join(home, "starts"))
		if strings.Count(string(raw), "server\n") >= 8 {
			break
		}
		if time.Now().After(deadline) {
			t.Fatal("failed modules did not restart")
		}
		time.Sleep(20 * time.Millisecond)
	}
	cancel()
	select {
	case err := <-done:
		if err != nil {
			t.Fatal(err)
		}
	case <-time.After(time.Second):
		t.Fatal("supervisor failed to stop during retry")
	}
}

func TestCancellationKillsStalledProcessGroup(t *testing.T) {
	home, _, executable := fixture(t, "server")
	if err := os.WriteFile(executable, []byte("#!/bin/sh\ntrap 'exit 0' TERM\nsleep 60 &\nwait\n"), 0700); err != nil {
		t.Fatal(err)
	}
	ctx, cancel := context.WithTimeout(context.Background(), 200*time.Millisecond)
	defer cancel()
	started := time.Now()
	_ = child(ctx, executable, "/unused", "server", home)
	if time.Since(started) > 2*time.Second {
		t.Fatal("process group failed to stop")
	}
}
