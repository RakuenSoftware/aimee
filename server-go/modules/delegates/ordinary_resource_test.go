package delegates

import (
	"context"
	"encoding/json"
	"errors"
	"strings"
	"testing"
)

func TestOrdinaryIntentNeedsFullCreatorEvidenceBeforeResolvingID(t *testing.T) {
	for _, defect := range []string{"", "foreign-label", "image", "entrypoint", "healthcheck", "command", "env", "bind", "privileged", "known-bad-posture"} {
		t.Run(defect, func(t *testing.T) {
			spec, err := BuildSandboxSpec(writeReq())
			if err != nil {
				t.Fatal(err)
			}
			image := "sha256:" + strings.Repeat("b", 64)
			id := strings.Repeat("c", 64)
			req := DockerCreateRequest{Spec: spec, ContainerName: "ordinary-creator", Image: image, WorkDir: writeReq().Worktree, Command: []string{"sleep", "infinity"}, LeaseNonce: strings.Repeat("a", 64), Startup: &ImageStartup{Entrypoint: []string{"/original-entrypoint"}, Healthcheck: &ImageHealthcheck{Test: []string{"CMD", "image-health"}, Retries: 3}}}
			observed := containerProfile{ID: id, Name: "/" + req.ContainerName, Image: image}
			observed.Config.Image = image
			observed.Config.WorkingDir = req.WorkDir
			observed.Config.Entrypoint = []string{"/original-entrypoint"}
			observed.Config.Cmd = req.Command
			health := *req.Startup.Healthcheck
			observed.Config.Healthcheck = &health
			observed.Config.Labels = map[string]string{OwnerLeaseLabel: req.LeaseNonce}
			var env []string
			for _, value := range spec.Env {
				env = append(env, value.Name+"="+value.Value)
			}
			observed.Config.Env = &env
			no := false
			observed.Host.Privileged = &no
			observed.Host.NetworkMode = "none"
			for _, mount := range spec.Mounts {
				observed.Host.Binds = append(observed.Host.Binds, renderBind(mount, ""))
			}
			switch defect {
			case "foreign-label":
				observed.Config.Labels[OwnerLeaseLabel] = "foreign"
			case "image":
				observed.Image = "sha256:" + strings.Repeat("f", 64)
			case "entrypoint":
				observed.Config.Entrypoint = []string{"/foreign-entrypoint"}
			case "healthcheck":
				observed.Config.Healthcheck.Retries++
			case "command":
				observed.Config.Cmd = []string{"foreign"}
			case "env":
				observed.Config.Env = nil
			case "bind":
				observed.Host.Binds = append(observed.Host.Binds, "/foreign:/extra:rw")
			case "privileged", "known-bad-posture":
				yes := true
				observed.Host.Privileged = &yes
			}
			removed := false
			known := ""
			if defect == "known-bad-posture" {
				known = id
			}
			run := func(_ context.Context, _ string, args ...string) (string, error) {
				switch args[0] {
				case "inspect":
					if known != "" {
						t.Fatal("durable identity re-admitted bad posture")
					}
					if args[2] == identityProbeFormat {
						return id, nil
					}
					bytes, _ := json.Marshal(observed)
					return string(bytes), nil
				case "ps":
					if removed {
						return "", nil
					}
					return id, nil
				case "rm":
					if defect != "" && defect != "known-bad-posture" {
						t.Fatal("label alone authorized foreign cleanup", defect, args)
					}
					if strings.Join(args, " ") != "rm -f -v "+id {
						t.Fatal("name-only or volume-leaking teardown", args)
					}
					removed = true
					return "", nil
				}
				return "", errors.New("unexpected operation")
			}
			got, err := (ContainerRunner{Run: run, RequireImmutableIdentity: true}).CleanupOwned(context.Background(), req, known)
			success := defect == "" || defect == "known-bad-posture"
			if (err == nil) != success || removed != success || (success && got != id) {
				t.Fatalf("err=%v removed=%v id=%s", err, removed, got)
			}
		})
	}
}
