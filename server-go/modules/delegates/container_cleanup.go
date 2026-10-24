package delegates

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"strings"
	"time"
)

func (r ContainerRunner) ownedAbsent(ctx context.Context, id string) (bool, error) {
	output, err := r.Run(ctx, r.docker(), "ps", "-aq", "--no-trunc", "--filter", "id="+id)
	return err == nil && strings.TrimSpace(output) == "", err
}

// CleanupOwned accepts only a retained creation intent. Stopped configuration
// proves teardown custody, never execution admission or runtime isolation.
func (r ContainerRunner) CleanupOwned(ctx context.Context, req DockerCreateRequest, id string) (string, error) {
	req.Spec.ownerHelper = true
	if r.Run == nil || !r.RequireImmutableIdentity {
		return "", errors.New("owned cleanup transport unavailable")
	}
	if _, err := DockerCreateArgs(req); err != nil {
		return "", err
	}
	owned, cancel := context.WithTimeout(context.WithoutCancel(ctx), 30*time.Second)
	defer cancel()
	resolving := id == ""
	if id != "" {
		if !immutableContainerID(id) {
			return "", errors.New("invalid durable creation identity")
		}
		absent, err := r.ownedAbsent(owned, id)
		if err != nil {
			return "", err
		}
		if absent {
			return id, nil
		}
		// A durable created/resolved identity already proves this exact resource
		// belongs to the lease. Refused posture is a reason to retire it, not a
		// condition that can revoke teardown custody.
		return r.removeOwned(owned, id)
	} else {
		output, err := r.Run(owned, r.docker(), "inspect", "--format", identityProbeFormat, req.ContainerName)
		id = strings.TrimSpace(output)
		if err != nil || !immutableContainerID(id) {
			return "", errors.New("creation outcome unresolved; retain intent")
		}
	}
	raw, err := r.Run(owned, r.docker(), "inspect", "--format", receiptProbeFormat, id)
	var observed containerProfile
	if err != nil || len(raw) > 1<<20 || json.Unmarshal([]byte(raw), &observed) != nil ||
		observed.ID != id || observed.Name != "/"+req.ContainerName || observed.Config.Image != req.Image ||
		!strings.HasPrefix(observed.Image, "sha256:") || !immutableContainerID(strings.TrimPrefix(observed.Image, "sha256:")) ||
		observed.Config.WorkingDir != req.WorkDir || observed.Config.Env == nil || !ownerStartupMatches(req, observed) {
		return "", errors.New("creation intent does not attest this container; retain journal")
	}
	// Docker records requested binds even when creation completed before start.
	var configured strings.Builder
	for _, bind := range observed.Host.Binds {
		parts := strings.Split(bind, ":")
		if len(parts) < 2 || len(parts) > 3 || (len(parts) == 3 && parts[2] != "ro" && parts[2] != "rw") {
			return "", errors.New("unrecognized retained bind configuration")
		}
		readonly := len(parts) == 3 && parts[2] == "ro"
		fmt.Fprintf(&configured, "%s|%s|%t;", parts[0], parts[1], !readonly)
	}
	if verifyMountReport(req.Spec, req.MountTable, configured.String()) != nil || verifyEnvironmentReport(req.Spec, strings.Join(*observed.Config.Env, "\n")) != nil {
		return "", errors.New("private creation scope differs; retain journal")
	}
	if observed.State.Running {
		var mounted strings.Builder
		for _, mount := range observed.Mounts {
			if mount.RW == nil {
				return "", errors.New("actual mount mode unavailable")
			}
			fmt.Fprintf(&mounted, "%s|%s|%t;", mount.Source, mount.Destination, *mount.RW)
		}
		if verifyMountReport(req.Spec, req.MountTable, mounted.String()) != nil {
			return "", errors.New("actual private mounts differ; retain journal")
		}
	}
	if resolving && r.Journal != nil {
		if err := r.Journal.Created(id); err != nil {
			return "", fmt.Errorf("persist resolved cleanup identity: %w", err)
		}
	}
	return r.removeOwned(owned, id)
}

func (r ContainerRunner) removeOwned(ctx context.Context, id string) (string, error) {
	if err := r.destroy(ctx, id); err != nil {
		return "", err
	}
	absent, err := r.ownedAbsent(ctx, id)
	if err != nil || !absent {
		return "", errors.New("owned removal not verified; retain journal")
	}
	return id, nil
}
