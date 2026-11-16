package delegates

import (
	"context"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"strings"

	"github.com/JBailes/aimee/server-go/modules/egress"
)

type PrivateTestContainerProfile struct {
	ContainerID string `json:"container_id"`
	ImageID     string `json:"image_id"`
	Digest      string `json:"profile_sha256"`
}

func immutableContainerID(id string) bool {
	if len(id) != 64 {
		return false
	}
	for _, c := range id {
		if !(c >= '0' && c <= '9' || c >= 'a' && c <= 'f') {
			return false
		}
	}
	return true
}

// AttestPrivateTestContainer checks the actual immutable worker through its
// held control resource. Image labels and successful CLI exits are not receipts.
func (runner ContainerRunner) AttestPrivateTestContainer(ctx context.Context, request DockerCreateRequest, id string, engine *egress.DockerEngine) (PrivateTestContainerProfile, error) {
	empty := PrivateTestContainerProfile{}
	if engine == nil || !immutableContainerID(id) {
		return empty, errors.New("fresh private worker identity unavailable")
	}
	workspaces, controls := 0, 0
	for _, mount := range request.Spec.Mounts {
		switch mount.Kind {
		case SandboxWorkspace:
			workspaces++
			if mount.Source != request.WorkDir || mount.Target != request.WorkDir {
				return empty, errors.New("private verification cannot expose another checkout")
			}
		case SandboxControlSocket:
			controls++
		default:
			return empty, errors.New("private verification requires self-contained Git metadata")
		}
	}
	expectedControls := 1
	if request.Spec.privateTest {
		expectedControls = 0
	}
	if workspaces != 1 || controls != expectedControls {
		return empty, errors.New("private worker resource set unavailable")
	}
	raw, err := engine.InspectContainer(ctx, id)
	if err != nil {
		return empty, err
	}
	var observed struct {
		ID     string                  `json:"Id"`
		Image  string                  `json:"Image"`
		State  struct{ Running *bool } `json:"State"`
		Config struct {
			User        string
			WorkingDir  string
			Env         *[]string
			Cmd         []string
			Entrypoint  []string
			Healthcheck struct{ Test []string }
		} `json:"Config"`
		Host struct {
			NetworkMode string
			Privileged  *bool
			PidMode     string
			IpcMode     string
			CapAdd      []string
			Devices     []json.RawMessage
		} `json:"HostConfig"`
		Network struct{ Networks json.RawMessage } `json:"NetworkSettings"`
		Mounts  []struct {
			Source      string
			Destination string
			RW          *bool
		} `json:"Mounts"`
	}
	if json.Unmarshal(raw, &observed) != nil || observed.ID != id ||
		!strings.HasPrefix(observed.Image, "sha256:") || !immutableContainerID(strings.TrimPrefix(observed.Image, "sha256:")) ||
		observed.State.Running == nil || !*observed.State.Running ||
		observed.Host.Privileged == nil || *observed.Host.Privileged || observed.Host.NetworkMode != "none" ||
		observed.Host.PidMode != "" || (observed.Host.IpcMode != "" && observed.Host.IpcMode != "private") ||
		len(observed.Host.CapAdd) != 0 || len(observed.Host.Devices) != 0 ||
		observed.Config.Env == nil || observed.Config.WorkingDir != request.WorkDir || (request.Spec.User != "" && observed.Config.User != request.Spec.User) {
		return empty, errors.New("private test worker profile unverified")
	}
	if request.Spec.privateTest && (len(observed.Config.Entrypoint) != 0 || len(observed.Config.Healthcheck.Test) != 1 || observed.Config.Healthcheck.Test[0] != "NONE" || len(observed.Config.Cmd) != 2 || observed.Config.Cmd[0] != "sleep" || observed.Config.Cmd[1] != "infinity") {
		return empty, errors.New("private test worker process unverified")
	}
	var mounts strings.Builder
	for _, mount := range observed.Mounts {
		if mount.RW == nil {
			return empty, errors.New("private worker mount mode unavailable")
		}
		fmt.Fprintf(&mounts, "%s|%s|%t;", mount.Source, mount.Destination, *mount.RW)
	}
	verdict := VerifyRuntimePosture(request.Spec, runner.MountTable, RuntimePostureReport{
		Network: string(observed.Network.Networks), Mounts: mounts.String(),
		Environment: strings.Join(*observed.Config.Env, "\n"),
	})
	if verdict.Refuse {
		return empty, errors.New("private test worker custody differs from approved profile")
	}
	// Instance paths belong to the snapshot identity. Cohorts bind the observed
	// immutable image, user and the enforced private-worker policy version.
	policy := "private-project-tests-v1"
	if request.Spec.privateTest {
		policy = "private-project-tests-no-control-v2"
	}
	profile, _ := json.Marshal([]any{policy, observed.Image, observed.Config.User,
		observed.Host.NetworkMode, observed.Host.PidMode, observed.Host.IpcMode, request.Spec.ReadOnly})
	digest := sha256.Sum256(profile)
	return PrivateTestContainerProfile{ContainerID: id, ImageID: observed.Image, Digest: hex.EncodeToString(digest[:])}, nil
}
