package delegates

import (
	"bytes"
	"context"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"strings"
	"time"

	"github.com/JBailes/aimee/server-go/modules/egress"
)

// PrivateTestRequest travels only from the configured verifier owner. Neither
// model output nor a delegate's self-reported test result is an input.
type PrivateTestRequest struct {
	Schema         string            `json:"schema"`
	ConfigSHA256   string            `json:"config_sha256"`
	SourceRevision string            `json:"source_revision"`
	SnapshotID     string            `json:"snapshot_id"`
	StepTimeoutMS  int64             `json:"step_timeout_ms"`
	Steps          []PrivateTestStep `json:"steps"`
	EnvChecks      []string          `json:"env_checks,omitempty"`
}

type PrivateTestStep struct {
	Name  string `json:"name"`
	Run   string `json:"run"`
	After string `json:"after"`
}

type PrivateTestStepReceipt struct {
	Name        string                        `json:"name"`
	RunSHA256   string                        `json:"run_sha256"`
	Status      string                        `json:"status"`
	Observation *egress.DockerExecObservation `json:"observation,omitempty"`
	Error       string                        `json:"error,omitempty"`
}

type PrivateTestReceipt struct {
	Schema                   string                      `json:"schema"`
	ConfigSHA256             string                      `json:"config_sha256"`
	SourceRevision           string                      `json:"source_revision"`
	SnapshotID               string                      `json:"snapshot_id"`
	MountIdentityVerified    bool                        `json:"mount_identity_verified"`
	Profile                  PrivateTestContainerProfile `json:"profile"`
	Steps                    []PrivateTestStepReceipt    `json:"steps"`
	EnvChecks                []PrivateTestStepReceipt    `json:"env_checks,omitempty"`
	Complete                 bool                        `json:"complete"`
	Passed                   bool                        `json:"passed"`
	Quiescent                bool                        `json:"quiescent"`
	RemovalAcknowledged      bool                        `json:"removal_acknowledged"`
	ConfiguredStepsStarted   int                         `json:"configured_steps_started"`
	OrdinaryFallbackEligible bool                        `json:"ordinary_fallback_eligible"`
	Error                    string                      `json:"error,omitempty"`
}

func hexDigest(s string) bool { return immutableContainerID(s) }

func DecodePrivateTestRequest(reader io.Reader) (PrivateTestRequest, error) {
	var request PrivateTestRequest
	encoded, err := io.ReadAll(io.LimitReader(reader, (64<<10)+1))
	if err != nil || len(encoded) > 64<<10 {
		return request, errors.New("private test request exceeds its bound")
	}
	decoder := json.NewDecoder(strings.NewReader(string(encoded)))
	decoder.DisallowUnknownFields()
	if err := decoder.Decode(&request); err != nil {
		return request, err
	}
	var trailing any
	if decoder.Decode(&trailing) != io.EOF {
		return request, errors.New("trailing private test request")
	}
	return request, validatePrivateTestRequest(request)
}

func validatePrivateTestRequest(request PrivateTestRequest) error {
	if request.Schema != "aimee.private-tests.request.v1" || !hexDigest(request.ConfigSHA256) || len(request.SnapshotID) != 32 || strings.Trim(request.SnapshotID, "0123456789abcdef") != "" ||
		request.SourceRevision == "" || len(request.SourceRevision) > 4096 || strings.ContainsRune(request.SourceRevision, 0) ||
		len(request.Steps) == 0 || len(request.Steps) > 16 || request.StepTimeoutMS < 1 || request.StepTimeoutMS > 24*60*60*1000 {
		return errors.New("invalid configured private test profile")
	}
	if len(request.EnvChecks) > 16 {
		return errors.New("too many configured environment prerequisites")
	}
	for _, tool := range request.EnvChecks {
		if tool == "" || len(tool) >= 64 {
			return errors.New("invalid configured prerequisite tool")
		}
		for i, c := range tool {
			if !(c >= 'a' && c <= 'z' || c >= 'A' && c <= 'Z' || c >= '0' && c <= '9' || c == '_' || (i > 0 && (c == '.' || c == '+' || c == '-'))) {
				return errors.New("configured prerequisites must be simple tool names")
			}
		}
	}
	names := make(map[string]int)
	for i, step := range request.Steps {
		if step.Name == "" || len(step.Name) >= 64 || len(step.Run) == 0 || len(step.Run) >= 512 || strings.ContainsRune(step.Run, 0) || strings.ContainsRune(step.Name, 0) {
			return errors.New("invalid configured private test step")
		}
		if _, duplicate := names[step.Name]; duplicate {
			return errors.New("duplicate configured private test step")
		}
		names[step.Name] = i
	}
	for _, step := range request.Steps {
		seen := make(map[string]bool)
		for name := step.Name; name != ""; {
			if seen[name] {
				return errors.New("cyclic configured private test dependency")
			}
			seen[name] = true
			index, found := names[name]
			if !found {
				return errors.New("unknown configured private test dependency")
			}
			name = request.Steps[index].After
		}
	}
	return nil
}

// PrivateTestFacts are trusted host facts, never fields from a model transcript.
type PrivateTestFacts struct {
	Image        string
	WorkDir      string
	User         string
	MountTable   string
	WorkerLabels map[string]string
}

func privateTestSpec(facts PrivateTestFacts) (SandboxSpec, error) {
	var empty SandboxSpec
	clean, ok := cleanAbs(facts.WorkDir)
	if !ok || clean != facts.WorkDir || strings.ContainsAny(clean, ":|;\r\n") || !baseImageValid(facts.Image) {
		return empty, errors.New("private snapshot/image unavailable")
	}
	uid, gid, ok := strings.Cut(facts.User, ":")
	if !ok || uid == "" || gid == "" || strings.Trim(uid, "0123456789") != "" || strings.Trim(gid, "0123456789") != "" {
		return empty, errors.New("numeric private worker identity required")
	}
	for _, path := range []string{facts.WorkDir, filepath.Join(facts.WorkDir, ".git")} {
		info, err := os.Lstat(path)
		if err != nil || !info.IsDir() || info.Mode()&os.ModeSymlink != 0 {
			return empty, errors.New("private tests require a self-contained Git snapshot")
		}
	}
	spec := SandboxSpec{privateTest: true, User: facts.User, Mounts: []SandboxMount{{Source: clean, Target: clean, Kind: SandboxWorkspace}},
		Env: []SandboxEnv{{Name: "GIT_CONFIG_COUNT", Value: "1"}, {Name: "GIT_CONFIG_KEY_0", Value: "safe.directory"}, {Name: "GIT_CONFIG_VALUE_0", Value: clean}}}
	return spec, ValidateSandboxSpec(spec)
}

type privateTestEngine interface {
	CreateContainer(context.Context, any) (string, error)
	StartContainer(context.Context, string) error
	InspectContainer(context.Context, string) (json.RawMessage, error)
	Exec(context.Context, string, string, string) (egress.DockerExecObservation, error)
	Remove(context.Context, string) error
}

// ExecutePrivateTests executes every configured command in a fresh worker with
// no parent socket, network or shared Git metadata. A complete receipt requires
// independent Engine observations and acknowledged removal, including failure
// and cancellation paths. It never labels a cached/skipped command as passed.
func ExecutePrivateTests(ctx context.Context, request PrivateTestRequest, facts PrivateTestFacts, engine *egress.DockerEngine) PrivateTestReceipt {
	if engine == nil {
		return PrivateTestReceipt{Schema: "aimee.private-tests.receipt.v1", Error: "held Docker engine unavailable"}
	}
	return executePrivateTests(ctx, request, facts, engine, func(ctx context.Context, req DockerCreateRequest, id string) (PrivateTestContainerProfile, error) {
		return (ContainerRunner{MountTable: facts.MountTable}).AttestPrivateTestContainer(ctx, req, id, engine)
	})
}

func executePrivateTests(ctx context.Context, request PrivateTestRequest, facts PrivateTestFacts, engine privateTestEngine,
	attest func(context.Context, DockerCreateRequest, string) (PrivateTestContainerProfile, error)) (receipt PrivateTestReceipt) {
	receipt = PrivateTestReceipt{Schema: "aimee.private-tests.receipt.v1", ConfigSHA256: request.ConfigSHA256, SourceRevision: request.SourceRevision, SnapshotID: request.SnapshotID}
	if err := validatePrivateTestRequest(request); err != nil {
		receipt.Error = err.Error()
		return
	}
	spec, err := privateTestSpec(facts)
	if err != nil {
		receipt.Error = err.Error()
		return
	}
	if engine == nil || attest == nil {
		receipt.Error = "held Docker engine unavailable"
		return
	}
	for _, step := range request.Steps {
		digest := sha256.Sum256([]byte(step.Run))
		receipt.Steps = append(receipt.Steps, PrivateTestStepReceipt{Name: step.Name, RunSHA256: hex.EncodeToString(digest[:]), Status: "unavailable"})
	}
	binds := []string{renderBind(spec.Mounts[0], facts.MountTable)}
	env := make([]string, 0, len(spec.Env))
	for _, entry := range spec.Env {
		env = append(env, entry.Name+"="+entry.Value)
	}
	configuration := map[string]any{"Labels": facts.WorkerLabels, "Image": facts.Image, "User": facts.User, "WorkingDir": facts.WorkDir, "Env": env,
		"Cmd": []string{"sleep", "infinity"}, "Entrypoint": []string{""}, "Healthcheck": map[string]any{"Test": []string{"NONE"}}, "HostConfig": map[string]any{"NetworkMode": "none", "Binds": binds, "Privileged": false, "IpcMode": "private"}}
	id, err := engine.CreateContainer(ctx, configuration)
	if err != nil || !immutableContainerID(id) {
		receipt.Error = "fresh Docker creation unacknowledged"
		return
	}
	receipt.Profile.ContainerID = id
	profileAttested, preflightUnavailable := false, false
	defer func() {
		cleanup, cancel := context.WithTimeout(context.WithoutCancel(ctx), 15*time.Second)
		defer cancel()
		if err := engine.Remove(cleanup, id); err != nil {
			receipt.Complete = false
			receipt.Passed = false
			receipt.Error = "private worker removal unacknowledged"
			return
		}
		receipt.RemovalAcknowledged = true
		receipt.Quiescent = true
		// Only a joined environment-preflight refusal can preserve the old
		// ordinary runner. A missing completion receipt after any configured
		// attempt is never evidence that the command had no effects.
		receipt.OrdinaryFallbackEligible = profileAttested && receipt.MountIdentityVerified && preflightUnavailable && receipt.ConfiguredStepsStarted == 0 && ctx.Err() == nil
	}()
	if err := engine.StartContainer(ctx, id); err != nil {
		receipt.Error = "fresh Docker start unacknowledged"
		return
	}
	profile, err := attest(ctx, DockerCreateRequest{Spec: spec, WorkDir: facts.WorkDir, Image: facts.Image}, id)
	if err != nil {
		receipt.Error = err.Error()
		return
	}
	receipt.Profile = profile
	profileAttested = true
	// Runtime mount paths are descriptive, not source identity. Read a host-
	// random marker through the worker itself before any prerequisite or test.
	// Never put the expected nonce in the native command or environment.
	markerCtx, markerCancel := context.WithTimeout(ctx, 15*time.Second)
	marker, markerErr := engine.Exec(markerCtx, id, facts.WorkDir, "cat -- .git/aimee-verification-lease")
	markerCancel()
	if markerErr != nil || !marker.Complete || marker.ContainerID != id || !immutableContainerID(marker.ExecID) || marker.Termination != "exited" || marker.ExitCode != "0" || marker.StdoutOmitted != "0" || !bytes.Equal(marker.Stdout, []byte(request.SnapshotID+"\n")) {
		receipt.Error = "private snapshot mount identity unverified"
		return
	}
	receipt.MountIdentityVerified = true
	// Prerequisites are independently observed in this exact worker before any
	// configured test can run. A missing tool is infrastructure, never a test
	// failure or a negative work-product grade.
	for _, tool := range request.EnvChecks {
		command := "command -v -- " + tool
		digest := sha256.Sum256([]byte(command))
		receipt.EnvChecks = append(receipt.EnvChecks, PrivateTestStepReceipt{Name: tool, RunSHA256: hex.EncodeToString(digest[:]), Status: "unavailable"})
	}
	for i, tool := range request.EnvChecks {
		if ctx.Err() != nil {
			receipt.Error = "verification cancelled before environment prerequisites"
			return
		}
		checkCtx, cancel := context.WithTimeout(ctx, time.Duration(request.StepTimeoutMS)*time.Millisecond)
		observation, err := engine.Exec(checkCtx, id, facts.WorkDir, "command -v -- "+tool)
		cancel()
		if err != nil || !observation.Complete || observation.ContainerID != id || !immutableContainerID(observation.ExecID) || observation.Termination != "exited" {
			receipt.EnvChecks[i].Error = "independent prerequisite execution unavailable"
			receipt.Error = "configured test environment unverified"
			preflightUnavailable = true
			return
		}
		receipt.EnvChecks[i].Observation = &observation
		if observation.ExitCode != "0" {
			receipt.EnvChecks[i].Error = "configured prerequisite tool unavailable"
			receipt.Error = "configured test environment unavailable"
			preflightUnavailable = true
			return
		}
		receipt.EnvChecks[i].Status = "passed"
	}
	indices := make(map[string]int)
	for i, step := range request.Steps {
		indices[step.Name] = i
	}
	done := make([]bool, len(request.Steps))
	left := len(done)
	for left > 0 {
		progress := false
		for i, step := range request.Steps {
			if done[i] {
				continue
			}
			if step.After != "" {
				dep := indices[step.After]
				if !done[dep] {
					continue
				}
				if receipt.Steps[dep].Status != "passed" {
					receipt.Steps[i].Status = "dependency_failed"
					done[i] = true
					left--
					progress = true
					continue
				}
			}
			done[i] = true
			left--
			progress = true
			if ctx.Err() != nil {
				receipt.Steps[i].Error = "verification cancelled"
				continue
			}
			stepCtx, cancel := context.WithTimeout(ctx, time.Duration(request.StepTimeoutMS)*time.Millisecond)
			receipt.ConfiguredStepsStarted++ // claim before any potentially effective native request
			observed, err := engine.Exec(stepCtx, id, facts.WorkDir, step.Run)
			cancel()
			if err != nil {
				receipt.Steps[i].Error = fmt.Sprintf("independent execution unavailable: %v", err)
				receipt.Error = "private execution ownership unresolved"
				// The timed-out exec may still be live. Join the worker before
				// attempting any further configured command.
				return
			}
			if !observed.Complete || observed.ContainerID != id || !immutableContainerID(observed.ExecID) || observed.Termination != "exited" {
				receipt.Steps[i].Error = "independent normal completion unavailable"
				receipt.Error = "private execution ownership unresolved"
				return
			}
			if observed.ExitCode == "124" || observed.ExitCode == "126" || observed.ExitCode == "127" {
				receipt.Steps[i].Error = "configured test infrastructure unavailable"
				continue
			}
			receipt.Steps[i].Observation = &observed
			if observed.ExitCode == "0" {
				receipt.Steps[i].Status = "passed"
			} else {
				receipt.Steps[i].Status = "failed"
			}
		}
		if !progress {
			receipt.Error = "configured dependency could not progress"
			return
		}
	}
	receipt.Complete = true
	receipt.Passed = true
	for _, step := range receipt.Steps {
		if step.Observation == nil {
			receipt.Complete = false
		}
		if step.Status != "passed" {
			receipt.Passed = false
		}
	}
	if ctx.Err() != nil {
		receipt.Complete = false
		receipt.Passed = false
		receipt.Error = "verification cancelled"
	}
	return
}
