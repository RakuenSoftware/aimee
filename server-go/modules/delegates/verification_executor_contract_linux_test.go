package delegates

import (
	"context"
	"encoding/json"
	"errors"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/JBailes/aimee/server-go/modules/egress"
)

type verifierEngineFixture struct {
	commands         []string
	configuration    map[string]any
	exits            []string
	removeError      bool
	cancelledCleanup bool
	failExec         bool
	markerMismatch   bool
	markerUnknown    bool
}

func (f *verifierEngineFixture) CreateContainer(_ context.Context, config any) (string, error) {
	f.configuration = config.(map[string]any)
	return strings.Repeat("a", 64), nil
}
func (f *verifierEngineFixture) StartContainer(context.Context, string) error { return nil }
func (f *verifierEngineFixture) InspectContainer(context.Context, string) (json.RawMessage, error) {
	return nil, errors.New("unused")
}
func (f *verifierEngineFixture) Exec(ctx context.Context, id, dir, command string) (egress.DockerExecObservation, error) {
	if command == "cat -- .git/aimee-verification-lease" {
		if f.markerUnknown {
			return egress.DockerExecObservation{}, errors.New("marker completion unknown")
		}
		marker := strings.Repeat("f", 32) + "\n"
		if f.markerMismatch {
			marker = "foreign\n"
		}
		return egress.DockerExecObservation{ContainerID: id, ExecID: strings.Repeat("b", 64), ExitCode: "0", Termination: "exited", Complete: true, Stdout: []byte(marker), StdoutOmitted: "0", StderrOmitted: "0"}, nil
	}
	f.commands = append(f.commands, command)
	if f.failExec {
		return egress.DockerExecObservation{}, errors.New("unobserved execution")
	}
	code := "0"
	if len(f.exits) >= len(f.commands) {
		code = f.exits[len(f.commands)-1]
	}
	return egress.DockerExecObservation{ContainerID: id, ExecID: strings.Repeat("b", 64), ExitCode: code, Termination: "exited", Complete: true, Stdout: []byte{0, 255}}, nil
}
func (f *verifierEngineFixture) Remove(ctx context.Context, _ string) error {
	f.cancelledCleanup = ctx.Err() != nil
	if f.removeError {
		return errors.New("lost removal acknowledgement")
	}
	return nil
}
func verifierFacts(t *testing.T) PrivateTestFacts {
	t.Helper()
	root := t.TempDir()
	if err := os.Mkdir(filepath.Join(root, ".git"), 0700); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(root, ".git", "aimee-verification-lease"), []byte(strings.Repeat("f", 32)+"\n"), 0400); err != nil {
		t.Fatal(err)
	}
	return PrivateTestFacts{Image: "test-image", WorkDir: root, User: "1000:1000"}
}
func verifierRequest() PrivateTestRequest {
	return PrivateTestRequest{Schema: "aimee.private-tests.request.v1", ConfigSHA256: strings.Repeat("c", 64), SourceRevision: "snapshot-commit", SnapshotID: strings.Repeat("f", 32), StepTimeoutMS: 1000, Steps: []PrivateTestStep{{Name: "test", Run: "trusted test", After: "build"}, {Name: "build", Run: "trusted build"}}}
}
func verifierAttest(_ context.Context, request DockerCreateRequest, id string) (PrivateTestContainerProfile, error) {
	if !request.Spec.privateTest || len(request.Spec.Mounts) != 1 {
		return PrivateTestContainerProfile{}, errors.New("unexpected custody")
	}
	return PrivateTestContainerProfile{ContainerID: id, ImageID: "sha256:" + strings.Repeat("d", 64), Digest: strings.Repeat("e", 64)}, nil
}
func TestPrivateVerifierReceiptsRequireExecutedCommandsAndRemoval(t *testing.T) {
	engine := &verifierEngineFixture{}
	receipt := executePrivateTests(context.Background(), verifierRequest(), verifierFacts(t), engine, verifierAttest)
	if !receipt.Complete || !receipt.Passed || !receipt.Quiescent || !receipt.RemovalAcknowledged || len(engine.commands) != 2 || engine.commands[0] != "trusted build" || engine.commands[1] != "trusted test" {
		t.Fatalf("incomplete owned execution: %+v", receipt)
	}
	configuration := engine.configuration
	host := configuration["HostConfig"].(map[string]any)
	if host["NetworkMode"] != "none" || len(host["Binds"].([]string)) != 1 || strings.Contains(strings.Join(configuration["Env"].([]string), "\n"), "AIMEE_API_ENDPOINT") {
		t.Fatal("verifier can reach parent effects")
	}
	encoded, _ := json.Marshal(receipt)
	if !strings.Contains(string(encoded), `"stdout_base64":"AP8="`) {
		t.Fatal("binary output corrupted")
	}
}
func TestPrivateVerifierNoPositiveSkippedOrUnobservedEvidence(t *testing.T) {
	for _, test := range []struct {
		name     string
		engine   verifierEngineFixture
		complete bool
		status   string
	}{
		{"failed dependency", verifierEngineFixture{exits: []string{"1"}}, false, "dependency_failed"},
		{"infrastructure", verifierEngineFixture{exits: []string{"127"}}, false, "dependency_failed"},
		{"missing exec receipt", verifierEngineFixture{failExec: true}, false, "unavailable"},
		{"missing removal", verifierEngineFixture{removeError: true}, false, "passed"},
	} {
		t.Run(test.name, func(t *testing.T) {
			receipt := executePrivateTests(context.Background(), verifierRequest(), verifierFacts(t), &test.engine, verifierAttest)
			if receipt.Complete != test.complete || receipt.Passed || receipt.Steps[0].Status != test.status {
				t.Fatalf("fabricated positive: %+v", receipt)
			}
		})
	}
}
func TestPrivateVerifierCancellationStillJoinsCleanup(t *testing.T) {
	ctx, cancel := context.WithCancel(context.Background())
	engine := &verifierEngineFixture{}
	attest := func(ctx context.Context, req DockerCreateRequest, id string) (PrivateTestContainerProfile, error) {
		cancel()
		return verifierAttest(ctx, req, id)
	}
	receipt := executePrivateTests(ctx, verifierRequest(), verifierFacts(t), engine, attest)
	if receipt.Complete || receipt.Passed || !receipt.Quiescent || !receipt.RemovalAcknowledged || engine.cancelledCleanup || len(engine.commands) != 0 {
		t.Fatalf("cancelled execution not quiescent: %+v", receipt)
	}
}
func TestPrivateVerifierRejectsBadConfigurationAndSharedMetadata(t *testing.T) {
	for _, mutate := range []func(*PrivateTestRequest){func(r *PrivateTestRequest) { r.Steps[1].After = "test" }, func(r *PrivateTestRequest) { r.Steps[0].After = "unknown" }, func(r *PrivateTestRequest) { r.Steps[0].Run = strings.Repeat("a", 512) }, func(r *PrivateTestRequest) { r.Steps = nil }} {
		request := verifierRequest()
		mutate(&request)
		if validatePrivateTestRequest(request) == nil {
			t.Fatal("invalid graph/profile accepted")
		}
	}
	facts := verifierFacts(t)
	os.RemoveAll(filepath.Join(facts.WorkDir, ".git"))
	os.WriteFile(filepath.Join(facts.WorkDir, ".git"), []byte("gitdir: /shared"), 0600)
	receipt := executePrivateTests(context.Background(), verifierRequest(), facts, &verifierEngineFixture{}, verifierAttest)
	if receipt.Error == "" || receipt.Quiescent {
		t.Fatal("linked metadata became private test custody")
	}
}

func TestPrivateVerifierChecksActualWorkerPrerequisitesBeforeTests(t *testing.T) {
	request := verifierRequest()
	request.EnvChecks = []string{"bash", "git"}
	engine := &verifierEngineFixture{}
	receipt := executePrivateTests(context.Background(), request, verifierFacts(t), engine, verifierAttest)
	if !receipt.Complete || !receipt.Passed || len(receipt.EnvChecks) != 2 || len(engine.commands) != 4 || engine.commands[0] != "command -v -- bash" || engine.commands[1] != "command -v -- git" || engine.commands[2] != "trusted build" {
		t.Fatalf("prerequisites not bound to actual worker: %+v / %v", receipt, engine.commands)
	}
	for _, check := range receipt.EnvChecks {
		if check.Status != "passed" || check.Observation == nil || check.Observation.ContainerID != receipt.Profile.ContainerID {
			t.Fatal("missing independent prerequisite observation", check)
		}
	}
	for _, fixture := range []verifierEngineFixture{{exits: []string{"1"}}, {exits: []string{"127"}}, {failExec: true}} {
		receipt = executePrivateTests(context.Background(), request, verifierFacts(t), &fixture, verifierAttest)
		if receipt.Complete || receipt.Passed || !receipt.Quiescent || !receipt.RemovalAcknowledged || len(fixture.commands) != 1 || receipt.Steps[0].Observation != nil || receipt.EnvChecks[0].Status != "unavailable" {
			t.Fatalf("missing environment became test grade: %+v / %v", receipt, fixture.commands)
		}
	}
	for _, tool := range []string{"", "-bad", "git;echo forged", "$(malicious)", "/bin/sh", strings.Repeat("a", 64)} {
		invalid := request
		invalid.EnvChecks = []string{tool}
		if validatePrivateTestRequest(invalid) == nil {
			t.Fatalf("unsafe prerequisite name accepted: %q", tool)
		}
	}
}

func TestPrivateVerifierDeclinesOnlyJoinedPreflightBeforeConfiguredAttempts(t *testing.T) {
	for _, mode := range []string{"missing prerequisite", "unknown prerequisite completion", "unknown configured completion", "unacknowledged cleanup", "cancelled"} {
		t.Run(mode, func(t *testing.T) {
			request := verifierRequest()
			engine := &verifierEngineFixture{}
			expectedEligible, expectedStarted := false, 0
			ctx, cancel := context.WithCancel(context.Background())
			defer cancel()
			switch mode {
			case "missing prerequisite":
				request.EnvChecks = []string{"absent_tool"}
				engine.exits = []string{"1"}
				expectedEligible = true
			case "unknown prerequisite completion":
				request.EnvChecks = []string{"bash"}
				engine.failExec = true
				expectedEligible = true
			case "unknown configured completion":
				engine.failExec = true
				expectedStarted = 1
			case "unacknowledged cleanup":
				request.EnvChecks = []string{"absent_tool"}
				engine.exits = []string{"1"}
				engine.removeError = true
			case "cancelled":
				cancel()
			}
			receipt := executePrivateTests(ctx, request, verifierFacts(t), engine, verifierAttest)
			if receipt.OrdinaryFallbackEligible != expectedEligible || receipt.ConfiguredStepsStarted != expectedStarted || receipt.Complete || receipt.Passed {
				t.Fatalf("unsafe ordinary fallback eligibility: %+v", receipt)
			}
		})
	}
}

func TestPrivateVerifierRequiresActualRandomSnapshotMarker(t *testing.T) {
	for _, engine := range []*verifierEngineFixture{{markerMismatch: true}, {markerUnknown: true}} {
		request := verifierRequest()
		request.EnvChecks = []string{"bash"}
		receipt := executePrivateTests(context.Background(), request, verifierFacts(t), engine, verifierAttest)
		if receipt.Complete || receipt.Passed || receipt.MountIdentityVerified || receipt.OrdinaryFallbackEligible || !receipt.Quiescent || !receipt.RemovalAcknowledged || receipt.ConfiguredStepsStarted != 0 || len(engine.commands) != 0 {
			t.Fatalf("path report replaced actual private snapshot identity: %+v / %v", receipt, engine.commands)
		}
	}
}
