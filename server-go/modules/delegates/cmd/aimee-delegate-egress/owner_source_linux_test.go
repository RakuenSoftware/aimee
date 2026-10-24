package main

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"strings"
	"testing"
	"time"

	"github.com/JBailes/aimee/server-go/modules/delegates"
	"golang.org/x/sys/unix"
)

func TestOwnerSourceExposureRefusesBeforeIntentAndCreator(t *testing.T) {
	for _, mode := range []string{"private", "ordinary"} {
		for _, alias := range []bool{false, true} {
			for _, readonly := range []bool{false, true} {
				t.Run(fmt.Sprintf("%s-alias%t-ro%t", mode, alias, readonly), func(t *testing.T) {
					fixture := privateLeaseFixture(t)
					source := filepath.Dir(filepath.Dir(filepath.Dir(fixture.path)))
					if alias {
						name := filepath.Join(t.TempDir(), "source-alias")
						if err := os.Symlink(source, name); err != nil {
							t.Fatal(err)
						}
						source = name
					}
					request := delegates.SandboxRequest{WritesAllowed: !readonly, RepoRoot: source, Worktree: source, IsGitCheckout: true, ParentSocketHost: fixture.socket, ParentSocketTarget: delegates.ControlSocketTarget}
					var spec delegates.SandboxSpec
					var err error
					if mode == "private" {
						spec, err = delegates.BuildOwnerSandboxSpec(request, fixture.helper)
					} else {
						spec, err = delegates.BuildSandboxSpec(request)
					}
					if err != nil {
						t.Fatal(err)
					}
					req := delegates.DockerCreateRequest{Spec: spec, ContainerName: "overlap-refused", Image: "fixture-image", WorkDir: source}
					if mode == "ordinary" {
						fixture.lease.schema = ordinaryLeaseSchema
						fixture.lease.resourceMode = ordinaryResourceMode
						fixture.lease.helperSHA = ""
						fixture.lease.artifacts = &ownerSourceIdentities{Parent: fixture.lease.artifacts.Parent}
						req.Image = "sha256:" + strings.Repeat("a", 64)
						req.LeaseNonce = filepath.Base(filepath.Dir(fixture.path))
						req.Startup = &delegates.ImageStartup{}
					}
					calls := 0
					_, err = (delegates.ContainerRunner{RequireImmutableIdentity: true, Journal: fixture.lease, Run: func(_ context.Context, _ string, _ ...string) (string, error) { calls++; return "", nil }}).Start(context.Background(), req)
					info, failure := fixture.parent.Stat()
					if err == nil || !strings.Contains(err.Error(), "source mount exposes private resource owner") || failure != nil || info.Size() != 0 || calls != 0 {
						t.Fatalf("err=%v size=%v calls=%d", err, info, calls)
					}
				})
			}
		}
	}
}

func TestGitMetadataCannotExposeOwnerDirectory(t *testing.T) {
	fixture := privateLeaseFixture(t)
	req := fixture.request
	home := filepath.Dir(filepath.Dir(filepath.Dir(fixture.path)))
	req.Spec.Mounts = append(req.Spec.Mounts, delegates.SandboxMount{Source: home, Target: home, ReadOnly: true, Kind: delegates.SandboxGitMetadata})
	err := fixture.lease.Intent(req)
	info, _ := fixture.parent.Stat()
	if err == nil || !strings.Contains(err.Error(), "source mount exposes private resource owner") || info.Size() != 0 {
		t.Fatal("git metadata bypassed private custody", err)
	}
}

// An actual isolated user/mount namespace reproduces distinct bind roots that
// namespace '..' cannot join: /home→host/private, /repo→host. Kernel mount roots
// must still identify the exact private parent reached beneath /repo/private.
func TestKernelBindAliasCannotHideOwnerDirectory(t *testing.T) {
	if os.Getenv("AIMEE_TEST_BIND_CHILD") != "1" {
		if runtime.GOOS != "linux" {
			t.Skip("Linux bind namespace capability unavailable")
		}
		binary, err := exec.LookPath("unshare")
		if errors.Is(err, exec.ErrNotFound) {
			t.Skip("unshare capability tool unavailable")
		}
		if err != nil {
			t.Fatal(err)
		}
		preflight, cancel := context.WithTimeout(context.Background(), 5*time.Second)
		output, err := exec.CommandContext(preflight, binary, "--user", "--map-root-user", "--mount", "/usr/bin/true").CombinedOutput()
		cancel()
		if err != nil {
			if strings.Contains(string(output), "Operation not permitted") || strings.Contains(string(output), "Permission denied") {
				t.Skipf("kernel user/mount namespaces unavailable: %s", output)
			}
			t.Fatalf("kernel namespace preflight failed: %v %s", err, output)
		}
		command := exec.Command(binary, "--user", "--map-root-user", "--mount", os.Args[0], "-test.run=^TestKernelBindAliasCannotHideOwnerDirectory$")
		command.Env = append(os.Environ(), "AIMEE_TEST_BIND_CHILD=1")
		output, err = command.CombinedOutput()
		if err != nil {
			t.Fatalf("isolated real kernel bind proof failed: %v %s", err, output)
		}
		return
	}
	root := t.TempDir()
	host := filepath.Join(root, "host")
	private := filepath.Join(host, "private")
	home := filepath.Join(root, "home")
	repo := filepath.Join(root, "repo")
	for _, path := range []string{private, home, repo} {
		if err := os.MkdirAll(path, 0700); err != nil {
			t.Fatal(err)
		}
	}
	if err := unix.Mount(private, home, "", unix.MS_BIND, ""); err != nil {
		t.Fatal(err)
	}
	defer unix.Unmount(home, 0)
	if err := unix.Mount(host, repo, "", unix.MS_BIND, ""); err != nil {
		t.Fatal(err)
	}
	defer unix.Unmount(repo, 0)
	parentPath := filepath.Join(home, "acp-owners", strings.Repeat("b", 64))
	if err := os.MkdirAll(parentPath, 0700); err != nil {
		t.Fatal(err)
	}
	journalPath := filepath.Join(parentPath, "lease.jsonl")
	journal, err := os.OpenFile(journalPath, os.O_CREATE|os.O_EXCL|os.O_RDWR, 0600)
	if err != nil {
		t.Fatal(err)
	}
	defer journal.Close()
	lease := &ownerLease{file: journal, check: journalPath}
	spec, err := delegates.BuildSandboxSpec(delegates.SandboxRequest{WritesAllowed: true, RepoRoot: repo, Worktree: repo, IsGitCheckout: true, ParentSocketHost: "/global/aimee-http.sock", ParentSocketTarget: delegates.ControlSocketTarget})
	if err != nil {
		t.Fatal(err)
	}
	err = lease.refuseSourceExposure(delegates.DockerCreateRequest{Spec: spec})
	if err == nil || !strings.Contains(err.Error(), "source mount exposes private resource owner") {
		t.Fatal("bind roots concealed source exposure", err)
	}
	// A recursive source bind includes a different-device tmpfs descendant.
	nested := filepath.Join(repo, "nested")
	alias := filepath.Join(root, "nested-home")
	for _, path := range []string{nested, alias} {
		if err := os.Mkdir(path, 0700); err != nil {
			t.Fatal(err)
		}
	}
	if err := unix.Mount("tmpfs", nested, "tmpfs", 0, "mode=0700"); err != nil {
		t.Fatal(err)
	}
	defer unix.Unmount(nested, 0)
	if err := unix.Mount(nested, alias, "", unix.MS_BIND, ""); err != nil {
		t.Fatal(err)
	}
	defer unix.Unmount(alias, 0)
	nestedParent := filepath.Join(alias, "acp-owners", strings.Repeat("c", 64))
	if err := os.MkdirAll(nestedParent, 0700); err != nil {
		t.Fatal(err)
	}
	nestedPath := filepath.Join(nestedParent, "lease.jsonl")
	nestedJournal, err := os.OpenFile(nestedPath, os.O_CREATE|os.O_EXCL|os.O_RDWR, 0600)
	if err != nil {
		t.Fatal(err)
	}
	defer nestedJournal.Close()
	nestedLease := &ownerLease{file: nestedJournal, check: nestedPath}
	if err := nestedLease.refuseSourceExposure(delegates.DockerCreateRequest{Spec: spec}); err == nil || !strings.Contains(err.Error(), "source mount exposes private resource owner") {
		t.Fatal("recursive different-device submount concealed owner", err)
	}
	if info, err := nestedJournal.Stat(); err != nil || info.Size() != 0 {
		t.Fatal("negative kernel check rewrote journal", err)
	}
	// The same recursive tmpfs does not expose the unrelated original private owner.
	unrelatedSource := filepath.Join(root, "benign-source")
	if err := os.Mkdir(unrelatedSource, 0700); err != nil {
		t.Fatal(err)
	}
	benignNested := filepath.Join(unrelatedSource, "nested")
	if err := os.Mkdir(benignNested, 0700); err != nil {
		t.Fatal(err)
	}
	if err := unix.Mount("tmpfs", benignNested, "tmpfs", 0, "mode=0700"); err != nil {
		t.Fatal(err)
	}
	defer unix.Unmount(benignNested, 0)
	benignSpec, err := delegates.BuildSandboxSpec(delegates.SandboxRequest{WritesAllowed: true, RepoRoot: unrelatedSource, Worktree: unrelatedSource, IsGitCheckout: true, ParentSocketHost: "/global/aimee-http.sock", ParentSocketTarget: delegates.ControlSocketTarget})
	if err != nil {
		t.Fatal(err)
	}
	if err := lease.refuseSourceExposure(delegates.DockerCreateRequest{Spec: benignSpec}); err != nil {
		t.Fatal("benign recursive source refused", err)
	}
	// A different sibling source on the SAME filesystem remains supported.
	separate := filepath.Join(root, "separate")
	if err := os.Mkdir(separate, 0700); err != nil {
		t.Fatal(err)
	}
	spec, err = delegates.BuildSandboxSpec(delegates.SandboxRequest{WritesAllowed: true, RepoRoot: separate, Worktree: separate, IsGitCheckout: true, ParentSocketHost: "/global/aimee-http.sock", ParentSocketTarget: delegates.ControlSocketTarget})
	if err != nil {
		t.Fatal(err)
	}
	if err := lease.refuseSourceExposure(delegates.DockerCreateRequest{Spec: spec}); err != nil {
		t.Fatal("distinct same-FS source refused", err)
	}
}

func TestKernelMountRecordEscapesAndAmbiguity(t *testing.T) {
	if _, err := kernelMounts([]byte("1 0 0:1 /private\\040name /owner\\134path rw - tmpfs tmpfs rw\n")); err != nil {
		t.Fatal(err)
	}
	for _, record := range []string{"1 0 0:1 /bad\\041escape /a rw - tmpfs x rw\n", "1 0 0:1 /a /b rw - tmpfs x rw\n1 0 0:1 /a /c rw - tmpfs x rw\n", "1 0 0:1 relative /a rw - tmpfs x rw\n", "1 0 0:1 /a/../b /a rw - tmpfs x rw\n"} {
		if _, err := kernelMounts([]byte(record)); err == nil {
			t.Fatal("ambiguous kernel mapping accepted", record)
		}
	}
}

func TestControllerNonRecursiveObserverLayoutIsExplicitlyUnsupported(t *testing.T) {
	fixture := privateLeaseFixture(t)
	root := filepath.Dir(filepath.Dir(filepath.Dir(fixture.path)))
	mapping := root + "\t/daemon/private\n" + fixture.request.WorkDir + "\t/daemon/source\n"
	for _, test := range []struct {
		name      string
		facts     []controllerBind
		wantError bool
	}{
		{"owner-hidden", []controllerBind{{"/daemon/private", root, true}}, true},
		{"source-hidden", []controllerBind{{"/daemon/source", fixture.request.WorkDir, true}}, true},
		{"recursive-false", []controllerBind{{"/daemon/private", root, false}, {"/daemon/source", fixture.request.WorkDir, false}}, false},
		{"recursive-default", nil, false},
		{"unmatched-source", []controllerBind{{"/foreign", root, true}}, true},
	} {
		t.Run(test.name, func(t *testing.T) {
			encoded, err := json.Marshal(test.facts)
			if err != nil {
				t.Fatal(err)
			}
			err = verifyControllerBinds(string(encoded), mapping, fixture.path, fixture.request.Spec)
			if (err != nil) != test.wantError {
				t.Fatalf("err=%v expectedError=%t", err, test.wantError)
			}
			info, _ := fixture.parent.Stat()
			if info.Size() != 0 {
				t.Fatal("observer refusal rewrote journal")
			}
		})
	}
	for _, evidence := range []string{`[{"source":"/daemon/private","target":"` + root + `","non_recursive":null}]`, `[{"source":"/daemon/private","target":"` + root + `"}]`} {
		if err := verifyControllerBinds(evidence, mapping, fixture.path, fixture.request.Spec); err == nil {
			t.Fatal("ambiguous observer recursion admitted")
		}
	}
	// An unrelated attested nonrecursive bind does not disable valid source layouts.
	unrelated := t.TempDir()
	encoded, _ := json.Marshal([]controllerBind{{"/daemon/unrelated", unrelated, true}})
	if err := verifyControllerBinds(string(encoded), mapping+unrelated+"\t/daemon/unrelated\n", fixture.path, fixture.request.Spec); err != nil {
		t.Fatal("unrelated controller bind refused", err)
	}
}
