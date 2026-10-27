package delegates

import (
	"context"
	"encoding/json"
	"errors"
	"strings"
	"testing"
)

func TestRetainedIntentCleanupAttestsCreatedStoppedResources(t *testing.T) {
	for _, defect := range []string{"", "running", "foreign-name", "image", "helper-bind", "capability", "healthcheck", "env", "missing-name", "known-absent", "known-refused-volume", "rm-fails"} {
		t.Run(defect, func(t *testing.T) {
			source := "/run/aimee/acp-owners/" + strings.Repeat("a", 64)
			request := writeReq()
			request.ParentSocketHost = source + "/aimee-http.sock"
			spec, err := BuildOwnerSandboxSpec(request, source+"/owner-io")
			if err != nil {
				t.Fatal(err)
			}
			req := DockerCreateRequest{Spec: spec, ContainerName: "retained-owner", Image: "configured-image", WorkDir: request.Worktree}
			id := strings.Repeat("d", 64)
			removed := false
			observed := containerProfile{ID: id, Name: "/" + req.ContainerName, Image: "sha256:" + strings.Repeat("b", 64)}
			observed.Config.Image = req.Image
			observed.Config.WorkingDir = req.WorkDir
			observed.Config.Entrypoint = []string{OwnerHelperTarget}
			observed.Config.Cmd = []string{"--idle-guard"}
			observed.Config.Healthcheck = &ImageHealthcheck{Test: []string{"NONE"}}
			var environment []string
			for _, entry := range spec.Env {
				environment = append(environment, entry.Name+"="+entry.Value)
			}
			observed.Config.Env = &environment
			no := false
			observed.Host.Privileged = &no
			observed.Host.NetworkMode = "none"
			observed.Host.CapDrop = []string{"ALL"}
			observed.Host.SecurityOpt = []string{"no-new-privileges"}
			for _, mount := range spec.Mounts {
				observed.Host.Binds = append(observed.Host.Binds, renderBind(mount, ""))
				writable := !mount.ReadOnly
				observed.Mounts = append(observed.Mounts, struct {
					Source      string
					Destination string
					RW          *bool
				}{mount.Source, mount.Target, &writable})
			}
			switch defect {
			case "running":
				observed.State.Running = true
			case "known-refused-volume":
				observed.State.Running = true
				writable := true
				observed.Mounts = append(observed.Mounts, struct {
					Source      string
					Destination string
					RW          *bool
				}{"/anonymous-owned-volume", "/image-volume", &writable})
			case "foreign-name":
				observed.Name = "/foreign"
			case "image":
				observed.Config.Image = "other-image"
			case "helper-bind":
				observed.Host.Binds = observed.Host.Binds[:len(observed.Host.Binds)-1]
			case "capability":
				observed.Host.CapAdd = []string{"SYS_ADMIN"}
			case "healthcheck":
				observed.Config.Healthcheck.Test = []string{"CMD-SHELL", "opaque-code"}
			case "env":
				observed.Config.Env = nil
			}
			run := func(call context.Context, _ string, args ...string) (string, error) {
				if call.Err() != nil {
					t.Fatal("cleanup used canceled request")
				}
				switch args[0] {
				case "inspect":
					if defect == "known-refused-volume" {
						t.Fatal("durable ownership was gated on refused current posture")
					}
					if args[2] == identityProbeFormat {
						if defect == "missing-name" {
							return "", errors.New("no such container")
						}
						return id, nil
					}
					if args[len(args)-1] != id {
						t.Fatal("metadata used mutable authority", args)
					}
					encoded, _ := json.Marshal(observed)
					return string(encoded), nil
				case "rm":
					if len(args) != 4 || args[1] != "-f" || args[2] != "-v" || args[3] != id {
						t.Fatal("name-only removal", args)
					}
					if defect == "rm-fails" {
						return "", errors.New("daemon denied")
					}
					removed = true
					return "", nil
				case "ps":
					if removed || defect == "known-absent" {
						return "", nil
					}
					return id, nil
				default:
					t.Fatal("teardown executed image code", args)
				}
				return "", nil
			}
			ctx, cancel := context.WithCancel(context.Background())
			cancel()
			known := ""
			if defect == "known-absent" || defect == "known-refused-volume" {
				known = id
			}
			got, err := (ContainerRunner{Run: run, RequireImmutableIdentity: true}).CleanupOwned(ctx, req, known)
			success := defect == "" || defect == "running" || defect == "known-absent" || defect == "known-refused-volume"
			if success && (err != nil || got != id || (!removed && defect != "known-absent")) {
				t.Fatal("retained creation not reclaimed", got, err, removed)
			}
			if !success && (err == nil || removed) {
				t.Fatal("foreign or uncertain resource received removal authority", got, err, removed)
			}
		})
	}
}
