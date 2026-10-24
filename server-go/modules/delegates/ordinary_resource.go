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
	const format = `{"id":{{json .Id}},"entrypoint":{{json (index .Config "Entrypoint")}},"healthcheck":{{json (index .Config "Healthcheck")}}}`
	output, err := run(ctx, docker, "image", "inspect", "--format", format, image)
	// Docker create normally acquires an absent named image on first use. Pinning
	// its startup must preserve that contract without refreshing a present tag or
	// mistaking an authentication/transport/format failure for absence.
	// Docker26 reports familiar DockerHub names and adds :latest only when
	// the final repository component has no tag. These equivalents classify
	// only the missing-image error; the original reference is always pulled.
	missingReference := image
	switch {
	case strings.HasPrefix(image, "docker.io/"):
		missingReference = strings.TrimPrefix(image, "docker.io/")
	case strings.HasPrefix(image, "index.docker.io/"):
		missingReference = strings.TrimPrefix(image, "index.docker.io/")
	}
	missingReference = strings.TrimPrefix(missingReference, "library/")
	if !strings.Contains(missingReference[strings.LastIndex(missingReference, "/")+1:], ":") {
		missingReference += ":latest"
	}
	missingError := strings.TrimSpace(output)
	missing := err != nil && ctx.Err() == nil && !errors.Is(err, context.Canceled) &&
		!errors.Is(err, context.DeadlineExceeded) && !strings.HasPrefix(image, "sha256:") &&
		!immutableContainerID(image) && (missingError == "Error response from daemon: No such image: "+image ||
		missingError == "Error response from daemon: No such image: "+missingReference)
	if missing {
		if _, pullErr := run(ctx, docker, "image", "pull", "--quiet", image); pullErr != nil || ctx.Err() != nil {
			return "", nil, errors.New("actual ordinary image startup identity unavailable")
		}
		output, err = run(ctx, docker, "image", "inspect", "--format", format, image)
	}
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
