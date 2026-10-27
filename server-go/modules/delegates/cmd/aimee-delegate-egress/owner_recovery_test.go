package main

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"os"
	"os/exec"
	"strconv"
	"strings"
	"syscall"
	"testing"
)

func TestRecoveryRequalifiesDurableDaemonBeforeExactTeardown(t *testing.T) {
	for _, mode := range []string{"same-daemon", "different-daemon", "missing-id", "info-failed", "legacy-v1", "partial-tail", "intent-only-absent", "replaced-artifacts"} {
		t.Run(mode, func(t *testing.T) {
			fixture := privateLeaseFixture(t)
			// A replacement producer has a new held endpoint. The durable old URI is provenance.
			fixture.lease.engine = "unix:///proc/12345/fd/99"
			if err := fixture.lease.Intent(fixture.request); err != nil {
				t.Fatal(err)
			}
			id := strings.Repeat("a", 64)
			if mode != "intent-only-absent" {
				if err := fixture.lease.Created(id); err != nil {
					t.Fatal(err)
				}
			}
			if mode == "legacy-v1" {
				bytes, err := os.ReadFile(fixture.path)
				if err != nil {
					t.Fatal(err)
				}
				var output []byte
				for _, line := range strings.Split(strings.TrimSuffix(string(bytes), "\n"), "\n") {
					var record ownerLeaseRecord
					if err := json.Unmarshal([]byte(line), &record); err != nil {
						t.Fatal(err)
					}
					record.Schema = ownerLeaseLegacySchema
					record.DaemonID = ""
					record.Artifacts = nil
					encoded, _ := json.Marshal(record)
					output = append(output, encoded...)
					output = append(output, '\n')
				}
				if err := fixture.parent.Truncate(0); err != nil {
					t.Fatal(err)
				}
				if _, err := fixture.parent.WriteAt(output, 0); err != nil {
					t.Fatal(err)
				}
			}
			if mode == "partial-tail" {
				if _, err := fixture.parent.WriteAt([]byte("{"), fixture.lease.validEnd); err != nil {
					t.Fatal(err)
				}
			}
			originals := *fixture.lease.artifacts
			if mode == "replaced-artifacts" {
				for _, path := range []string{fixture.socket, fixture.helper} {
					held, err := os.Open(path)
					// A bound socket cannot be opened for reading. The listener is already held by fixture.
					if err == nil {
						defer held.Close()
					}
					if err := os.Remove(path); err != nil {
						t.Fatal(err)
					}
					if err := os.WriteFile(path, []byte("foreign replacement"), 0600); err != nil {
						t.Fatal(err)
					}
				}
			}
			endpoint := fmt.Sprintf("unix:///proc/%d/fd/98", os.Getppid())
			fd, err := syscall.Dup(int(fixture.parent.Fd()))
			if err != nil {
				t.Fatal(err)
			}
			calls, removed := 0, false
			err = cleanupOwnerLease(context.Background(), []string{"--recover", "--engine-endpoint", endpoint, "--lease-fd", strconv.Itoa(fd), "--lease-check", fixture.path}, func(_ context.Context, _ string, args ...string) (string, error) {
				calls++
				if len(args) < 3 || args[0] != "--host" || args[1] != endpoint {
					t.Fatal("recovery escaped current held daemon", args)
				}
				operation := args[2:]
				switch operation[0] {
				case "info":
					if mode == "info-failed" {
						return "", errors.New("unavailable")
					}
					if mode == "different-daemon" {
						return `"different daemon"`, nil
					}
					if mode == "missing-id" {
						return `""`, nil
					}
					return `"opaque-daemon-fixture:v2"`, nil
				case "ps":
					if removed {
						return "", nil
					}
					return id, nil
				case "rm":
					if mode != "same-daemon" && mode != "replaced-artifacts" {
						t.Fatal("unqualified teardown", mode, args)
					}
					if strings.Join(operation, " ") != "rm -f -v "+id {
						t.Fatal("not exact CID teardown", args)
					}
					removed = true
					return "", nil
				case "inspect":
					if mode != "intent-only-absent" {
						t.Fatal("knownCID cleanup re-admitted current posture", args)
					}
					return "", errors.New("not found")
				default:
					t.Fatal("unexpected command", args)
				}
				return "", nil
			})
			success := mode == "same-daemon" || mode == "replaced-artifacts"
			if (err == nil) != success || removed != success {
				t.Fatalf("mode=%s err=%v removed=%v", mode, err, removed)
			}
			if (mode == "legacy-v1" || mode == "partial-tail") && calls != 0 {
				t.Fatal("incomplete/legacy recovery contacted daemon", calls)
			}
			if success {
				read := &ownerLease{file: fixture.parent}
				if err := read.read(); err != nil || !read.removed || read.engine != fixture.lease.engine || *read.artifacts != originals {
					t.Fatal("recovery changed durable source/provenance", err)
				}
			}
		})
	}
}

// Real processes exercise kernel OFD flock inheritance, not a mocked lock API.
// The creator's duplicate survives its parent's close and prevents another open
// description from acquiring cleanup authority until the helper actually exits.
func TestLeaseLockSurvivesInheritedHelperUntilActualExit(t *testing.T) {
	if os.Getenv("AIMEE_TEST_OWNER_LOCK_CHILD") == "1" {
		file := os.NewFile(3, "inherited-lease")
		if file == nil || syscall.Flock(3, syscall.LOCK_EX|syscall.LOCK_NB) != nil {
			os.Exit(2)
		}
		if _, err := os.Stdout.Write([]byte("READY\n")); err != nil {
			os.Exit(3)
		}
		var release [1]byte
		if _, err := os.Stdin.Read(release[:]); err != nil {
			os.Exit(4)
		}
		file.Close()
		os.Exit(0)
	}
	fixture := privateLeaseFixture(t)
	competing, err := os.OpenFile(fixture.path, os.O_RDWR, 0)
	if err != nil {
		t.Fatal(err)
	}
	if err := syscall.Flock(int(competing.Fd()), syscall.LOCK_EX|syscall.LOCK_NB); err != syscall.EWOULDBLOCK {
		competing.Close()
		t.Fatal("creator never acquired exclusive lease", err)
	}
	competing.Close()
	command := exec.Command(os.Args[0], "-test.run=^TestLeaseLockSurvivesInheritedHelperUntilActualExit$")
	command.Env = append(os.Environ(), "AIMEE_TEST_OWNER_LOCK_CHILD=1")
	command.ExtraFiles = []*os.File{fixture.parent}
	input, err := command.StdinPipe()
	if err != nil {
		t.Fatal(err)
	}
	output, err := command.StdoutPipe()
	if err != nil {
		t.Fatal(err)
	}
	if err := command.Start(); err != nil {
		t.Fatal(err)
	}
	defer func() { command.Process.Kill(); command.Wait() }()
	ready := make([]byte, 6)
	if _, err := output.Read(ready); err != nil || string(ready) != "READY\n" {
		t.Fatalf("helper did not hold lease: %q %v", ready, err)
	}
	fixture.lease.file.Close()
	fixture.parent.Close()
	contender, err := os.OpenFile(fixture.path, os.O_RDWR, 0)
	if err != nil {
		t.Fatal(err)
	}
	defer contender.Close()
	if err := syscall.Flock(int(contender.Fd()), syscall.LOCK_EX|syscall.LOCK_NB); err != syscall.EWOULDBLOCK {
		t.Fatal("live inherited helper lost exclusion", err)
	}
	if _, err := input.Write([]byte("x")); err != nil {
		t.Fatal(err)
	}
	input.Close()
	if err := command.Wait(); err != nil {
		t.Fatal(err)
	}
	if err := syscall.Flock(int(contender.Fd()), syscall.LOCK_EX|syscall.LOCK_NB); err != nil {
		t.Fatal("exited helper stranded lock", err)
	}
}

func TestLegacyJournalStillCleansOnlyThroughOriginalHeldEndpoint(t *testing.T) {
	fixture := privateLeaseFixture(t)
	if err := fixture.lease.Intent(fixture.request); err != nil {
		t.Fatal(err)
	}
	id := strings.Repeat("b", 64)
	if err := fixture.lease.Created(id); err != nil {
		t.Fatal(err)
	}
	encoded, err := os.ReadFile(fixture.path)
	if err != nil {
		t.Fatal(err)
	}
	var legacy []byte
	for _, line := range strings.Split(strings.TrimSuffix(string(encoded), "\n"), "\n") {
		var record ownerLeaseRecord
		if err := json.Unmarshal([]byte(line), &record); err != nil {
			t.Fatal(err)
		}
		record.Schema = ownerLeaseLegacySchema
		record.DaemonID = ""
		record.Artifacts = nil
		line, _ := json.Marshal(record)
		legacy = append(legacy, line...)
		legacy = append(legacy, '\n')
	}
	if err := fixture.parent.Truncate(0); err != nil {
		t.Fatal(err)
	}
	if _, err := fixture.parent.WriteAt(legacy, 0); err != nil {
		t.Fatal(err)
	}
	fd, err := syscall.Dup(int(fixture.parent.Fd()))
	if err != nil {
		t.Fatal(err)
	}
	calls := 0
	if err := cleanupOwnerLease(context.Background(), []string{"--engine-endpoint", fixture.lease.engine, "--lease-fd", strconv.Itoa(fd), "--lease-check", fixture.path}, func(_ context.Context, _ string, args ...string) (string, error) {
		calls++
		if len(args) < 3 || args[0] != "--host" || args[1] != fixture.lease.engine || args[2] != "ps" || args[len(args)-1] != "id="+id {
			t.Fatal("legacy cleanup added authority or changed endpoint", args)
		}
		return "", nil
	}); err != nil || calls != 1 {
		t.Fatal("legacy retained endpoint cleanup regressed", err, calls)
	}
}
