package main

import (
	"context"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"net"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"syscall"
	"testing"

	"github.com/JBailes/aimee/server-go/modules/delegates"
)

func verifiedRunner(_ context.Context, _ string, args ...string) (string, error) {
	if len(args) == 0 || args[0] != "inspect" {
		return "", nil
	}
	switch args[2] {
	case `{{json .NetworkSettings.Networks}}`:
		return `{"none":{"IPAddress":""}}`, nil
	case `{{range .Mounts}}{{.Source}}|{{.Destination}}|{{.RW}};{{end}}`:
		return "/srv/repo|/srv/repo|true;/run/aimee/aimee-http.sock|/run/aimee/aimee-http.sock|true;", nil
	case `{{range .Config.Env}}{{println .}}{{end}}`:
		return "AIMEE_API_ENDPOINT=" + delegates.ControlEndpoint + "\nGIT_CONFIG_COUNT=1\nGIT_CONFIG_KEY_0=safe.directory\nGIT_CONFIG_VALUE_0=*\nPATH=/usr/bin:/bin\n", nil
	}
	return "", nil
}

func TestAcquireRequiresUnixSocketAndReturnsVerifiedContainer(t *testing.T) {
	tmp := t.TempDir()
	socket := filepath.Join(tmp, delegates.ControlSocketBasename)
	listener, err := net.Listen("unix", socket)
	if err != nil {
		t.Fatal(err)
	}
	defer listener.Close()
	name, err := acquire(context.Background(), []string{"--task", "d1", "--image", "ubuntu:22.04",
		"--workdir", "/srv/repo", "--repo-root", "/srv/repo", "--worktree", "/srv/repo",
		"--socket-check", socket, "--socket-source", "/run/aimee/aimee-http.sock", "--writes-allowed"}, verifiedRunner)
	if err != nil {
		t.Fatal(err)
	}
	if !strings.HasPrefix(name, "aimee-delegate-d1-") {
		t.Fatalf("unexpected name %q", name)
	}
}

func TestAcquireRejectsPlainFileAndMissingWorkspace(t *testing.T) {
	path := filepath.Join(t.TempDir(), delegates.ControlSocketBasename)
	if err := os.WriteFile(path, []byte("not a socket"), 0o600); err != nil {
		t.Fatal(err)
	}
	if _, err := acquire(context.Background(), []string{"--task", "d1", "--image", "ubuntu:22.04",
		"--socket-check", path, "--socket-source", "/run/aimee/aimee-http.sock"}, verifiedRunner); err == nil {
		t.Fatal("plain file or empty workspace was accepted")
	}
}

func TestMachineRunnerSeparatesSuccessfulStdoutFromWarnings(t *testing.T) {
	output, err := commandRunner(context.Background(), "/bin/sh", "-c", "printf machine-receipt; printf warning >&2")
	if err != nil || output != "machine-receipt" {
		t.Fatal("warnings corrupted the successful machine receipt", output, err)
	}
	output, err = commandRunner(context.Background(), "/bin/sh", "-c", "printf 'No such container' >&2; exit 1")
	if err == nil || !strings.Contains(output, "No such container") {
		t.Fatal("failed cleanup lost its diagnostic", output, err)
	}
}

func TestVerifierRequiresLaunchingParentIdentity(t *testing.T) {
	for _, expected := range []int{0, -1, os.Getpid()} {
		if _, _, err := verifierParentContext(context.Background(), expected); err == nil {
			t.Fatalf("unowned parent %d accepted", expected)
		}
	}
	owned, join, err := verifierParentContext(context.Background(), os.Getppid())
	if err != nil {
		t.Fatal(err)
	}
	join()
	if owned.Err() != context.Canceled {
		t.Fatal("parent watcher was not joined")
	}
}

func ownerSocket(t *testing.T) (string, *net.UnixListener) {
	t.Helper()
	parent := filepath.Join(t.TempDir(), "acp-owners", strings.Repeat("a", 64))
	if err := os.MkdirAll(parent, 0700); err != nil {
		t.Fatal(err)
	}
	directory, err := os.Open(parent)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { directory.Close() })
	listener, err := net.ListenUnix("unix", &net.UnixAddr{Name: fmt.Sprintf("/proc/self/fd/%d/aimee-http.sock", directory.Fd()), Net: "unix"})
	if err != nil {
		t.Fatal(err)
	}
	listener.SetUnlinkOnClose(false)
	t.Cleanup(func() { listener.Close() })
	socket := filepath.Join(parent, delegates.ControlSocketBasename)
	if err = os.Chmod(socket, 0600); err != nil {
		t.Fatal(err)
	}
	return socket, listener
}

func TestPrivateAcquireReceiptBindsActualIDAndOwnerSocket(t *testing.T) {
	socket, _ := ownerSocket(t)
	helper := filepath.Join(filepath.Dir(socket), "owner-io")
	content := []byte("owner-measured-helper-fixture")
	if err := os.WriteFile(helper, content, 0555); err != nil {
		t.Fatal(err)
	}
	// WriteFile applies the process umask; the measured fixture requires exact mode.
	if err := os.Chmod(helper, 0555); err != nil {
		t.Fatal(err)
	}
	measured := sha256.Sum256(content)
	digest := hex.EncodeToString(measured[:])
	endpoint := fmt.Sprintf("unix:///proc/%d/fd/99", os.Getppid())
	id := strings.Repeat("b", 64)
	run := func(ctx context.Context, binary string, args ...string) (string, error) {
		if len(args) < 3 || args[0] != "--host" || args[1] != endpoint {
			t.Fatal("ambient daemon authority", args)
		}
		args = args[2:]
		if args[0] == "create" {
			return id, nil
		}
		if args[0] == "inspect" && args[2] == `{{.Id}}` {
			return id, nil
		}
		if args[0] == "start" && args[1] != id {
			t.Fatal("name replaced resource ID", args)
		}
		if args[0] == "inspect" && args[2] == `{{json .}}` {
			return `{"Id":"` + id + `","Image":"sha256:` + strings.Repeat("c", 64) + `","State":{"Running":true},"Config":{"WorkingDir":"/srv/repo","Entrypoint":["/run/aimee/owner-io"],"Cmd":["--idle-guard"],"Healthcheck":{"Test":["NONE"]}},"HostConfig":{"Privileged":false,"NetworkMode":"none","CapDrop":["ALL"],"SecurityOpt":["no-new-privileges"]}}`, nil
		}
		if args[0] == "inspect" && args[2] == `{{range .Mounts}}{{.Source}}|{{.Destination}}|{{.RW}};{{end}}` {
			return "/srv/repo|/srv/repo|true;" + socket + "|" + delegates.ControlSocketTarget + "|true;" + helper + "|" + delegates.OwnerHelperTarget + "|false;", nil
		}
		return verifiedRunner(ctx, binary, args...)
	}
	args := []string{"--receipt-json", "--task", "acp1", "--image", "ubuntu:22.04", "--workdir", "/srv/repo", "--repo-root", "/srv/repo", "--worktree", "/srv/repo", "--writes-allowed", "--socket-check", socket, "--socket-source", socket, "--engine-endpoint", endpoint, "--helper-check", helper, "--helper-source", helper, "--helper-sha256", digest}
	leasePath := filepath.Join(filepath.Dir(socket), "lease.jsonl")
	lease, err := os.OpenFile(leasePath, os.O_CREATE|os.O_RDWR, 0600)
	if err != nil {
		t.Fatal(err)
	}
	defer lease.Close()
	leaseArgs := func() []string {
		if err := lease.Truncate(0); err != nil {
			t.Fatal(err)
		}
		fd, err := syscall.Dup(int(lease.Fd()))
		if err != nil {
			t.Fatal(err)
		}
		return append(append([]string{}, args...), "--lease-fd", strconv.Itoa(fd), "--lease-check", leasePath)
	}
	encoded, err := acquire(context.Background(), leaseArgs(), run)
	if err != nil {
		t.Fatal(err)
	}
	var receipt map[string]any
	if json.Unmarshal([]byte(encoded), &receipt) != nil || receipt["schema"] != "aimee.container-acquire.v1" || receipt["container_id"] != id || receipt["control_socket_source"] != socket || receipt["control_socket_target"] != delegates.ControlSocketTarget || receipt["verified_posture"] != true || receipt["engine_endpoint"] != endpoint || receipt["helper_sha256"] != digest || receipt["helper_target"] != delegates.OwnerHelperTarget || receipt["helper_source"] != helper || receipt["fresh_container"] != true || receipt["trusted_startup"] != "owner-io-idle-guard-v1" {
		t.Fatal("receipt lost runtime custody", encoded)
	}
	// Changing the owner listener after the runtime verdict cannot produce a receipt.
	for _, replacement := range []bool{false, true} {
		if err := os.Chmod(socket, 0600); err != nil {
			t.Fatal(err)
		}
		cleaned := false
		_, err = acquire(context.Background(), leaseArgs(), func(ctx context.Context, binary string, argv ...string) (string, error) {
			if len(argv) < 3 || argv[0] != "--host" || argv[1] != endpoint {
				t.Fatal("cleanup changed daemon", argv)
			}
			operation := argv[2:]
			if operation[0] == "rm" {
				cleaned = argv[len(argv)-1] == id
				return "", nil
			}
			output, err := run(ctx, binary, argv...)
			if operation[0] == "inspect" && operation[2] == `{{json .}}` {
				if replacement {
					if err := os.Remove(socket); err != nil {
						t.Fatal(err)
					}
					directory, err := os.Open(filepath.Dir(socket))
					if err != nil {
						t.Fatal(err)
					}
					defer directory.Close()
					listener, err := net.ListenUnix("unix", &net.UnixAddr{Name: fmt.Sprintf("/proc/self/fd/%d/aimee-http.sock", directory.Fd()), Net: "unix"})
					if err != nil {
						t.Fatal(err)
					}
					listener.SetUnlinkOnClose(false)
					t.Cleanup(func() { listener.Close() })
					if err := os.Chmod(socket, 0600); err != nil {
						t.Fatal(err)
					}
				} else if err := os.Chmod(socket, 0666); err != nil {
					t.Fatal(err)
				}
			}
			return output, err
		})
		if err == nil || !cleaned {
			t.Fatal("changed socket retained handoff authority", replacement, err, cleaned)
		}
	}

}

func TestPrivateControlSocketRejectsUnscopedOrChangedAuthority(t *testing.T) {
	socket, _ := ownerSocket(t)
	for _, mutate := range []func() func(){
		func() func() { os.Chmod(socket, 0666); return func() { os.Chmod(socket, 0600) } },
		func() func() {
			os.Chmod(filepath.Dir(socket), 0755)
			return func() { os.Chmod(filepath.Dir(socket), 0700) }
		},
		func() func() {
			os.Chmod(filepath.Dir(filepath.Dir(socket)), 0755)
			return func() { os.Chmod(filepath.Dir(filepath.Dir(socket)), 0700) }
		},
	} {
		restore := mutate()
		_, err := privateControlSocket(socket, socket, "")
		restore()
		if err == nil {
			t.Fatal("nonprivate socket accepted")
		}
	}
	if _, err := privateControlSocket(socket, "/run/aimee/aimee-http.sock", ""); err == nil {
		t.Fatal("foreign source namespace accepted")
	}
	if _, err := privateControlSocket("/run/aimee/aimee-http.sock", "/run/aimee/aimee-http.sock", ""); err == nil {
		t.Fatal("global UID authority accepted")
	}
	if _, err := privateControlSocket(socket, "/host"+socket, "/	/host"); err != nil {
		t.Fatal("exact owner namespace translation refused", err)
	}
}
