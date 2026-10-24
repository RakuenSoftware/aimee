package main

import (
	"context"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"syscall"
	"testing"

	"github.com/JBailes/aimee/server-go/modules/delegates"
)

type leaseFixture struct {
	lease                *ownerLease
	parent               *os.File
	path, socket, helper string
	request              delegates.DockerCreateRequest
}

func privateLeaseFixture(t *testing.T) *leaseFixture {
	t.Helper()
	socket, _ := ownerSocket(t)
	helper := filepath.Join(filepath.Dir(socket), "owner-io")
	content := []byte("measured owner helper fixture")
	if err := os.WriteFile(helper, content, 0555); err != nil {
		t.Fatal(err)
	}
	// WriteFile applies the process umask; the measured fixture requires exact mode.
	if err := os.Chmod(helper, 0555); err != nil {
		t.Fatal(err)
	}
	hash := sha256.Sum256(content)
	path := filepath.Join(filepath.Dir(socket), "lease.jsonl")
	parent, err := os.OpenFile(path, os.O_CREATE|os.O_RDWR, 0600)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { parent.Close() })
	descriptor, err := syscall.Dup(int(parent.Fd()))
	if err != nil {
		t.Fatal(err)
	}
	lease, err := openOwnerLease(descriptor, path, socket)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { lease.file.Close() })
	lease.engine = fmt.Sprintf("unix:///proc/%d/fd/99", os.Getppid())
	lease.helperSHA = hex.EncodeToString(hash[:])
	lease.daemonID = "opaque-daemon-fixture:v2"
	socketInfo, err := os.Lstat(socket)
	if err != nil {
		t.Fatal(err)
	}
	helperInfo, err := os.Lstat(helper)
	if err != nil {
		t.Fatal(err)
	}
	parentInfo, err := os.Lstat(filepath.Dir(socket))
	if err != nil {
		t.Fatal(err)
	}
	lease.artifacts = &ownerSourceIdentities{Socket: sourceIdentity(socketInfo), Helper: sourceIdentity(helperInfo), Parent: sourceIdentity(parentInfo)}

	source := delegates.SandboxRequest{WritesAllowed: true, RepoRoot: "/srv/repo", Worktree: "/srv/repo", IsGitCheckout: true, ParentSocketHost: socket, ParentSocketTarget: delegates.ControlSocketTarget}
	spec, err := delegates.BuildOwnerSandboxSpec(source, helper)
	if err != nil {
		t.Fatal(err)
	}
	return &leaseFixture{lease, parent, path, socket, helper, delegates.DockerCreateRequest{Spec: spec, ContainerName: "pending-private-owner", Image: "configured-image", WorkDir: "/srv/repo"}}
}
func TestCreationJournalPreservesTypedIntentAndOnlyIgnoresIncompleteTail(t *testing.T) {
	fixture := privateLeaseFixture(t)
	lease := fixture.lease
	if err := lease.Intent(fixture.request); err != nil {
		t.Fatal(err)
	}
	intent, err := os.ReadFile(fixture.path)
	if err != nil {
		t.Fatal(err)
	}
	id := strings.Repeat("a", 64)
	if _, err := fixture.parent.WriteAt([]byte(`{"schema":"aimee.container-lease.v1","kind":"created"`), int64(len(intent))); err != nil {
		t.Fatal(err)
	}
	restored := &ownerLease{file: fixture.parent}
	if err := restored.read(); err != nil || restored.id != "" {
		t.Fatal("partial identity received authority", err, restored.id)
	}
	restored.recovering = true
	if err := restored.Created(id); err != nil {
		t.Fatal(err)
	}
	recovered, err := os.ReadFile(fixture.path)
	if err != nil {
		t.Fatal(err)
	}
	if !strings.Contains(string(recovered), `"kind":"resolved"`) {
		t.Fatal("resolved identity lost provenance")
	}
	for _, tail := range []string{`{broken}` + "\n", `{"schema":"other","kind":"created"}` + "\n", string(intent), `{"schema":"aimee.container-lease.v1","kind":"created","container_id":"name-alias"}` + "\n"} {
		if err := os.WriteFile(fixture.path, append(append([]byte{}, intent...), tail...), 0600); err != nil {
			t.Fatal(err)
		}
		if err := (&ownerLease{file: fixture.parent}).read(); err == nil {
			t.Fatal("invalid complete journal line accepted", tail)
		}
	}
	if err := os.WriteFile(fixture.path, recovered, 0600); err != nil {
		t.Fatal(err)
	}
	final := &ownerLease{file: fixture.parent}
	if err := final.read(); err != nil || final.id != id || final.request.ContainerName != fixture.request.ContainerName {
		t.Fatal("typed recovered source changed", err, final.id)
	}
	flags, _, failure := syscall.Syscall(syscall.SYS_FCNTL, fixture.lease.file.Fd(), syscall.F_GETFD, 0)
	if failure != 0 || flags&syscall.FD_CLOEXEC == 0 {
		t.Fatal("private journal could leak into Docker child")
	}
}
func TestCreationJournalFileAuthorityAndFreshness(t *testing.T) {
	fixture := privateLeaseFixture(t)
	for _, defect := range []string{"mode", "hardlink", "different-path"} {
		t.Run(defect, func(t *testing.T) {
			check := fixture.path
			switch defect {
			case "mode":
				os.Chmod(fixture.path, 0644)
				defer os.Chmod(fixture.path, 0600)
			case "hardlink":
				if err := os.Link(fixture.path, fixture.path+".other"); err != nil {
					t.Fatal(err)
				}
				defer os.Remove(fixture.path + ".other")
			case "different-path":
				check = filepath.Join(filepath.Dir(fixture.path), "other.jsonl")
			}
			descriptor, err := syscall.Dup(int(fixture.parent.Fd()))
			if err != nil {
				t.Fatal(err)
			}
			got, err := openOwnerLease(descriptor, check, fixture.socket)
			if got != nil {
				got.file.Close()
			}
			if err == nil {
				t.Fatal("unowned lease file accepted", defect)
			}
		})
	}
	if err := fixture.lease.Intent(fixture.request); err != nil {
		t.Fatal(err)
	}
	if err := fixture.lease.Intent(fixture.request); err == nil {
		t.Fatal("old intent reused for a new creation")
	}
}
func TestCleanupLeaseRecoversCreateGapBeforeImmutableRemoval(t *testing.T) {
	fixture := privateLeaseFixture(t)
	if err := fixture.lease.Intent(fixture.request); err != nil {
		t.Fatal(err)
	}
	id := strings.Repeat("c", 64)
	removed := false
	config := map[string]any{"WorkingDir": fixture.request.WorkDir, "Image": fixture.request.Image, "Entrypoint": []string{delegates.OwnerHelperTarget}, "Cmd": []string{"--idle-guard"}, "Healthcheck": map[string]any{"Test": []string{"NONE"}}}
	var environment, binds []string
	for _, entry := range fixture.request.Spec.Env {
		environment = append(environment, entry.Name+"="+entry.Value)
	}
	config["Env"] = environment
	for _, mount := range fixture.request.Spec.Mounts {
		bind := mount.Source + ":" + mount.Target
		if mount.ReadOnly {
			bind += ":ro"
		}
		binds = append(binds, bind)
	}
	metadata := map[string]any{"Id": id, "Name": "/" + fixture.request.ContainerName, "Image": "sha256:" + strings.Repeat("b", 64), "State": map[string]any{"Running": false}, "Config": config, "HostConfig": map[string]any{"Privileged": false, "NetworkMode": "none", "CapDrop": []string{"ALL"}, "SecurityOpt": []string{"no-new-privileges"}, "Binds": binds}}
	run := func(_ context.Context, _ string, args ...string) (string, error) {
		if len(args) >= 3 && args[0] == "--host" && args[2] == "info" {
			return `"opaque-daemon-fixture:v2"`, nil
		}
		if len(args) < 3 || args[0] != "--host" || args[1] != fixture.lease.engine {
			t.Fatal("cleanup changed daemon", args)
		}
		args = args[2:]
		switch args[0] {
		case "inspect":
			if args[2] == `{{.Id}}` {
				return id, nil
			}
			if args[len(args)-1] != id {
				t.Fatal("untyped metadata resource", args)
			}
			encoded, _ := json.Marshal(metadata)
			return string(encoded), nil
		case "rm":
			if len(args) != 4 || args[1] != "-f" || args[2] != "-v" || args[3] != id {
				t.Fatal("cleanup did not scope anonymous-volume removal to exact ID", args)
			}
			before := &ownerLease{file: fixture.parent}
			if err := before.read(); err != nil || before.id != id {
				t.Fatal("removal preceded durable actual identity", err, before.id)
			}
			if args[len(args)-1] != id {
				t.Fatal("name-only deletion", args)
			}
			removed = true
			return "", nil
		case "ps":
			if removed {
				return "", nil
			}
			return id, nil
		default:
			t.Fatal("cleanup executed image code", args)
		}
		return "", nil
	}
	descriptor, err := syscall.Dup(int(fixture.parent.Fd()))
	if err != nil {
		t.Fatal(err)
	}
	args := []string{"--engine-endpoint", fixture.lease.engine, "--lease-fd", strconv.Itoa(descriptor), "--lease-check", fixture.path}
	if err := cleanupOwnerLease(context.Background(), args, run); err != nil {
		t.Fatal(err)
	}
	final := &ownerLease{file: fixture.parent}
	if err := final.read(); err != nil || !final.removed || final.id != id || !removed {
		t.Fatal("removal receipt lost custody", err, final.id, final.removed)
	}
}

func TestDurableKnownIDAbsenceDoesNotReAdmitMutableFilePosture(t *testing.T) {
	for _, mutation := range []string{"files", "renamed-parent", "unlinked-lease"} {
		t.Run(mutation, func(t *testing.T) {
			fixture := privateLeaseFixture(t)
			id := strings.Repeat("d", 64)
			if err := fixture.lease.Intent(fixture.request); err != nil {
				t.Fatal(err)
			}
			if err := fixture.lease.Created(id); err != nil {
				t.Fatal(err)
			}
			switch mutation {
			case "files":
				if err := os.Remove(fixture.socket); err != nil {
					t.Fatal(err)
				}
				if err := os.Remove(fixture.helper); err != nil {
					t.Fatal(err)
				}
			case "renamed-parent":
				if err := os.Rename(filepath.Dir(fixture.path), filepath.Dir(fixture.path)+".moved"); err != nil {
					t.Fatal(err)
				}
			case "unlinked-lease":
				if err := os.Remove(fixture.path); err != nil {
					t.Fatal(err)
				}
			}
			descriptor, err := syscall.Dup(int(fixture.parent.Fd()))
			if err != nil {
				t.Fatal(err)
			}
			args := []string{"--engine-endpoint", fixture.lease.engine, "--lease-fd", strconv.Itoa(descriptor), "--lease-check", fixture.path}
			called := false
			if err := cleanupOwnerLease(context.Background(), args, func(_ context.Context, _ string, args ...string) (string, error) {
				if len(args) >= 3 && args[0] == "--host" && args[2] == "info" {
					return `"opaque-daemon-fixture:v2"`, nil
				}
				if args[0] != "--host" || args[1] != fixture.lease.engine || args[2] != "ps" || args[len(args)-1] != "id="+id {
					t.Fatal("absence introduced new authority", args)
				}
				called = true
				return "", nil
			}); err != nil || !called {
				t.Fatal("known removal lost durable custody", err)
			}
		})
	}
}

func TestRetainedLeaseCopyDoesNotInheritActualSourceIdentity(t *testing.T) {
	fixture := privateLeaseFixture(t)
	if err := fixture.lease.Intent(fixture.request); err != nil {
		t.Fatal(err)
	}
	bytes, err := os.ReadFile(fixture.path)
	if err != nil {
		t.Fatal(err)
	}
	copyPath := fixture.path + ".copy"
	if err := os.WriteFile(copyPath, bytes, 0600); err != nil {
		t.Fatal(err)
	}
	copied, err := os.OpenFile(copyPath, os.O_RDWR, 0600)
	if err != nil {
		t.Fatal(err)
	}
	defer copied.Close()
	descriptor, err := syscall.Dup(int(copied.Fd()))
	if err != nil {
		t.Fatal(err)
	}
	lease, err := openOwnerLeaseMode(descriptor, fixture.path, fixture.socket, true)
	if err != nil {
		t.Fatal(err)
	}
	defer lease.file.Close()
	if err := lease.read(); err == nil {
		t.Fatal("copied bytes acquired retained descriptor provenance")
	}
}

func TestUnresolvedCleanupRetainsCompleteIntentAndPartialEvidence(t *testing.T) {
	for _, missing := range []bool{false, true} {
		t.Run(strconv.FormatBool(missing), func(t *testing.T) {
			fixture := privateLeaseFixture(t)
			if err := fixture.lease.Intent(fixture.request); err != nil {
				t.Fatal(err)
			}
			if _, err := fixture.parent.WriteAt([]byte(`{"kind":"created"`), fixture.lease.validEnd); err != nil {
				t.Fatal(err)
			}
			before, err := os.ReadFile(fixture.path)
			if err != nil {
				t.Fatal(err)
			}
			descriptor, err := syscall.Dup(int(fixture.parent.Fd()))
			if err != nil {
				t.Fatal(err)
			}
			args := []string{"--engine-endpoint", fixture.lease.engine, "--lease-fd", strconv.Itoa(descriptor), "--lease-check", fixture.path}
			id := strings.Repeat("e", 64)
			err = cleanupOwnerLease(context.Background(), args, func(_ context.Context, _ string, args ...string) (string, error) {
				if len(args) >= 3 && args[0] == "--host" && args[2] == "info" {
					return `"opaque-daemon-fixture:v2"`, nil
				}
				if args[0] != "--host" || args[1] != fixture.lease.engine || args[2] != "inspect" {
					t.Fatal("uncertain cleanup gained effect authority", args)
				}
				if args[4] == `{{.Id}}` {
					if missing {
						return "", fmt.Errorf("not found")
					}
					return id, nil
				}
				return `{"Id":"` + id + `"}`, nil
			})
			after, failure := os.ReadFile(fixture.path)
			if err == nil || failure != nil || string(before) != string(after) {
				t.Fatal("uncertain outcome erased retained journal", err, failure)
			}
		})
	}
}

func TestEmptyPreCreateLeaseClaimsNoContainerWithoutManufacturedIdentity(t *testing.T) {
	fixture := privateLeaseFixture(t)
	descriptor, err := syscall.Dup(int(fixture.parent.Fd()))
	if err != nil {
		t.Fatal(err)
	}
	args := []string{"--engine-endpoint", fixture.lease.engine, "--lease-fd", strconv.Itoa(descriptor), "--lease-check", fixture.path}
	input, output, err := os.Pipe()
	if err != nil {
		t.Fatal(err)
	}
	original := os.Stdout
	os.Stdout = output
	defer func() { os.Stdout = original; input.Close(); output.Close() }()
	if err := cleanupOwnerLease(context.Background(), args, func(context.Context, string, ...string) (string, error) {
		t.Fatal("blank journal dispatched a Docker operation")
		return "", nil
	}); err != nil {
		t.Fatal(err)
	}
	output.Close()
	os.Stdout = original
	encoded, err := io.ReadAll(input)
	if err != nil {
		t.Fatal(err)
	}
	var receipt map[string]any
	if json.Unmarshal(encoded, &receipt) != nil || receipt["schema"] != "aimee.container-cleanup.v1" || receipt["removed"] != false || receipt["no_container"] != true || receipt["container_id"] != nil {
		t.Fatal("blank cleanup manufactured identity", string(encoded))
	}
	info, err := fixture.parent.Stat()
	if err != nil || info.Size() != 0 {
		t.Fatal("pre-create proof manufactured identity evidence", err)
	}
}

func TestDurableCreatedCIDRetiresRefusedPostureAndAnonymousVolumes(t *testing.T) {
	fixture := privateLeaseFixture(t)
	id := strings.Repeat("f", 64)
	if err := fixture.lease.Intent(fixture.request); err != nil {
		t.Fatal(err)
	}
	if err := fixture.lease.Created(id); err != nil {
		t.Fatal(err)
	}
	descriptor, err := syscall.Dup(int(fixture.parent.Fd()))
	if err != nil {
		t.Fatal(err)
	}
	removed := false
	args := []string{"--engine-endpoint", fixture.lease.engine, "--lease-fd", strconv.Itoa(descriptor), "--lease-check", fixture.path}
	if err := cleanupOwnerLease(context.Background(), args, func(_ context.Context, _ string, args ...string) (string, error) {
		if len(args) >= 3 && args[0] == "--host" && args[2] == "info" {
			return `"opaque-daemon-fixture:v2"`, nil
		}
		if args[0] != "--host" || args[1] != fixture.lease.engine {
			t.Fatal("retirement switched daemon", args)
		}
		operation := args[2:]
		switch operation[0] {
		case "ps":
			if operation[len(operation)-1] != "id="+id {
				t.Fatal("raw sweep authority", operation)
			}
			if removed {
				return "", nil
			}
			return id, nil
		case "rm":
			if len(operation) != 4 || operation[1] != "-f" || operation[2] != "-v" || operation[3] != id {
				t.Fatal("retirement lost exact resource scope", operation)
			}
			removed = true
			return "", nil
		default:
			t.Fatal("refused or unavailable posture blocked durable cleanup", operation)
		}
		return "", nil
	}); err != nil || !removed {
		t.Fatal("durable owned resource not retired", err)
	}
}
