package delegates

import (
	"context"
	"errors"
	"strings"
	"testing"
)

// fakeDocker records what was run and answers from a script, so the sequencing
// and the failure handling can be tested without a daemon.
type fakeDocker struct {
	calls []string
	// probeOut is what `inspect` returns.
	probeOut string
	// probeErr makes the probe itself fail.
	probeErr bool
	// failOn makes the named subcommand fail ("create", "start", ...).
	failOn string
}

func (f *fakeDocker) run(ctx context.Context, name string, args ...string) (string, error) {
	sub := ""
	if len(args) > 0 {
		sub = args[0]
	}
	f.calls = append(f.calls, sub)
	if f.failOn != "" && sub == f.failOn {
		return "", errors.New("command failed")
	}
	if sub == "inspect" {
		if f.probeErr {
			return "", errors.New("probe failed")
		}
		return f.probeOut, nil
	}
	return "", nil
}

func (f *fakeDocker) ran(sub string) bool {
	for _, c := range f.calls {
		if c == sub {
			return true
		}
	}
	return false
}

func startReq(t *testing.T) DockerCreateRequest {
	t.Helper()
	spec, err := BuildSandboxSpec(writeReq())
	if err != nil {
		t.Fatalf("build spec: %v", err)
	}
	return DockerCreateRequest{
		Spec: spec, ContainerName: "aimee-delegate-1", Image: "ubuntu:22.04",
	}
}

// The happy path: created, started, proven isolated, handed over.
func TestContainerStartIsolated(t *testing.T) {
	fake := &fakeDocker{probeOut: "none=;"}
	runner := ContainerRunner{Run: fake.run}

	res, err := runner.Start(context.Background(), startReq(t))
	if err != nil {
		t.Fatalf("start: %v", err)
	}
	if res.Refused || res.Warned {
		t.Errorf("an isolated container was not handed over: %+v", res)
	}
	if !fake.ran("create") || !fake.ran("start") || !fake.ran("inspect") {
		t.Errorf("expected create, start and inspect: %v", fake.calls)
	}
	if fake.ran("rm") {
		t.Errorf("an isolated container was destroyed: %v", fake.calls)
	}
}

// A container that got a network is not a sandbox, and must not be left
// running -- a leftover container with network access is exactly what the check
// exists to prevent.
func TestContainerStartDestroysOnBreach(t *testing.T) {
	fake := &fakeDocker{probeOut: "bridge=172.17.0.2;"}
	runner := ContainerRunner{Run: fake.run, RequireIsolation: true}

	res, err := runner.Start(context.Background(), startReq(t))
	if err != nil {
		t.Fatalf("start returned an error rather than a refusal: %v", err)
	}
	if !res.Refused {
		t.Fatalf("a container with network egress was handed over: %+v", res)
	}
	if !fake.ran("rm") {
		t.Errorf("the refused container was left running: %v", fake.calls)
	}
	if !strings.Contains(res.Reason, "bypass the egress proxy") {
		t.Errorf("reason does not say what is at stake: %q", res.Reason)
	}
}

// "The probe failed" and "the sandbox is open" are indistinguishable, so under
// the requirement an unprovable container is destroyed too.
func TestContainerStartDestroysOnUnprovableIsolation(t *testing.T) {
	fake := &fakeDocker{probeErr: true}
	runner := ContainerRunner{Run: fake.run, RequireIsolation: true}

	res, err := runner.Start(context.Background(), startReq(t))
	if err != nil {
		t.Fatalf("start: %v", err)
	}
	if !res.Refused {
		t.Fatalf("an unprovable container was handed over: %+v", res)
	}
	if !fake.ran("rm") {
		t.Errorf("the refused container was left running: %v", fake.calls)
	}
}

// Without the requirement an unprovable container runs, but not silently.
func TestContainerStartWarnsWhenIsolationNotRequired(t *testing.T) {
	fake := &fakeDocker{probeErr: true}
	runner := ContainerRunner{Run: fake.run}

	res, err := runner.Start(context.Background(), startReq(t))
	if err != nil {
		t.Fatalf("start: %v", err)
	}
	if res.Refused {
		t.Errorf("refused without the requirement: %+v", res)
	}
	if !res.Warned || res.Reason == "" {
		t.Errorf("an unverified container ran with nothing reported: %+v", res)
	}
	if fake.ran("rm") {
		t.Errorf("destroyed a container it decided to run: %v", fake.calls)
	}
}

// A container that was created but could not start still exists, so it is
// cleaned up rather than left behind.
func TestContainerStartCleansUpWhenStartFails(t *testing.T) {
	fake := &fakeDocker{failOn: "start"}
	runner := ContainerRunner{Run: fake.run}

	if _, err := runner.Start(context.Background(), startReq(t)); err == nil {
		t.Fatal("a failed start was reported as success")
	}
	if !fake.ran("rm") {
		t.Errorf("the created container was left behind: %v", fake.calls)
	}
}

// Nothing was created, so there is nothing to clean up and no probe to run.
func TestContainerStartCreateFailure(t *testing.T) {
	fake := &fakeDocker{failOn: "create"}
	runner := ContainerRunner{Run: fake.run}

	if _, err := runner.Start(context.Background(), startReq(t)); err == nil {
		t.Fatal("a failed create was reported as success")
	}
	if fake.ran("inspect") {
		t.Errorf("probed a container that was never created: %v", fake.calls)
	}
}

// A spec that lost its guarantees must never reach the daemon.
func TestContainerStartRefusesABrokenSpecBeforeRunningAnything(t *testing.T) {
	fake := &fakeDocker{}
	runner := ContainerRunner{Run: fake.run}

	broken := DockerCreateRequest{
		Spec: SandboxSpec{
			ReadOnly: true,
			Mounts:   []SandboxMount{{Source: "/srv/repo", Target: "/srv/repo"}},
		},
		ContainerName: "c1", Image: "ubuntu:22.04",
	}
	if _, err := runner.Start(context.Background(), broken); err == nil {
		t.Fatal("a broken spec was accepted")
	}
	if len(fake.calls) != 0 {
		t.Errorf("ran commands for a spec that should have been refused: %v", fake.calls)
	}
}

// The delegate's work is already on the host via the bind mount, so stopping
// discards only the container.
func TestContainerStop(t *testing.T) {
	fake := &fakeDocker{}
	runner := ContainerRunner{Run: fake.run}

	if err := runner.Stop(context.Background(), "aimee-delegate-1"); err != nil {
		t.Fatalf("stop: %v", err)
	}
	if !fake.ran("rm") {
		t.Errorf("stop did not remove the container: %v", fake.calls)
	}
	if err := runner.Stop(context.Background(), "  "); err == nil {
		t.Error("an empty container name was accepted")
	}
}

func TestContainerRunnerRequiresACommandRunner(t *testing.T) {
	var runner ContainerRunner
	if _, err := runner.Start(context.Background(), startReq(t)); err == nil {
		t.Error("started with no command runner")
	}
	if err := runner.Stop(context.Background(), "c1"); err == nil {
		t.Error("stopped with no command runner")
	}
}
