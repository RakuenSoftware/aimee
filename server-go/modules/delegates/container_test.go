package delegates

import (
	"context"
	"errors"
	"strings"
	"testing"
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
