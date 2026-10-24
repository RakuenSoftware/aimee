package delegates

import (
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"reflect"
	"strings"
	"testing"
	"text/template"
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

// Docker's inspect formatter treats absent map keys as errors. Optional image
// properties must still become JSON null, while Config itself remains required.
func TestResolveOrdinaryImageOptionalFieldsUseDockerTemplateSemantics(t *testing.T) {
	image := "sha256:" + strings.Repeat("a", 64)
	health := &ImageHealthcheck{Test: []string{"CMD", "image-health"}, Retries: 3}
	for _, tc := range []struct {
		name       string
		config     map[string]any
		entrypoint []string
		health     *ImageHealthcheck
	}{
		{name: "absent", config: map[string]any{}},
		{name: "null", config: map[string]any{"Entrypoint": nil, "Healthcheck": nil}},
		{name: "entrypoint-only", config: map[string]any{"Entrypoint": []string{"/original-entrypoint"}}, entrypoint: []string{"/original-entrypoint"}},
		{name: "healthcheck-only", config: map[string]any{"Healthcheck": health}, health: health},
		{name: "both-present", config: map[string]any{"Entrypoint": []string{"/original-entrypoint"}, "Healthcheck": health}, entrypoint: []string{"/original-entrypoint"}, health: health},
	} {
		t.Run(tc.name, func(t *testing.T) {
			tc.config["Env"] = []string{"PRIVATE_IMAGE_ENV_MUST_NOT_BE_EMITTED"}
			calls := 0
			ctx, cancel := context.WithCancel(context.Background())
			defer cancel()
			runner := func(got context.Context, binary string, args ...string) (string, error) {
				calls++
				if got != ctx || binary != "held-docker" || len(args) != 5 || args[0] != "image" || args[1] != "inspect" || args[2] != "--format" || args[4] != image {
					t.Fatalf("transport/arguments changed: %q", args)
				}
				format, err := template.New("docker").Option("missingkey=error").Funcs(template.FuncMap{"json": func(v any) (string, error) { raw, err := json.Marshal(v); return string(raw), err }}).Parse(args[3])
				if err != nil {
					return "", err
				}
				var output bytes.Buffer
				err = format.Execute(&output, map[string]any{"Id": image, "Config": tc.config})
				if strings.Contains(output.String(), "PRIVATE_IMAGE_ENV") {
					t.Fatal("unrequested image configuration escaped")
				}
				return output.String(), err
			}
			id, startup, err := ResolveOrdinaryImage(ctx, runner, "held-docker", image)
			if err != nil || id != image || startup == nil || !reflect.DeepEqual(startup.Entrypoint, tc.entrypoint) || !reflect.DeepEqual(startup.Healthcheck, tc.health) || calls != 1 {
				t.Fatalf("startup not preserved: %q %#v %v calls%d", id, startup, err, calls)
			}
		})
	}
}

func TestResolveOrdinaryImageMalformedOrUnavailableStillRefuses(t *testing.T) {
	image := "sha256:" + strings.Repeat("a", 64)
	for _, tc := range []struct {
		name, output string
		err          error
	}{
		{name: "malformed-json", output: "{"},
		{name: "malformed-entrypoint", output: `{"id":"` + image + `","entrypoint":"invalid","healthcheck":null}`},
		{name: "malformed-healthcheck", output: `{"id":"` + image + `","entrypoint":null,"healthcheck":[]}`},
		{name: "invalid-id", output: `{"id":"mutable-name","entrypoint":null,"healthcheck":null}`},
		{name: "transport", err: errors.New("transport refused")},
	} {
		t.Run(tc.name, func(t *testing.T) {
			calls := 0
			id, startup, err := ResolveOrdinaryImage(context.Background(), func(context.Context, string, ...string) (string, error) { calls++; return tc.output, tc.err }, "held-docker", image)
			if err == nil || id != "" || startup != nil || calls != 1 {
				t.Fatal("failed observation was admitted or retried")
			}
		})
	}
}

func TestResolveOrdinaryImageRequiresObservedConfig(t *testing.T) {
	image := "sha256:" + strings.Repeat("a", 64)
	calls := 0
	id, startup, err := ResolveOrdinaryImage(context.Background(), func(_ context.Context, _ string, args ...string) (string, error) {
		calls++
		format, err := template.New("docker").Option("missingkey=error").Funcs(template.FuncMap{"json": func(v any) (string, error) { raw, err := json.Marshal(v); return string(raw), err }}).Parse(args[3])
		if err != nil {
			return "", err
		}
		var output bytes.Buffer
		err = format.Execute(&output, map[string]any{"Id": image})
		return output.String(), err
	}, "held-docker", image)
	if err == nil || id != "" || startup != nil || calls != 1 {
		t.Fatal("missing required Config was admitted or retried")
	}
}
