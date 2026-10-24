package main

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"syscall"
	"testing"

	"github.com/JBailes/aimee/server-go/modules/delegates"
)

func TestOrdinaryCreatorReceiptKeepsImageStartupAndOnlyOwnsJournalParent(t *testing.T) {
	for _, defect := range []string{"", "image-unavailable", "wrong-entrypoint", "wrong-healthcheck", "missing-cid"} {
		t.Run(defect, func(t *testing.T) {
			fixture := privateLeaseFixture(t)
			endpoint := fixture.lease.engine
			imageID := "sha256:" + strings.Repeat("c", 64)
			cid := strings.Repeat("d", 64)
			health := &delegates.ImageHealthcheck{Test: []string{"CMD-SHELL", "image-health"}, Interval: 5000000000, Timeout: 1000000000, Retries: 3}
			entrypoint := []string{"/image-owned-entrypoint", "--image-option"}
			var request delegates.DockerCreateRequest
			creates, starts := 0, 0
			removed := false
			run := func(_ context.Context, _ string, argv ...string) (string, error) {
				if len(argv) < 3 || argv[0] != "--host" || argv[1] != endpoint {
					t.Fatal("ordinary command escaped held daemon", argv)
				}
				args := argv[2:]
				switch args[0] {
				case "info":
					return `"opaque-daemon-fixture:v2"`, nil
				case "image":
					if defect == "image-unavailable" {
						return "", errors.New("unavailable")
					}
					bytes, _ := json.Marshal(map[string]any{"id": imageID, "entrypoint": entrypoint, "healthcheck": health})
					return string(bytes), nil
				case "create":
					creates++
					observed := &ownerLease{file: fixture.parent}
					if err := observed.read(); err != nil || observed.schema != ordinaryLeaseSchema || observed.resourceMode != ordinaryResourceMode || observed.daemonID != "opaque-daemon-fixture:v2" || !observed.artifacts.validOrdinary() {
						t.Fatal("create preceded typed durable ordinary ownership", err)
					}
					request = *observed.request
					if request.Image != imageID || request.Startup == nil || request.Startup.Healthcheck.Interval != health.Interval || request.Startup.Entrypoint[0] != entrypoint[0] || request.LeaseNonce != filepath.Base(filepath.Dir(fixture.path)) {
						t.Fatal("image/lease source was not pinned", request)
					}
					joined := strings.Join(args, " ")
					if strings.Contains(joined, "--entrypoint") || strings.Contains(joined, "--no-healthcheck") || strings.Contains(joined, "--cap-drop") || !strings.Contains(joined, "--label "+delegates.OwnerLeaseLabel+"="+request.LeaseNonce) || !strings.Contains(joined, imageID+" sleep infinity") {
						t.Fatal("ordinary image startup replaced or owner label missing", args)
					}
					if defect == "missing-cid" {
						return "", nil
					}
					return cid, nil
				case "start":
					starts++
					if args[1] != cid {
						t.Fatal("name-based start", args)
					}
					return "", nil
				case "inspect":
					if args[len(args)-1] != cid {
						t.Fatal("post-acquire name inspection granted custody", args)
					}
					switch args[2] {
					case `{{json .NetworkSettings.Networks}}`:
						return `{"none":{"IPAddress":""}}`, nil
					case `{{range .Mounts}}{{.Source}}|{{.Destination}}|{{.RW}};{{end}}`:
						var out strings.Builder
						for _, mount := range request.Spec.Mounts {
							fmt.Fprintf(&out, "%s|%s|%t;", mount.Source, mount.Target, !mount.ReadOnly)
						}
						return out.String(), nil
					case `{{range .Config.Env}}{{println .}}{{end}}`:
						var out []string
						for _, env := range request.Spec.Env {
							out = append(out, env.Name+"="+env.Value)
						}
						return strings.Join(out, "\n"), nil
					case `{{json .}}`:
						actualEntry := entrypoint
						actualHealth := health
						if defect == "wrong-entrypoint" {
							actualEntry = []string{"/foreign-entrypoint"}
						}
						if defect == "wrong-healthcheck" {
							copy := *health
							copy.Timeout++
							actualHealth = &copy
						}
						bytes, _ := json.Marshal(map[string]any{"Id": cid, "Image": imageID, "State": map[string]any{"Running": true}, "Config": map[string]any{"Image": imageID, "WorkingDir": request.WorkDir, "Entrypoint": actualEntry, "Healthcheck": actualHealth, "Cmd": []string{"sleep", "infinity"}, "Labels": map[string]string{delegates.OwnerLeaseLabel: request.LeaseNonce}}, "HostConfig": map[string]any{"NetworkMode": "none", "Privileged": false}})
						return string(bytes), nil
					}
					t.Fatal("unexpected inspect", args)
				case "rm":
					if strings.Join(args, " ") != "rm -f -v "+cid {
						t.Fatal("not exact creator cleanup", args)
					}
					removed = true
					return "", nil
				case "ps":
					if removed {
						return "", nil
					}
					return cid, nil
				}
				return "", errors.New("unexpected command")
			}
			fd, err := syscall.Dup(int(fixture.parent.Fd()))
			if err != nil {
				t.Fatal(err)
			}
			args := []string{"--resource-receipt-json", "--lease-fd", strconv.Itoa(fd), "--lease-check", fixture.path, "--engine-endpoint", endpoint, "--task", "ordinary-attempt", "--image", "mutable-image-tag", "--repo-root", fixture.request.WorkDir, "--worktree", fixture.request.WorkDir, "--workdir", fixture.request.WorkDir, "--socket-source", fixture.socket, "--socket-check", fixture.socket, "--writes-allowed"}
			receipt, err := acquire(context.Background(), args, run)
			if defect != "" {
				if err == nil {
					t.Fatal("unqualified ordinary resource admitted", defect)
				}
				if defect == "image-unavailable" && creates != 0 {
					t.Fatal("create without image identity")
				}
				if defect == "missing-cid" && starts != 0 {
					t.Fatal("unidentified creation started")
				}
				if (defect == "wrong-entrypoint" || defect == "wrong-healthcheck") && !removed {
					t.Fatal("failed actual startup validation lost retirement")
				}
				return
			}
			var result map[string]any
			if err != nil || json.Unmarshal([]byte(receipt), &result) != nil || result["container_id"] != cid || result["image_id"] != imageID || result["resource_mode"] != ordinaryResourceMode || result["schema"] != "aimee.container-resource.v1" || result["helper_target"] != nil || result["trusted_startup"] != nil {
				t.Fatal("ordinary receipt forged or lost ownership", err, receipt)
			}
			// Checked retirement never claims ownership of the callback or image helper.
			if creates != 1 || starts != 1 || request.Startup == nil {
				t.Fatal("creator lifetime mismatch")
			}
			cleanupFD, err := syscall.Dup(int(fixture.parent.Fd()))
			if err != nil {
				t.Fatal(err)
			}
			if err := cleanupOwnerLease(context.Background(), []string{"--engine-endpoint", endpoint, "--lease-fd", strconv.Itoa(cleanupFD), "--lease-check", fixture.path}, run); err != nil {
				t.Fatal(err)
			}
			if _, err := os.Lstat(fixture.socket); err != nil {
				t.Fatal("global callback source was retired", err)
			}
			final := &ownerLease{file: fixture.parent}
			if err := final.read(); err != nil || !final.removed || !final.artifacts.validOrdinary() {
				t.Fatal("ordinary journal retirement invalid", err)
			}
		})
	}
}

func TestOrdinaryV3RecoveryRequalifiesDaemonWithoutOwningGlobalArtifacts(t *testing.T) {
	fixture := privateLeaseFixture(t)
	source := delegates.SandboxRequest{WritesAllowed: true, RepoRoot: fixture.request.WorkDir, Worktree: fixture.request.WorkDir, IsGitCheckout: true, ParentSocketHost: fixture.socket, ParentSocketTarget: delegates.ControlSocketTarget}
	spec, err := delegates.BuildSandboxSpec(source)
	if err != nil {
		t.Fatal(err)
	}
	fixture.lease.schema = ordinaryLeaseSchema
	fixture.lease.resourceMode = ordinaryResourceMode
	fixture.lease.helperSHA = ""
	fixture.lease.engine = "unix:///proc/12345/fd/88"
	fixture.lease.artifacts = &ownerSourceIdentities{Parent: fixture.lease.artifacts.Parent}
	req := delegates.DockerCreateRequest{Spec: spec, ContainerName: "ordinary-owner", Image: "sha256:" + strings.Repeat("a", 64), WorkDir: fixture.request.WorkDir, Command: []string{"sleep", "infinity"}, LeaseNonce: filepath.Base(filepath.Dir(fixture.path)), Startup: &delegates.ImageStartup{}}
	if err := fixture.lease.Intent(req); err != nil {
		t.Fatal(err)
	}
	cid := strings.Repeat("b", 64)
	if err := fixture.lease.Created(cid); err != nil {
		t.Fatal(err)
	}
	fd, err := syscall.Dup(int(fixture.parent.Fd()))
	if err != nil {
		t.Fatal(err)
	}
	endpoint := fmt.Sprintf("unix:///proc/%d/fd/98", os.Getppid())
	calls := 0
	if err := cleanupOwnerLease(context.Background(), []string{"--recover", "--engine-endpoint", endpoint, "--lease-fd", strconv.Itoa(fd), "--lease-check", fixture.path}, func(_ context.Context, _ string, args ...string) (string, error) {
		calls++
		if len(args) < 3 || args[0] != "--host" || args[1] != endpoint {
			t.Fatal("recovery endpoint changed", args)
		}
		if args[2] == "info" {
			return `"opaque-daemon-fixture:v2"`, nil
		}
		if args[2] == "ps" && args[len(args)-1] == "id="+cid {
			return "", nil
		}
		t.Fatal("unexpected operation", args)
		return "", nil
	}); err != nil || calls != 2 {
		t.Fatal("v3 exact absent resource not retired", err, calls)
	}
	final := &ownerLease{file: fixture.parent}
	if err := final.read(); err != nil || !final.removed || !final.artifacts.validOrdinary() || final.engine != fixture.lease.engine {
		t.Fatal("v3 custody changed", err)
	}
	for _, path := range []string{fixture.socket, fixture.helper} {
		if _, err := os.Lstat(path); err != nil {
			t.Fatal("unowned callback/helper changed", err)
		}
	}
}
