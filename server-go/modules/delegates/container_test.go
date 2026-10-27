package delegates

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"reflect"
	"strings"
	"testing"
	"time"
)

type fakeDocker struct {
	calls  []string
	report RuntimePostureReport
	failOn string
}

func (f *fakeDocker) run(_ context.Context, _ string, args ...string) (string, error) {
	sub := ""
	if len(args) > 0 {
		sub = args[0]
	}
	f.calls = append(f.calls, sub)
	if f.failOn == sub {
		return "", errors.New("command failed")
	}
	if sub != "inspect" {
		return "", nil
	}
	switch args[2] {
	case networkProbeFormat:
		if f.report.NetworkFailed {
			return "", errors.New("network probe failed")
		}
		return f.report.Network, nil
	case mountProbeFormat:
		if f.report.MountsFailed {
			return "", errors.New("mount probe failed")
		}
		return f.report.Mounts, nil
	case environmentProbeFormat:
		if f.report.EnvironmentFailed {
			return "", errors.New("environment probe failed")
		}
		return f.report.Environment, nil
	default:
		return "", errors.New("unexpected inspect format")
	}
}

func (f *fakeDocker) ran(sub string) bool {
	for _, call := range f.calls {
		if call == sub {
			return true
		}
	}
	return false
}

func startReq(t *testing.T) DockerCreateRequest {
	t.Helper()
	spec, err := BuildSandboxSpec(writeReq())
	if err != nil {
		t.Fatal(err)
	}
	return DockerCreateRequest{Spec: spec, ContainerName: "aimee-delegate-1", Image: "ubuntu:22.04"}
}

func fakeFor(req DockerCreateRequest) *fakeDocker {
	return &fakeDocker{report: postureReport(req.Spec)}
}

func TestContainerStartRequiresConfirmedRuntimePosture(t *testing.T) {
	req := startReq(t)
	fake := fakeFor(req)
	result, err := (ContainerRunner{Run: fake.run}).Start(context.Background(), req)
	if err != nil || result.Refused {
		t.Fatalf("confirmed container not handed over: result=%+v err=%v", result, err)
	}
	for _, call := range []string{"create", "start", "inspect"} {
		if !fake.ran(call) {
			t.Fatalf("expected %s in %v", call, fake.calls)
		}
	}
	if fake.ran("rm") {
		t.Fatalf("confirmed container destroyed: %v", fake.calls)
	}
}

func TestContainerStartDestroysEveryUnconfirmedPosture(t *testing.T) {
	for name, mutate := range map[string]func(*RuntimePostureReport){
		"network breach":    func(r *RuntimePostureReport) { r.Network = "bridge=172.17.0.2;" },
		"network probe":     func(r *RuntimePostureReport) { r.NetworkFailed = true },
		"mount probe":       func(r *RuntimePostureReport) { r.MountsFailed = true },
		"extra mount":       func(r *RuntimePostureReport) { r.Mounts += "/var/run/docker.sock|/var/run/docker.sock|true;" },
		"environment probe": func(r *RuntimePostureReport) { r.EnvironmentFailed = true },
		"credential":        func(r *RuntimePostureReport) { r.Environment += "GITHUB_TOKEN=forbidden\n" },
	} {
		t.Run(name, func(t *testing.T) {
			req := startReq(t)
			fake := fakeFor(req)
			mutate(&fake.report)
			result, err := (ContainerRunner{Run: fake.run}).Start(context.Background(), req)
			if err != nil || !result.Refused || result.Reason == "" || !fake.ran("rm") {
				t.Fatalf("unconfirmed posture escaped: result=%+v err=%v calls=%v", result, err, fake.calls)
			}
		})
	}
}

func TestContainerRefusalSurfacesCleanupFailure(t *testing.T) {
	req := startReq(t)
	fake := fakeFor(req)
	fake.report.Network = "bridge=172.17.0.2;"
	fake.failOn = "rm"
	result, err := (ContainerRunner{Run: fake.run}).Start(context.Background(), req)
	if err == nil || !result.Refused {
		t.Fatalf("cleanup failure hidden: result=%+v err=%v", result, err)
	}
}

func TestContainerAcquireReverifiesResume(t *testing.T) {
	req := startReq(t)
	fake := fakeFor(req)
	result, err := (ContainerRunner{Run: fake.run}).Acquire(context.Background(), req)
	if err != nil || result.Refused || !fake.ran("inspect") || fake.ran("create") {
		t.Fatalf("resume not verified: result=%+v err=%v calls=%v", result, err, fake.calls)
	}
}

func TestContainerStartCleansUpWhenStartFails(t *testing.T) {
	req := startReq(t)
	fake := fakeFor(req)
	fake.failOn = "start"
	if _, err := (ContainerRunner{Run: fake.run}).Start(context.Background(), req); err == nil || !fake.ran("rm") {
		t.Fatalf("failed start cleanup: err=%v calls=%v", err, fake.calls)
	}
}

func TestContainerStopAndRunnerValidation(t *testing.T) {
	fake := &fakeDocker{}
	if err := (ContainerRunner{Run: fake.run}).Stop(context.Background(), "aimee-delegate-1"); err != nil || !fake.ran("rm") {
		t.Fatalf("stop failed: %v %v", err, fake.calls)
	}
	if err := (ContainerRunner{Run: fake.run}).Stop(context.Background(), "  "); err == nil {
		t.Fatal("empty name accepted")
	}
	if _, err := (ContainerRunner{}).Start(context.Background(), startReq(t)); err == nil {
		t.Fatal("missing runner accepted")
	}
}

func TestContainerRefusalExplainsNetworkBypass(t *testing.T) {
	req := startReq(t)
	fake := fakeFor(req)
	fake.report.Network = "bridge=172.17.0.2;"
	result, err := (ContainerRunner{Run: fake.run}).Start(context.Background(), req)
	if err != nil || !strings.Contains(result.Reason, "bypass the egress proxy") {
		t.Fatalf("bad refusal: result=%+v err=%v", result, err)
	}
}

func TestFreshContainerReceiptPinsStartAndPostureToCreatedID(t *testing.T) {
	req := startReq(t)
	fake := fakeFor(req)
	id := strings.Repeat("a", 64)
	runner := func(ctx context.Context, binary string, args ...string) (string, error) {
		if args[0] == "create" {
			return id + "\n", nil
		}
		if args[0] == "start" && args[1] != id {
			t.Fatal("fresh start selected a mutable name", args)
		}
		if args[0] == "inspect" && args[len(args)-1] != id {
			t.Fatal("posture selected a mutable name", args)
		}
		return fake.run(ctx, binary, args...)
	}
	result, err := (ContainerRunner{Run: runner}).Start(context.Background(), req)
	if err != nil || result.Refused || result.ID != id || result.Name != req.ContainerName {
		t.Fatal("lost creation receipt or display identity", result, err)
	}
}

func TestImmutableAcquisitionPinsResumeAndRefusesUnobservedReceipts(t *testing.T) {
	for _, fresh := range []bool{false, true} {
		for _, defect := range []string{"", "alias", "foreign", "image", "stopped", "workdir", "network", "socket"} {
			t.Run(fmt.Sprint(fresh, "/", defect), func(t *testing.T) {
				req := startReq(t)
				req.WorkDir = writeReq().Worktree
				fake := fakeFor(req)
				id := strings.Repeat("a", 64)
				inspectedName := false
				cleaned := ""
				run := func(ctx context.Context, binary string, args ...string) (string, error) {
					if args[0] == "create" {
						if defect == "alias" {
							return req.ContainerName, nil
						}
						return id, nil
					}
					if args[0] == "inspect" && args[2] == identityProbeFormat {
						if args[len(args)-1] != req.ContainerName {
							t.Fatal("resume lookup lost display identity", args)
						}
						inspectedName = true
						if fresh {
							return "", errors.New("not found")
						}
						if defect == "alias" {
							return req.ContainerName, nil
						}
						return id, nil
					}
					if args[0] == "rm" {
						cleaned = args[len(args)-1]
						return "", nil
					}
					if (args[0] == "start" && args[1] != id) || (args[0] == "inspect" && args[len(args)-1] != id) {
						t.Fatal("mutable container authority", args)
					}
					if args[0] == "inspect" && args[2] == receiptProbeFormat {
						observed := map[string]any{"Id": id, "Image": "sha256:" + strings.Repeat("b", 64), "State": map[string]any{"Running": true}, "Config": map[string]any{"WorkingDir": req.WorkDir}}
						switch defect {
						case "foreign":
							observed["Id"] = strings.Repeat("c", 64)
						case "image":
							observed["Image"] = "ubuntu:22.04"
						case "stopped":
							observed["State"] = map[string]any{"Running": false}
						case "workdir":
							observed["Config"] = map[string]any{"WorkingDir": "/foreign"}
						}
						raw, _ := json.Marshal(observed)
						return string(raw), nil
					}
					if defect == "network" {
						fake.report.Network = `{"bridge":{"IPAddress":"172.17.0.2"}}`
					}
					if defect == "socket" {
						fake.report.Mounts = strings.ReplaceAll(fake.report.Mounts, "/run/aimee/aimee-http.sock|", "/run/aimee/vault.sock|")
					}
					return fake.run(ctx, binary, args...)
				}
				result, err := (ContainerRunner{Run: run, RequireImmutableIdentity: true}).Acquire(context.Background(), req)
				if err != nil {
					t.Fatal(err)
				}
				if defect == "" {
					if result.Refused || result.ID != id || result.ImageID != "sha256:"+strings.Repeat("b", 64) || result.WorkDir != req.WorkDir || !inspectedName {
						t.Fatal("no actual receipt", result)
					}
				} else {
					if !result.Refused {
						t.Fatal("unobserved container acquired", result)
					}
					if defect != "alias" && cleaned != id {
						t.Fatal("cleanup lost immutable identity", cleaned)
					}
				}
			})
		}
	}
}

func TestOwnerHelperStartupIsFreshAndDaemonAttested(t *testing.T) {
	for _, defect := range []string{"", "entrypoint", "command", "healthcheck", "privileged", "cap-add", "cap-drop", "pid", "ipc", "devices", "no-new-privileges", "helper-writable"} {
		t.Run(defect, func(t *testing.T) {
			request := writeReq()
			source := "/run/aimee/acp-owners/" + strings.Repeat("e", 64)
			request.ParentSocketHost = source + "/aimee-http.sock"
			spec, err := BuildOwnerSandboxSpec(request, source+"/owner-io")
			if err != nil {
				t.Fatal(err)
			}
			req := DockerCreateRequest{Spec: spec, ContainerName: "fresh-owner", Image: "opaque-image", WorkDir: request.Worktree, Command: []string{"untrusted-image-command"}}
			fake := fakeFor(req)
			id := strings.Repeat("b", 64)
			created := false
			removed := false
			config := map[string]any{"WorkingDir": req.WorkDir, "Entrypoint": []string{OwnerHelperTarget}, "Cmd": []string{"--idle-guard"}, "Healthcheck": map[string]any{"Test": []string{"NONE"}}}
			host := map[string]any{"Privileged": false, "NetworkMode": "none", "CapDrop": []string{"ALL"}, "SecurityOpt": []string{"no-new-privileges"}}
			switch defect {
			case "entrypoint":
				config["Entrypoint"] = []string{"/image/start"}
			case "command":
				config["Cmd"] = []string{"sleep", "infinity"}
			case "healthcheck":
				config["Healthcheck"] = map[string]any{"Test": []string{"CMD-SHELL", "image-code"}}
			case "privileged":
				host["Privileged"] = true
			case "cap-add":
				host["CapAdd"] = []string{"SYS_PTRACE"}
			case "cap-drop":
				host["CapDrop"] = []string{}
			case "pid":
				host["PidMode"] = "host"
			case "ipc":
				host["IpcMode"] = "host"
			case "devices":
				host["Devices"] = []map[string]any{{"PathOnHost": "/dev/sda"}}
			case "no-new-privileges":
				host["SecurityOpt"] = []string{}
			case "helper-writable":
				fake.report.Mounts = strings.ReplaceAll(fake.report.Mounts, OwnerHelperTarget+"|false", OwnerHelperTarget+"|true")
			}
			run := func(ctx context.Context, binary string, args ...string) (string, error) {
				switch args[0] {
				case "create":
					created = true
					command := strings.Join(args, " ")
					for _, flag := range []string{"--entrypoint " + OwnerHelperTarget, "--no-healthcheck", "--cap-drop ALL", "--security-opt no-new-privileges", source + "/owner-io:" + OwnerHelperTarget + ":ro", "--idle-guard"} {
						if !strings.Contains(command, flag) {
							t.Fatal("missing startup protection", flag, command)
						}
					}
					if strings.Contains(command, "untrusted-image-command") {
						t.Fatal("image command retained")
					}
					return id, nil
				case "start":
					if !created || args[1] != id {
						t.Fatal("resumed opaque image process", args)
					}
				case "rm":
					removed = args[len(args)-1] == id
					return "", nil
				case "inspect":
					if !created || args[len(args)-1] != id {
						t.Fatal("mutable name authority", args)
					}
					if args[2] == receiptProbeFormat {
						raw, _ := json.Marshal(map[string]any{"Id": id, "Image": "sha256:" + strings.Repeat("c", 64), "State": map[string]any{"Running": true}, "Config": config, "HostConfig": host})
						return string(raw), nil
					}
				default:
					t.Fatal("image code executed before owner handoff", args)
				}
				return fake.run(ctx, binary, args...)
			}
			result, err := (ContainerRunner{Run: run, RequireImmutableIdentity: true}).Acquire(context.Background(), req)
			if err != nil || (defect == "" && (result.Refused || removed)) || (defect != "" && (!result.Refused || !removed)) {
				t.Fatal("owner startup escaped actual posture", result, err, removed)
			}
		})
	}
}

func TestStrictAcquisitionCleansKnownIDAfterRequestCancellation(t *testing.T) {
	for _, stage := range []string{"start", "posture", "post-acquire"} {
		for _, removalFails := range []bool{false, true} {
			t.Run(fmt.Sprint(stage, "/", removalFails), func(t *testing.T) {
				req := startReq(t)
				req.WorkDir = writeReq().Worktree
				id := strings.Repeat("d", 64)
				ctx, cancel := context.WithCancel(context.Background())
				defer cancel()
				cleaned := false
				run := func(call context.Context, _ string, args ...string) (string, error) {
					switch args[0] {
					case "create":
						return id, nil
					case "start":
						if stage == "start" {
							cancel()
							return "", call.Err()
						}
						return "", nil
					case "inspect":
						cancel()
						return "", call.Err()
					case "rm":
						if !reflect.DeepEqual(args, []string{"rm", "-f", "-v", id}) || call.Err() != nil {
							t.Fatal("request cancellation abandoned exact container", args, call.Err())
						}
						if deadline, ok := call.Deadline(); !ok || time.Until(deadline) <= 0 || time.Until(deadline) > 11*time.Second {
							t.Fatal("cleanup lost independent bound")
						}
						cleaned = true
						if removalFails {
							return "", errors.New("daemon refused removal")
						}
						return "", nil
					default:
						t.Fatal("unexpected operation", args)
					}
					return "", nil
				}
				runner := ContainerRunner{Run: run, RequireImmutableIdentity: true}
				var err error
				if stage == "post-acquire" {
					cancel()
					err = runner.Stop(ctx, id)
				} else {
					_, err = runner.Start(ctx, req)
				}
				if !cleaned || (removalFails && (err == nil || !strings.Contains(err.Error(), id))) {
					t.Fatal("cleanup custody lost", cleaned, err)
				}
			})
		}
	}
}

type creationJournalFixture struct {
	events []string
	fail   string
}

func (j *creationJournalFixture) Intent(DockerCreateRequest) error {
	j.events = append(j.events, "intent")
	if j.fail == "intent" {
		return errors.New("intent sync failed")
	}
	return nil
}
func (j *creationJournalFixture) Created(id string) error {
	j.events = append(j.events, "created:"+id)
	if j.fail == "created" {
		return errors.New("identity sync failed")
	}
	return nil
}
func TestCreationJournalPrecedesCreateAndStart(t *testing.T) {
	for _, failure := range []string{"", "intent", "created"} {
		t.Run(failure, func(t *testing.T) {
			req := startReq(t)
			req.WorkDir = writeReq().Worktree
			journal := &creationJournalFixture{fail: failure}
			id := strings.Repeat("f", 64)
			created, started, cleaned := false, false, false
			run := func(_ context.Context, _ string, args ...string) (string, error) {
				switch args[0] {
				case "create":
					if len(journal.events) != 1 || journal.events[0] != "intent" {
						t.Fatal("creation preceded durable intent")
					}
					created = true
					return id, nil
				case "start":
					if len(journal.events) != 2 || journal.events[1] != "created:"+id {
						t.Fatal("execution preceded durable identity")
					}
					started = true
					return "", errors.New("fixture stops before execution")
				case "rm":
					cleaned = args[len(args)-1] == id
					return "", nil
				default:
					t.Fatal("unexpected command", args)
				}
				return "", nil
			}
			_, err := (ContainerRunner{Run: run, RequireImmutableIdentity: true, Journal: journal}).Start(context.Background(), req)
			if err == nil || (failure == "intent" && (created || started || cleaned)) || (failure == "created" && (!created || started || !cleaned)) || (failure == "" && (!created || !started || !cleaned)) {
				t.Fatal("creation handoff order differs", failure, created, started, cleaned, err)
			}
		})
	}
}

func TestStrictRemovalScopesAnonymousVolumesToExactOwnedID(t *testing.T) {
	id := strings.Repeat("b", 64)
	for _, strict := range []bool{false, true} {
		called := false
		target := "legacy-name"
		expected := []string{"rm", "-f", target}
		if strict {
			target = id
			expected = []string{"rm", "-f", "-v", id}
		}
		runner := ContainerRunner{RequireImmutableIdentity: strict, Run: func(_ context.Context, _ string, args ...string) (string, error) {
			called = true
			if !reflect.DeepEqual(args, expected) {
				t.Fatal("removal widened or changed legacy semantics", args, expected)
			}
			return "", nil
		}}
		if err := runner.Stop(context.Background(), target); err != nil || !called {
			t.Fatal("owned removal missing", err)
		}
		if strict {
			called = false
			if err := runner.Stop(context.Background(), "name-alias"); err == nil || called {
				t.Fatal("strict removal accepted mutable name", err)
			}
		}
	}
}
