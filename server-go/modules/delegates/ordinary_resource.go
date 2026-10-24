package delegates

import (
	"context"
	"encoding/json"
	"errors"
	"reflect"
	"slices"
	"strings"
)

// ResolveOrdinaryImage observes inherited startup without executing image code.
// Pinning the actual image ID makes the intent's startup contract immutable.
func ResolveOrdinaryImage(ctx context.Context, run CommandRunner, docker, image string) (string, *ImageStartup, error) {
	if run == nil || !baseImageValid(image) {
		return "", nil, errors.New("ordinary image reference/transport unavailable")
	}
	output, err := run(ctx, docker, "image", "inspect", "--format", `{"id":{{json .Id}},"entrypoint":{{json (index .Config "Entrypoint")}},"healthcheck":{{json (index .Config "Healthcheck")}}}`, image)
	var observed struct {
		ID          string
		Entrypoint  []string
		Healthcheck *ImageHealthcheck
	}
	if err != nil || len(output) > 65536 || json.Unmarshal([]byte(output), &observed) != nil || !strings.HasPrefix(observed.ID, "sha256:") || !immutableContainerID(strings.TrimPrefix(observed.ID, "sha256:")) {
		return "", nil, errors.New("actual ordinary image startup identity unavailable")
	}
	return observed.ID, &ImageStartup{Entrypoint: observed.Entrypoint, Healthcheck: observed.Healthcheck}, nil
}

func ordinaryStartupMatches(req DockerCreateRequest, observed containerProfile) bool {
	return req.Startup != nil && observed.Config.Labels[OwnerLeaseLabel] == req.LeaseNonce && observed.Image == req.Image && observed.Config.Image == req.Image &&
		slices.Equal(observed.Config.Entrypoint, req.Startup.Entrypoint) && slices.Equal(observed.Config.Cmd, req.Command) && reflect.DeepEqual(observed.Config.Healthcheck, req.Startup.Healthcheck) &&
		observed.Host.Privileged != nil && !*observed.Host.Privileged && observed.Host.NetworkMode == "none" && observed.Host.PidMode == "" && (observed.Host.IpcMode == "" || observed.Host.IpcMode == "private") && len(observed.Host.CapAdd) == 0 && len(observed.Host.Devices) == 0 && (req.Spec.User == "" || req.Spec.User == observed.Config.User)
}
